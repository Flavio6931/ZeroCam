package com.zerocam.mobile

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.math.BigInteger
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.ServerSocket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.AlgorithmParameters
import java.security.KeyFactory
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPrivateKeySpec
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/**
 * Pruebas unitarias JVM del protocolo cifrado ZeroCam Wire v2 y vectores cruzados exactos con Windows CNG.
 */
class WireProtocolTest {

    @Test
    fun encodeHeader_matchesWindowsWireV2Layout() {
        val ptsUs = 0x0102030405060708L
        val payloadLen = 123456
        val hdr = WireProtocol.encodeHeader(
            type = WireProtocol.TYPE_VIDEO_H264,
            flags = WireProtocol.packFlags(isKeyframe = true, hasConfig = true, rotationDegreesCw = 180),
            ptsUs = ptsUs,
            payloadSize = payloadLen
        )

        assertEquals(20, hdr.size)
        // 'ZCAM' little-endian (0x4D41435A -> 5A 43 41 4D)
        assertArrayEquals(byteArrayOf(0x5A, 0x43, 0x41, 0x4D), hdr.sliceArray(0..3))
        assertEquals(2.toByte(), hdr[4]) // version = Wire v2
        assertEquals(1.toByte(), hdr[5]) // type = VideoH264

        val bb = ByteBuffer.wrap(hdr).order(ByteOrder.LITTLE_ENDIAN)
        bb.int // magic
        bb.get() // version
        bb.get() // type
        val flags = bb.short.toInt() and 0xFFFF
        assertEquals(0x0B, flags) // bit0=1, bit1=1, bits2..3 = 2 (180 deg) -> 11
        assertEquals(ptsUs, bb.long)
        assertEquals(payloadLen, bb.int)

        assertEquals(0, (WireProtocol.packFlags(false, false, 0).toInt() ushr 2) and 0x3)
        assertEquals(1, (WireProtocol.packFlags(false, false, 90).toInt() ushr 2) and 0x3)
        assertEquals(2, (WireProtocol.packFlags(false, false, 180).toInt() ushr 2) and 0x3)
        assertEquals(3, (WireProtocol.packFlags(false, false, 270).toInt() ushr 2) and 0x3)
    }

    @Test
    fun parseSetVideoCommand_validatesAndParsesPayload() {
        val okJson = """{"cmd":"set_video","width":1920,"height":1080,"fps":60,"bitrate":8000000}"""
        assertEquals(VideoControlConfig(1920, 1080, 60, 8_000_000), WireProtocol.parseSetVideoCommand(okJson))

        assertNull(WireProtocol.parseSetVideoCommand("""{"cmd":"ping","width":1280,"height":720,"fps":30,"bitrate":4000000}"""))
        assertNull(WireProtocol.parseSetVideoCommand("""{"cmd":"set_video","width":1281,"height":720,"fps":30,"bitrate":4000000}"""))
        assertNull(WireProtocol.parseSetVideoCommand("""{"cmd":"set_video","width":1280,"height":720,"fps":5,"bitrate":4000000}"""))
        assertNull(WireProtocol.parseSetVideoCommand("""{"cmd":"set_video","width":1280,"height":720,"fps":30,"bitrate":10000}"""))
        assertNull(WireProtocol.parseSetVideoCommand("""not-json"""))
    }

    @Test
    fun parseFocusCommand_validatesModesAndCoords() {
        val pt = WireProtocol.parseFocusCommand("""{"cmd":"focus","mode":"point","x":0.25,"y":0.75}""")
        assertEquals(FocusControlCommand("point", 0.25f, 0.75f), pt)
        assertEquals(FocusControlCommand("face"), WireProtocol.parseFocusCommand("""{"cmd":"focus","mode":"face"}"""))
        assertEquals(FocusControlCommand("lock"), WireProtocol.parseFocusCommand("""{"cmd":"focus","mode":"lock"}"""))
        assertNull(WireProtocol.parseFocusCommand("""{"cmd":"focus","mode":"point","x":1.5,"y":0.5}"""))
        assertNull(WireProtocol.parseFocusCommand("""{"cmd":"other","mode":"face"}"""))
    }

    @Test
    fun parseCameraControlCommand_parsesAllOptionalFields() {
        val ctrl = WireProtocol.parseCameraControlCommand(
            """{"cmd":"cam_ctrl","front":1,"torch":0,"zoom":2.00,"ev":-1,"prefer_fps":1,"focus_mode":"lock"}"""
        )
        assertEquals(
            CameraRemoteControl(front = true, torch = false, zoom = 2.0f, exposureEv = -1, preferFps = true, focusMode = "lock"),
            ctrl
        )
        assertNull(WireProtocol.parseCameraControlCommand("""{"cmd":"set_video","front":1}"""))
    }

    @Test
    fun isStopStreamCommand_matchesOnlyStopStream() {
        assertEquals(true, WireProtocol.isStopStreamCommand("""{"cmd":"stop_stream"}"""))
        assertEquals(true, WireProtocol.isStopStreamCommand(""" { "cmd" : "stop_stream" } """))
        assertEquals(true, WireProtocol.isStopStreamCommand("""{"v":1,"cmd":"stop_stream"}"""))
        assertEquals(false, WireProtocol.isStopStreamCommand("""{"cmd":"stop_stream_v2"}"""))
        assertEquals(false, WireProtocol.isStopStreamCommand("""{"cmd":"set_video"}"""))
        assertEquals(false, WireProtocol.isStopStreamCommand("""{"cmd":"set_video","width":1920,"height":1080,"fps":60,"bitrate":12000000}"""))
        assertEquals(false, WireProtocol.isStopStreamCommand("""{"cmd":"cam_ctrl","front":0,"torch":0,"zoom":1.00,"ev":0,"prefer_fps":1,"focus_mode":"face"}"""))
        assertEquals(false, WireProtocol.isStopStreamCommand("stop_stream"))
    }

    @Test
    fun isPreferUsbCommand_matchesOnlyPreferUsb() {
        assertEquals(true, WireProtocol.isPreferUsbCommand("""{"cmd":"prefer_usb"}"""))
        assertEquals(true, WireProtocol.isPreferUsbCommand(""" { "cmd" : "prefer_usb" } """))
        assertEquals(false, WireProtocol.isPreferUsbCommand("""{"cmd":"prefer_usb_v2"}"""))
        assertEquals(false, WireProtocol.isPreferUsbCommand("""{"cmd":"stop_stream"}"""))
        assertEquals(false, WireProtocol.isPreferUsbCommand("prefer_usb"))
    }

    @Test
    fun discoveryV2_verifiesEcdsaSignatureAndRejectsSpoofingOrLegacy() {
        val kp = WireCrypto.generateP256KeyPair()
        val pubSec1 = WireCrypto.encodeSec1PublicKey(kp.public as ECPublicKey)
        val pubHex = WireCrypto.bytesToHexUpper(pubSec1)
        val fpHex = WireCrypto.computeFingerprintHex(pubSec1)
        val nonce = "0123456789ABCDEF"

        val canonical = "ZCAM_HOST_V2|PORT=5002|NAME=DESKTOP-LAB|PIN=1|FP=$fpHex|NONCE=$nonce|PUB=$pubHex"
        val sig = WireCrypto.signEcdsaP1363(kp.private, canonical.toByteArray(Charsets.UTF_8))
        val validReply = "$canonical|SIG=${WireCrypto.bytesToHexUpper(sig)}"

        val parsed = WireProtocol.parseAndVerifyDiscoveryV2Reply(validReply, "192.168.1.42", nonce)
        assertNotNull(parsed)
        assertEquals(DiscoveredHost("192.168.1.42", 5002, "DESKTOP-LAB", true, fpHex, pubHex), parsed)

        // Rechaza nonce distinto (anti-replay)
        assertNull(WireProtocol.parseAndVerifyDiscoveryV2Reply(validReply, "192.168.1.42", "FEDCBA9876543210"))
        // Rechaza manipulación del puerto o nombre tras la firma
        assertNull(WireProtocol.parseAndVerifyDiscoveryV2Reply(validReply.replace("PORT=5002", "PORT=9999"), "192.168.1.42", nonce))
        // Rechaza huella FP alterada
        assertNull(WireProtocol.parseAndVerifyDiscoveryV2Reply(validReply.replace("FP=$fpHex", "FP=0000000000000000"), "192.168.1.42", nonce))
        // Rechaza respuestas legadas v1 sin autenticar
        assertNull(WireProtocol.parseAndVerifyDiscoveryV2Reply("ZCAM_HOST_V1|PORT=5000|NAME=PC|PIN=1", "192.168.1.42", nonce))
    }

    @Test
    fun wireCrypto_crossPlatformVectors_matchWindowsCngBitForBit() {
        // 1. RFC 5869 HKDF-SHA256 Test Case 1
        val rfcIkm = WireCrypto.hexToBytes("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")!!
        val rfcSalt = WireCrypto.hexToBytes("000102030405060708090a0b0c")!!
        val rfcInfo = WireCrypto.hexToBytes("f0f1f2f3f4f5f6f7f8f9")!!
        val okm = WireCrypto.hkdfSha256(rfcSalt, rfcIkm, rfcInfo, 42)
        assertEquals(
            "3CB25F25FAACD57A90434F64D0362F2A2D2D0A90CF1A5A4C5DB02D56ECC4C5BF34007208D5B887185865",
            WireCrypto.bytesToHexUpper(okm)
        )

        // 2. RFC 5903 ECDH P-256 (mismo vector verificado en Windows CNG WireCrypto.cpp)
        val params = AlgorithmParameters.getInstance("EC").apply {
            init(ECGenParameterSpec("secp256r1"))
        }.getParameterSpec(ECParameterSpec::class.java)
        val dA = BigInteger("C88F01F510D9AC3F70A292DAA2316DE544E9AAB8AFE84049C62A9C57862D1433", 16)
        val privA = KeyFactory.getInstance("EC").generatePrivate(ECPrivateKeySpec(dA, params))
        val xA = WireCrypto.hexToBytes("DAD0B65394221CF9B051E1FECA5787D098DFE637FC90B9EF945D0C3772581180")!!
        val yA = WireCrypto.hexToBytes("5271A0461CDB8252D61F1C456FA3E59AB1F45B33ACCF5F58389E0577B8990BB3")!!
        val xB = WireCrypto.hexToBytes("D12DFB5289C8D4F81208B70270398C342296970A0BCCB74C736FC7554494BF63")!!
        val yB = WireCrypto.hexToBytes("56FBF3CA366CC23E8157854C13C58D6AAC23F046ADA30F8353E74F33039872AB")!!
        val pubASec1 = byteArrayOf(0x04) + xA + yA
        val pubBSec1 = byteArrayOf(0x04) + xB + yB

        val zGot = WireCrypto.computeEcdhSharedSecretBE(privA, pubBSec1)
        assertEquals(
            "D6840F6B42F6EDAFD13116E0E12565202FEF8E9ECE7DCE03812464D04B9442DE",
            WireCrypto.bytesToHexUpper(zGot)
        )

        // 3. Key schedule completo Wire v2 (PBKDF2 100k + HKDF-SHA256 + HMAC) idéntico a zerocam_selftest.exe
        val cNonce = ByteArray(16) { (it + 1).toByte() }
        val sNonce = ByteArray(16) { (0xA0 + it).toByte() }
        val fakeSig = ByteArray(64) { (0x30 + it).toByte() }
        val dk = WireCrypto.deriveWireV2Keys(
            authMode = WireProtocol.AUTH_MODE_WIFI_PIN,
            clientNonce = cNonce,
            clientEphPub = pubASec1,
            serverNonce = sNonce,
            serverEphPub = pubBSec1,
            serverIdPub = pubBSec1,
            sigEcdsa = fakeSig,
            ecdhSecretBE = zGot,
            pin = "482910"
        )
        val cProof = WireCrypto.computeHandshakeProof(dk.authKey, dk.transcriptHash, "client")
        val sProof = WireCrypto.computeHandshakeProof(dk.authKey, dk.transcriptHash, "server")

        assertEquals("AB322C06A2F8DFD6181ACF663285933A3A7250AD9A42444C7985B5B5CB40C019", WireCrypto.bytesToHexUpper(dk.c2sKey))
        assertEquals("654B4A0B6789E767A73C1B24", WireCrypto.bytesToHexUpper(dk.c2sIvBase))
        assertEquals("A962D59A70900BF490315F0D6D8E61005D6E8FC7CFEDB61EBC13070D357204D0", WireCrypto.bytesToHexUpper(cProof))
        assertEquals("BD0769342CBECF83445ABC561AF473B4F7762F08DCE7E0E5C2086B9A54AA4F6B", WireCrypto.bytesToHexUpper(sProof))

        // 4. AES-256-GCM AEAD exacto con cabecera AAD de 20 B (coincide bit a bit con Windows CNG)
        val aad = byteArrayOf(0x5A, 0x43, 0x41, 0x4D, 2, 3, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 28, 0, 0, 0)
        val enc = Aes256GcmCipher(dk.c2sKey, dk.c2sIvBase)
        val dec = Aes256GcmCipher(dk.c2sKey, dk.c2sIvBase)
        val ct0 = enc.encryptNext(aad, "ZeroCamWire2".toByteArray(Charsets.US_ASCII))
        assertEquals("618A57BDCF41B60BF8116225DE132DEE096EE29A025CC099EADEC0AC", WireCrypto.bytesToHexUpper(ct0))

        // Rechaza manipulación de AAD y de tag GCM
        val badAad = aad.copyOf().also { it[5] = (it[5].toInt() xor 0xFF).toByte() }
        try {
            dec.decryptNext(badAad, ct0)
            fail("Debió rechazar AAD manipulado")
        } catch (_: Exception) {
        }
        val badTag = ct0.copyOf().also { it[it.lastIndex] = (it[it.lastIndex].toInt() xor 0x01).toByte() }
        try {
            dec.decryptNext(aad, badTag)
            fail("Debió rechazar tag GCM manipulado")
        } catch (_: Exception) {
        }

        // Descifrado legítimo y paquete vacío
        assertEquals("ZeroCamWire2", String(dec.decryptNext(aad, ct0), Charsets.US_ASCII))
        val ctEmpty = enc.encryptNext(aad, ByteArray(0))
        assertEquals(16, ctEmpty.size)
        assertEquals(0, dec.decryptNext(aad, ctEmpty).size)

        enc.close()
        dec.close()
        dk.wipe()
    }

    @Test
    fun ecdsaP1363DerRoundtrip_worksAndRejectsTampering() {
        val kp = WireCrypto.generateP256KeyPair()
        val pubSec1 = WireCrypto.encodeSec1PublicKey(kp.public as ECPublicKey)
        val msg = "ZCAM-V2-SH-TEST-MESSAGE".toByteArray(Charsets.UTF_8)
        val sig64 = WireCrypto.signEcdsaP1363(kp.private, msg)
        assertEquals(64, sig64.size)
        assertTrue(WireCrypto.verifyEcdsaP1363(pubSec1, msg, sig64))
        assertFalse(WireCrypto.verifyEcdsaP1363(pubSec1, "TAMPERED".toByteArray(Charsets.UTF_8), sig64))
        val der = WireCrypto.p1363ToDer(sig64)
        assertArrayEquals(sig64, WireCrypto.derToP1363(der))

        // Rechaza DER truncado justo antes de sLen sin lanzar ArrayIndexOutOfBoundsException.
        val truncatedDer = byteArrayOf(0x30, 0x06, 0x02, 0x03, 0x01, 0x02, 0x03, 0x02)
        try {
            WireCrypto.derToP1363(truncatedDer)
            fail("Debió rechazar DER truncado")
        } catch (_: IllegalArgumentException) {
            // Esperado
        }
    }

    @Test
    fun tcpStreamClient_disconnectAbortsMidHandshakeImmediately() {
        ServerSocket(0, 1, InetAddress.getByName("127.0.0.1")).use { server ->
            val accepted = CountDownLatch(1)
            val serverThread = Thread {
                try {
                    val s = server.accept()
                    accepted.countDown()
                    // Mantiene el socket abierto sin responder ServerHello para simular peer colgado.
                    Thread.sleep(5000)
                    s.close()
                } catch (_: Exception) {
                }
            }.apply { isDaemon = true; start() }

            val client = TcpStreamClient()
            val resultRef = AtomicReference<WireHandshakeResult?>()
            val done = CountDownLatch(1)
            val t0 = System.currentTimeMillis()
            val cliThread = Thread {
                val res = client.connect("127.0.0.1", server.localPort, isUsb = true, timeoutMs = 6000)
                resultRef.set(res)
                done.countDown()
            }.apply { isDaemon = true; start() }

            assertTrue("El servidor de prueba debía aceptar conexión", accepted.await(2, TimeUnit.SECONDS))
            Thread.sleep(80)
            client.disconnect()

            assertTrue("disconnect() debe abortar connect() inmediatamente sin esperar 6 s", done.await(1500, TimeUnit.MILLISECONDS))
            val elapsed = System.currentTimeMillis() - t0
            assertTrue("Aborto demasiado lento (${elapsed} ms)", elapsed < 2000)
            assertFalse(resultRef.get()?.ok ?: true)
            assertFalse(client.isConnected())
            cliThread.join(500)
            serverThread.interrupt()
        }
    }

    @Test
    fun discoveryV2_requiredPubHexRejectsWrongKeyAndAcceptsPinnedHost() {
        val kpWrong = WireCrypto.generateP256KeyPair()
        val pubWrong = WireCrypto.encodeSec1PublicKey(kpWrong.public as ECPublicKey)
        val pubWrongHex = WireCrypto.bytesToHexUpper(pubWrong)
        val fpWrong = WireCrypto.computeFingerprintHex(pubWrong)

        val kpPinned = WireCrypto.generateP256KeyPair()
        val pubPinned = WireCrypto.encodeSec1PublicKey(kpPinned.public as ECPublicKey)
        val pubPinnedHex = WireCrypto.bytesToHexUpper(pubPinned)
        val fpPinned = WireCrypto.computeFingerprintHex(pubPinned)

        DatagramSocket(0, InetAddress.getByName("127.0.0.1")).use { responder ->
            val port = responder.localPort
            responder.soTimeout = 3000
            val srv = Thread {
                try {
                    val buf = ByteArray(512)
                    val pkt = DatagramPacket(buf, buf.size)
                    responder.receive(pkt)
                    val msg = String(pkt.data, 0, pkt.length, Charsets.UTF_8)
                    val nonce = msg.substringAfter("NONCE=", "")
                    if (nonce.isNotEmpty()) {
                        // 1. Responde primero un atacante/equipo distinto con firma válida propia.
                        val c1 = "ZCAM_HOST_V2|PORT=5999|NAME=ROGUE-PC|PIN=1|FP=$fpWrong|NONCE=$nonce|PUB=$pubWrongHex"
                        val s1 = WireCrypto.bytesToHexUpper(WireCrypto.signEcdsaP1363(kpWrong.private, c1.toByteArray(Charsets.UTF_8)))
                        val r1 = "$c1|SIG=$s1".toByteArray(Charsets.UTF_8)
                        responder.send(DatagramPacket(r1, r1.size, pkt.address, pkt.port))

                        // 2. Responde después el equipo legítimo cuya clave pública completa está fijada.
                        val c2 = "ZCAM_HOST_V2|PORT=5004|NAME=TRUSTED-PC|PIN=1|FP=$fpPinned|NONCE=$nonce|PUB=$pubPinnedHex"
                        val s2 = WireCrypto.bytesToHexUpper(WireCrypto.signEcdsaP1363(kpPinned.private, c2.toByteArray(Charsets.UTF_8)))
                        val r2 = "$c2|SIG=$s2".toByteArray(Charsets.UTF_8)
                        responder.send(DatagramPacket(r2, r2.size, pkt.address, pkt.port))
                    }
                } catch (_: Exception) {
                }
            }.apply { isDaemon = true; start() }

            val found = WireProtocol.discoverPcHostInfo(
                timeoutMs = 1500,
                targetIp = "127.0.0.1",
                requiredFpHex = fpPinned,
                requiredPubHex = pubPinnedHex,
                discoveryPort = port
            )
            srv.join(1000)
            assertNotNull(found)
            assertEquals("TRUSTED-PC", found!!.name)
            assertEquals(5004, found.port)
            assertEquals(pubPinnedHex, found.publicKeyHex)
        }
    }
}
