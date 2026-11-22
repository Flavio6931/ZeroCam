package com.zerocam.mobile

import android.content.Context

/**
 * Estado persistido de preferencias del usuario en ZeroCam Mobile (PIN protegido con AndroidKeyStore).
 */
data class AppSettings(
    val wifiMode: Boolean = false,
    val manualHost: String = "",
    val internalPort: Int = WireProtocol.DEFAULT_PORT,
    val wifiPin: String = "",
    val trustedHostFp: String = "",
    val trustedHostPub: String = "",
    val trustedHostName: String = "",
    val frontCamera: Boolean = false,
    val videoWidth: Int = 1280,
    val videoHeight: Int = 720,
    val videoFps: Int = 30,
    val videoBitrate: Int = 4_000_000,
    val powerSave: Boolean = false,
    val torchEnabled: Boolean = false,
    val autoFallback: Boolean = true
)

/**
 * Repositorio ligero de lectura/escritura de preferencias para MainActivity, SettingsActivity y ZeroCamService.
 */
object AppPrefs {
    const val PREFS_NAME = "zerocam_prefs"
    private const val KEY_WIFI_MODE = "wifi_mode"
    private const val KEY_MANUAL_HOST = "manual_host"
    private const val KEY_INTERNAL_PORT = "internal_port"
    private const val KEY_TRUSTED_FP = "trusted_host_fp"
    private const val KEY_TRUSTED_PUB = "trusted_host_pub"
    private const val KEY_TRUSTED_NAME = "trusted_host_name"
    private const val KEY_FRONT_CAM = "front_cam"
    private const val KEY_WIDTH = "width"
    private const val KEY_HEIGHT = "height"
    private const val KEY_FPS = "fps"
    private const val KEY_BITRATE = "bitrate"
    private const val KEY_POWER_SAVE = "power_save"
    private const val KEY_TORCH = "torch"
    private const val KEY_AUTO_FALLBACK = "auto_fallback"

    /**
     * Carga las preferencias guardadas aplicando valores por defecto seguros y descifrando el PIN con AndroidKeyStore.
     */
    fun load(context: Context): AppSettings {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        return AppSettings(
            wifiMode = prefs.getBoolean(KEY_WIFI_MODE, false),
            manualHost = prefs.getString(KEY_MANUAL_HOST, "")?.trim().orEmpty(),
            internalPort = prefs.getInt(KEY_INTERNAL_PORT, WireProtocol.DEFAULT_PORT)
                .takeIf { it in 1024..65535 } ?: WireProtocol.DEFAULT_PORT,
            wifiPin = SecurePinStore.loadPin(context),
            trustedHostFp = prefs.getString(KEY_TRUSTED_FP, "")?.trim()?.uppercase().orEmpty(),
            trustedHostPub = prefs.getString(KEY_TRUSTED_PUB, "")?.trim()?.uppercase().orEmpty(),
            trustedHostName = prefs.getString(KEY_TRUSTED_NAME, "")?.trim().orEmpty(),
            frontCamera = prefs.getBoolean(KEY_FRONT_CAM, false),
            videoWidth = prefs.getInt(KEY_WIDTH, 1280),
            videoHeight = prefs.getInt(KEY_HEIGHT, 720),
            videoFps = prefs.getInt(KEY_FPS, 30),
            videoBitrate = prefs.getInt(KEY_BITRATE, 4_000_000),
            powerSave = false,
            torchEnabled = prefs.getBoolean(KEY_TORCH, false),
            autoFallback = prefs.getBoolean(KEY_AUTO_FALLBACK, true)
        )
    }

    /**
     * Persiste las preferencias completas en disco cifrando el PIN con AndroidKeyStore.
     */
    fun save(context: Context, s: AppSettings) {
        SecurePinStore.savePin(context, s.wifiPin)
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit().apply {
            putBoolean(KEY_WIFI_MODE, s.wifiMode)
            putString(KEY_MANUAL_HOST, s.manualHost)
            putInt(KEY_INTERNAL_PORT, s.internalPort)
            putString(KEY_TRUSTED_FP, s.trustedHostFp.uppercase())
            putString(KEY_TRUSTED_PUB, s.trustedHostPub.uppercase())
            putString(KEY_TRUSTED_NAME, s.trustedHostName)
            putBoolean(KEY_FRONT_CAM, s.frontCamera)
            putInt(KEY_WIDTH, s.videoWidth)
            putInt(KEY_HEIGHT, s.videoHeight)
            putInt(KEY_FPS, s.videoFps)
            putInt(KEY_BITRATE, s.videoBitrate)
            putBoolean(KEY_POWER_SAVE, false)
            putBoolean(KEY_TORCH, s.torchEnabled)
            putBoolean(KEY_AUTO_FALLBACK, s.autoFallback)
            apply()
        }
    }

    /**
     * Fija la identidad confiable (huella FP + clave pública SEC1) de la PC tras conexión USB o confirmación del usuario.
     */
    fun saveTrustedHost(context: Context, fpHex: String, pubHex: String, hostName: String) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit().apply {
            putString(KEY_TRUSTED_FP, fpHex.trim().uppercase())
            putString(KEY_TRUSTED_PUB, pubHex.trim().uppercase())
            if (hostName.isNotBlank()) {
                putString(KEY_TRUSTED_NAME, hostName.trim())
            }
            apply()
        }
    }

    /**
     * Elimina la PC vinculada confiable (permite emparejar un equipo diferente).
     */
    fun clearTrustedHost(context: Context) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit().apply {
            remove(KEY_TRUSTED_FP)
            remove(KEY_TRUSTED_PUB)
            remove(KEY_TRUSTED_NAME)
            apply()
        }
    }

    /**
     * Actualiza únicamente la configuración de video (resolución, FPS y bitrate) sincronizada desde PC.
     */
    fun updateVideoConfig(context: Context, width: Int, height: Int, fps: Int, bitrate: Int) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit().apply {
            putInt(KEY_WIDTH, width)
            putInt(KEY_HEIGHT, height)
            putInt(KEY_FPS, fps)
            putInt(KEY_BITRATE, bitrate)
            apply()
        }
    }

    /**
     * Actualiza lente frontal y linterna cuando se controlan remotamente desde PC.
     */
    fun updateCameraFlags(context: Context, frontCamera: Boolean, torchEnabled: Boolean) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit().apply {
            putBoolean(KEY_FRONT_CAM, frontCamera)
            putBoolean(KEY_TORCH, torchEnabled)
            apply()
        }
    }
}
