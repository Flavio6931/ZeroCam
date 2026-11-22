package com.zerocam.mobile

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Almacén cifrado del PIN Wi-Fi respaldado por AndroidKeyStore (AES-256-GCM en hardware/TEE) sin texto claro en SharedPreferences.
 */
object SecurePinStore {
    private const val ANDROID_KEYSTORE = "AndroidKeyStore"
    private const val KEY_ALIAS = "zerocam_wifi_pin_aes_v2"
    private const val KEY_WIFI_PIN_ENC = "wifi_pin_enc"
    private const val KEY_WIFI_PIN_LEGACY = "wifi_pin"
    private const val GCM_IV_LEN = 12
    private const val GCM_TAG_BITS = 128

    /** Lee el PIN Wi-Fi cifrado; migra automáticamente si existía una clave heredada en texto claro. */
    fun loadPin(context: Context): String {
        val prefs = context.getSharedPreferences(AppPrefs.PREFS_NAME, Context.MODE_PRIVATE)
        val encB64 = prefs.getString(KEY_WIFI_PIN_ENC, null)
        if (!encB64.isNullOrBlank()) {
            val decrypted = decryptFromBase64(encB64)
            if (decrypted != null) {
                if (prefs.contains(KEY_WIFI_PIN_LEGACY)) {
                    prefs.edit().remove(KEY_WIFI_PIN_LEGACY).apply()
                }
                return decrypted
            }
            // Si la clave del Keystore fue invalidada por el sistema, limpia el blob corrupto sin crashear.
            prefs.edit().remove(KEY_WIFI_PIN_ENC).apply()
        }

        val legacy = prefs.getString(KEY_WIFI_PIN_LEGACY, "")?.trim().orEmpty()
        if (legacy.isNotEmpty()) {
            savePin(context, legacy)
            return legacy
        }
        return ""
    }

    /** Cifra y persiste el PIN Wi-Fi con AndroidKeyStore eliminando siempre cualquier clave heredada en texto claro. */
    fun savePin(context: Context, pin: String) {
        val prefs = context.getSharedPreferences(AppPrefs.PREFS_NAME, Context.MODE_PRIVATE)
        val cleaned = pin.trim()
        if (cleaned.isEmpty()) {
            prefs.edit().remove(KEY_WIFI_PIN_ENC).remove(KEY_WIFI_PIN_LEGACY).apply()
            return
        }
        val encB64 = encryptToBase64(cleaned) ?: return
        prefs.edit()
            .putString(KEY_WIFI_PIN_ENC, encB64)
            .remove(KEY_WIFI_PIN_LEGACY)
            .apply()
    }

    private fun getOrCreateSecretKey(): SecretKey {
        val ks = KeyStore.getInstance(ANDROID_KEYSTORE).apply { load(null) }
        val existing = ks.getKey(KEY_ALIAS, null) as? SecretKey
        if (existing != null) return existing

        val spec = KeyGenParameterSpec.Builder(
            KEY_ALIAS,
            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
        )
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .setRandomizedEncryptionRequired(true)
            .build()

        val kg = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, ANDROID_KEYSTORE)
        kg.init(spec)
        return kg.generateKey()
    }

    private fun encryptToBase64(plain: String): String? {
        return try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.ENCRYPT_MODE, getOrCreateSecretKey())
            val iv = cipher.iv
            if (iv == null || iv.size != GCM_IV_LEN) return null
            val ct = cipher.doFinal(plain.toByteArray(Charsets.UTF_8))
            val combined = ByteArray(iv.size + ct.size)
            System.arraycopy(iv, 0, combined, 0, iv.size)
            System.arraycopy(ct, 0, combined, iv.size, ct.size)
            Base64.encodeToString(combined, Base64.NO_WRAP)
        } catch (_: Exception) {
            null
        }
    }

    private fun decryptFromBase64(b64: String): String? {
        return try {
            val combined = Base64.decode(b64, Base64.NO_WRAP)
            if (combined.size <= GCM_IV_LEN + 16) return null
            val iv = combined.copyOfRange(0, GCM_IV_LEN)
            val ct = combined.copyOfRange(GCM_IV_LEN, combined.size)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, getOrCreateSecretKey(), GCMParameterSpec(GCM_TAG_BITS, iv))
            val plainBytes = cipher.doFinal(ct)
            val result = String(plainBytes, Charsets.UTF_8)
            plainBytes.fill(0)
            result
        } catch (_: Exception) {
            null
        }
    }
}
