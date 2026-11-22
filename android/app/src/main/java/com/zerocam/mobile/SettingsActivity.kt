package com.zerocam.mobile

import android.content.SharedPreferences
import android.graphics.Color
import android.os.Bundle
import android.text.InputType
import android.widget.EditText
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat

/**
 * Pantalla dedicada de Ajustes de ZeroCam Mobile (réplica fiel del diseño Figma "Android / Ajustes").
 */
class SettingsActivity : AppCompatActivity() {

    private lateinit var settings: AppSettings

    private lateinit var tvPickCamera: TextView
    private lateinit var tvPickRes: TextView
    private lateinit var tvPickFps: TextView
    private lateinit var tvPickBitrate: TextView
    private lateinit var tvPinValue: TextView
    private lateinit var tvWifiHostValue: TextView
    private lateinit var tvTrustedHostSubtitle: TextView
    private lateinit var tvTrustedHostAction: TextView
    private lateinit var swOled: SwitchCompat
    private lateinit var swFallback: SwitchCompat

    private val prefsListener = SharedPreferences.OnSharedPreferenceChangeListener { _, _ ->
        runOnUiThread { reloadFromPrefsIfChanged() }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)

        settings = AppPrefs.load(this)
        bindViews()
        renderValues()
        setupListeners()
    }

    override fun onResume() {
        super.onResume()
        getSharedPreferences(AppPrefs.PREFS_NAME, MODE_PRIVATE)
            .registerOnSharedPreferenceChangeListener(prefsListener)
        reloadFromPrefsIfChanged()
    }

    override fun onPause() {
        getSharedPreferences(AppPrefs.PREFS_NAME, MODE_PRIVATE)
            .unregisterOnSharedPreferenceChangeListener(prefsListener)
        super.onPause()
    }

    private fun reloadFromPrefsIfChanged() {
        val latest = AppPrefs.load(this)
        if (latest != settings) {
            settings = latest
            renderValues()
        }
    }

    private fun bindViews() {
        findViewById<ImageButton>(R.id.btnBackSettings).setOnClickListener { finish() }
        tvPickCamera = findViewById(R.id.tvPickCamera)
        tvPickRes = findViewById(R.id.tvPickRes)
        tvPickFps = findViewById(R.id.tvPickFps)
        tvPickBitrate = findViewById(R.id.tvPickBitrate)
        tvPinValue = findViewById(R.id.tvPinValue)
        tvWifiHostValue = findViewById(R.id.tvWifiHostValue)
        tvTrustedHostSubtitle = findViewById(R.id.tvTrustedHostSubtitle)
        tvTrustedHostAction = findViewById(R.id.tvTrustedHostAction)
        swOled = findViewById(R.id.swSettingsOled)
        swFallback = findViewById(R.id.swSettingsFallback)
    }

    private fun renderValues() {
        tvPickCamera.text = if (settings.frontCamera) "Frontal" else "Trasera"
        tvPickRes.text = when (settings.videoWidth) {
            640 -> "640x480 (SD)"
            1920 -> "1080p Full HD"
            else -> "720p HD"
        }
        tvPickFps.text = "${settings.videoFps} FPS"
        tvPickBitrate.text = when {
            settings.videoBitrate <= 2_000_000 -> "Baja (2 Mbps)"
            settings.videoBitrate >= 12_000_000 -> "Ultra (12 Mbps)"
            settings.videoBitrate >= 8_000_000 -> "Alta (8 Mbps)"
            else -> "Media (4 Mbps)"
        }
        // Enmascara siempre el PIN en pantalla para evitar exposición visual.
        tvPinValue.text = if (settings.wifiPin.isEmpty()) "Sin PIN" else "••••"
        tvWifiHostValue.text = if (settings.manualHost.isEmpty()) "Auto (UDP)" else settings.manualHost
        if (settings.trustedHostFp.isNotEmpty()) {
            val fpDisp = WireCrypto.formatFingerprintDisplay(settings.trustedHostFp)
            val label = settings.trustedHostName.ifEmpty { "PC confiable" }
            tvTrustedHostSubtitle.text = "$label • $fpDisp"
            tvTrustedHostAction.text = "Desvincular"
            tvTrustedHostAction.setTextColor(Color.parseColor("#EF4444"))
        } else {
            tvTrustedHostSubtitle.text = "Sin vincular (conecta por USB o Wi-Fi)"
            tvTrustedHostAction.text = "Sin vincular"
            tvTrustedHostAction.setTextColor(Color.parseColor("#9AA4B2"))
        }
        if (swOled.isChecked != settings.powerSave) swOled.isChecked = settings.powerSave
        if (swFallback.isChecked != settings.autoFallback) swFallback.isChecked = settings.autoFallback
    }

    private fun persistAndApplyLive() {
        AppPrefs.save(this, settings)
        renderValues()
        if (ZeroCamService.currentStatus.running) {
            ZeroCamService.applyLocalSettings(
                width = settings.videoWidth,
                height = settings.videoHeight,
                fps = settings.videoFps,
                bitrate = settings.videoBitrate,
                front = settings.frontCamera,
                autoFallback = settings.autoFallback
            )
        }
    }

    private fun setupListeners() {
        findViewById<LinearLayout>(R.id.boxPickCamera).setOnClickListener {
            showSingleChoice("Lente de cámara", arrayOf("Trasera", "Frontal"), if (settings.frontCamera) 1 else 0) { idx ->
                settings = settings.copy(frontCamera = (idx == 1))
                persistAndApplyLive()
            }
        }

        findViewById<LinearLayout>(R.id.boxPickRes).setOnClickListener {
            val cur = when (settings.videoWidth) { 640 -> 0; 1920 -> 2; else -> 1 }
            showSingleChoice("Resolución", arrayOf("640x480 (SD)", "1280x720 (HD)", "1920x1080 (Full HD)"), cur) { idx ->
                val (w, h) = when (idx) { 0 -> 640 to 480; 2 -> 1920 to 1080; else -> 1280 to 720 }
                settings = settings.copy(videoWidth = w, videoHeight = h)
                persistAndApplyLive()
            }
        }

        findViewById<LinearLayout>(R.id.boxPickFps).setOnClickListener {
            val cur = when (settings.videoFps) { 15 -> 0; 24 -> 1; 60 -> 3; else -> 2 }
            showSingleChoice("Cuadros por segundo", arrayOf("15 FPS", "24 FPS", "30 FPS", "60 FPS"), cur) { idx ->
                val fps = when (idx) { 0 -> 15; 1 -> 24; 3 -> 60; else -> 30 }
                settings = settings.copy(videoFps = fps)
                persistAndApplyLive()
            }
        }

        findViewById<LinearLayout>(R.id.boxPickBitrate).setOnClickListener {
            val cur = when {
                settings.videoBitrate <= 2_000_000 -> 0
                settings.videoBitrate >= 12_000_000 -> 3
                settings.videoBitrate >= 8_000_000 -> 2
                else -> 1
            }
            showSingleChoice(
                "Calidad H.264",
                arrayOf("Baja (2 Mbps)", "Media (4 Mbps)", "Alta (8 Mbps)", "Ultra (12 Mbps)"),
                cur
            ) { idx ->
                val bps = when (idx) { 0 -> 2_000_000; 2 -> 8_000_000; 3 -> 12_000_000; else -> 4_000_000 }
                settings = settings.copy(videoBitrate = bps)
                persistAndApplyLive()
            }
        }

        findViewById<LinearLayout>(R.id.rowEditPin).setOnClickListener {
            showTextPrompt(
                title = "PIN Wi-Fi (obligatorio para Wi-Fi)",
                hint = "Escribe el PIN de 6 dígitos de ZeroCam PC",
                initial = settings.wifiPin,
                inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            ) { value ->
                settings = settings.copy(wifiPin = value.trim())
                AppPrefs.save(this, settings)
                renderValues()
            }
        }

        findViewById<LinearLayout>(R.id.rowEditWifiHost).setOnClickListener {
            showTextPrompt(
                title = "Conexión Wi-Fi (IP manual)",
                hint = "Vacío = Auto (UDP)",
                initial = settings.manualHost,
                inputType = InputType.TYPE_CLASS_TEXT
            ) { value ->
                settings = settings.copy(manualHost = value.trim())
                AppPrefs.save(this, settings)
                renderValues()
            }
        }

        findViewById<LinearLayout>(R.id.rowTrustedHost).setOnClickListener {
            if (settings.trustedHostFp.isEmpty()) return@setOnClickListener
            val fpDisp = WireCrypto.formatFingerprintDisplay(settings.trustedHostFp)
            val label = settings.trustedHostName.ifEmpty { "PC vinculada" }
            AlertDialog.Builder(this, R.style.ZeroCamAlertDialog)
                .setTitle("Desvincular PC confiable")
                .setMessage("¿Eliminar la confianza para $label?\nHuella FP: $fpDisp\n\nTendrás que reconectar por USB o confirmar la nueva huella por Wi-Fi.")
                .setPositiveButton("Desvincular") { _, _ ->
                    AppPrefs.clearTrustedHost(this)
                    settings = AppPrefs.load(this)
                    renderValues()
                }
                .setNegativeButton("Cancelar", null)
                .show()
        }

        swOled.setOnCheckedChangeListener { _, isChecked ->
            settings = settings.copy(powerSave = isChecked)
            AppPrefs.save(this, settings)
            if (ZeroCamService.currentStatus.running && isChecked) {
                ZeroCamService.setPreviewSurface(null)
            }
        }

        swFallback.setOnCheckedChangeListener { _, isChecked ->
            settings = settings.copy(autoFallback = isChecked)
            persistAndApplyLive()
        }
    }

    private fun showSingleChoice(title: String, items: Array<String>, checkedItem: Int, onPicked: (Int) -> Unit) {
        AlertDialog.Builder(this, R.style.ZeroCamAlertDialog)
            .setTitle(title)
            .setSingleChoiceItems(items, checkedItem) { dlg, which ->
                onPicked(which)
                dlg.dismiss()
            }
            .setNegativeButton("Cancelar", null)
            .show()
    }

    private fun showTextPrompt(
        title: String,
        hint: String,
        initial: String,
        inputType: Int,
        onSaved: (String) -> Unit
    ) {
        val density = resources.displayMetrics.density
        val pad = (20 * density).toInt()
        val et = EditText(this).apply {
            this.hint = hint
            setText(initial)
            setTextColor(Color.parseColor("#F1F3F7"))
            setHintTextColor(Color.parseColor("#6B7485"))
            setBackgroundResource(R.drawable.bg_input_box)
            setPadding((14 * density).toInt(), (10 * density).toInt(), (14 * density).toInt(), (10 * density).toInt())
            this.inputType = inputType
            setSelection(text.length)
        }
        val wrap = LinearLayout(this).apply {
            setPadding(pad, (12 * density).toInt(), pad, 0)
            addView(et, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        }
        AlertDialog.Builder(this, R.style.ZeroCamAlertDialog)
            .setTitle(title)
            .setView(wrap)
            .setPositiveButton("Guardar") { _, _ -> onSaved(et.text.toString()) }
            .setNegativeButton("Cancelar", null)
            .show()
    }
}
