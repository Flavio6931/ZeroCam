package com.zerocam.mobile

import java.math.BigInteger
import java.security.AlgorithmParameters
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.PrivateKey
import java.security.SecureRandom
import java.security.Signature
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.PBEKeySpec
import javax.crypto.spec.SecretKeySpec

/**
 * Conjunto de claves de sesión derivadas del handshake Wire v2; invocar [wipe] al cerrar la sesión.
 */
class WireV2DerivedKeys(
    val c2sKey: ByteArray,
    val s2cKey: ByteArray,
    val c2sIvBase: ByteArray,
    val s2cIvBase: ByteArray,
    val authKey: ByteArray,
    val transcriptHash: ByteArray
) {
    /** Limpia de memoria las claves simétricas y de autenticación derivadas. */
    fun wipe() {
        c2sKey.fill(0)
        s2cKey.fill(0)
        c2sIvBase.fill(0)
        s2cIvBase.fill(0)
        authKey.fill(0)
        transcriptHash.fill(0)
    }
}

/**
 * Cifrador/descifrador AEAD AES-256-GCM con contador monotónico de 64 bits (< 2^48) y AAD por cabecera.
 */
class Aes256GcmCipher(key32: ByteArray, ivBase12: ByteArray) {
    private val keyBytes: ByteArray = key32.copyOf()
    private val ivBase: ByteArray = ivBase12.copyOf()
    private val keySpec = SecretKeySpec(keyBytes, "AES")
    private var seq: Long = 0L
    private var closed: Boolean = false

    init {
        require(keyBytes.size == WireCrypto.AEAD_KEY_BYTES) { "La clave AES-256 debe medir 32 bytes." }
        require(ivBase.size == WireCrypto.AEAD_IV_BYTES) { "El IV base GCM debe medir 12 bytes." }
    }

    /** Cifra el siguiente paquete con AAD de 20 B devolviendo ciphertext||tag[16] e incrementando seq. */
    fun encryptNext(aad: ByteArray, plaintext: ByteArray): ByteArray {
        check(!closed) { "Cifrador AEAD cerrado." }
        check(seq < WireCrypto.MAX_AEAD_SEQ) { "Límite de secuencia AEAD excedido; requiere re-handshake." }
        val nonce = WireCrypto.buildAeadNonce(ivBase, seq)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, keySpec, GCMParameterSpec(128, nonce))
        if (aad.isNotEmpty()) {
            cipher.updateAAD(aad)
        }
        val out = cipher.doFinal(plaintext)
        seq++
        return out
    }

    /** Descifra y verifica el tag GCM de 16 B del siguiente paquete incrementando seq solo si es auténtico. */
    fun decryptNext(aad: ByteArray, ctAndTag: ByteArray): ByteArray {
        check(!closed) { "Cifrador AEAD cerrado." }
        check(seq < WireCrypto.MAX_AEAD_SEQ) { "Límite de secuencia AEAD excedido." }
        require(ctAndTag.size >= WireCrypto.AEAD_TAG_BYTES) { "Payload AEAD truncado." }
        val nonce = WireCrypto.buildAeadNonce(ivBase, seq)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, keySpec, GCMParameterSpec(128, nonce))
        if (aad.isNotEmpty()) {
            cipher.updateAAD(aad)
        }
        val out = cipher.doFinal(ctAndTag)
        seq++
        return out
    }

    /** Invalida el cifrador y limpia el material sensible en memoria. */
    fun close() {
        closed = true
        keyBytes.fill(0)
        ivBase.fill(0)
    }
}

/**
 * Primitivas criptográficas nativas JCA (compatible desde Android API 26) interoperables byte a byte con Windows CNG.
 */
object WireCrypto {
    const val NONCE_BYTES = 16
    const val PUBKEY_SEC1_BYTES = 65
    const val ECDSA_SIG_BYTES = 64
    const val PROOF_BYTES = 32
    const val AEAD_KEY_BYTES = 32
    const val AEAD_IV_BYTES = 12
    const val AEAD_TAG_BYTES = 16
    const val PBKDF2_ITERATIONS = 100_000
    const val MAX_AEAD_SEQ = 1L shl 48

    private val secureRandom = SecureRandom()

    private val p256Spec: ECParameterSpec by lazy {
        val params = AlgorithmParameters.getInstance("EC")
        params.init(ECGenParameterSpec("secp256r1"))
        params.getParameterSpec(ECParameterSpec::class.java)
    }

    /** Genera bytes aleatorios criptográficamente seguros. */
    fun randomBytes(len: Int): ByteArray = ByteArray(len).also { secureRandom.nextBytes(it) }

    /** Codifica un arreglo de bytes en hexadecimal en mayúsculas. */
    fun bytesToHexUpper(bytes: ByteArray): String {
        val hexChars = "0123456789ABCDEF"
        val sb = StringBuilder(bytes.size * 2)
        for (b in bytes) {
            val v = b.toInt() and 0xFF
            sb.append(hexChars[v ushr 4])
            sb.append(hexChars[v and 0x0F])
        }
        return sb.toString()
    }

    /** Decodifica una cadena hexadecimal (par) a bytes o devuelve null si es inválida. */
    fun hexToBytes(hex: String): ByteArray? {
        if (hex.length % 2 != 0) return null
        val out = ByteArray(hex.length / 2)
        for (i in out.indices) {
            val hi = Character.digit(hex[i * 2], 16)
            val lo = Character.digit(hex[i * 2 + 1], 16)
            if (hi < 0 || lo < 0) return null
            out[i] = ((hi shl 4) or lo).toByte()
        }
        return out
    }

    /** Compara dos arreglos de bytes en tiempo constante. */
    fun constantTimeEquals(a: ByteArray, b: ByteArray): Boolean = MessageDigest.isEqual(a, b)

    /** Calcula SHA-256. */
    fun sha256(data: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(data)

    /** Calcula HMAC-SHA256 (soportando clave vacía conforme a RFC 2104). */
    fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray {
        val effectiveKey = if (key.isEmpty()) ByteArray(1) else key
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(effectiveKey, "HmacSHA256"))
        return mac.doFinal(data)
    }

    /** Deriva una clave con PBKDF2-HMAC-SHA256 (nativo desde API 26). */
    fun pbkdf2HmacSha256(pin: String, salt: ByteArray, iterations: Int = PBKDF2_ITERATIONS, outLen: Int = 32): ByteArray {
        val chars = pin.toCharArray()
        val spec = PBEKeySpec(chars, salt, iterations, outLen * 8)
        return try {
            SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).encoded
        } finally {
            spec.clearPassword()
            chars.fill('\u0000')
        }
    }

    /** Deriva material de claves con RFC 5869 HKDF-SHA256 idéntico a Windows CNG WireCrypto.cpp. */
    fun hkdfSha256(salt: ByteArray, ikm: ByteArray, info: ByteArray, okmLen: Int): ByteArray {
        require(okmLen in 1..(255 * 32))
        val actualSalt = if (salt.isEmpty()) ByteArray(32) else salt
        val prk = hmacSha256(actualSalt, ikm)
        val okm = ByteArray(okmLen)
        var prevT = ByteArray(0)
        var written = 0
        var counter = 1
        try {
            while (written < okmLen) {
                val input = ByteArray(prevT.size + info.size + 1)
                System.arraycopy(prevT, 0, input, 0, prevT.size)
                System.arraycopy(info, 0, input, prevT.size, info.size)
                input[input.size - 1] = counter.toByte()
                val t = hmacSha256(prk, input)
                input.fill(0)
                val take = minOf(t.size, okmLen - written)
                System.arraycopy(t, 0, okm, written, take)
                prevT.fill(0)
                prevT = t
                written += take
                counter++
            }
            return okm
        } finally {
            prevT.fill(0)
            prk.fill(0)
        }
    }

    /** Calcula la huella FP (16 hex mayúsculas = primeros 8 B de SHA-256) de una clave pública SEC1 de 65 B. */
    fun computeFingerprintHex(pubSec1: ByteArray): String {
        require(pubSec1.size == PUBKEY_SEC1_BYTES && pubSec1[0] == 0x04.toByte())
        val digest = sha256(pubSec1)
        return bytesToHexUpper(digest.copyOfRange(0, 8))
    }

    /** Formatea una huella FP de 16 hex como XXXX-XXXX-XXXX-XXXX para la interfaz. */
    fun formatFingerprintDisplay(fpHex16: String): String {
        if (fpHex16.length != 16) return fpHex16.ifEmpty { "Sin vincular" }
        return "${fpHex16.substring(0, 4)}-${fpHex16.substring(4, 8)}-${fpHex16.substring(8, 12)}-${fpHex16.substring(12, 16)}"
    }

    /** Genera un par de claves efímero EC P-256 (secp256r1). */
    fun generateP256KeyPair(): KeyPair {
        val kpg = KeyPairGenerator.getInstance("EC")
        kpg.initialize(ECGenParameterSpec("secp256r1"), secureRandom)
        return kpg.generateKeyPair()
    }

    /** Codifica una ECPublicKey P-256 en formato SEC1 sin comprimir (65 B: 0x04 || X[32] || Y[32]). */
    fun encodeSec1PublicKey(pub: ECPublicKey): ByteArray {
        val x = bigIntToFixed32(pub.w.affineX)
        val y = bigIntToFixed32(pub.w.affineY)
        val out = ByteArray(PUBKEY_SEC1_BYTES)
        out[0] = 0x04
        System.arraycopy(x, 0, out, 1, 32)
        System.arraycopy(y, 0, out, 33, 32)
        return out
    }

    /** Decodifica una clave pública SEC1 sin comprimir (65 B) a ECPublicKey P-256 (compatible API 26). */
    fun decodeSec1PublicKey(sec1: ByteArray): ECPublicKey {
        require(sec1.size == PUBKEY_SEC1_BYTES && sec1[0] == 0x04.toByte()) { "Clave pública SEC1 P-256 inválida." }
        val x = BigInteger(1, sec1.copyOfRange(1, 33))
        val y = BigInteger(1, sec1.copyOfRange(33, 65))
        val kf = KeyFactory.getInstance("EC")
        return kf.generatePublic(ECPublicKeySpec(ECPoint(x, y), p256Spec)) as ECPublicKey
    }

    /** Calcula el secreto compartido ECDH P-256 en orden big-endian (32 B exactos). */
    fun computeEcdhSharedSecretBE(privKey: PrivateKey, peerPubSec1: ByteArray): ByteArray {
        val peerPub = decodeSec1PublicKey(peerPubSec1)
        val ka = KeyAgreement.getInstance("ECDH")
        ka.init(privKey)
        ka.doPhase(peerPub, true)
        val raw = ka.generateSecret()
        if (raw.size == 32) return raw
        val padded = ByteArray(32)
        val srcOff = maxOf(0, raw.size - 32)
        val len = minOf(raw.size, 32)
        System.arraycopy(raw, srcOff, padded, 32 - len, len)
        raw.fill(0)
        return padded
    }

    /** Convierte una firma ECDSA P-256 IEEE P1363 raw (64 B: r[32]||s[32]) a ASN.1 DER para JCA API 26. */
    fun p1363ToDer(sig64: ByteArray): ByteArray {
        require(sig64.size == ECDSA_SIG_BYTES) { "La firma IEEE P1363 P-256 debe medir 64 bytes." }
        val r = BigInteger(1, sig64.copyOfRange(0, 32))
        val s = BigInteger(1, sig64.copyOfRange(32, 64))
        require(r.signum() > 0 && s.signum() > 0) { "Componentes r/s inválidos en firma ECDSA." }
        val rBytes = r.toByteArray()
        val sBytes = s.toByteArray()
        val seqLen = 2 + rBytes.size + 2 + sBytes.size
        require(seqLen < 128)
        val out = ByteArray(2 + seqLen)
        var idx = 0
        out[idx++] = 0x30
        out[idx++] = seqLen.toByte()
        out[idx++] = 0x02
        out[idx++] = rBytes.size.toByte()
        System.arraycopy(rBytes, 0, out, idx, rBytes.size)
        idx += rBytes.size
        out[idx++] = 0x02
        out[idx++] = sBytes.size.toByte()
        System.arraycopy(sBytes, 0, out, idx, sBytes.size)
        return out
    }

    /** Convierte una firma ECDSA P-256 ASN.1 DER a formato IEEE P1363 raw de 64 bytes. */
    fun derToP1363(der: ByteArray): ByteArray {
        require(der.size >= 8 && der[0] == 0x30.toByte()) { "Firma DER inválida." }
        val seqLen = der[1].toInt() and 0xFF
        require(seqLen < 128 && 2 + seqLen == der.size) { "Longitud SEQUENCE DER inválida." }
        var idx = 2
        require(der[idx++] == 0x02.toByte()) { "Falta tag INTEGER r en firma DER." }
        val rLen = der[idx++].toInt() and 0xFF
        require(rLen > 0 && idx + rLen + 2 < der.size) { "Longitud r inválida en firma DER." }
        val r = BigInteger(1, der.copyOfRange(idx, idx + rLen))
        idx += rLen
        require(der[idx++] == 0x02.toByte()) { "Falta tag INTEGER s en firma DER." }
        val sLen = der[idx++].toInt() and 0xFF
        require(sLen > 0 && idx + sLen == der.size) { "Longitud s inválida en firma DER." }
        val s = BigInteger(1, der.copyOfRange(idx, idx + sLen))

        val out = ByteArray(ECDSA_SIG_BYTES)
        System.arraycopy(bigIntToFixed32(r), 0, out, 0, 32)
        System.arraycopy(bigIntToFixed32(s), 0, out, 32, 32)
        return out
    }

    /** Firma un mensaje con ECDSA P-256 + SHA-256 devolviendo formato IEEE P1363 (64 B). */
    fun signEcdsaP1363(privKey: PrivateKey, msg: ByteArray): ByteArray {
        val sig = Signature.getInstance("SHA256withECDSA")
        sig.initSign(privKey, secureRandom)
        sig.update(msg)
        return derToP1363(sig.sign())
    }

    /** Verifica una firma ECDSA P-256 en formato IEEE P1363 (64 B) contra una clave pública SEC1 de 65 B. */
    fun verifyEcdsaP1363(pubSec1: ByteArray, msg: ByteArray, sig64: ByteArray): Boolean {
        return try {
            val pub = decodeSec1PublicKey(pubSec1)
            val der = p1363ToDer(sig64)
            val sig = Signature.getInstance("SHA256withECDSA")
            sig.initVerify(pub)
            sig.update(msg)
            sig.verify(der)
        } catch (_: Exception) {
            false
        }
    }

    /** Construye el bloque exacto de 238 B firmado en ServerHello de Wire v2. */
    fun buildServerHelloSignedBytes(
        authMode: Byte,
        clientNonce: ByteArray,
        clientEphPub: ByteArray,
        serverNonce: ByteArray,
        serverEphPub: ByteArray,
        serverIdPub: ByteArray
    ): ByteArray {
        val prefix = "ZCAM-V2-SH".toByteArray(Charsets.US_ASCII)
        val out = ByteArray(prefix.size + 1 + NONCE_BYTES + PUBKEY_SEC1_BYTES + NONCE_BYTES + PUBKEY_SEC1_BYTES + PUBKEY_SEC1_BYTES)
        var off = 0
        System.arraycopy(prefix, 0, out, off, prefix.size); off += prefix.size
        out[off++] = authMode
        System.arraycopy(clientNonce, 0, out, off, NONCE_BYTES); off += NONCE_BYTES
        System.arraycopy(clientEphPub, 0, out, off, PUBKEY_SEC1_BYTES); off += PUBKEY_SEC1_BYTES
        System.arraycopy(serverNonce, 0, out, off, NONCE_BYTES); off += NONCE_BYTES
        System.arraycopy(serverEphPub, 0, out, off, PUBKEY_SEC1_BYTES); off += PUBKEY_SEC1_BYTES
        System.arraycopy(serverIdPub, 0, out, off, PUBKEY_SEC1_BYTES)
        return out
    }

    /** Deriva las claves AEAD y de autenticación Wire v2 (PBKDF2 100k + HKDF-SHA256) idénticas a Windows CNG. */
    fun deriveWireV2Keys(
        authMode: Byte,
        clientNonce: ByteArray,
        clientEphPub: ByteArray,
        serverNonce: ByteArray,
        serverEphPub: ByteArray,
        serverIdPub: ByteArray,
        sigEcdsa: ByteArray,
        ecdhSecretBE: ByteArray,
        pin: String
    ): WireV2DerivedKeys {
        require(authMode == WireProtocol.AUTH_MODE_USB_LOCAL || authMode == WireProtocol.AUTH_MODE_WIFI_PIN)
        val pinKey = if (authMode == WireProtocol.AUTH_MODE_WIFI_PIN) {
            require(pin.isNotEmpty()) { "El PIN Wi-Fi es obligatorio en modo Wi-Fi." }
            val saltPrefix = "ZCAM-V2-PIN-SALT".toByteArray(Charsets.US_ASCII)
            val saltPre = ByteArray(saltPrefix.size + NONCE_BYTES + NONCE_BYTES)
            System.arraycopy(saltPrefix, 0, saltPre, 0, saltPrefix.size)
            System.arraycopy(clientNonce, 0, saltPre, saltPrefix.size, NONCE_BYTES)
            System.arraycopy(serverNonce, 0, saltPre, saltPrefix.size + NONCE_BYTES, NONCE_BYTES)
            val pinSalt = sha256(saltPre)
            try {
                pbkdf2HmacSha256(pin, pinSalt, PBKDF2_ITERATIONS, 32)
            } finally {
                pinSalt.fill(0)
            }
        } else {
            ByteArray(32)
        }

        val transPrefix = "ZCAM-V2-TRANSCRIPT".toByteArray(Charsets.US_ASCII)
        val transcript = ByteArray(transPrefix.size + 1 + NONCE_BYTES + PUBKEY_SEC1_BYTES + NONCE_BYTES + PUBKEY_SEC1_BYTES + PUBKEY_SEC1_BYTES + ECDSA_SIG_BYTES)
        var off = 0
        System.arraycopy(transPrefix, 0, transcript, off, transPrefix.size); off += transPrefix.size
        transcript[off++] = authMode
        System.arraycopy(clientNonce, 0, transcript, off, NONCE_BYTES); off += NONCE_BYTES
        System.arraycopy(clientEphPub, 0, transcript, off, PUBKEY_SEC1_BYTES); off += PUBKEY_SEC1_BYTES
        System.arraycopy(serverNonce, 0, transcript, off, NONCE_BYTES); off += NONCE_BYTES
        System.arraycopy(serverEphPub, 0, transcript, off, PUBKEY_SEC1_BYTES); off += PUBKEY_SEC1_BYTES
        System.arraycopy(serverIdPub, 0, transcript, off, PUBKEY_SEC1_BYTES); off += PUBKEY_SEC1_BYTES
        System.arraycopy(sigEcdsa, 0, transcript, off, ECDSA_SIG_BYTES)
        val transcriptHash = sha256(transcript)

        val ikm = ByteArray(64)
        System.arraycopy(ecdhSecretBE, 0, ikm, 0, 32)
        System.arraycopy(pinKey, 0, ikm, 32, 32)
        pinKey.fill(0)

        val okm = try {
            hkdfSha256(transcriptHash, ikm, "ZCAM-V2-KEYS".toByteArray(Charsets.US_ASCII), 120)
        } finally {
            ikm.fill(0)
        }

        return try {
            WireV2DerivedKeys(
                c2sKey = okm.copyOfRange(0, 32),
                s2cKey = okm.copyOfRange(32, 64),
                c2sIvBase = okm.copyOfRange(64, 76),
                s2cIvBase = okm.copyOfRange(76, 88),
                authKey = okm.copyOfRange(88, 120),
                transcriptHash = transcriptHash
            )
        } finally {
            okm.fill(0)
        }
    }

    /** Calcula la prueba HMAC-SHA256 de ClientFinish ("client") o ServerFinish ("server"). */
    fun computeHandshakeProof(authKey: ByteArray, transcriptHash: ByteArray, label: String): ByteArray {
        val labelBytes = label.toByteArray(Charsets.US_ASCII)
        val input = ByteArray(transcriptHash.size + labelBytes.size)
        System.arraycopy(transcriptHash, 0, input, 0, transcriptHash.size)
        System.arraycopy(labelBytes, 0, input, transcriptHash.size, labelBytes.size)
        return hmacSha256(authKey, input)
    }

    /** Calcula nonce[12] = ivBase XOR (0x00000000 || seq64_be). */
    fun buildAeadNonce(ivBase: ByteArray, seq: Long): ByteArray {
        val out = ivBase.copyOf()
        for (i in 0 until 8) {
            val seqByte = ((seq ushr ((7 - i) * 8)) and 0xFFL).toInt()
            out[4 + i] = ((out[4 + i].toInt() and 0xFF) xor seqByte).toByte()
        }
        return out
    }

    private fun bigIntToFixed32(bi: BigInteger): ByteArray {
        val raw = bi.toByteArray()
        val out = ByteArray(32)
        when {
            raw.size == 32 -> System.arraycopy(raw, 0, out, 0, 32)
            raw.size == 33 && raw[0] == 0.toByte() -> System.arraycopy(raw, 1, out, 0, 32)
            raw.size < 32 -> System.arraycopy(raw, 0, out, 32 - raw.size, raw.size)
            else -> throw IllegalArgumentException("Coordenada P-256 fuera de rango (${raw.size} B).")
        }
        return out
    }
}
