#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "ZeroCamGuids.h"

namespace zerocam {

// Parámetros del ring buffer GPU compartido y fallback NV12 en CPU.
inline constexpr uint32_t kIpcMagic         = 0x5A434950u; // 'ZCIP'
inline constexpr uint32_t kIpcVersion       = 1;
inline constexpr uint32_t kRingSlots        = 3;
inline constexpr uint32_t kMaxCpuWidth      = 1920;
inline constexpr uint32_t kMaxCpuHeight     = 1080;
inline constexpr uint32_t kCpuSlotBytes     = kMaxCpuWidth * kMaxCpuHeight * 3u / 2u; // 3,110,400 B (NV12 1080p)
inline constexpr uint32_t kCpuFallbackSlots = 2;

/// Empaqueta (width | height | stride | slot) en un entero atómico de 64 bits para evitar carreras 720p↔1080p.
inline constexpr uint64_t PackCpuFrameMeta(uint32_t width, uint32_t height, uint32_t stride, uint32_t slot) noexcept {
    if (width < 2u || height < 2u || (width & 1u) != 0u || (height & 1u) != 0u ||
        width > kMaxCpuWidth || height > kMaxCpuHeight ||
        stride < width || stride > kMaxCpuWidth || slot >= kCpuFallbackSlots) {
        return 0ull;
    }
    return (static_cast<uint64_t>(width) & 0xFFFFull) |
           ((static_cast<uint64_t>(height) & 0xFFFFull) << 16) |
           ((static_cast<uint64_t>(stride) & 0xFFFFull) << 32) |
           ((static_cast<uint64_t>(slot) & 0xFFull) << 48) |
           (1ull << 56);
}

/// Desempaqueta los metadatos atómicos de un frame NV12 en CPU y valida sus cotas básicas.
inline bool UnpackCpuFrameMeta(
    uint64_t meta,
    uint32_t& outWidth,
    uint32_t& outHeight,
    uint32_t& outStride,
    uint32_t& outSlot) noexcept {
    if ((meta & (1ull << 56)) == 0ull) {
        return false;
    }
    const uint32_t w = static_cast<uint32_t>(meta & 0xFFFFull);
    const uint32_t h = static_cast<uint32_t>((meta >> 16) & 0xFFFFull);
    const uint32_t stride = static_cast<uint32_t>((meta >> 32) & 0xFFFFull);
    const uint32_t slot = static_cast<uint32_t>((meta >> 48) & 0xFFull);
    if (w < 2u || h < 2u || (w & 1u) != 0u || (h & 1u) != 0u ||
        w > kMaxCpuWidth || h > kMaxCpuHeight ||
        stride < w || stride > kMaxCpuWidth || slot >= kCpuFallbackSlots) {
        return false;
    }
    const size_t requiredBytes = static_cast<size_t>(stride) * (static_cast<size_t>(h) + static_cast<size_t>(h) / 2u);
    if (requiredBytes > kCpuSlotBytes) {
        return false;
    }
    outWidth = w;
    outHeight = h;
    outStride = stride;
    outSlot = slot;
    return true;
}

// Estructura en memoria compartida respaldada por archivo para IPC cross-session sin SeCreateGlobalPrivilege.
struct alignas(64) ZeroCamSharedMemory {
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;
    std::atomic<uint32_t> producerAlive;
    std::atomic<uint32_t> consumerCount;
    std::atomic<uint64_t> frameSequence;
    std::atomic<uint32_t> activeSlot;
    std::atomic<uint32_t> gpuHandlesGeneration;
    std::atomic<uint32_t> clientNeedsCpuFallback;
    std::atomic<uint32_t> usesKeyedMutex;

    std::atomic<uint32_t> adapterLuidLow;
    std::atomic<int32_t>  adapterLuidHigh;
    std::atomic<uint32_t> width;
    std::atomic<uint32_t> height;
    std::atomic<uint32_t> fpsNum;
    std::atomic<uint32_t> fpsDen;
    std::atomic<uint64_t> timestampHns;

    std::atomic<uint64_t> dxgiSharedHandles[kRingSlots];

    std::atomic<uint32_t> activeCpuSlot;
    std::atomic<uint32_t> cpuStride;
    std::atomic<uint64_t> cpuFrameMeta;
    uint8_t  reserved[16];
    uint8_t  cpuNv12Buffer[kCpuFallbackSlots][kCpuSlotBytes];
};

static_assert(std::atomic<uint32_t>::is_always_lock_free, "atomic<uint32_t> debe ser lock-free para memoria compartida.");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "atomic<uint64_t> debe ser lock-free para memoria compartida.");
static_assert(offsetof(ZeroCamSharedMemory, cpuNv12Buffer) == 128, "Layout estable de ZeroCamSharedMemory.");

/// Verifica que un directorio existente no sea un reparse point (symlink / junction).
inline bool IsSafeNonReparseParentDir(const std::wstring& filePath) noexcept {
    const size_t pos = filePath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return true;
    }
    const std::wstring parent = filePath.substr(0, pos);
    const DWORD attrs = GetFileAttributesW(parent.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

/// Verifica por handle que el archivo abierto es un archivo de disco regular sin reparse points ni hardlinks.
inline bool ValidateOpenedIpcFileHandle(HANDLE h) noexcept {
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        return false;
    }
    if (GetFileType(h) != FILE_TYPE_DISK) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info)) {
        return false;
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return false;
    }
    if (info.nNumberOfLinks != 1) {
        return false;
    }
    return true;
}

// Resuelve la ruta del archivo de IPC priorizando C:\ProgramData\ZeroCam\ipc_ring.bin (override solo en tests).
inline std::wstring ResolveIpcFilePath(bool allowTempFallback) {
#ifdef ZEROCAM_ALLOW_IPC_ENV_OVERRIDE
    wchar_t envBuf[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(kZeroCamIpcEnvOverride, envBuf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::wstring(envBuf, len);
    }
#endif

    if (!allowTempFallback) {
        return std::wstring(kZeroCamDefaultIpcPath);
    }

    // Verifica si ProgramData\ZeroCam existe y es escribible sin reparse points; si no, usa %TEMP%\ZeroCam.
    if (IsSafeNonReparseParentDir(kZeroCamDefaultIpcPath)) {
        const DWORD attr = GetFileAttributesW(kZeroCamDefaultIpcPath);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
            HANDLE probe = CreateFileW(
                kZeroCamDefaultIpcPath,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (probe != INVALID_HANDLE_VALUE) {
                const bool valid = ValidateOpenedIpcFileHandle(probe);
                CloseHandle(probe);
                if (valid) {
                    return std::wstring(kZeroCamDefaultIpcPath);
                }
            }
        }
    }

    wchar_t tempDir[MAX_PATH]{};
    const DWORD tlen = GetTempPathW(MAX_PATH, tempDir);
    std::wstring dir = (tlen > 0 && tlen < MAX_PATH) ? std::wstring(tempDir) + L"ZeroCam" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\ipc_ring.bin";
}

// Garantiza que los directorios padres existan antes de abrir el archivo IPC.
inline bool EnsureParentDirExists(const std::wstring& filePath) {
    const size_t pos = filePath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return true;
    }
    const std::wstring parent = filePath.substr(0, pos);
    CreateDirectoryW(parent.c_str(), nullptr);
    const DWORD attrs = GetFileAttributesW(parent.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

// Canal de memoria compartida respaldado por archivo para comunicar ZeroCam.exe y FrameServer.exe.
class SharedIpcChannel {
public:
    SharedIpcChannel() = default;
    ~SharedIpcChannel() { Close(); }

    SharedIpcChannel(const SharedIpcChannel&) = delete;
    SharedIpcChannel& operator=(const SharedIpcChannel&) = delete;

    // Abre o crea el mapeo compartido en modo productor o consumidor.
    bool Open(bool asProducer, bool allowTempFallback = false) {
        Close();
        path_ = ResolveIpcFilePath(asProducer && allowTempFallback);
        if (asProducer) {
            if (!EnsureParentDirExists(path_)) {
                return false;
            }
        } else if (!IsSafeNonReparseParentDir(path_)) {
            return false;
        }

        const DWORD creation = asProducer ? OPEN_ALWAYS : OPEN_EXISTING;
        fileHandle_ = CreateFileW(
            path_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            creation,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);

        if (fileHandle_ == INVALID_HANDLE_VALUE) {
            return false;
        }

        if (!ValidateOpenedIpcFileHandle(fileHandle_)) {
            Close(false);
            return false;
        }

        LARGE_INTEGER requiredSize{};
        requiredSize.QuadPart = static_cast<LONGLONG>(sizeof(ZeroCamSharedMemory));
        LARGE_INTEGER currentSize{};
        if (!GetFileSizeEx(fileHandle_, &currentSize)) {
            Close(false);
            return false;
        }
        if (currentSize.QuadPart < requiredSize.QuadPart) {
            // El consumidor (FrameServer) nunca debe extender archivos truncados por un actor externo.
            if (!asProducer) {
                Close(false);
                return false;
            }
            if (!SetFilePointerEx(fileHandle_, requiredSize, nullptr, FILE_BEGIN) || !SetEndOfFile(fileHandle_)) {
                Close(false);
                return false;
            }
        }

        const DWORD sizeHigh = static_cast<DWORD>(static_cast<uint64_t>(sizeof(ZeroCamSharedMemory)) >> 32);
        const DWORD sizeLow  = static_cast<DWORD>(static_cast<uint64_t>(sizeof(ZeroCamSharedMemory)) & 0xFFFFFFFFu);
        mappingHandle_ = CreateFileMappingW(fileHandle_, nullptr, PAGE_READWRITE, sizeHigh, sizeLow, nullptr);
        if (!mappingHandle_) {
            Close(false);
            return false;
        }

        void* ptr = MapViewOfFile(mappingHandle_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ZeroCamSharedMemory));
        if (!ptr) {
            Close(false);
            return false;
        }

        view_ = static_cast<ZeroCamSharedMemory*>(ptr);
        if (asProducer) {
            if (view_->magic.load(std::memory_order_acquire) != kIpcMagic ||
                view_->version.load(std::memory_order_acquire) != kIpcVersion) {
                view_->producerAlive.store(0u, std::memory_order_relaxed);
                view_->consumerCount.store(0u, std::memory_order_relaxed);
                view_->frameSequence.store(0ull, std::memory_order_relaxed);
                view_->activeSlot.store(0u, std::memory_order_relaxed);
                view_->gpuHandlesGeneration.store(0u, std::memory_order_relaxed);
                view_->clientNeedsCpuFallback.store(0u, std::memory_order_relaxed);
                view_->usesKeyedMutex.store(0u, std::memory_order_relaxed);
                view_->adapterLuidLow.store(0u, std::memory_order_relaxed);
                view_->adapterLuidHigh.store(0, std::memory_order_relaxed);
                view_->width.store(0u, std::memory_order_relaxed);
                view_->height.store(0u, std::memory_order_relaxed);
                view_->fpsNum.store(30u, std::memory_order_relaxed);
                view_->fpsDen.store(1u, std::memory_order_relaxed);
                view_->timestampHns.store(0ull, std::memory_order_relaxed);
                for (uint32_t i = 0; i < kRingSlots; ++i) {
                    view_->dxgiSharedHandles[i].store(0ull, std::memory_order_relaxed);
                }
                view_->activeCpuSlot.store(0u, std::memory_order_relaxed);
                view_->cpuStride.store(0u, std::memory_order_relaxed);
                view_->cpuFrameMeta.store(0ull, std::memory_order_relaxed);
                std::memset(view_->reserved, 0, sizeof(view_->reserved));
                std::memset(view_->cpuNv12Buffer, 0, sizeof(view_->cpuNv12Buffer));
                view_->version.store(kIpcVersion, std::memory_order_release);
                view_->magic.store(kIpcMagic, std::memory_order_release);
            }
            view_->producerAlive.store(GetCurrentProcessId(), std::memory_order_release);
        }

        if (view_->magic.load(std::memory_order_acquire) != kIpcMagic ||
            view_->version.load(std::memory_order_acquire) != kIpcVersion) {
            Close(false);
            return false;
        }
        return true;
    }

    // Cierra la vista y los handles sin eliminar el archivo persistente en disco.
    void Close(bool clearProducer = false) {
        if (view_) {
            if (clearProducer) {
                view_->producerAlive.store(0, std::memory_order_release);
            }
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (mappingHandle_) {
            CloseHandle(mappingHandle_);
            mappingHandle_ = nullptr;
        }
        if (fileHandle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(fileHandle_);
            fileHandle_ = INVALID_HANDLE_VALUE;
        }
    }

    ZeroCamSharedMemory* Data() const noexcept { return view_; }
    const std::wstring& Path() const noexcept { return path_; }
    bool IsOpen() const noexcept { return view_ != nullptr; }

private:
    HANDLE fileHandle_    = INVALID_HANDLE_VALUE;
    HANDLE mappingHandle_ = nullptr;
    ZeroCamSharedMemory* view_ = nullptr;
    std::wstring path_;
};

} // namespace zerocam
