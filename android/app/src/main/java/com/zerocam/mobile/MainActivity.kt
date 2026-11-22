package com.zerocam.mobile

import android.Manifest
import android.annotation.SuppressLint
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.widget.Button
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import androidx.core.content.ContextCompat
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Actividad principal multi-estado de ZeroCam Mobile: Inicio Limpio, Vista Live y Modo Ahorro OLED.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var viewHome: View
    private lateinit var viewLive: View
    private lateinit var viewOled: View

    // Controles Home
    private lateinit var tabUsb: TextView
    private lateinit var tabWifi: TextView
    private lateinit var dotAutoDetect: View
    private lateinit var tvAutoHint: TextView
    private lateinit var cardProfileSummary: LinearLayout
    private lateinit var tvProfileTitle: TextView
    private lateinit var tvProfileSubtitle: TextView
    private lateinit var swPowerSave: SwitchCompat
    private lateinit var swTorch: SwitchCompat
    private lateinit var btnStartStream: Button
    private lateinit var tvHomeStatus: TextView

    // Controles Live
    private lateinit var previewSurfaceView: SurfaceView
    private lateinit var dotLiveHeader: View
    private lateinit var tvLiveHeaderBadge: TextView
    private lateinit var dotRecBadge: View
    private lateinit var tvRecBadge: TextView
    private lateinit var btnLiveTorch: ImageButton
    private lateinit var dotLiveStrip: View
    private lateinit var tvLiveHostStatus: TextView
    private lateinit var tvLiveMetrics: TextView

    // Controles OLED
    private lateinit var tvOledTitle: TextView
    private lateinit var tvOledStats: TextView

    private var settings = AppSettings()
    private var lastSyncedConfigRev = -1L
    private var lastOledSystemBars = false
    @Volatile private var lastDiscoveredHost: DiscoveredHost? = null
    @Volatile private var lastDiscoveredAtMs: Long = 0L
    @Volatile private var manualStopPaused = false
    @Volatile private var securityAbortPaused = false
    @Volatile private var lastAutoProbeMs = 0L
    @Volatile private var trustDialogShowing = false
    @Volatile private var lastHandledSecurityAbortMs = 0L
    private var lastKnownWifiPin = ""
    private val discoveryInFlight = AtomicBoolean(false)
    private val discoveryExecutor: ExecutorService = Executors.newSingleThreadExecutor { r ->
        Thread(r, "ZeroCamDiscovery").apply { isDaemon = true }
    }
    private val uiHandler = Handler(Looper.getMainLooper())
    private val statusPollRunnable = object : Runnable {
        override fun run() {
            refreshStatusUi()
            val now = System.currentTimeMillis()
            if (!ZeroCamService.currentStatus.running &&
                !securityAbortPaused &&
                !trustDialogShowing &&
                (now - lastAutoProbeMs >= 900L) &&
                (now - ZeroCamService.lastRemoteStopAtMs >= 2200L)
            ) {
                pollPcAndAutoStartIfReady()
            }
            uiHandler.postDelayed(this, 500L)
        }
    }

    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { grants ->
        if (grants[Manifest.permission.CAMERA] == true) {
            startStreamService()
        } else {
            tvHomeStatus.text = "Permiso de cámara requerido para transmitir."
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        settings = AppPrefs.load(this)
        bindViews()
        setupListeners()
        syncHomeControlsFromSettings()
        refreshStatusUi()
        handleAutoStartIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleAutoStartIntent(intent)
    }

    /**
     * Procesa el arranque remoto enviado por Windows vía ADB al iniciar el túnel USB.
     */
    private fun handleAutoStartIntent(intent: Intent?) {
        if (intent?.getBooleanExtra("auto_start", false) != true) return
        intent.removeExtra("auto_start")
        manualStopPaused = false
        securityAbortPaused = false
        lastAutoProbeMs = 0L
        if (!ZeroCamService.currentStatus.running && !isFinishing && !isDestroyed) {
            val hasCam = androidx.core.content.ContextCompat.checkSelfPermission(
                this,
                android.Manifest.permission.CAMERA
            ) == android.content.pm.PackageManager.PERMISSION_GRANTED
            if (hasCam) {
                pollPcAndAutoStartIfReady()
            } else {
                ensurePermissionsAndStart()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        settings = AppPrefs.load(this)
        if (settings.wifiPin != lastKnownWifiPin) {
            lastKnownWifiPin = settings.wifiPin
            manualStopPaused = false
            securityAbortPaused = false
        }
        if (settings.trustedHostFp.isEmpty()) {
            lastDiscoveredHost = null
            lastDiscoveredAtMs = 0L
        }
        syncHomeControlsFromSettings()
        refreshStatusUi()
        uiHandler.post(statusPollRunnable)
        if (settings.wifiMode && !ZeroCamService.currentStatus.running) {
            triggerWifiAutoDetect()
        }
    }

    override fun onPause() {
        uiHandler.removeCallbacks(statusPollRunnable)
        super.onPause()
    }

    override fun onDestroy() {
        uiHandler.removeCallbacks(statusPollRunnable)
        discoveryExecutor.shutdownNow()
        super.onDestroy()
    }

    private fun bindViews() {
        viewHome = findViewById(R.id.viewHome)
        viewLive = findViewById(R.id.viewLive)
        viewOled = findViewById(R.id.viewOled)

        tabUsb = findViewById(R.id.tabUsb)
        tabWifi = findViewById(R.id.tabWifi)
        dotAutoDetect = findViewById(R.id.dotAutoDetect)
        tvAutoHint = findViewById(R.id.tvAutoHint)
        cardProfileSummary = findViewById(R.id.cardProfileSummary)
        tvProfileTitle = findViewById(R.id.tvProfileTitle)
        tvProfileSubtitle = findViewById(R.id.tvProfileSubtitle)
        swPowerSave = findViewById(R.id.swPowerSave)
        swTorch = findViewById(R.id.swTorch)
        btnStartStream = findViewById(R.id.btnStartStream)
        tvHomeStatus = findViewById(R.id.tvHomeStatus)

        previewSurfaceView = findViewById(R.id.previewSurface)
        dotLiveHeader = findViewById(R.id.dotLiveHeader)
        tvLiveHeaderBadge = findViewById(R.id.tvLiveHeaderBadge)
        dotRecBadge = findViewById(R.id.dotRecBadge)
        tvRecBadge = findViewById(R.id.tvRecBadge)
        btnLiveTorch = findViewById(R.id.btnLiveTorch)
        dotLiveStrip = findViewById(R.id.dotLiveStrip)
        tvLiveHostStatus = findViewById(R.id.tvLiveHostStatus)
        tvLiveMetrics = findViewById(R.id.tvLiveMetrics)

        tvOledTitle = findViewById(R.id.tvOledTitle)
        tvOledStats = findViewById(R.id.tvOledStats)
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun setupListeners() {
        val openSettings = { startActivity(Intent(this, SettingsActivity::class.java)) }
        findViewById<ImageButton>(R.id.btnHomeSettings).setOnClickListener { openSettings() }
        findViewById<ImageButton>(R.id.btnLiveSettings).setOnClickListener { openSettings() }
        cardProfileSummary.setOnClickListener { openSettings() }

        tabUsb.setOnClickListener {
            manualStopPaused = false
            securityAbortPaused = false
            lastAutoProbeMs = 0L
            if (!settings.wifiMode) return@setOnClickListener
            settings = settings.copy(wifiMode = false)
            AppPrefs.save(this, settings)
            syncTransportTabs()
            updateTransportHint()
        }

        tabWifi.setOnClickListener {
            manualStopPaused = false
            securityAbortPaused = false
            lastAutoProbeMs = 0L
            if (settings.wifiMode) return@setOnClickListener
            settings = settings.copy(wifiMode = true)
            AppPrefs.save(this, settings)
            syncTransportTabs()
            updateTransportHint()
            if (!ZeroCamService.currentStatus.running) triggerWifiAutoDetect()
        }

        swPowerSave.setOnCheckedChangeListener { _, isChecked ->
            if (settings.powerSave == isChecked) return@setOnCheckedChangeListener
            settings = settings.copy(powerSave = isChecked)
            AppPrefs.save(this, settings)
            if (isChecked) ZeroCamService.setPreviewSurface(null)
            refreshStatusUi()
        }

        swTorch.setOnCheckedChangeListener { _, isChecked ->
            if (settings.torchEnabled == isChecked) return@setOnCheckedChangeListener
            setTorchState(isChecked)
        }

        btnStartStream.setOnClickListener {
            manualStopPaused = false
            securityAbortPaused = false
            ensurePermissionsAndStart()
        }

        val stopStream = {
            manualStopPaused = true
            ZeroCamService.lastRemoteStopAtMs = System.currentTimeMillis()
            stopService(Intent(this, ZeroCamService::class.java))
            ZeroCamService.setPreviewSurface(null)
            refreshStatusUi()
        }
        findViewById<Button>(R.id.btnLiveStop).setOnClickListener { stopStream() }
        findViewById<Button>(R.id.btnOledStop).setOnClickListener { stopStream() }

        findViewById<ImageButton>(R.id.btnLiveSwitchCam).setOnClickListener {
            val nextFront = !settings.frontCamera
            settings = settings.copy(frontCamera = nextFront)
            AppPrefs.save(this, settings)
            updateProfileSummary()
            ZeroCamService.switchCamera(nextFront)
        }

        btnLiveTorch.setOnClickListener {
            setTorchState(!settings.torchEnabled)
        }

        findViewById<ImageButton>(R.id.btnLiveOled).setOnClickListener {
            settings = settings.copy(powerSave = true)
            AppPrefs.save(this, settings)
            swPowerSave.isChecked = true
            ZeroCamService.setPreviewSurface(null)
            refreshStatusUi()
        }

        findViewById<Button>(R.id.btnOledShowPreview).setOnClickListener {
            settings = settings.copy(powerSave = false)
            AppPrefs.save(this, settings)
            swPowerSave.isChecked = false
            refreshStatusUi()
            val surf = previewSurfaceView.holder.surface
            if (surf.isValid) ZeroCamService.setPreviewSurface(surf)
        }

        previewSurfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                if (!settings.powerSave && ZeroCamService.currentStatus.running) {
                    ZeroCamService.setPreviewSurface(holder.surface)
                }
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, w: Int, h: Int) {}

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                ZeroCamService.setPreviewSurface(null)
            }
        })

        previewSurfaceView.setOnTouchListener { v, event ->
            if (event.action == MotionEvent.ACTION_UP && ZeroCamService.currentStatus.running && v.width > 0 && v.height > 0) {
                val nx = (event.x / v.width.toFloat()).coerceIn(0.02f, 0.98f)
                val ny = (event.y / v.height.toFloat()).coerceIn(0.02f, 0.98f)
                ZeroCamService.focusAtPoint(nx, ny)
                tvLiveHostStatus.text = "Enfocando punto seleccionado..."
            }
            true
        }
    }

    private fun setTorchState(enabled: Boolean) {
        settings = settings.copy(torchEnabled = enabled)
        AppPrefs.save(this, settings)
        swTorch.isChecked = enabled
        updateTorchButtonTint()
        ZeroCamService.setTorch(enabled)
    }

    private fun updateTorchButtonTint() {
        btnLiveTorch.setColorFilter(
            Color.parseColor(if (settings.torchEnabled) "#F59E0B" else "#F1F3F7")
        )
    }

    private fun syncHomeControlsFromSettings() {
        swPowerSave.isChecked = settings.powerSave
        swTorch.isChecked = settings.torchEnabled
        updateTorchButtonTint()
        syncTransportTabs()
        updateProfileSummary()
        updateTransportHint()
    }

    private fun syncTransportTabs() {
        if (settings.wifiMode) {
            tabWifi.setBackgroundResource(R.drawable.bg_segmented_active)
            tabWifi.setTextColor(Color.WHITE)
            tabUsb.background = null
            tabUsb.setTextColor(Color.parseColor("#9AA4B2"))
        } else {
            tabUsb.setBackgroundResource(R.drawable.bg_segmented_active)
            tabUsb.setTextColor(Color.WHITE)
            tabWifi.background = null
            tabWifi.setTextColor(Color.parseColor("#9AA4B2"))
        }
    }

    private fun updateProfileSummary() {
        val camLabel = if (settings.frontCamera) "Cámara Frontal" else "Cámara Trasera"
        val resLabel = when (settings.videoWidth) {
            640 -> "640x480 (SD)"
            1920 -> "1080p Full HD"
            else -> "720p HD"
        }
        val qualityLabel = when {
            settings.videoBitrate <= 2_000_000 -> "Baja (2 Mbps)"
            settings.videoBitrate >= 12_000_000 -> "Ultra (12 Mbps)"
            settings.videoBitrate >= 8_000_000 -> "Alta (8 Mbps)"
            else -> "Media (4 Mbps)"
        }
        val pinSuffix = if (settings.wifiPin.isNotEmpty()) "  •  PIN activo" else ""
        tvProfileTitle.text = "$camLabel • Ajustes de video"
        tvProfileSubtitle.text = "$resLabel  •  ${settings.videoFps} FPS  •  $qualityLabel$pinSuffix"
    }

    private fun updateTransportHint() {
        if (!settings.wifiMode) {
            dotAutoDetect.setBackgroundResource(R.drawable.dot_success)
            tvAutoHint.setTextColor(Color.parseColor("#22C55E"))
            tvAutoHint.text = "PC detectada automáticamente por USB"
        } else if (settings.manualHost.isNotEmpty()) {
            dotAutoDetect.setBackgroundResource(R.drawable.dot_success)
            tvAutoHint.setTextColor(Color.parseColor("#22C55E"))
            tvAutoHint.text = "Wi-Fi listo (IP manual: ${settings.manualHost})"
        } else {
            dotAutoDetect.setBackgroundResource(R.drawable.dot_muted)
            tvAutoHint.setTextColor(Color.parseColor("#9AA4B2"))
            tvAutoHint.text = "Autodetección Wi-Fi activa en red local"
        }
    }

    private fun triggerWifiAutoDetect() {
        if (!settings.wifiMode || settings.manualHost.isNotEmpty()) {
            updateTransportHint()
            return
        }
        if (!discoveryInFlight.compareAndSet(false, true)) {
            return
        }
        dotAutoDetect.setBackgroundResource(R.drawable.dot_muted)
        tvAutoHint.setTextColor(Color.parseColor("#9AA4B2"))
        tvAutoHint.text = "Buscando PC ZeroCam en tu Wi-Fi..."
        lastDiscoveredHost = null
        lastDiscoveredAtMs = 0L
        val reqFp = settings.trustedHostFp.ifEmpty { null }
        val reqPub = settings.trustedHostPub.ifEmpty { null }
        try {
            discoveryExecutor.execute {
                val found = try {
                    WireProtocol.discoverPcHostInfo(
                        timeoutMs = 1500,
                        requiredFpHex = reqFp,
                        requiredPubHex = reqPub
                    )
                } finally {
                    discoveryInFlight.set(false)
                }
                lastDiscoveredHost = found
                lastDiscoveredAtMs = if (found != null) System.currentTimeMillis() else 0L
                runOnUiThread {
                    if (isFinishing || isDestroyed || !settings.wifiMode) return@runOnUiThread
                    if (found != null) {
                        dotAutoDetect.setBackgroundResource(R.drawable.dot_success)
                        tvAutoHint.setTextColor(Color.parseColor("#22C55E"))
                        val fpDisp = WireCrypto.formatFingerprintDisplay(found.fingerprintHex)
                        tvAutoHint.text = "PC detectada: ${found.name} • FP $fpDisp"
                    } else {
                        dotAutoDetect.setBackgroundResource(R.drawable.dot_muted)
                        tvAutoHint.setTextColor(Color.parseColor("#9AA4B2"))
                        tvAutoHint.text = if (reqFp != null) {
                            "Esperando PC vinculada (FP ${WireCrypto.formatFingerprintDisplay(reqFp)})"
                        } else {
                            "Abre ZeroCam en tu PC en modo Wi-Fi (autodetección Wire v2)"
                        }
                    }
                }
            }
        } catch (_: Exception) {
            discoveryInFlight.set(false)
        }
    }

    private fun ensurePermissionsAndStart() {
        val needed = mutableListOf<String>()
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.CAMERA)
        }
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (needed.isEmpty()) startStreamService() else permLauncher.launch(needed.toTypedArray())
    }

    private fun startStreamService() {
        settings = AppPrefs.load(this)
        if (settings.wifiMode) {
            if (settings.wifiPin.isEmpty()) {
                tvHomeStatus.text = "Configura primero el PIN Wi-Fi de tu PC en Ajustes."
                return
            }
            if (settings.trustedHostFp.isEmpty()) {
                promptVerifyAndTrustWifiHost()
                return
            }
        }
        launchZeroCamServiceNow()
    }

    /** Descubre o sondea la PC Wi-Fi para que el usuario verifique su huella FP antes de fijarla como confiable. */
    private fun promptVerifyAndTrustWifiHost() {
        if (trustDialogShowing) return
        trustDialogShowing = true
        tvHomeStatus.text = "Verificando identidad ECDSA (FP) de la PC..."
        val manualIp = settings.manualHost
        val port = settings.internalPort
        val cached = lastDiscoveredHost
        val cacheFresh = cached != null && (System.currentTimeMillis() - lastDiscoveredAtMs) <= 8_000L
        try {
            discoveryExecutor.execute {
                val host = try {
                    when {
                        manualIp.isNotEmpty() -> {
                            WireProtocol.discoverPcHostInfo(timeoutMs = 1200, targetIp = manualIp)
                                ?: TcpStreamClient().probeServerIdentity(manualIp, port, timeoutMs = 2000)
                        }
                        cacheFresh && cached!!.fingerprintHex.isNotEmpty() -> cached
                        else -> WireProtocol.discoverPcHostInfo(timeoutMs = 1800)
                    }
                } catch (_: Exception) {
                    null
                }
                runOnUiThread {
                    if (isFinishing || isDestroyed) {
                        trustDialogShowing = false
                        return@runOnUiThread
                    }
                    if (host == null || host.fingerprintHex.isEmpty()) {
                        trustDialogShowing = false
                        tvHomeStatus.text = "No se encontró ninguna PC ZeroCam v1.2.0 en tu Wi-Fi."
                        return@runOnUiThread
                    }
                    val fpDisp = WireCrypto.formatFingerprintDisplay(host.fingerprintHex)
                    AlertDialog.Builder(this, R.style.ZeroCamAlertDialog)
                        .setTitle("Confirmar identidad de la PC")
                        .setMessage(
                            "Equipo: ${host.name}\n" +
                                "Dirección: ${host.ip}\n" +
                                "Huella FP: $fpDisp\n\n" +
                                "Comprueba que esta huella coincida con Ajustes → Red y Seguridad en ZeroCam Windows."
                        )
                        .setPositiveButton("Confiar y transmitir") { _, _ ->
                            manualStopPaused = false
                            securityAbortPaused = false
                            trustDialogShowing = false
                            AppPrefs.saveTrustedHost(this, host.fingerprintHex, host.publicKeyHex, host.name)
                            settings = AppPrefs.load(this)
                            launchZeroCamServiceNow()
                        }
                        .setNegativeButton("Cancelar") { _, _ ->
                            manualStopPaused = true
                            trustDialogShowing = false
                            tvHomeStatus.text = "Vinculación Wi-Fi cancelada."
                        }
                        .setOnCancelListener {
                            manualStopPaused = true
                            trustDialogShowing = false
                        }
                        .setOnDismissListener {
                            trustDialogShowing = false
                        }
                        .show()
                }
            }
        } catch (_: Exception) {
            trustDialogShowing = false
        }
    }

    /**
     * Sondea la disponibilidad de ZeroCam PC (túnel USB 127.0.0.1 o Wi-Fi) cuando la app está en espera
     * para iniciar la transmisión automáticamente con 1 clic desde el escritorio.
     */
    private fun pollPcAndAutoStartIfReady() {
        if (isFinishing || isDestroyed) return
        if (ZeroCamService.currentStatus.running || securityAbortPaused || trustDialogShowing) return
        val now = System.currentTimeMillis()
        if (now - ZeroCamService.lastRemoteStopAtMs < 2200L) return
        if (now - lastAutoProbeMs < 900L) return

        val hasCamPerm = androidx.core.content.ContextCompat.checkSelfPermission(
            this,
            android.Manifest.permission.CAMERA
        ) == android.content.pm.PackageManager.PERMISSION_GRANTED
        if (!hasCamPerm) return

        val cfg = AppPrefs.load(this)
        settings = cfg
        if (!discoveryInFlight.compareAndSet(false, true)) return
        lastAutoProbeMs = now

        val clearManualPauseIfHostAbsent = {
            if (manualStopPaused) {
                manualStopPaused = false
                runOnUiThread {
                    if (!isFinishing && !isDestroyed) refreshStatusUi()
                }
            }
        }

        try {
            discoveryExecutor.execute {
                try {
                    // Prioridad Cable USB (127.0.0.1) sin exigir PIN incluso si la pestaña previa estaba en Wi-Fi.
                    val usbHost = TcpStreamClient().probeServerIdentity(
                        "127.0.0.1",
                        cfg.internalPort,
                        timeoutMs = 200
                    )
                    if (usbHost != null && usbHost.fingerprintHex.isNotEmpty()) {
                        if (manualStopPaused) return@execute
                        runOnUiThread {
                            if (isFinishing || isDestroyed) return@runOnUiThread
                            if (ZeroCamService.currentStatus.running || manualStopPaused || securityAbortPaused || trustDialogShowing) return@runOnUiThread
                            if (System.currentTimeMillis() - ZeroCamService.lastRemoteStopAtMs < 2200L) return@runOnUiThread
                            if (settings.wifiMode) {
                                settings = settings.copy(wifiMode = false)
                                AppPrefs.save(this, settings)
                                syncTransportTabs()
                                updateTransportHint()
                            }
                            launchZeroCamServiceNow(preferUsb = true)
                        }
                        return@execute
                    }

                    if (cfg.wifiPin.length != 6) {
                        clearManualPauseIfHostAbsent()
                        return@execute
                    }

                    val trustedFp = cfg.trustedHostFp
                    val trustedPub = cfg.trustedHostPub
                    val manualIp = cfg.manualHost
                    val host = if (manualIp.isNotEmpty()) {
                        WireProtocol.discoverPcHostInfo(
                            timeoutMs = 450,
                            targetIp = manualIp,
                            requiredFpHex = trustedFp.ifEmpty { null },
                            requiredPubHex = trustedPub.ifEmpty { null }
                        ) ?: TcpStreamClient().probeServerIdentity(manualIp, cfg.internalPort, timeoutMs = 350)
                    } else {
                        WireProtocol.discoverPcHostInfo(
                            timeoutMs = 550,
                            requiredFpHex = trustedFp.ifEmpty { null },
                            requiredPubHex = trustedPub.ifEmpty { null }
                        )
                    }
                    if (host != null && host.fingerprintHex.isNotEmpty()) {
                        if (trustedFp.isNotEmpty() && !host.fingerprintHex.equals(trustedFp, ignoreCase = true)) {
                            clearManualPauseIfHostAbsent()
                            return@execute
                        }
                        lastDiscoveredHost = host
                        lastDiscoveredAtMs = System.currentTimeMillis()
                        if (manualStopPaused) return@execute
                        runOnUiThread {
                            if (isFinishing || isDestroyed) return@runOnUiThread
                            if (ZeroCamService.currentStatus.running || manualStopPaused || securityAbortPaused || trustDialogShowing) return@runOnUiThread
                            if (System.currentTimeMillis() - ZeroCamService.lastRemoteStopAtMs < 2200L) return@runOnUiThread
                            if (!settings.wifiMode) {
                                settings = settings.copy(wifiMode = true)
                                AppPrefs.save(this, settings)
                                syncTransportTabs()
                                updateTransportHint()
                            }
                            if (settings.trustedHostFp.isEmpty()) {
                                promptVerifyAndTrustWifiHost()
                            } else {
                                launchZeroCamServiceNow(preferUsb = false)
                            }
                        }
                    } else {
                        clearManualPauseIfHostAbsent()
                    }
                } catch (_: Exception) {
                } finally {
                    discoveryInFlight.set(false)
                }
            }
        } catch (_: Exception) {
            discoveryInFlight.set(false)
        }
    }

    private fun launchZeroCamServiceNow(preferUsb: Boolean = !settings.wifiMode) {
        val host = if (preferUsb) "127.0.0.1" else settings.manualHost

        if (previewSurfaceView.holder.surface.isValid) {
            ZeroCamService.setPreviewSurface(previewSurfaceView.holder.surface)
        } else {
            ZeroCamService.setPreviewSurface(null)
        }

        // Nunca se pasa el PIN por Intent; ZeroCamService lo lee cifrado desde SecurePinStore (AndroidKeyStore).
        val intent = Intent(this, ZeroCamService::class.java).apply {
            putExtra(ZeroCamService.EXTRA_HOST, host)
            putExtra(ZeroCamService.EXTRA_PORT, settings.internalPort)
            putExtra(ZeroCamService.EXTRA_WIDTH, settings.videoWidth)
            putExtra(ZeroCamService.EXTRA_HEIGHT, settings.videoHeight)
            putExtra(ZeroCamService.EXTRA_FPS, settings.videoFps)
            putExtra(ZeroCamService.EXTRA_BITRATE, settings.videoBitrate)
            putExtra(ZeroCamService.EXTRA_FRONT, settings.frontCamera)
            putExtra(ZeroCamService.EXTRA_NO_PREVIEW, false)
            putExtra(ZeroCamService.EXTRA_TORCH, settings.torchEnabled)
            putExtra(ZeroCamService.EXTRA_AUTO_FALLBACK, settings.autoFallback)
        }
        ContextCompat.startForegroundService(this, intent)
        refreshStatusUi()
    }

    private fun applySystemBars(oledPureBlack: Boolean) {
        if (lastOledSystemBars == oledPureBlack) return
        lastOledSystemBars = oledPureBlack
        val barColor = if (oledPureBlack) Color.BLACK else Color.parseColor("#0F1117")
        window.statusBarColor = barColor
        window.navigationBarColor = barColor
    }

    private fun refreshStatusUi() {
        val secAbortAt = ZeroCamService.lastSecurityAbortAtMs
        if (secAbortAt > lastHandledSecurityAbortMs) {
            lastHandledSecurityAbortMs = secAbortAt
            securityAbortPaused = true
        }

        val st = ZeroCamService.currentStatus
        if (st.configRevision != lastSyncedConfigRev) {
            lastSyncedConfigRev = st.configRevision
            settings = AppPrefs.load(this)
            updateProfileSummary()
            updateTorchButtonTint()
            swTorch.isChecked = settings.torchEnabled
        }
        if (st.running && st.connected && settings.wifiMode != st.isWifi) {
            settings = settings.copy(wifiMode = st.isWifi)
            AppPrefs.save(this, settings)
            syncTransportTabs()
            updateTransportHint()
        }
        val showOled = false
        val showLive = st.running

        viewHome.visibility = if (!st.running) View.VISIBLE else View.GONE
        viewLive.visibility = if (showLive) View.VISIBLE else View.GONE
        viewOled.visibility = if (showOled) View.VISIBLE else View.GONE
        applySystemBars(showOled)

        if (!st.running) {
            val hasCam = ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
            tvHomeStatus.text = when {
                st.statusText.isNotBlank() && st.statusText != "En espera" && st.statusText != "Detenido" -> st.statusText
                !hasCam -> "Toca Iniciar una vez para conceder permiso de cámara"
                settings.wifiMode && settings.wifiPin.length != 6 -> "Configura el PIN Wi-Fi de tu PC en ⚙ Ajustes"
                manualStopPaused -> "Pausado en móvil  •  Reinicia en PC o toca Iniciar"
                securityAbortPaused -> "Revisa PIN / vinculación en ⚙ Ajustes"
                else -> "En espera  •  Pulsa Iniciar Cámara en tu PC"
            }
            return
        }

        val w = if (st.width > 0) st.width else settings.videoWidth
        val h = if (st.height > 0) st.height else settings.videoHeight
        val mbps = if (st.kbps > 0) st.kbps / 1000.0 else settings.videoBitrate / 1_000_000.0
        val fpsDisp = if (st.fps > 0.5) st.fps else settings.videoFps.toDouble()
        val pLabel = when {
            h >= 1080 -> "1080p"
            h >= 720 -> "720p"
            else -> "480p"
        }

        if (showOled) {
            tvOledTitle.text = if (st.isWifi) "Transmitiendo por Wi-Fi" else "Transmitiendo por Cable USB"
            tvOledStats.text = String.format(Locale.US, "%d×%d @ %.0f fps  •  %.1f Mbps", w, h, fpsDisp, mbps)
            return
        }

        updateTorchButtonTint()
        tvLiveHeaderBadge.text = if (st.isWifi) "LIVE Wi-Fi" else "LIVE USB"
        dotLiveHeader.setBackgroundResource(if (st.connected) R.drawable.dot_success else R.drawable.dot_warning)
        dotRecBadge.setBackgroundResource(if (st.connected) R.drawable.dot_success else R.drawable.dot_warning)
        dotLiveStrip.setBackgroundResource(if (st.connected) R.drawable.dot_success else R.drawable.dot_warning)

        tvRecBadge.text = String.format(Locale.US, "REC  %s%.0f  %.1f Mbps", pLabel, fpsDisp, mbps)
        tvLiveHostStatus.setTextColor(Color.parseColor(if (st.connected) "#22C55E" else "#F59E0B"))
        tvLiveHostStatus.text = if (st.connected) {
            if (st.isWifi) "Conectado a ${st.hostLabel} (Wi-Fi)" else "Conectado por Cable USB"
        } else {
            st.statusText
        }
        tvLiveMetrics.text = String.format(Locale.US, "%.0f fps • %.1f Mbps", fpsDisp, mbps)
    }
}
