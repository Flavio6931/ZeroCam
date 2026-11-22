package com.zerocam.mobile

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Rect
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.MediaCodec
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Range
import android.util.Size
import android.view.Surface
import androidx.core.content.ContextCompat
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Capturador Camera2 Surface-a-Surface con Face-Priority AF, Tap/Click-to-Focus, zoom, EV y control de FPS.
 */
class Camera2Streamer(
    private val context: Context,
    private val onError: (String) -> Unit
) {
    private val stateLock = Any()
    @Volatile private var isStopped: Boolean = true
    private var openGeneration: Long = 0L

    private var bgThread: HandlerThread? = null
    private var bgHandler: Handler? = null
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var builder: CaptureRequest.Builder? = null

    private var encoderSurface: Surface? = null
    private var previewSurface: Surface? = null
    private var selectedFpsRange: Range<Int>? = null
    private var torchEnabled: Boolean = false
    @Volatile private var flashAvailable: Boolean = false

    private var activeArray: Rect? = null
    private var maxAfRegions: Int = 0
    private var maxAeRegions: Int = 0
    private var faceDetectMode: Int = CaptureRequest.STATISTICS_FACE_DETECT_MODE_OFF
    private var hasAfContinuousVideo: Boolean = false
    private var hasAfAuto: Boolean = false
    private var aeCompRange: Range<Int>? = null
    private var aeCompStepFloat: Float = 0.333f
    private var maxDigitalZoom: Float = 1.0f
    private var zoomRatioRange: Range<Float>? = null

    @Volatile private var focusMode: String = "face" // "face", "auto", "lock"
    @Volatile private var zoomRatio: Float = 1.0f
    @Volatile private var exposureEv: Int = 0
    @Volatile private var manualPointUntilMs: Long = 0L
    private var lastFaceUpdateMs: Long = 0L
    private var lastFaceCenterX: Float = -1f
    private var lastFaceCenterY: Float = -1f
    private var activeRegion: MeteringRectangle? = null

    companion object {
        /**
         * Busca el ID de cámara (trasera o frontal) y la resolución compatible más cercana para MediaCodec.
         */
        fun resolveCameraAndSize(
            context: Context,
            frontCamera: Boolean,
            targetWidth: Int,
            targetHeight: Int
        ): Pair<String, Size>? {
            val mgr = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
            val wantedFacing = if (frontCamera) {
                CameraCharacteristics.LENS_FACING_FRONT
            } else {
                CameraCharacteristics.LENS_FACING_BACK
            }

            var chosenId: String? = null
            for (id in mgr.cameraIdList) {
                val chars = mgr.getCameraCharacteristics(id)
                if (chars.get(CameraCharacteristics.LENS_FACING) == wantedFacing) {
                    chosenId = id
                    break
                }
            }
            if (chosenId == null) {
                chosenId = mgr.cameraIdList.firstOrNull() ?: return null
            }

            val chars = mgr.getCameraCharacteristics(chosenId)
            val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            val sizes = map?.getOutputSizes(MediaCodec::class.java)
                ?.filter { it.width % 2 == 0 && it.height % 2 == 0 }
                .orEmpty()

            val best = sizes.minByOrNull {
                abs(it.width - targetWidth) * 10 + abs(it.height - targetHeight)
            } ?: Size(targetWidth, targetHeight)

            return chosenId to best
        }

        /**
         * Devuelve (sensorOrientationDeg, isFrontFacing) para calcular rotación física automática.
         */
        fun getCameraSensorInfo(context: Context, cameraId: String): Pair<Int, Boolean> {
            val mgr = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
            val chars = mgr.getCameraCharacteristics(cameraId)
            val sensorOrient = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
            val isFront = chars.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
            return sensorOrient to isFront
        }
    }

    /**
     * Abre el dispositivo Camera2 y arranca la sesión hacia encoderSurface (y previewSurface si no es nulo).
     */
    fun start(
        cameraId: String,
        targetFps: Int,
        encoderSurface: Surface,
        previewSurface: Surface?,
        initialTorch: Boolean,
        initialFocusMode: String = focusMode,
        initialZoom: Float = zoomRatio,
        initialEv: Int = exposureEv,
        preferFps: Boolean = true
    ) {
        stop()
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            onError("Falta permiso CAMERA en Android.")
            return
        }

        this.encoderSurface = encoderSurface
        this.previewSurface = previewSurface
        this.torchEnabled = initialTorch
        this.focusMode = initialFocusMode
        this.zoomRatio = initialZoom.coerceIn(1.0f, 3.0f)
        this.exposureEv = initialEv.coerceIn(-2, 2)
        this.manualPointUntilMs = 0L
        this.activeRegion = null
        this.lastFaceCenterX = -1f
        this.lastFaceCenterY = -1f

        val thread = HandlerThread("ZeroCamCamera2").apply { start() }
        val handler = Handler(thread.looper)
        val myGen: Long
        synchronized(stateLock) {
            bgThread = thread
            bgHandler = handler
            isStopped = false
            myGen = ++openGeneration
        }

        val mgr = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val chars = try {
            mgr.getCameraCharacteristics(cameraId)
        } catch (e: Exception) {
            stop()
            onError("Error consultando características Camera2: ${e.message}")
            return
        }
        flashAvailable = (chars.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true)
        this.torchEnabled = initialTorch && flashAvailable

        activeArray = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
        maxAfRegions = chars.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF) ?: 0
        maxAeRegions = chars.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AE) ?: 0
        aeCompRange = chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
        aeCompStepFloat = chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)?.toFloat()?.takeIf { it > 0f } ?: 0.333f
        maxDigitalZoom = chars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1.0f
        zoomRatioRange = if (Build.VERSION.SDK_INT >= 30) {
            chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
        } else {
            null
        }

        val afModes = chars.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: IntArray(0)
        hasAfContinuousVideo = afModes.contains(CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
        hasAfAuto = afModes.contains(CaptureRequest.CONTROL_AF_MODE_AUTO)

        val faceModes = chars.get(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES) ?: IntArray(0)
        faceDetectMode = when {
            faceModes.contains(CaptureRequest.STATISTICS_FACE_DETECT_MODE_FULL) -> CaptureRequest.STATISTICS_FACE_DETECT_MODE_FULL
            faceModes.contains(CaptureRequest.STATISTICS_FACE_DETECT_MODE_SIMPLE) -> CaptureRequest.STATISTICS_FACE_DETECT_MODE_SIMPLE
            else -> CaptureRequest.STATISTICS_FACE_DETECT_MODE_OFF
        }

        selectedFpsRange = pickBestFpsRange(
            chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES),
            targetFps,
            preferFps
        )

        try {
            mgr.openCamera(cameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    val stale = synchronized(stateLock) {
                        if (isStopped || myGen != openGeneration) {
                            true
                        } else {
                            cameraDevice = camera
                            false
                        }
                    }
                    if (stale) {
                        try { camera.close() } catch (_: Exception) {}
                        return
                    }
                    createSession(camera, myGen)
                }

                override fun onDisconnected(camera: CameraDevice) {
                    try { camera.close() } catch (_: Exception) {}
                    synchronized(stateLock) {
                        if (cameraDevice === camera) {
                            cameraDevice = null
                        }
                    }
                }

                override fun onError(camera: CameraDevice, error: Int) {
                    try { camera.close() } catch (_: Exception) {}
                    val report = synchronized(stateLock) {
                        if (cameraDevice === camera) {
                            cameraDevice = null
                        }
                        !isStopped && myGen == openGeneration
                    }
                    if (report) {
                        onError("Error Camera2 (código $error)")
                    }
                }
            }, handler)
        } catch (e: Exception) {
            stop()
            onError("Error abriendo Camera2: ${e.message}")
        }
    }

    /**
     * Activa o desactiva la linterna en caliente sin reiniciar la sesión de captura.
     */
    fun setTorch(enabled: Boolean) {
        if (!flashAvailable) {
            torchEnabled = false
            return
        }
        torchEnabled = enabled
        val handler = bgHandler ?: return
        handler.post { reapplyRepeatingRequest() }
    }

    /**
     * Cambia entre enfoque continuo asistido por rostro ("face") o bloqueado ("lock").
     */
    fun setFocusMode(mode: String) {
        focusMode = if (mode == "lock") "lock" else "face"
        manualPointUntilMs = 0L
        if (focusMode != "lock") {
            activeRegion = null
            lastFaceCenterX = -1f
            lastFaceCenterY = -1f
        }
        bgHandler?.post { reapplyRepeatingRequest() }
    }

    /**
     * Enfoca puntualmente en coordenadas normalizadas [0..1] y mantiene foco 3.5s antes de volver al seguimiento.
     */
    fun triggerFocusPoint(normX: Float, normY: Float) {
        val handler = bgHandler ?: return
        handler.post {
            val session = captureSession ?: return@post
            val b = builder ?: return@post
            val arr = activeArray ?: return@post

            val nx = normX.coerceIn(0.02f, 0.98f)
            val ny = normY.coerceIn(0.02f, 0.98f)
            val boxW = (arr.width() * 0.14f).roundToInt().coerceAtLeast(32)
            val boxH = (arr.height() * 0.14f).roundToInt().coerceAtLeast(32)
            val cx = arr.left + (nx * arr.width()).roundToInt()
            val cy = arr.top + (ny * arr.height()).roundToInt()
            val rect = Rect(
                (cx - boxW / 2).coerceIn(arr.left, arr.right - 2),
                (cy - boxH / 2).coerceIn(arr.top, arr.bottom - 2),
                (cx + boxW / 2).coerceIn(arr.left + 2, arr.right),
                (cy + boxH / 2).coerceIn(arr.top + 2, arr.bottom)
            )
            val mr = MeteringRectangle(rect, MeteringRectangle.METERING_WEIGHT_MAX)
            activeRegion = mr
            manualPointUntilMs = System.currentTimeMillis() + 3500L

            try {
                applyCommonRequestControls(b)
                if (hasAfAuto) {
                    b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO)
                }
                if (maxAfRegions > 0) b.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(mr))
                if (maxAeRegions > 0) b.set(CaptureRequest.CONTROL_AE_REGIONS, arrayOf(mr))

                b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_CANCEL)
                session.capture(b.build(), null, handler)

                b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
                session.setRepeatingRequest(b.build(), captureCallback, handler)

                b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
                session.capture(b.build(), null, handler)
                b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_IDLE)
            } catch (_: Exception) {
            }
        }
    }

    /**
     * Ajusta el zoom del lente en caliente respetando límites de hardware.
     */
    fun setZoomRatio(zoom: Float) {
        zoomRatio = zoom.coerceIn(1.0f, 3.0f)
        bgHandler?.post { reapplyRepeatingRequest() }
    }

    /**
     * Ajusta la compensación de exposición (EV -2..+2) en caliente.
     */
    fun setExposureEv(evSteps: Int) {
        exposureEv = evSteps.coerceIn(-2, 2)
        bgHandler?.post { reapplyRepeatingRequest() }
    }

    /**
     * Actualiza la superficie de preview en caliente (p. ej. al activar/desactivar modo ahorro).
     */
    fun updatePreviewSurface(newPreview: Surface?) {
        val cam: CameraDevice?
        val handler: Handler?
        val gen: Long
        synchronized(stateLock) {
            previewSurface = newPreview
            if (isStopped) return
            cam = cameraDevice
            handler = bgHandler
            gen = openGeneration
        }
        if (cam != null && handler != null) {
            handler.post { createSession(cam, gen) }
        }
    }

    /**
     * Cierra la sesión Camera2 y detiene el hilo dedicado evitando fugas ante aperturas en vuelo.
     */
    fun stop() {
        val threadToJoin: HandlerThread?
        val sessToClose: CameraCaptureSession?
        val camToClose: CameraDevice?
        synchronized(stateLock) {
            isStopped = true
            openGeneration++
            sessToClose = captureSession
            captureSession = null
            camToClose = cameraDevice
            cameraDevice = null
            builder = null
            threadToJoin = bgThread
            bgThread = null
            bgHandler = null
        }
        try { sessToClose?.close() } catch (_: Exception) {}
        try { camToClose?.close() } catch (_: Exception) {}
        threadToJoin?.quitSafely()
        if (threadToJoin != null && threadToJoin !== Thread.currentThread()) {
            try { threadToJoin.join(500) } catch (_: Exception) {}
        }
    }

    private fun reapplyRepeatingRequest() {
        val session = captureSession ?: return
        val b = builder ?: return
        val handler = bgHandler ?: return
        try {
            applyCommonRequestControls(b)
            session.setRepeatingRequest(b.build(), captureCallback, handler)
        } catch (_: Exception) {
        }
    }

    private fun applyCommonRequestControls(b: CaptureRequest.Builder) {
        b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
        selectedFpsRange?.let { b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }

        if (flashAvailable) {
            b.set(
                CaptureRequest.FLASH_MODE,
                if (torchEnabled) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF
            )
        }

        if (faceDetectMode != CaptureRequest.STATISTICS_FACE_DETECT_MODE_OFF) {
            b.set(CaptureRequest.STATISTICS_FACE_DETECT_MODE, faceDetectMode)
        }

        if (focusMode == "lock") {
            if (hasAfAuto) b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO)
        } else if (System.currentTimeMillis() > manualPointUntilMs && hasAfContinuousVideo) {
            b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
        }

        val mr = activeRegion
        if (mr != null) {
            if (maxAfRegions > 0) b.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(mr))
            if (maxAeRegions > 0) b.set(CaptureRequest.CONTROL_AE_REGIONS, arrayOf(mr))
        } else {
            val arr = activeArray
            if (arr != null) {
                val resetRect = arrayOf(MeteringRectangle(arr, 0))
                if (maxAfRegions > 0) b.set(CaptureRequest.CONTROL_AF_REGIONS, resetRect)
                if (maxAeRegions > 0) b.set(CaptureRequest.CONTROL_AE_REGIONS, resetRect)
            }
        }

        aeCompRange?.let { rng ->
            val targetRaw = (exposureEv.toFloat() / aeCompStepFloat).roundToInt().coerceIn(rng.lower, rng.upper)
            b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, targetRaw)
        }

        val zr = zoomRatioRange
        if (Build.VERSION.SDK_INT >= 30 && zr != null) {
            b.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoomRatio.coerceIn(zr.lower, zr.upper))
        } else {
            val arr = activeArray
            if (arr != null && maxDigitalZoom > 1.01f) {
                val z = zoomRatio.coerceIn(1.0f, maxDigitalZoom)
                val cropW = (arr.width() / z).roundToInt().coerceAtLeast(2)
                val cropH = (arr.height() / z).roundToInt().coerceAtLeast(2)
                val left = arr.left + (arr.width() - cropW) / 2
                val top = arr.top + (arr.height() - cropH) / 2
                b.set(CaptureRequest.SCALER_CROP_REGION, Rect(left, top, left + cropW, top + cropH))
            }
        }
    }

    private val captureCallback = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureCompleted(session: CameraCaptureSession, request: CaptureRequest, result: TotalCaptureResult) {
            if (focusMode != "face") return
            val now = System.currentTimeMillis()
            if (now < manualPointUntilMs) return
            val arr = activeArray ?: return
            val faces = result.get(CaptureResult.STATISTICS_FACES)
            if (faces.isNullOrEmpty()) return
            if (now - lastFaceUpdateMs < 800L) return

            val primary = faces.maxByOrNull { it.bounds.width().toLong() * it.bounds.height().toLong() } ?: return
            val clipped = Rect(
                primary.bounds.left.coerceIn(arr.left, arr.right - 2),
                primary.bounds.top.coerceIn(arr.top, arr.bottom - 2),
                primary.bounds.right.coerceIn(arr.left + 2, arr.right),
                primary.bounds.bottom.coerceIn(arr.top + 2, arr.bottom)
            )
            if (clipped.width() < 16 || clipped.height() < 16) return

            val cxNorm = (clipped.exactCenterX() - arr.left) / arr.width().toFloat()
            val cyNorm = (clipped.exactCenterY() - arr.top) / arr.height().toFloat()
            if (lastFaceCenterX >= 0f &&
                abs(cxNorm - lastFaceCenterX) < 0.08f &&
                abs(cyNorm - lastFaceCenterY) < 0.08f) {
                return
            }

            lastFaceUpdateMs = now
            lastFaceCenterX = cxNorm
            lastFaceCenterY = cyNorm
            val mr = MeteringRectangle(clipped, MeteringRectangle.METERING_WEIGHT_MAX)
            activeRegion = mr
            val b = builder ?: return
            val handler = bgHandler ?: return
            try {
                applyCommonRequestControls(b)
                session.setRepeatingRequest(b.build(), this, handler)
            } catch (_: Exception) {
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun createSession(camera: CameraDevice, expectedGen: Long) {
        val encSurf: Surface
        val handler: Handler
        val prev: Surface?
        synchronized(stateLock) {
            if (isStopped || expectedGen != openGeneration || cameraDevice !== camera) return
            encSurf = encoderSurface ?: return
            handler = bgHandler ?: return
            prev = previewSurface
        }
        val targets = mutableListOf(encSurf)
        if (prev != null && prev.isValid) {
            targets.add(prev)
        }

        try {
            val reqBuilder = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                targets.forEach { addTarget(it) }
                applyCommonRequestControls(this)
            }
            synchronized(stateLock) {
                if (isStopped || expectedGen != openGeneration || cameraDevice !== camera) return
                builder = reqBuilder
            }

            val stateCb = object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    val stale = synchronized(stateLock) {
                        if (isStopped || expectedGen != openGeneration || cameraDevice !== camera) {
                            true
                        } else {
                            captureSession = session
                            false
                        }
                    }
                    if (stale) {
                        try { session.close() } catch (_: Exception) {}
                        return
                    }
                    try {
                        session.setRepeatingRequest(reqBuilder.build(), captureCallback, handler)
                    } catch (e: Exception) {
                        if (!isStopped) {
                            onError("Error iniciando captura continua: ${e.message}")
                        }
                    }
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    if (!isStopped) {
                        onError("No se pudo configurar la sesión Camera2.")
                    }
                }
            }

            if (Build.VERSION.SDK_INT >= 28) {
                val configs = targets.map { OutputConfiguration(it) }
                val cfg = SessionConfiguration(
                    SessionConfiguration.SESSION_REGULAR,
                    configs,
                    { r -> handler.post(r) },
                    stateCb
                )
                camera.createCaptureSession(cfg)
            } else {
                camera.createCaptureSession(targets, stateCb, handler)
            }
        } catch (e: Exception) {
            if (!isStopped) {
                onError("Error creando sesión Camera2: ${e.message}")
            }
        }
    }

    private fun pickBestFpsRange(ranges: Array<Range<Int>>?, targetFps: Int, preferFps: Boolean): Range<Int>? {
        val valid = ranges?.filter { it.lower > 0 && it.upper >= it.lower }.orEmpty()
        if (valid.isEmpty()) return null
        valid.firstOrNull { it.lower == targetFps && it.upper == targetFps }?.let { return it }
        if (preferFps) {
            val minFloor = minOf(targetFps, 30)
            valid.filter { it.lower >= minFloor && it.upper >= targetFps }
                .minByOrNull { (it.upper - it.lower) * 10 + abs(it.upper - targetFps) }
                ?.let { return it }
        }
        return valid.filter { it.upper >= targetFps && it.lower <= targetFps }
            .minByOrNull { it.upper - it.lower }
            ?: valid.minByOrNull { abs(it.upper - targetFps) }
    }
}
