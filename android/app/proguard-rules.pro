# Reglas ProGuard / R8 endurecidas para ZeroCam Mobile (AAPT2 mantiene automáticamente Activities y Services del Manifest).
-repackageclasses 'o'
-allowaccessmodification

# Elimina llamadas de diagnóstico Log.v/d/i en compilaciones Release.
-assumenosideeffects class android.util.Log {
    public static int v(...);
    public static int d(...);
    public static int i(...);
}
