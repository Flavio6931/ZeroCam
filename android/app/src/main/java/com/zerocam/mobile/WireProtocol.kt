package com.zerocam.mobile

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Parámetros de video recibidos remotamente desde ZeroCam PC mediante ControlJson.
 */
data class VideoControlConfig(
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrateBps: Int
)

/**
 * Comando de enfoque remoto recibido desde Windows ("face", "lock" o "point" con coordenadas [0..1]).
 */
data class FocusControlCommand(
    val mode: String,
    val x: Float = 0.5f,
    val y: Float = 0.5f
)

/**
 * Controles físicos de cámara enviados desde Windows hacia Android en caliente.
 */
data class CameraRemoteControl(
    val front: Boolean? = null,
    val torch: Boolean? = null,
    val zoom: Float? = null,
    val exposureEv: Int? = null,
    val preferFps: Boolean? = null,
    val focusMode: String? = null
)

/**
 * Información de una instancia ZeroCam PC descubierta y autenticada vía Discovery UDP v2 (ECDSA P-256).
 */
data class DiscoveredHost(
    val ip: String,
    val port: Int,
    val name: String,
    val pinRequired: Boolean,
    val fingerprintHex: String = "",
    val publicKeyHex: String = ""
)

/**
 * Definición del protocolo binario cifrado ZeroCam Wire v2 compatible byte a byte con el receptor Windows.
 */
object WireProtocol {
    const val MAGIC: Int = 0x4D41435A // 'ZCAM' little-endian
    const val VERSION: Byte = 2
    const val HEADER_BYTES: Int = 20

    const val TYPE_VIDEO_H264: Byte = 1
    const val TYPE_VIDEO_HEVC: Byte = 2
    const val TYPE_CONTROL_JSON: Byte = 3
    const val TYPE_HEARTBEAT: Byte = 4
    const val TYPE_HANDSHAKE_CLIENT_HELLO: Byte = 16
    const val TYPE_HANDSHAKE_SERVER_HELLO: Byte = 17
    const val TYPE_HANDSHAKE_CLIENT_FINISH: Byte = 18
    const val TYPE_HANDSHAKE_SERVER_FINISH: Byte = 19
    const val TYPE_HANDSHAKE_ERROR: Byte = 20

    const val AUTH_MODE_USB_LOCAL: Byte = 0
    const val AUTH_MODE_WIFI_PIN: Byte = 1

    const val ERR_PIN_INVALID: Byte = 1
    const val ERR_RATE_LIMITED: Byte = 2
    const val ERR_PROTOCOL: Byte = 3
    const val ERR_PIN_REQUIRED: Byte = 4

    const val CLIENT_HELLO_BYTES: Int = WireCrypto.NONCE_BYTES + WireCrypto.PUBKEY_SEC1_BYTES // 81 B
    const val SERVER_HELLO_BYTES: Int = 1 + WireCrypto.NONCE_BYTES + WireCrypto.PUBKEY_SEC1_BYTES + WireCrypto.PUBKEY_SEC1_BYTES + WireCrypto.ECDSA_SIG_BYTES // 211 B

    const val FLAG_KEYFRAME: Short = 1
    const val FLAG_CONFIG_SPS_PPS: Short = 2
    const val FLAG_ROT_SHIFT: Int = 2

    const val DEFAULT_PORT: Int = 5000
    const val DISCOVERY_PORT: Int = 5001
    const val DISCOVERY_MAGIC_V2: String = "ZCAM_DISCOVER_V2"
    const val DISCOVERY_HOST_V2: String = "ZCAM_HOST_V2"

    /**
     * Empaqueta las banderas de keyframe, config SPS/PPS y rotación horaria (0, 90, 180, 270).
     */
    fun packFlags(isKeyframe: Boolean, hasConfig: Boolean, rotationDegreesCw: Int): Short {
        var f = 0
        if (isKeyframe) f = f or FLAG_KEYFRAME.toInt()
        if (hasConfig) f = f or FLAG_CONFIG_SPS_PPS.toInt()
        val rotCode = ((rotationDegreesCw / 90) % 4 + 4) % 4
        f = f or (rotCode shl FLAG_ROT_SHIFT)
        return f.toShort()
    }

    /**
     * Empaqueta la cabecera fija de 20 bytes en orden little-endian para envío TCP.
     */
    fun encodeHeader(type: Byte, flags: Short, ptsUs: Long, payloadSize: Int): ByteArray {
        val buf = ByteBuffer.allocate(HEADER_BYTES).order(ByteOrder.LITTLE_ENDIAN)
        buf.putInt(MAGIC)
        buf.put(VERSION)
        buf.put(type)
        buf.putShort(flags)
        buf.putLong(ptsUs)
        buf.putInt(payloadSize)
        return buf.array()
    }

    /**
     * Parsea y valida un comando JSON "set_video" enviado por ZeroCam PC en tiempo real.
     */
    fun parseSetVideoCommand(json: String): VideoControlConfig? {
        val cmdMatch = Regex(""""cmd"\s*:\s*"([^"]+)"""").find(json) ?: return null
        if (cmdMatch.groupValues[1] != "set_video") return null

        fun extractInt(key: String): Int? =
            Regex(""""$key"\s*:\s*(\d+)""").find(json)?.groupValues?.get(1)?.toIntOrNull()

        val w = extractInt("width") ?: return null
        val h = extractInt("height") ?: return null
        val fps = extractInt("fps") ?: return null
        val bitrate = extractInt("bitrate") ?: return null

        if (w !in 320..3840 || h !in 240..2160 || (w % 2 != 0) || (h % 2 != 0)) return null
        if (fps !in 10..120 || bitrate !in 500_000..25_000_000) return null
        return VideoControlConfig(w, h, fps, bitrate)
    }

    private fun extractObjectCommand(json: String): String? {
        val trimmed = json.trim()
        if (!trimmed.startsWith("{") || !trimmed.endsWith("}")) return null
        return Regex(""""cmd"\s*:\s*"([^"]+)"""").find(trimmed)?.groupValues?.get(1)
    }

    /**
     * Detecta el comando remoto {"cmd":"stop_stream"} enviado al detener el servidor en PC.
     */
    fun isStopStreamCommand(json: String): Boolean = extractObjectCommand(json) == "stop_stream"

    /**
     * Detecta el comando remoto {"cmd":"prefer_usb"} enviado cuando se conecta el cable USB estando en Wi-Fi.
     */
    fun isPreferUsbCommand(json: String): Boolean = extractObjectCommand(json) == "prefer_usb"

    /**
     * Parsea un comando JSON "focus" ("point", "face", "lock") para enfocar en Android.
     */
    fun parseFocusCommand(json: String): FocusControlCommand? {
        val cmdMatch = Regex(""""cmd"\s*:\s*"([^"]+)"""").find(json) ?: return null
        if (cmdMatch.groupValues[1] != "focus") return null
        val mode = Regex(""""mode"\s*:\s*"([^"]+)"""").find(json)?.groupValues?.get(1) ?: "point"
        if (mode == "face" || mode == "lock" || mode == "auto") {
            return FocusControlCommand(mode = mode)
        }
        if (mode != "point") return null

        val x = Regex(""""x"\s*:\s*(-?\d+(?:\.\d+)?)""").find(json)?.groupValues?.get(1)?.toFloatOrNull() ?: return null
        val y = Regex(""""y"\s*:\s*(-?\d+(?:\.\d+)?)""").find(json)?.groupValues?.get(1)?.toFloatOrNull() ?: return null
        if (x !in 0.0f..1.0f || y !in 0.0f..1.0f) return null
        return FocusControlCommand(mode = "point", x = x, y = y)
    }

    /**
     * Parsea un comando JSON "cam_ctrl" para ajustar lente, linterna, zoom, EV, FPS y modo de enfoque.
     */
    fun parseCameraControlCommand(json: String): CameraRemoteControl? {
        val cmdMatch = Regex(""""cmd"\s*:\s*"([^"]+)"""").find(json) ?: return null
        if (cmdMatch.groupValues[1] != "cam_ctrl") return null

        fun extractInt(key: String): Int? =
            Regex(""""$key"\s*:\s*(-?\d+)""").find(json)?.groupValues?.get(1)?.toIntOrNull()

        fun extractBool(key: String): Boolean? {
            extractInt(key)?.let { return it != 0 }
            val bStr = Regex(""""$key"\s*:\s*(true|false)""").find(json)?.groupValues?.get(1) ?: return null
            return bStr == "true"
        }

        val zoom = Regex(""""zoom"\s*:\s*(-?\d+(?:\.\d+)?)""").find(json)?.groupValues?.get(1)?.toFloatOrNull()
            ?.takeIf { it in 1.0f..5.0f }
        val ev = extractInt("ev")?.coerceIn(-2, 2)
        val focusMode = Regex(""""focus_mode"\s*:\s*"([^"]+)"""").find(json)?.groupValues?.get(1)
            ?.takeIf { it == "face" || it == "lock" || it == "auto" }

        return CameraRemoteControl(
            front = extractBool("front"),
            torch = extractBool("torch"),
            zoom = zoom,
            exposureEv = ev,
            preferFps = extractBool("prefer_fps"),
            focusMode = focusMode
        )
    }

    /**
     * Parsea y verifica criptográficamente una respuesta Discovery UDP v2 (ECDSA P-256 + nonce anti-replay + FP).
     */
    fun parseAndVerifyDiscoveryV2Reply(reply: String, hostIp: String, expectedNonce: String): DiscoveredHost? {
        if (!reply.startsWith("$DISCOVERY_HOST_V2|") || hostIp.isBlank() || expectedNonce.length < 16) return null
        val sigMarker = "|SIG="
        val sigIdx = reply.lastIndexOf(sigMarker)
        if (sigIdx <= 0) return null

        val canonical = reply.substring(0, sigIdx)
        val sigHex = reply.substring(sigIdx + sigMarker.length).trim()
        if (sigHex.length != WireCrypto.ECDSA_SIG_BYTES * 2) return null

        val fields = mutableMapOf<String, String>()
        val parts = canonical.split('|')
        if (parts.isEmpty() || parts[0] != DISCOVERY_HOST_V2) return null
        for (i in 1 until parts.size) {
            val eq = parts[i].indexOf('=')
            if (eq <= 0) continue
            fields[parts[i].substring(0, eq)] = parts[i].substring(eq + 1)
        }

        val nonce = fields["NONCE"] ?: return null
        if (!nonce.equals(expectedNonce, ignoreCase = true)) return null

        val port = fields["PORT"]?.toIntOrNull()?.takeIf { it in 1..65535 } ?: return null
        val name = fields["NAME"]?.trim().orEmpty().ifEmpty { hostIp }
        val pinRequired = fields["PIN"] == "1"
        val fpHex = fields["FP"]?.trim()?.uppercase() ?: return null
        val pubHex = fields["PUB"]?.trim()?.uppercase() ?: return null
        if (fpHex.length != 16 || pubHex.length != WireCrypto.PUBKEY_SEC1_BYTES * 2) return null

        val pubBytes = WireCrypto.hexToBytes(pubHex) ?: return null
        val sigBytes = WireCrypto.hexToBytes(sigHex) ?: return null
        if (pubBytes.size != WireCrypto.PUBKEY_SEC1_BYTES || pubBytes[0] != 0x04.toByte()) return null
        if (sigBytes.size != WireCrypto.ECDSA_SIG_BYTES) return null

        // Comprueba correspondencia exacta FP = SHA-256(PUB)[0..7] y firma ECDSA P-256 sobre la carga canónica.
        val computedFp = WireCrypto.computeFingerprintHex(pubBytes)
        if (!computedFp.equals(fpHex, ignoreCase = true)) return null
        if (!WireCrypto.verifyEcdsaP1363(pubBytes, canonical.toByteArray(Charsets.UTF_8), sigBytes)) return null

        return DiscoveredHost(
            ip = hostIp,
            port = port,
            name = name,
            pinRequired = pinRequired,
            fingerprintHex = fpHex,
            publicKeyHex = pubHex
        )
    }

    /**
     * Emite una solicitud Discovery UDP v2 con nonce aleatorio y devuelve el primer host cuya firma ECDSA,
     * huella FP y clave pública completa (si se exigen) coincidan exactamente.
     */
    fun discoverPcHostInfo(
        timeoutMs: Int = 1500,
        targetIp: String = "255.255.255.255",
        requiredFpHex: String? = null,
        requiredPubHex: String? = null,
        discoveryPort: Int = DISCOVERY_PORT
    ): DiscoveredHost? {
        return try {
            val nonce = WireCrypto.bytesToHexUpper(WireCrypto.randomBytes(8))
            val req = "$DISCOVERY_MAGIC_V2|NONCE=$nonce".toByteArray(Charsets.UTF_8)
            val deadline = System.currentTimeMillis() + timeoutMs.coerceAtLeast(200)
            DatagramSocket().use { socket ->
                socket.broadcast = (targetIp == "255.255.255.255")
                val outPkt = DatagramPacket(
                    req,
                    req.size,
                    InetAddress.getByName(targetIp),
                    discoveryPort
                )
                socket.send(outPkt)

                val recvBuf = ByteArray(1024)
                while (true) {
                    val remaining = (deadline - System.currentTimeMillis()).toInt()
                    if (remaining <= 0) break
                    socket.soTimeout = remaining
                    val inPkt = DatagramPacket(recvBuf, recvBuf.size)
                    socket.receive(inPkt)
                    val msg = String(inPkt.data, 0, inPkt.length, Charsets.UTF_8)
                    val hostIp = inPkt.address.hostAddress ?: continue
                    val parsed = parseAndVerifyDiscoveryV2Reply(msg, hostIp, nonce) ?: continue
                    if (!requiredFpHex.isNullOrBlank() &&
                        !parsed.fingerprintHex.equals(requiredFpHex, ignoreCase = true)
                    ) {
                        continue
                    }
                    if (!requiredPubHex.isNullOrBlank() &&
                        !parsed.publicKeyHex.equals(requiredPubHex, ignoreCase = true)
                    ) {
                        continue
                    }
                    return parsed
                }
                null
            }
        } catch (_: Exception) {
            null
        }
    }

    /**
     * Emite un broadcast UDP v2 devolviendo únicamente IP y puerto de ZeroCam PC verificado.
     */
    fun discoverPcHost(
        timeoutMs: Int = 1500,
        requiredFpHex: String? = null,
        requiredPubHex: String? = null
    ): Pair<String, Int>? =
        discoverPcHostInfo(
            timeoutMs = timeoutMs,
            requiredFpHex = requiredFpHex,
            requiredPubHex = requiredPubHex
        )?.let { it.ip to it.port }
}
