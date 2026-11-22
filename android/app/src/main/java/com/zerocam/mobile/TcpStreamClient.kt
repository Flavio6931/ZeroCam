package com.zerocam.mobile

import java.io.BufferedOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.interfaces.ECPublicKey
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * Resultado del handshake autenticado ZeroCam Wire v2 (ECC P-256 + AES-256-GCM).
 */
data class WireHandshakeResult(
    val ok: Boolean,
    val serverFpHex: String = "",
    val serverPubHex: String = "",
    val errorCode: Byte = 0,
    val errorMessage: String = ""
)

/**
 * Cliente TCP cifrado de baja latencia bajo ZeroCam Wire v2 (ECDH/ECDSA P-256 + AES-256-GCM).
 */
class TcpStreamClient(
    private val onControlJson: ((String) -> Unit)? = null,
    private val onCongestionRequestKeyFrame: (() -> Unit)? = null
) {
    private class QueuedVideoFrame(
        val payload: ByteArray,
        val ptsUs: Long,
        val isKeyframe: Boolean,
        val hasConfig: Boolean,
        val rotationDegreesCw: Int
    )

    private val connected = AtomicBoolean(false)
    private val sendLock = Any()
    private val bytesSentWindow = AtomicLong(0)
    private val framesSentTotal = AtomicLong(0)
    private val videoQueue = ArrayBlockingQueue<QueuedVideoFrame>(4)

    @Volatile private var socket: Socket? = null
    @Volatile private var connectingSocket: Socket? = null
    @Volatile private var outStream: OutputStream? = null
    @Volatile private var sendCipher: Aes256GcmCipher? = null
    @Volatile private var lastActivityMs: Long = 0L
    @Volatile private var dropUntilKeyframe: Boolean = false
    @Volatile private var lastCongestionIdrMs: Long = 0L
    private var connectGeneration: Long = 0L
    private var readerThread: Thread? = null
    private var senderThread: Thread? = null

    fun isConnected(): Boolean = connected.get()
    fun totalFramesSent(): Long = framesSentTotal.get()

    /**
     * Aborta el socket sin esperar bloqueo si la escritura quedó congelada (p. ej. al desenchufar USB).
     */
    fun forceAbortIfStalled(maxIdleMs: Long): Boolean {
        if (!connected.get()) return false
        val idle = System.currentTimeMillis() - lastActivityMs
        if (idle <= maxIdleMs) return false
        connected.set(false)
        try { socket?.close() } catch (_: Exception) {}
        return true
    }

    /**
     * Consume y reinicia los bytes acumulados en la ventana actual para calcular bitrate en kbps.
     */
    fun drainBytesWindow(): Long = bytesSentWindow.getAndSet(0)

    /**
     * Consulta la identidad ECDSA (huella FP + clave pública) de una PC por TCP manual antes de confiar en ella.
     */
    fun probeServerIdentity(host: String, port: Int, timeoutMs: Int = 2000): DiscoveredHost? {
        var sock: Socket? = null
        return try {
            sock = Socket()
            sock.tcpNoDelay = true
            sock.soTimeout = timeoutMs
            sock.connect(InetSocketAddress(host, port), timeoutMs)
            val input = sock.getInputStream()
            val out = BufferedOutputStream(sock.getOutputStream(), 4096)

            val clientNonce = WireCrypto.randomBytes(WireCrypto.NONCE_BYTES)
            val ephKp = WireCrypto.generateP256KeyPair()
            val clientEphPub = WireCrypto.encodeSec1PublicKey(ephKp.public as ECPublicKey)
            val chPayload = ByteArray(WireProtocol.CLIENT_HELLO_BYTES)
            System.arraycopy(clientNonce, 0, chPayload, 0, WireCrypto.NONCE_BYTES)
            System.arraycopy(clientEphPub, 0, chPayload, WireCrypto.NONCE_BYTES, WireCrypto.PUBKEY_SEC1_BYTES)

            val chHdr = WireProtocol.encodeHeader(WireProtocol.TYPE_HANDSHAKE_CLIENT_HELLO, 0, 0L, chPayload.size)
            out.write(chHdr)
            out.write(chPayload)
            out.flush()

            val shHdrBuf = ByteArray(WireProtocol.HEADER_BYTES)
            if (!readExact(input, shHdrBuf, shHdrBuf.size)) return null
            val bb = ByteBuffer.wrap(shHdrBuf).order(ByteOrder.LITTLE_ENDIAN)
            val magic = bb.int
            val version = bb.get()
            val type = bb.get()
            bb.short
            bb.long
            val payloadLen = bb.int
            if (magic != WireProtocol.MAGIC || version != WireProtocol.VERSION ||
                type != WireProtocol.TYPE_HANDSHAKE_SERVER_HELLO || payloadLen != WireProtocol.SERVER_HELLO_BYTES
            ) {
                return null
            }

            val sh = ByteArray(payloadLen)
            if (!readExact(input, sh, payloadLen)) return null
            val authMode = sh[0]
            val serverNonce = sh.copyOfRange(1, 17)
            val serverEphPub = sh.copyOfRange(17, 82)
            val serverIdPub = sh.copyOfRange(82, 147)
            val sigEcdsa = sh.copyOfRange(147, 211)
            val signedMsg = WireCrypto.buildServerHelloSignedBytes(
                authMode, clientNonce, clientEphPub, serverNonce, serverEphPub, serverIdPub
            )
            if (!WireCrypto.verifyEcdsaP1363(serverIdPub, signedMsg, sigEcdsa)) return null

            val fpHex = WireCrypto.computeFingerprintHex(serverIdPub)
            val pubHex = WireCrypto.bytesToHexUpper(serverIdPub)
            DiscoveredHost(
                ip = host,
                port = port,
                name = host,
                pinRequired = (authMode == WireProtocol.AUTH_MODE_WIFI_PIN),
                fingerprintHex = fpHex,
                publicKeyHex = pubHex
            )
        } catch (_: Exception) {
            null
        } finally {
            try { sock?.close() } catch (_: Exception) {}
        }
    }

    /**
     * Establece conexión TCP y ejecuta el handshake Wire v2 de 4 mensajes activando cifrado AEAD AES-256-GCM.
     */
    fun connect(
        host: String,
        port: Int,
        isUsb: Boolean,
        wifiPin: String = "",
        expectedFpHex: String? = null,
        expectedPubHex: String? = null,
        timeoutMs: Int = 3000
    ): WireHandshakeResult {
        disconnect()
        if (!isUsb && wifiPin.isEmpty()) {
            return WireHandshakeResult(
                ok = false,
                errorCode = WireProtocol.ERR_PIN_REQUIRED,
                errorMessage = "Configura el PIN Wi-Fi de tu PC en Ajustes"
            )
        }
        if (!isUsb && expectedFpHex.isNullOrBlank()) {
            return WireHandshakeResult(
                ok = false,
                errorCode = WireProtocol.ERR_PROTOCOL,
                errorMessage = "PC Wi-Fi no verificada (vincula primero por USB o confirma huella FP)"
            )
        }

        var sock: Socket? = null
        var derivedKeys: WireV2DerivedKeys? = null
        var c2sCipher: Aes256GcmCipher? = null
        var s2cCipher: Aes256GcmCipher? = null
        val myGen = synchronized(sendLock) { ++connectGeneration }
        return try {
            sock = Socket()
            sock.tcpNoDelay = true
            sock.sendBufferSize = if (isUsb) 256 * 1024 else 128 * 1024
            sock.soTimeout = maxOf(timeoutMs, 6000)
            // Registra el socket en vuelo para que disconnect() pueda abortar inmediatamente un handshake bloqueado.
            synchronized(sendLock) {
                if (myGen != connectGeneration) {
                    return failClose(sock, 0, "Conexión cancelada")
                }
                connectingSocket = sock
            }
            sock.connect(InetSocketAddress(host, port), timeoutMs)

            val input = sock.getInputStream()
            val rawOut = BufferedOutputStream(sock.getOutputStream(), 4096)
            val directOut = sock.getOutputStream()

            // 1. Mensaje ClientHello (nonce[16] || clientEphPub[65])
            val clientNonce = WireCrypto.randomBytes(WireCrypto.NONCE_BYTES)
            val ephKp = WireCrypto.generateP256KeyPair()
            val clientEphPub = WireCrypto.encodeSec1PublicKey(ephKp.public as ECPublicKey)
            val chPayload = ByteArray(WireProtocol.CLIENT_HELLO_BYTES)
            System.arraycopy(clientNonce, 0, chPayload, 0, WireCrypto.NONCE_BYTES)
            System.arraycopy(clientEphPub, 0, chPayload, WireCrypto.NONCE_BYTES, WireCrypto.PUBKEY_SEC1_BYTES)
            val chHdr = WireProtocol.encodeHeader(WireProtocol.TYPE_HANDSHAKE_CLIENT_HELLO, 0, 0L, chPayload.size)
            rawOut.write(chHdr)
            rawOut.write(chPayload)
            rawOut.flush()

            // 2. Mensaje ServerHello (authMode[1] || serverNonce[16] || serverEphPub[65] || serverIdPub[65] || sig[64])
            val shHdrBuf = ByteArray(WireProtocol.HEADER_BYTES)
            if (!readExact(input, shHdrBuf, shHdrBuf.size)) {
                return failClose(sock, 0, "Conexión cerrada durante ServerHello")
            }
            val shBb = ByteBuffer.wrap(shHdrBuf).order(ByteOrder.LITTLE_ENDIAN)
            val shMagic = shBb.int
            val shVer = shBb.get()
            val shType = shBb.get()
            shBb.short
            shBb.long
            val shLen = shBb.int
            if (shMagic != WireProtocol.MAGIC || shVer != WireProtocol.VERSION) {
                return failClose(sock, WireProtocol.ERR_PROTOCOL, "La PC requiere ZeroCam v1.2.0 (Wire v2)")
            }
            if (shType == WireProtocol.TYPE_HANDSHAKE_ERROR && shLen == 1) {
                val errArr = ByteArray(1)
                val code = if (readExact(input, errArr, 1)) errArr[0] else WireProtocol.ERR_PROTOCOL
                return failClose(sock, code, describeHandshakeError(code))
            }
            if (shType != WireProtocol.TYPE_HANDSHAKE_SERVER_HELLO || shLen != WireProtocol.SERVER_HELLO_BYTES) {
                return failClose(sock, WireProtocol.ERR_PROTOCOL, "ServerHello inválido")
            }

            val sh = ByteArray(shLen)
            if (!readExact(input, sh, shLen)) {
                return failClose(sock, 0, "ServerHello truncado")
            }
            val authMode = sh[0]
            val expectedAuthMode = if (isUsb) WireProtocol.AUTH_MODE_USB_LOCAL else WireProtocol.AUTH_MODE_WIFI_PIN
            if (authMode != expectedAuthMode) {
                return failClose(sock, WireProtocol.ERR_PROTOCOL, "Modo de autenticación rechazado (posible downgrade)")
            }

            val serverNonce = sh.copyOfRange(1, 17)
            val serverEphPub = sh.copyOfRange(17, 82)
            val serverIdPub = sh.copyOfRange(82, 147)
            val sigEcdsa = sh.copyOfRange(147, 211)

            val signedMsg = WireCrypto.buildServerHelloSignedBytes(
                authMode, clientNonce, clientEphPub, serverNonce, serverEphPub, serverIdPub
            )
            if (!WireCrypto.verifyEcdsaP1363(serverIdPub, signedMsg, sigEcdsa)) {
                return failClose(sock, WireProtocol.ERR_PROTOCOL, "Firma ECDSA de la PC inválida")
            }

            val serverFpHex = WireCrypto.computeFingerprintHex(serverIdPub)
            val serverPubHex = WireCrypto.bytesToHexUpper(serverIdPub)
            if (!isUsb) {
                if (!serverFpHex.equals(expectedFpHex, ignoreCase = true)) {
                    return failClose(
                        sock,
                        WireProtocol.ERR_PROTOCOL,
                        "Huella FP de la PC no coincide — desvincúlala en Ajustes si cambiaste de equipo"
                    )
                }
                if (!expectedPubHex.isNullOrBlank() && !serverPubHex.equals(expectedPubHex, ignoreCase = true)) {
                    return failClose(sock, WireProtocol.ERR_PROTOCOL, "Clave pública de la PC no coincide")
                }
            }

            // 3. Derivación ECDH + PBKDF2(100k) + HKDF-SHA256 y envío de ClientFinish
            val ecdhSecret = WireCrypto.computeEcdhSharedSecretBE(ephKp.private, serverEphPub)
            val keys = try {
                WireCrypto.deriveWireV2Keys(
                    authMode = authMode,
                    clientNonce = clientNonce,
                    clientEphPub = clientEphPub,
                    serverNonce = serverNonce,
                    serverEphPub = serverEphPub,
                    serverIdPub = serverIdPub,
                    sigEcdsa = sigEcdsa,
                    ecdhSecretBE = ecdhSecret,
                    pin = if (isUsb) "" else wifiPin
                )
            } finally {
                ecdhSecret.fill(0)
            }
            derivedKeys = keys

            val clientProof = WireCrypto.computeHandshakeProof(keys.authKey, keys.transcriptHash, "client")
            val cfHdr = WireProtocol.encodeHeader(WireProtocol.TYPE_HANDSHAKE_CLIENT_FINISH, 0, 0L, clientProof.size)
            rawOut.write(cfHdr)
            rawOut.write(clientProof)
            rawOut.flush()

            // 4. Recepción y verificación de ServerFinish (o rechazo de PIN / rate-limit)
            val sfHdrBuf = ByteArray(WireProtocol.HEADER_BYTES)
            if (!readExact(input, sfHdrBuf, sfHdrBuf.size)) {
                return failClose(sock, 0, "Conexión cerrada esperando ServerFinish")
            }
            val sfBb = ByteBuffer.wrap(sfHdrBuf).order(ByteOrder.LITTLE_ENDIAN)
            val sfMagic = sfBb.int
            val sfVer = sfBb.get()
            val sfType = sfBb.get()
            sfBb.short
            sfBb.long
            val sfLen = sfBb.int
            if (sfMagic != WireProtocol.MAGIC || sfVer != WireProtocol.VERSION) {
                return failClose(sock, WireProtocol.ERR_PROTOCOL, "Respuesta ServerFinish inválida")
            }
            if (sfType == WireProtocol.TYPE_HANDSHAKE_ERROR && sfLen == 1) {
                val errArr = ByteArray(1)
                val code = if (readExact(input, errArr, 1)) errArr[0] else WireProtocol.ERR_PIN_INVALID
                return failClose(sock, code, describeHandshakeError(code))
            }
            if (sfType != WireProtocol.TYPE_HANDSHAKE_SERVER_FINISH || sfLen != WireCrypto.PROOF_BYTES) {
                return failClose(sock, WireProtocol.ERR_PROTOCOL, "ServerFinish malformado")
            }

            val recvServerProof = ByteArray(sfLen)
            if (!readExact(input, recvServerProof, sfLen)) {
                return failClose(sock, 0, "ServerFinish truncado")
            }
            val expectedServerProof = WireCrypto.computeHandshakeProof(keys.authKey, keys.transcriptHash, "server")
            if (!WireCrypto.constantTimeEquals(expectedServerProof, recvServerProof)) {
                return failClose(sock, WireProtocol.ERR_PROTOCOL, "Prueba criptográfica de la PC inválida")
            }

            c2sCipher = Aes256GcmCipher(keys.c2sKey, keys.c2sIvBase)
            s2cCipher = Aes256GcmCipher(keys.s2cKey, keys.s2cIvBase)
            keys.wipe()
            derivedKeys = null

            sock.soTimeout = 0
            videoQueue.clear()
            dropUntilKeyframe = false
            val promoted = synchronized(sendLock) {
                if (connectingSocket === sock) {
                    connectingSocket = null
                }
                if (myGen != connectGeneration) {
                    false
                } else {
                    socket = sock
                    outStream = directOut
                    sendCipher = c2sCipher
                    lastActivityMs = System.currentTimeMillis()
                    connected.set(true)
                    true
                }
            }
            if (!promoted) {
                c2sCipher.close()
                s2cCipher.close()
                return failClose(sock, 0, "Conexión cancelada durante el handshake")
            }
            val activeS2c = s2cCipher
            c2sCipher = null
            s2cCipher = null
            startSenderThread(sock)
            startControlReader(sock, input, activeS2c)
            WireHandshakeResult(ok = true, serverFpHex = serverFpHex, serverPubHex = serverPubHex)
        } catch (e: Exception) {
            try { sock?.close() } catch (_: Exception) {}
            try { c2sCipher?.close() } catch (_: Exception) {}
            try { s2cCipher?.close() } catch (_: Exception) {}
            WireHandshakeResult(ok = false, errorMessage = e.message ?: "Error de conexión")
        } finally {
            derivedKeys?.wipe()
            synchronized(sendLock) {
                if (connectingSocket === sock) {
                    connectingSocket = null
                }
            }
        }
    }

    /**
     * Encola un Access Unit H.264 sin bloquear MediaCodec y descarta P-frames atrasados ante congestión Wi-Fi.
     */
    fun sendVideoPacket(
        payload: ByteArray,
        ptsUs: Long,
        isKeyframe: Boolean,
        hasConfig: Boolean,
        rotationDegreesCw: Int = 0
    ): Boolean {
        if (!connected.get() || payload.size > 8 * 1024 * 1024) return false
        if (dropUntilKeyframe) {
            if (!isKeyframe) return false
            dropUntilKeyframe = false
        }

        val frame = QueuedVideoFrame(payload, ptsUs, isKeyframe, hasConfig, rotationDegreesCw)
        if (videoQueue.offer(frame)) {
            return true
        }

        // Cola saturada: descarta backlog para evitar bufferbloat y solicita IDR fresco si era delta P-frame.
        videoQueue.clear()
        if (isKeyframe) {
            return videoQueue.offer(frame)
        }
        dropUntilKeyframe = true
        val now = System.currentTimeMillis()
        if (now - lastCongestionIdrMs >= 400L) {
            lastCongestionIdrMs = now
            try { onCongestionRequestKeyFrame?.invoke() } catch (_: Exception) {}
        }
        return false
    }

    /**
     * Cifra con AES-256-GCM y envía un comando ControlJson bajo sendLock.
     */
    fun sendControlJson(json: String): Boolean {
        if (!connected.get()) return false
        val body = json.toByteArray(Charsets.UTF_8)
        val header = WireProtocol.encodeHeader(
            type = WireProtocol.TYPE_CONTROL_JSON,
            flags = 0,
            ptsUs = 0L,
            payloadSize = body.size + WireCrypto.AEAD_TAG_BYTES
        )
        return synchronized(sendLock) {
            if (!connected.get()) return@synchronized false
            val out = outStream ?: return@synchronized false
            val cipher = sendCipher ?: return@synchronized false
            try {
                val ctAndTag = cipher.encryptNext(header, body)
                val pkt = ByteArray(header.size + ctAndTag.size)
                System.arraycopy(header, 0, pkt, 0, header.size)
                System.arraycopy(ctAndTag, 0, pkt, header.size, ctAndTag.size)
                out.write(pkt)
                out.flush()
                lastActivityMs = System.currentTimeMillis()
                true
            } catch (_: Exception) {
                disconnect()
                false
            }
        }
    }

    /**
     * Cierra el socket activo (y cualquier handshake en vuelo), limpia cifradores AEAD y detiene los hilos de I/O.
     */
    fun disconnect() {
        connected.set(false)
        dropUntilKeyframe = false
        videoQueue.clear()
        val inFlight: Socket?
        synchronized(sendLock) {
            connectGeneration++
            inFlight = connectingSocket
            connectingSocket = null
            try { sendCipher?.close() } catch (_: Exception) {}
            sendCipher = null
            try { outStream?.close() } catch (_: Exception) {}
            try { socket?.close() } catch (_: Exception) {}
            outStream = null
            socket = null
        }
        try { inFlight?.close() } catch (_: Exception) {}
        senderThread?.interrupt()
        senderThread = null
        readerThread?.interrupt()
        readerThread = null
    }

    private fun startSenderThread(sock: Socket) {
        val t = Thread({
            try {
                while (connected.get() && socket === sock && !Thread.currentThread().isInterrupted) {
                    val item = try {
                        videoQueue.poll(200, TimeUnit.MILLISECONDS)
                    } catch (_: InterruptedException) {
                        break
                    } ?: continue

                    if (!connected.get() || socket !== sock) break
                    val flags = WireProtocol.packFlags(item.isKeyframe, item.hasConfig, item.rotationDegreesCw)
                    val header = WireProtocol.encodeHeader(
                        type = WireProtocol.TYPE_VIDEO_H264,
                        flags = flags,
                        ptsUs = item.ptsUs,
                        payloadSize = item.payload.size + WireCrypto.AEAD_TAG_BYTES
                    )
                    val ok = synchronized(sendLock) {
                        if (!connected.get() || socket !== sock) return@synchronized false
                        val out = outStream ?: return@synchronized false
                        val cipher = sendCipher ?: return@synchronized false
                        try {
                            val ctAndTag = cipher.encryptNext(header, item.payload)
                            val pkt = ByteArray(header.size + ctAndTag.size)
                            System.arraycopy(header, 0, pkt, 0, header.size)
                            System.arraycopy(ctAndTag, 0, pkt, header.size, ctAndTag.size)
                            out.write(pkt)
                            out.flush()
                            lastActivityMs = System.currentTimeMillis()
                            bytesSentWindow.addAndGet(pkt.size.toLong())
                            framesSentTotal.incrementAndGet()
                            true
                        } catch (_: Exception) {
                            false
                        }
                    }
                    if (!ok) {
                        if (socket === sock) disconnect()
                        break
                    }
                }
            } catch (_: Exception) {
                if (socket === sock) disconnect()
            }
        }, "ZeroCamTcpSender")
        t.isDaemon = true
        t.start()
        senderThread = t
    }

    private fun failClose(sock: Socket, code: Byte, msg: String): WireHandshakeResult {
        try { sock.close() } catch (_: Exception) {}
        return WireHandshakeResult(ok = false, errorCode = code, errorMessage = msg)
    }

    private fun describeHandshakeError(code: Byte): String = when (code) {
        WireProtocol.ERR_PIN_INVALID -> "PIN Wi-Fi incorrecto — revísalo en Ajustes"
        WireProtocol.ERR_RATE_LIMITED -> "Bloqueado 5 min por demasiados intentos de PIN"
        WireProtocol.ERR_PIN_REQUIRED -> "PIN Wi-Fi requerido — configúralo en Ajustes"
        else -> "Error de protocolo Wire v2 con la PC"
    }

    private fun startControlReader(sock: Socket, input: InputStream, recvCipher: Aes256GcmCipher) {
        val t = Thread({
            val hdrBuf = ByteArray(WireProtocol.HEADER_BYTES)
            try {
                while (connected.get() && socket === sock && !Thread.currentThread().isInterrupted) {
                    if (!readExact(input, hdrBuf, hdrBuf.size)) break
                    val bb = ByteBuffer.wrap(hdrBuf).order(ByteOrder.LITTLE_ENDIAN)
                    val magic = bb.int
                    val version = bb.get()
                    val type = bb.get()
                    bb.short // flags
                    bb.long // ptsUs
                    val payloadLen = bb.int
                    if (magic != WireProtocol.MAGIC || version != WireProtocol.VERSION ||
                        payloadLen < WireCrypto.AEAD_TAG_BYTES || payloadLen > 1024 * 1024 + WireCrypto.AEAD_TAG_BYTES
                    ) {
                        break
                    }
                    val encBody = ByteArray(payloadLen)
                    if (!readExact(input, encBody, payloadLen)) break
                    val plain = recvCipher.decryptNext(hdrBuf, encBody)
                    if (type == WireProtocol.TYPE_CONTROL_JSON && plain.isNotEmpty()) {
                        try {
                            onControlJson?.invoke(String(plain, Charsets.UTF_8))
                        } catch (_: Exception) {
                        }
                    }
                }
            } catch (_: Exception) {
            } finally {
                recvCipher.close()
                // Solo limpia el estado global si este hilo sigue correspondiendo al socket vigente (evita carrera de reconexión).
                if (socket === sock) {
                    connected.set(false)
                }
                try { sock.close() } catch (_: Exception) {}
            }
        }, "ZeroCamTcpReader")
        t.isDaemon = true
        t.start()
        readerThread = t
    }

    private fun readExact(input: InputStream, dst: ByteArray, len: Int): Boolean {
        var offset = 0
        while (offset < len) {
            val r = input.read(dst, offset, len - offset)
            if (r <= 0) return false
            offset += r
        }
        return true
    }
}
