#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace zerocam {

// Utilidad ligera para ejecutar adb.exe sin abrir consola y configurar túneles USB.
class AdbBridge {
public:
    /// Valida que un número de serie ADB contenga solo [A-Za-z0-9._:-] y no comience por '-'.
    static bool IsValidAdbSerial(const std::string& serial) noexcept;
    // Busca dispositivos Android conectados por USB y ejecuta adb reverse entre el puerto móvil y el puerto PC dado.
    static bool SetupUsbTunnel(
        uint16_t mobilePort,
        uint16_t pcPort,
        std::wstring& outMessage,
        bool wakeMobileApp = false,
        const std::atomic<bool>* cancelFlag = nullptr);
    // Sobrecarga equivalente cuando ambos extremos utilizan el mismo puerto TCP.
    static bool SetupUsbTunnel(uint16_t port, std::wstring& outMessage);
    /// Detiene el daemon adb.exe embebido junto a ZeroCam.exe y cierra su Job Object sin afectar otros adb del sistema.
    static void ShutdownBundledAdb();
#ifdef ZEROCAM_ALLOW_IPC_ENV_OVERRIDE
    /// Expone la captura acotada con timeout personalizable exclusivamente para pruebas de regresión.
    static bool RunProcessCaptureForTest(
        const std::wstring& exePath,
        const std::wstring& args,
        uint64_t timeoutMs,
        std::string& outStdout,
        unsigned long& outExitCode);
    /// Lanza un proceso auxiliar dentro del Job Object de AdbBridge y devuelve su HANDLE (cerrar tras verificar) para tests.
    static void* SpawnInAdbJobForTest(const std::wstring& exePath, const std::wstring& args);
#endif

private:
    static std::wstring FindAdbExecutable();
    static bool RunProcessCapture(
        const std::wstring& exePath,
        const std::wstring& args,
        std::string& outStdout,
        unsigned long& outExitCode,
        uint64_t timeoutMs = 5000ull,
        const std::atomic<bool>* cancelFlag = nullptr,
        bool attachToAdbJob = false);
};

} // namespace zerocam
