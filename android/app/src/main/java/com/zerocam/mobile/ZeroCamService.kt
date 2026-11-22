package com.zerocam.mobile

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.view.OrientationEventListener
import android.view.Surface
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.abs

/**
 * Estado observable del servicio de transmisión para reflejar telemetría en la UI.
 */
data class StreamUiStatus(
    val running: Boolean = false,
    val connected: Boolean = false,
    val statusText: String = "En espera",
    val hostLabel: String = "",
    val isWifi: Boolean = false,
    val bitrateKbpsTarget: Int = 4000,
    val width: Int = 0,
    val height: Int = 0,
    val targetFps: Int = 30,
    val fps: Double = 0.0,
    val kbps: Long = 0L,
    val configRevision: Long = 0L
)

/**
 * Servicio en primer plano que mantiene activa la captura HW + envío TCP incluso en modo ahorro OLED.
 */
class ZeroCamService : Service() {

    companion object {
        const val EXTRA_HOST = "host"
        const val EXTRA_PORT = "port"
        const val EXTRA_WIDTH = "width"
        const val EXTRA_HEIGHT = "height"
        const val EXTRA_FPS = "fps"
        const val EXTRA_BITRATE = "bitrate"
        const val EXTRA_FRONT = "front"
        const val EXTRA_NO_PREVIEW = "no_preview"
        const val EXTRA_TORCH = "torch"
        const val EXTRA_AUTO_FALLBACK = "auto_fallback"

        private const val CHANNEL_ID = "zerocam_stream"
        private const val NOTIF_ID = 1001

        @Volatile var currentStatus: StreamUiStatus = StreamUiStatus()
            private set
        @Volatile var lastRemoteStopAtMs: Long = 0L
        @Volatile var lastSecurityAbortAtMs: Long = 0L

        private val configRevCounter = AtomicLong(0L)
        @Volatile private var activeInstance: ZeroCamService? = null
        @Volatile private var pendingPreviewSurface: Surface? = null

        /**
         * Actualiza la superficie de vista previa local sin reiniciar la transmisión hacia PC.
         */
        fun setPreviewSurface(surface: Surface?) {
            pendingPreviewSurface = surface
            activeInstance?.let { inst ->
                inst.activeNoPreview = (surface == null)
                inst.camera?.updatePreviewSurface(surface)
            }
        }

        /**
         * Cambia la linterna en caliente si la sesión Camera2 está activa.
         */
        fun setTorch(enabled: Boolean) {
            activeInstance?.let { inst ->
                inst.activeTorch = enabled
                inst.camera?.setTorch(enabled)
            }
        }

        /**
         * Alterna entre cámara trasera y frontal en caliente manteniendo la conexión TCP cifrada activa.
         */
        fun switchCamera(front: Boolean) {
            val inst = activeInstance ?: return
            inst.enqueueReconfig(
                VideoControlConfig(inst.activeReqW, inst.activeReqH, inst.activeFps, inst.activeBitrate),
                front,
                inst.activeAutoFallback,
                forceReopen = false
            )
        }

        /**
         * Aplica cambios de resolución, FPS, bitrate o compatibilidad hechos desde Ajustes en caliente.
         */
        fun applyLocalSettings(width: Int, height: Int, fps: Int, bitrate: Int, front: Boolean, autoFallback: Boolean) {
            val inst = activeInstance ?: return
            inst.enqueueReconfig(
                VideoControlConfig(width, height, fps, bitrate),
                front,
                autoFallback,
                forceReopen = false
            )
        }

        /**
         * Solicita enfoque puntual sobre un punto normalizado [0..1] desde Touch/Tap local en móvil.
         */
        fun focusAtPoint(normX: Float, normY: Float) {
            activeInstance?.camera?.triggerFocusPoint(normX, normY)
        }
    }

    private data class PendingReconfig(
        val cfg: VideoControlConfig,
        val front: Boolean,
        val autoFallback: Boolean,
        val forceReopen: Boolean
    )

    private val stopFlag = AtomicBoolean(false)
    private val pipelineLock = Any()
    private val pendingReconfig = AtomicReference<PendingReconfig?>(null)
    private val reconfigExecutor: ExecutorService = Executors.newSingleThreadExecutor { r ->
        Thread(r, "ZeroCamReconfig").apply { isDaemon = true }
    }
    private var workerThread: Thread? = null
    private var wakeLock: PowerManager.WakeLock? = null
    @Volatile private var encoder: HardwareEncoder? = null
    @Volatile private var camera: Camera2Streamer? = null
    @Volatile private var tcpClient: TcpStreamClient? = null
    private var orientationListener: OrientationEventListener? = null
    @Volatile private var activeOrientationCameraId: String? = null
    @Volatile private var currentRotationCw: Int = 0
    @Volatile private var lastSnappedDeviceDeg: Int = -1

    @Volatile private var activeReqW: Int = 1280
    @Volatile private var activeReqH: Int = 720
    @Volatile private var activeFps: Int = 30
    @Volatile private var activeBitrate: Int = 4_000_000
    @Volatile private var activeFront: Boolean = false
    @Volatile private var activeNoPreview: Boolean = false
    @Volatile private var activeTorch: Boolean = false
    @Volatile private var activeAutoFallback: Boolean = true
    @Volatile private var activeHost: String = ""
    @Volatile private var activeHostLabel: String = ""
    @Volatile private var activeIsWifi: Boolean = false
    @Volatile private var activePort: Int = WireProtocol.DEFAULT_PORT
    @Volatile private var activeFocusMode: String = "face"
    @Volatile private var activeZoom: Float = 1.0f
    @Volatile private var activeExposureEv: Int = 0
    @Volatile private var activePreferFps: Boolean = true
    @Volatile private var authRejected: Boolean = false
    @Volatile private var lastPreferUsbAtMs: Long = 0L
    @Volatile private var framesInWindow: Long = 0L
    @Volatile private var windowStartMs: Long = 0L

    override fun onBind(intent: Intent?): IBinder? = null

    /** Encola y fusiona (coalescing) cambios rápidos de cámara/video en un único hilo ejecutor dedicado. */
    private fun enqueueReconfig(
        cfg: VideoControlConfig,
        front: Boolean,
        autoFallback: Boolean,
        forceReopen: Boolean
    ) {
        if (stopFlag.get() || reconfigExecutor.isShutdown) return
        while (true) {
            val prev = pendingReconfig.get()
            val merged = PendingReconfig(
                cfg = cfg,
                front = front,
                autoFallback = autoFallback,
                forceReopen = forceReopen || (prev?.forceReopen == true)
            )
            if (pendingReconfig.compareAndSet(prev, merged)) {
                break
            }
        }
        try {
            reconfigExecutor.execute {
                val job = pendingReconfig.getAndSet(null) ?: return@execute
                if (!stopFlag.get()) {
                    applyLocalVideoConfig(job.cfg, job.front, job.autoFallback, job.forceReopen)
                }
            }
        } catch (_: Exception) {
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        activeInstance = this
        ensureNotificationChannel()
        val notif = buildNotification("Iniciando transmisión ZeroCam...")
        if (Build.VERSION.SDK_INT >= 29) {
            ServiceCompat.startForeground(this, NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
        } else {
            startForeground(NOTIF_ID, notif)
        }

        val host = intent?.getStringExtra(EXTRA_HOST)?.trim().orEmpty()
        val port = intent?.getIntExtra(EXTRA_PORT, WireProtocol.DEFAULT_PORT) ?: WireProtocol.DEFAULT_PORT
        val reqW = intent?.getIntExtra(EXTRA_WIDTH, 1280) ?: 1280
        val reqH = intent?.getIntExtra(EXTRA_HEIGHT, 720) ?: 720
        val fps = intent?.getIntExtra(EXTRA_FPS, 30) ?: 30
        val bitrate = intent?.getIntExtra(EXTRA_BITRATE, 4_000_000) ?: 4_000_000
        val front = intent?.getBooleanExtra(EXTRA_FRONT, false) ?: false
        val noPreview = intent?.getBooleanExtra(EXTRA_NO_PREVIEW, false) ?: false
        val torch = intent?.getBooleanExtra(EXTRA_TORCH, false) ?: false
        val autoFallback = intent?.getBooleanExtra(EXTRA_AUTO_FALLBACK, true) ?: true

        stopPipeline()
        stopFlag.set(false)
        authRejected = false
        acquireWakeLock()

        workerThread = Thread({
            runPipelineLoop(host, port, reqW, reqH, fps, bitrate, front, noPreview, torch, autoFallback)
        }, "ZeroCamServiceLoop").apply { start() }

        return START_NOT_STICKY
    }

    override fun onDestroy() {
        stopPipeline()
        pendingReconfig.set(null)
        reconfigExecutor.shutdownNow()
        releaseWakeLock()
        activeInstance = null
        if (!authRejected) {
            currentStatus = StreamUiStatus(running = false, connected = false, statusText = "Detenido", configRevision = configRevCounter.get())
        }
        super.onDestroy()
    }

    private fun runPipelineLoop(
        configuredHost: String,
        configuredPort: Int,
        reqW: Int,
        reqH: Int,
        fps: Int,
        bitrate: Int,
        front: Boolean,
        noPreview: Boolean,
        torch: Boolean,
        autoFallback: Boolean
    ) {
        activeReqW = reqW
        activeReqH = reqH
        activeFps = fps
        activeBitrate = bitrate
        activeFront = front
        activeNoPreview = noPreview
        activeTorch = torch
        activeAutoFallback = autoFallback
        activeHost = configuredHost
        activeIsWifi = (configuredHost != "127.0.0.1")
        activeHostLabel = if (activeIsWifi) configuredHost.ifEmpty { "Wi-Fi" } else "Cable USB"
        activePort = configuredPort

        val client = TcpStreamClient(onControlJson = { json -> handleRemoteControlJson(json) })
        tcpClient = client

        val initialSize = startCaptureComponents(client, reqW, reqH, fps, bitrate, front, noPreview, torch, autoFallback)
        if (initialSize == null) {
            currentStatus = StreamUiStatus(
                running = false,
                statusText = "No se encontró cámara o encoder compatible",
                configRevision = configRevCounter.get()
            )
            stopSelf()
            return
        }

        // Bucle de conexión Wire v2 cifrado con prioridad Cable USB (127.0.0.1) y salto automático USB <-> Wi-Fi.
        while (!stopFlag.get() && !authRejected) {
            if (client.isConnected()) {
                client.forceAbortIfStalled(1800L)
                if (client.isConnected()) {
                    try {
                        Thread.sleep(250L)
                    } catch (_: InterruptedException) {
                        break
                    }
                    continue
                }
            }

            val curW = currentStatus.width.takeIf { it > 0 } ?: activeReqW
            val curH = currentStatus.height.takeIf { it > 0 } ?: activeReqH
            var connectedNow = false

            // 1. Siempre da prioridad al Cable USB (túnel 127.0.0.1, 350 ms) sin PIN y auto-fijando FP.
            run {
                var usbPort = configuredPort
                var usbRes = client.connect("127.0.0.1", usbPort, isUsb = true, timeoutMs = 350)
                if (!usbRes.ok && usbPort != WireProtocol.DEFAULT_PORT && !stopFlag.get()) {
                    usbPort = WireProtocol.DEFAULT_PORT
                    usbRes = client.connect("127.0.0.1", usbPort, isUsb = true, timeoutMs = 350)
                }
                if (usbRes.ok) {
                    activeIsWifi = false
                    activeHost = "127.0.0.1"
                    activePort = usbPort
                    activeHostLabel = "Cable USB"
                    if (usbRes.serverFpHex.isNotEmpty() && usbRes.serverPubHex.isNotEmpty()) {
                        val existing = AppPrefs.load(applicationContext)
                        val keepName = existing.trustedHostName.ifEmpty { "PC (USB)" }
                        AppPrefs.saveTrustedHost(applicationContext, usbRes.serverFpHex, usbRes.serverPubHex, keepName)
                        configRevCounter.incrementAndGet()
                    }
                    connectedNow = true
                }
            }

            // 2. Modo Wi-Fi o salto automático al desconectar el cable USB (únicamente hacia PC con FP confiable + Wire v2).
            if (!connectedNow && !stopFlag.get()) {
                val persisted = AppPrefs.load(applicationContext)
                val wifiPin = persisted.wifiPin
                val trustedFp = persisted.trustedHostFp
                val trustedPub = persisted.trustedHostPub
                val isUsbFailover = (configuredHost == "127.0.0.1")

                if (trustedFp.isEmpty()) {
                    if (!isUsbFailover) {
                        abortWithSecurityError("Vincula primero tu PC confirmando su huella FP")
                        break
                    }
                    // En modo USB sin FP fijado aún, esperar reconexión USB sin saltar a desconocidos.
                    currentStatus = currentStatus.copy(
                        running = true,
                        connected = false,
                        statusText = "Esperando PC por Cable USB...",
                        hostLabel = "Cable USB",
                        isWifi = false,
                        configRevision = configRevCounter.get()
                    )
                } else if (wifiPin.isEmpty()) {
                    if (!isUsbFailover) {
                        abortWithSecurityError("Configura el PIN Wi-Fi de tu PC en Ajustes")
                        break
                    }
                    currentStatus = currentStatus.copy(
                        running = true,
                        connected = false,
                        statusText = "USB desconectado — configura PIN Wi-Fi en Ajustes para salto automático",
                        hostLabel = "Cable USB",
                        isWifi = false,
                        configRevision = configRevCounter.get()
                    )
                } else {
                    var wifiHost = if (configuredHost.isNotEmpty() && configuredHost != "127.0.0.1") {
                        configuredHost
                    } else {
                        persisted.manualHost
                    }
                    var wifiPort = configuredPort
                    var wifiLabel = persisted.trustedHostName.ifEmpty { wifiHost.ifEmpty { "Wi-Fi" } }

                    if (wifiHost.isEmpty()) {
                        currentStatus = StreamUiStatus(
                            running = true,
                            connected = false,
                            statusText = if (isUsbFailover) {
                                "USB desconectado — buscando PC confiable por Wi-Fi..."
                            } else {
                                "Buscando PC confiable (FP ${WireCrypto.formatFingerprintDisplay(trustedFp)})..."
                            },
                            hostLabel = "Wi-Fi",
                            isWifi = true,
                            bitrateKbpsTarget = activeBitrate / 1000,
                            width = curW,
                            height = curH,
                            targetFps = activeFps,
                            configRevision = configRevCounter.get()
                        )
                        val discovered = WireProtocol.discoverPcHostInfo(
                            timeoutMs = 1200,
                            requiredFpHex = trustedFp,
                            requiredPubHex = trustedPub.ifEmpty { null }
                        )
                        if (discovered != null) {
                            wifiHost = discovered.ip
                            wifiPort = discovered.port
                            wifiLabel = discovered.name
                        }
                    }

                    if (wifiHost.isNotEmpty() && !stopFlag.get()) {
                        currentStatus = StreamUiStatus(
                            running = true,
                            connected = false,
                            statusText = "Autenticando Wire v2 con $wifiLabel...",
                            hostLabel = wifiLabel,
                            isWifi = true,
                            bitrateKbpsTarget = activeBitrate / 1000,
                            width = curW,
                            height = curH,
                            targetFps = activeFps,
                            configRevision = configRevCounter.get()
                        )
                        val wifiRes = client.connect(
                            host = wifiHost,
                            port = wifiPort,
                            isUsb = false,
                            wifiPin = wifiPin,
                            expectedFpHex = trustedFp,
                            expectedPubHex = trustedPub,
                            timeoutMs = 2000
                        )
                        if (wifiRes.ok) {
                            activeIsWifi = true
                            activeHost = wifiHost
                            activePort = wifiPort
                            activeHostLabel = wifiLabel
                            // Actualiza el nombre/clave fijada únicamente tras completar el handshake Wire v2 autenticado.
                            if (wifiLabel.isNotBlank() && wifiLabel != persisted.trustedHostName) {
                                val effectivePub = trustedPub.ifEmpty { wifiRes.serverPubHex }
                                AppPrefs.saveTrustedHost(applicationContext, trustedFp, effectivePub, wifiLabel)
                            }
                            connectedNow = true
                        } else if (wifiRes.errorCode == WireProtocol.ERR_PIN_INVALID ||
                            wifiRes.errorCode == WireProtocol.ERR_RATE_LIMITED ||
                            wifiRes.errorCode == WireProtocol.ERR_PIN_REQUIRED ||
                            wifiRes.errorCode == WireProtocol.ERR_PROTOCOL
                        ) {
                            abortWithSecurityError(wifiRes.errorMessage.ifEmpty { "Rechazado por seguridad Wire v2" })
                            break
                        }
                    }
                }
            }

            if (connectedNow) {
                encoder?.requestKeyFrame()
                val postW = currentStatus.width.takeIf { it > 0 } ?: curW
                val postH = currentStatus.height.takeIf { it > 0 } ?: curH
                val modePrefix = if (activeIsWifi) "Wi-Fi cifrado ($activeHostLabel)" else "Cable USB"
                updateNotification("Transmitiendo por $modePrefix (${postW}x$postH @ $activeFps fps)")
                currentStatus = StreamUiStatus(
                    running = true,
                    connected = true,
                    statusText = "Transmitiendo a $activeHostLabel (AES-256-GCM)",
                    hostLabel = activeHostLabel,
                    isWifi = activeIsWifi,
                    bitrateKbpsTarget = activeBitrate / 1000,
                    width = postW,
                    height = postH,
                    targetFps = activeFps,
                    configRevision = configRevCounter.get()
                )
            }

            try {
                Thread.sleep(700L)
            } catch (_: InterruptedException) {
                break
            }
        }
    }

    private fun abortWithSecurityError(message: String) {
        lastSecurityAbortAtMs = System.currentTimeMillis()
        authRejected = true
        stopFlag.set(true)
        tcpClient?.disconnect()
        currentStatus = StreamUiStatus(
            running = false,
            connected = false,
            statusText = message,
            configRevision = configRevCounter.get()
        )
        stopSelf()
    }

    private data class CaptureProfile(val width: Int, val height: Int, val fps: Int)

    /**
     * Inicia o reinicia Camera2 y MediaCodec probando perfiles descendentes si autoFallback está activo.
     */
    private fun startCaptureComponents(
        client: TcpStreamClient,
        reqW: Int,
        reqH: Int,
        fps: Int,
        bitrate: Int,
        front: Boolean,
        noPreview: Boolean,
        torch: Boolean,
        autoFallback: Boolean
    ): android.util.Size? = synchronized(pipelineLock) {
        if (stopFlag.get()) return null

        camera?.stop()
        camera = null
        encoder?.stop()
        encoder = null

        val candidates = if (autoFallback) {
            buildList {
                add(CaptureProfile(reqW, reqH, fps))
                if (fps > 30) add(CaptureProfile(reqW, reqH, 30))
                if (reqW > 1280 || reqH > 720 || fps > 30) add(CaptureProfile(1280, 720, 30))
                add(CaptureProfile(640, 480, 30))
            }.distinct()
        } else {
            listOf(CaptureProfile(reqW, reqH, fps))
        }

        for ((index, profile) in candidates.withIndex()) {
            if (stopFlag.get()) return null
            val resolved = Camera2Streamer.resolveCameraAndSize(this, front, profile.width, profile.height) ?: continue
            val (cameraId, size) = resolved
            val candBitrate = when {
                index == 0 -> bitrate
                profile.width <= 640 -> minOf(bitrate, 2_000_000)
                profile.width <= 1280 && profile.fps <= 30 -> minOf(bitrate, 4_000_000)
                else -> bitrate
            }

            framesInWindow = 0L
            windowStartMs = System.currentTimeMillis()
            client.drainBytesWindow()

            var createdEnc: HardwareEncoder? = null
            val hwEnc = HardwareEncoder(
                width = size.width,
                height = size.height,
                fps = profile.fps,
                bitrateBps = candBitrate,
                onEncodedFrame = { payload, ptsUs, isKey, hasCfg ->
                    val curEnc = encoder
                    if (curEnc != null && curEnc === createdEnc && client.isConnected() &&
                        client.sendVideoPacket(payload, ptsUs, isKey, hasCfg, currentRotationCw)
                    ) {
                        framesInWindow++
                        val now = System.currentTimeMillis()
                        val elapsed = now - windowStartMs
                        if (elapsed >= 500L) {
                            val lastFps = (framesInWindow * 1000.0) / elapsed.toDouble()
                            val lastKbps = (client.drainBytesWindow() * 8L) / elapsed
                            framesInWindow = 0L
                            windowStartMs = now
                            currentStatus = currentStatus.copy(
                                running = true,
                                connected = true,
                                hostLabel = activeHostLabel,
                                isWifi = activeIsWifi,
                                bitrateKbpsTarget = activeBitrate / 1000,
                                width = size.width,
                                height = size.height,
                                targetFps = activeFps,
                                fps = lastFps,
                                kbps = lastKbps,
                                configRevision = configRevCounter.get()
                            )
                        }
                    }
                },
                onError = { msg ->
                    currentStatus = currentStatus.copy(statusText = msg)
                }
            )
            createdEnc = hwEnc

            if (!hwEnc.start()) continue
            val encSurf = hwEnc.inputSurface
            if (encSurf == null) {
                hwEnc.stop()
                continue
            }

            val (sensorOrientation, facingFront) = Camera2Streamer.getCameraSensorInfo(this, cameraId)
            if (lastSnappedDeviceDeg < 0) lastSnappedDeviceDeg = 0
            currentRotationCw = computeRotationCw(sensorOrientation, facingFront, lastSnappedDeviceDeg)
            if (activeOrientationCameraId != cameraId) {
                activeOrientationCameraId = cameraId
                startOrientationTracking(sensorOrientation, facingFront)
            }

            if (index > 0) {
                activeReqW = profile.width
                activeReqH = profile.height
                activeFps = profile.fps
                activeBitrate = candBitrate
            }

            encoder = hwEnc
            val cam = Camera2Streamer(this) { err ->
                currentStatus = currentStatus.copy(statusText = err)
            }
            camera = cam
            val previewSurf = if (noPreview) null else pendingPreviewSurface
            cam.start(
                cameraId = cameraId,
                targetFps = profile.fps,
                encoderSurface = encSurf,
                previewSurface = previewSurf,
                initialTorch = torch,
                initialFocusMode = activeFocusMode,
                initialZoom = activeZoom,
                initialEv = activeExposureEv,
                preferFps = activePreferFps
            )

            val mbps = candBitrate / 1_000_000
            if (client.isConnected()) {
                hwEnc.requestKeyFrame()
                updateNotification("Transmitiendo ${size.width}x${size.height} @ ${profile.fps} fps ($mbps Mbps)")
                currentStatus = currentStatus.copy(
                    running = true,
                    connected = true,
                    hostLabel = activeHostLabel,
                    isWifi = activeIsWifi,
                    bitrateKbpsTarget = candBitrate / 1000,
                    width = size.width,
                    height = size.height,
                    targetFps = profile.fps,
                    statusText = "Transmitiendo ${size.width}x${size.height} @ ${profile.fps} fps ($mbps Mbps)",
                    configRevision = configRevCounter.get()
                )
            } else {
                val fallbackMsg = if (index > 0) {
                    "Ajustado a ${size.width}x${size.height} @ ${profile.fps}fps"
                } else {
                    currentStatus.statusText
                }
                currentStatus = currentStatus.copy(
                    running = true,
                    hostLabel = activeHostLabel,
                    isWifi = activeIsWifi,
                    bitrateKbpsTarget = candBitrate / 1000,
                    width = size.width,
                    height = size.height,
                    targetFps = profile.fps,
                    statusText = fallbackMsg,
                    configRevision = configRevCounter.get()
                )
            }
            return size
        }
        return null
    }

    private fun handleRemoteControlJson(json: String) {
        if (WireProtocol.isStopStreamCommand(json)) {
            lastRemoteStopAtMs = System.currentTimeMillis()
            authRejected = true
            stopFlag.set(true)
            tcpClient?.disconnect()
            currentStatus = currentStatus.copy(
                running = false,
                connected = false,
                statusText = "Detenido desde PC  •  Esperando inicio en tu PC",
                configRevision = configRevCounter.incrementAndGet()
            )
            stopSelf()
            return
        }
        if (WireProtocol.isPreferUsbCommand(json)) {
            val now = System.currentTimeMillis()
            if (activeIsWifi && (now - lastPreferUsbAtMs >= 4000L)) {
                lastPreferUsbAtMs = now
                tcpClient?.disconnect()
            }
            return
        }
        WireProtocol.parseFocusCommand(json)?.let { f ->
            when (f.mode) {
                "point" -> camera?.triggerFocusPoint(f.x, f.y)
                else -> {
                    activeFocusMode = f.mode
                    camera?.setFocusMode(f.mode)
                }
            }
            return
        }
        WireProtocol.parseCameraControlCommand(json)?.let { ctrl ->
            var needReopen = false
            var nextFront = activeFront
            var nextTorch = activeTorch

            ctrl.focusMode?.let {
                activeFocusMode = it
                camera?.setFocusMode(it)
            }
            ctrl.zoom?.let {
                activeZoom = it
                camera?.setZoomRatio(it)
            }
            ctrl.exposureEv?.let {
                activeExposureEv = it
                camera?.setExposureEv(it)
            }
            ctrl.torch?.let {
                nextTorch = it
                activeTorch = it
                camera?.setTorch(it)
            }
            ctrl.front?.let {
                if (it != activeFront) {
                    nextFront = it
                    needReopen = true
                }
            }
            ctrl.preferFps?.let {
                if (it != activePreferFps) {
                    activePreferFps = it
                    needReopen = true
                }
            }

            AppPrefs.updateCameraFlags(applicationContext, nextFront, nextTorch)
            val rev = configRevCounter.incrementAndGet()
            currentStatus = currentStatus.copy(configRevision = rev)

            if (needReopen) {
                enqueueReconfig(
                    VideoControlConfig(activeReqW, activeReqH, activeFps, activeBitrate),
                    nextFront,
                    activeAutoFallback,
                    forceReopen = true
                )
            }
            return
        }

        val cfg = WireProtocol.parseSetVideoCommand(json) ?: return
        AppPrefs.updateVideoConfig(applicationContext, cfg.width, cfg.height, cfg.fps, cfg.bitrateBps)
        val rev = configRevCounter.incrementAndGet()
        currentStatus = currentStatus.copy(
            width = cfg.width,
            height = cfg.height,
            targetFps = cfg.fps,
            bitrateKbpsTarget = cfg.bitrateBps / 1000,
            configRevision = rev
        )
        enqueueReconfig(cfg, activeFront, activeAutoFallback, forceReopen = false)
    }

    /**
     * Aplica bitrate en caliente sin cortes o reconfigura cámara+encoder si cambia resolución/FPS/lente.
     */
    private fun applyLocalVideoConfig(
        cfg: VideoControlConfig,
        front: Boolean,
        autoFallback: Boolean,
        forceReopen: Boolean = false
    ) {
        synchronized(pipelineLock) {
            if (stopFlag.get()) return
            val client = tcpClient ?: return

            if (!forceReopen && cfg.width == activeReqW && cfg.height == activeReqH &&
                cfg.fps == activeFps && front == activeFront && autoFallback == activeAutoFallback) {
                if (cfg.bitrateBps != activeBitrate && encoder?.updateBitrate(cfg.bitrateBps) == true) {
                    activeBitrate = cfg.bitrateBps
                    val mbps = cfg.bitrateBps / 1_000_000
                    val w = currentStatus.width.takeIf { it > 0 } ?: cfg.width
                    val h = currentStatus.height.takeIf { it > 0 } ?: cfg.height
                    updateNotification("Transmitiendo ${w}x$h @ $activeFps fps ($mbps Mbps)")
                    currentStatus = currentStatus.copy(
                        bitrateKbpsTarget = cfg.bitrateBps / 1000,
                        targetFps = activeFps,
                        statusText = "Transmitiendo ${w}x$h @ $activeFps fps ($mbps Mbps)",
                        configRevision = configRevCounter.get()
                    )
                }
                return
            }

            activeReqW = cfg.width
            activeReqH = cfg.height
            activeFps = cfg.fps
            activeBitrate = cfg.bitrateBps
            activeFront = front
            activeAutoFallback = autoFallback
            startCaptureComponents(
                client = client,
                reqW = cfg.width,
                reqH = cfg.height,
                fps = cfg.fps,
                bitrate = cfg.bitrateBps,
                front = front,
                noPreview = activeNoPreview,
                torch = activeTorch,
                autoFallback = autoFallback
            )
        }
    }

    private fun computeRotationCw(sensorOrientation: Int, facingFront: Boolean, snappedDeviceDeg: Int): Int {
        val dir = if (facingFront) -snappedDeviceDeg else snappedDeviceDeg
        return (sensorOrientation + dir + 360) % 360
    }

    private fun startOrientationTracking(sensorOrientation: Int, facingFront: Boolean) {
        try { orientationListener?.disable() } catch (_: Exception) {}
        orientationListener = null
        val listener = object : OrientationEventListener(this) {
            override fun onOrientationChanged(rawDeg: Int) {
                if (rawDeg == ORIENTATION_UNKNOWN) return
                val snapped = ((rawDeg + 45) / 90 * 90) % 360
                val prev = lastSnappedDeviceDeg
                if (prev >= 0 && snapped != prev) {
                    val diffToPrev = abs(((rawDeg - prev + 540) % 360) - 180)
                    if (diffToPrev < 65) return
                }
                lastSnappedDeviceDeg = snapped
                currentRotationCw = computeRotationCw(sensorOrientation, facingFront, snapped)
            }
        }
        if (listener.canDetectOrientation()) {
            listener.enable()
            orientationListener = listener
        }
    }

    private fun stopPipeline() {
        stopFlag.set(true)
        pendingReconfig.set(null)
        try { orientationListener?.disable() } catch (_: Exception) {}
        orientationListener = null
        activeOrientationCameraId = null
        tcpClient?.disconnect()
        workerThread?.interrupt()
        workerThread?.join(1200)
        workerThread = null
        synchronized(pipelineLock) {
            camera?.stop()
            camera = null
            encoder?.stop()
            encoder = null
            tcpClient = null
        }
    }

    private fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) return
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "ZeroCam:StreamWake").apply {
            acquire(6 * 60 * 60 * 1000L)
        }
    }

    private fun releaseWakeLock() {
        try {
            if (wakeLock?.isHeld == true) wakeLock?.release()
        } catch (_: Exception) {
        }
        wakeLock = null
    }

    private fun ensureNotificationChannel() {
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (mgr.getNotificationChannel(CHANNEL_ID) == null) {
            val ch = NotificationChannel(CHANNEL_ID, "Transmisión ZeroCam", NotificationManager.IMPORTANCE_LOW)
            mgr.createNotificationChannel(ch)
        }
    }

    private fun buildNotification(text: String): Notification {
        val openIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("ZeroCam Mobile activo")
            .setContentText(text)
            .setSmallIcon(R.drawable.ic_stat_zerocam)
            .setColor(ContextCompat.getColor(this, R.color.brand_signal))
            .setContentIntent(openIntent)
            .setOngoing(true)
            .build()
    }

    private fun updateNotification(text: String) {
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        mgr.notify(NOTIF_ID, buildNotification(text))
    }
}
