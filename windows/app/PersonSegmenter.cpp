#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.AI.MachineLearning.h>
#include "PersonSegmenter.h"
#include "resource.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

namespace zerocam {

namespace {

// Localiza el recurso RCDATA IDR_RVM_ONNX directamente en la imagen PE en memoria (cero disco, cero TOCTOU).
bool FindEmbeddedModelResource(const uint8_t*& outPtr, size_t& outSize) noexcept {
    outPtr = nullptr;
    outSize = 0;
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_RVM_ONNX), RT_RCDATA);
    if (!res) return false;
    const DWORD bytes = SizeofResource(nullptr, res);
    HGLOBAL loaded = LoadResource(nullptr, res);
    if (!loaded || bytes == 0) return false;
    const void* raw = LockResource(loaded);
    if (!raw) return false;
    outPtr = static_cast<const uint8_t*>(raw);
    outSize = static_cast<size_t>(bytes);
    return true;
}

// Carga un modelo ONNX directamente desde RAM usando InMemoryRandomAccessStream + LearningModel::LoadFromStream.
winrt::Windows::AI::MachineLearning::LearningModel LoadModelFromMemoryBytes(const uint8_t* data, size_t size) {
    using namespace winrt::Windows::AI::MachineLearning;
    using namespace winrt::Windows::Storage::Streams;
    if (!data || size == 0 || size > 128u * 1024u * 1024u) {
        return nullptr;
    }
    InMemoryRandomAccessStream memStream;
    DataWriter writer(memStream);
    writer.WriteBytes(winrt::array_view<const uint8_t>(data, data + size));
    writer.StoreAsync().get();
    writer.FlushAsync().get();
    writer.DetachStream();
    memStream.Seek(0);
    auto streamRef = RandomAccessStreamReference::CreateFromStream(memStream);
    return LearningModel::LoadFromStream(streamRef);
}

// Detecta si la entrada 'src' del modelo RVM espera Float16 (fp16) o Float32 (fp32).
bool DetectModelUsesFp16(const winrt::Windows::AI::MachineLearning::LearningModel& model) {
    using namespace winrt::Windows::AI::MachineLearning;
    for (auto const& feat : model.InputFeatures()) {
        if (feat.Name() == L"src") {
            if (auto td = feat.try_as<TensorFeatureDescriptor>()) {
                return td.TensorKind() == TensorKind::Float16;
            }
        }
    }
    return true;
}

// Convierte un flotante normalizado [0..1] a byte uint8 protegiendo contra NaN/Inf.
inline uint8_t FloatToByte(float v) noexcept {
    if (!std::isfinite(v)) return 0u;
    return static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
}

} // namespace

struct PersonSegmenter::Impl {
    static constexpr size_t kPlane = static_cast<size_t>(kModelW) * kModelH;
    static constexpr float kDownsampleGpu = 0.75f; // Backbone 384x216 + Deep Guided Filter 512x288 (~14 ms DirectML)
    static constexpr float kDownsampleCpu = 0.50f; // Backbone 256x144 + Deep Guided Filter 512x288 (~32 ms CPU)

    const uint8_t* modelDataPtr = nullptr;
    size_t modelDataSize = 0;
    std::vector<uint8_t> ownedModelBytes;
    std::atomic<bool> available{ false };
    std::atomic<bool> busy{ false };
    std::atomic<bool> usingGpu{ false };
    std::atomic<bool> stopRequested{ false };
    std::atomic<uint64_t> resetEpoch{ 0 };
    std::atomic<double> lastMs{ 0.0 };

    std::mutex reqMutex;
    std::condition_variable cv;
    bool hasPending = false;
    bool pendingUseGpu = true;
    uint64_t pendingEpoch = 0;
    std::vector<float> pendingNchw;

    mutable std::mutex matteMutex;
    std::vector<uint8_t> latestMatteBgra;
    uint64_t matteGen = 0;

    std::thread worker;

    // Bucle principal del hilo WinML con carga diferida del modelo y memoria temporal recurrente r1..r4.
    void WorkerMain() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            using namespace winrt::Windows::AI::MachineLearning;
            LearningModel model{ nullptr };
            LearningModelSession session{ nullptr };
            bool modelLoaded = false;
            bool modelIsFp16 = true;
            bool sessionCreated = false;
            bool sessionIsGpu = false;
            bool forceCpuFallback = false;
            int gpuConsecutiveFails = 0;
            int cpuConsecutiveFails = 0;

            const std::vector<int64_t> srcShape = { 1, 3, static_cast<int64_t>(kModelH), static_cast<int64_t>(kModelW) };
            const std::vector<int64_t> recInitShape = { 1, 1, 1, 1 };
            const std::vector<int64_t> dsShape = { 1 };
            const std::vector<float> zeroScalar = { 0.0f };

            winrt::Windows::Foundation::IInspectable r1{ nullptr };
            winrt::Windows::Foundation::IInspectable r2{ nullptr };
            winrt::Windows::Foundation::IInspectable r3{ nullptr };
            winrt::Windows::Foundation::IInspectable r4{ nullptr };
            TensorFloat dsTensor{ nullptr };
            bool hasRecurrent = false;
            uint64_t lastRecurrentEpoch = UINT64_MAX;
            auto lastEvalSteady = std::chrono::steady_clock::now();

            std::vector<float> localNchw(kPlane * 3u);
            std::vector<float> phaBuf(kPlane);
            std::vector<float> fgrBuf(kPlane * 3u);
            std::vector<uint8_t> packBgra(kPlane * 4u);

            auto releaseSession = [&]() {
                r1 = r2 = r3 = r4 = nullptr;
                dsTensor = nullptr;
                hasRecurrent = false;
                if (session) {
                    try {
                        session.Close();
                    } catch (...) {}
                    session = nullptr;
                }
                sessionCreated = false;
            };

            auto resetRecurrentTensors = [&]() {
                if (modelIsFp16) {
                    r1 = TensorFloat16Bit::CreateFromArray(recInitShape, zeroScalar);
                    r2 = TensorFloat16Bit::CreateFromArray(recInitShape, zeroScalar);
                    r3 = TensorFloat16Bit::CreateFromArray(recInitShape, zeroScalar);
                    r4 = TensorFloat16Bit::CreateFromArray(recInitShape, zeroScalar);
                } else {
                    r1 = TensorFloat::CreateFromArray(recInitShape, zeroScalar);
                    r2 = TensorFloat::CreateFromArray(recInitShape, zeroScalar);
                    r3 = TensorFloat::CreateFromArray(recInitShape, zeroScalar);
                    r4 = TensorFloat::CreateFromArray(recInitShape, zeroScalar);
                }
                hasRecurrent = true;
            };

            while (!stopRequested.load(std::memory_order_acquire)) {
                bool reqGpu = true;
                uint64_t jobEpoch = 0;
                {
                    std::unique_lock<std::mutex> lk(reqMutex);
                    cv.wait(lk, [&]() { return stopRequested.load(std::memory_order_acquire) || hasPending; });
                    if (stopRequested.load(std::memory_order_acquire)) break;
                    localNchw.swap(pendingNchw);
                    reqGpu = pendingUseGpu;
                    jobEpoch = pendingEpoch;
                    hasPending = false;
                    busy.store(true, std::memory_order_release);
                }

                // Descarta inmediatamente si ocurrió un reset de geometría/estado antes de inferir.
                if (jobEpoch != resetEpoch.load(std::memory_order_acquire)) {
                    busy.store(false, std::memory_order_release);
                    continue;
                }

                bool attemptingGpu = false;
                try {
                    // Carga diferida desde RAM: mientras Desenfoque esté Apagado, WinML no consume CPU, disco ni VRAM.
                    if (!modelLoaded) {
                        try {
                            model = LoadModelFromMemoryBytes(modelDataPtr, modelDataSize);
                        } catch (...) {
                            model = nullptr;
                        }
                        if (!model) {
                            available.store(false, std::memory_order_release);
                            busy.store(false, std::memory_order_release);
                            break;
                        }
                        modelIsFp16 = DetectModelUsesFp16(model);
                        modelLoaded = true;
                    }

                    const bool wantGpu = reqGpu && !forceCpuFallback;
                    attemptingGpu = sessionCreated ? sessionIsGpu : wantGpu;
                    if (!sessionCreated || sessionIsGpu != wantGpu) {
                        releaseSession();
                        sessionIsGpu = false;
                        usingGpu.store(false, std::memory_order_release);

                        if (wantGpu) {
                            try {
                                attemptingGpu = true;
                                LearningModelDevice gpuDev(LearningModelDeviceKind::DirectXHighPerformance);
                                session = LearningModelSession(model, gpuDev);
                                sessionIsGpu = true;
                                sessionCreated = true;
                            } catch (...) {
                                // Respaldo automático a CPU si DirectML no pudo inicializarse en este adaptador.
                                forceCpuFallback = true;
                                attemptingGpu = false;
                                LearningModelDevice cpuDev(LearningModelDeviceKind::Cpu);
                                session = LearningModelSession(model, cpuDev);
                                sessionIsGpu = false;
                                sessionCreated = true;
                            }
                        } else {
                            attemptingGpu = false;
                            LearningModelDevice cpuDev(LearningModelDeviceKind::Cpu);
                            session = LearningModelSession(model, cpuDev);
                            sessionIsGpu = false;
                            sessionCreated = true;
                        }

                        usingGpu.store(sessionIsGpu, std::memory_order_release);
                        const std::vector<float> dsVec = { sessionIsGpu ? kDownsampleGpu : kDownsampleCpu };
                        dsTensor = TensorFloat::CreateFromArray(dsShape, dsVec);
                    }

                    const auto now = std::chrono::steady_clock::now();
                    const bool stalePause = hasRecurrent &&
                        (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEvalSteady).count() > 1500);
                    if (!hasRecurrent || jobEpoch != lastRecurrentEpoch || stalePause) {
                        resetRecurrentTensors();
                        lastRecurrentEpoch = jobEpoch;
                    }

                    const auto t0 = std::chrono::steady_clock::now();
                    LearningModelBinding binding(session);
                    if (modelIsFp16) {
                        auto srcT = TensorFloat16Bit::CreateFromArray(srcShape, localNchw);
                        binding.Bind(L"src", srcT);
                    } else {
                        auto srcT = TensorFloat::CreateFromArray(srcShape, localNchw);
                        binding.Bind(L"src", srcT);
                    }
                    binding.Bind(L"r1i", r1);
                    binding.Bind(L"r2i", r2);
                    binding.Bind(L"r3i", r3);
                    binding.Bind(L"r4i", r4);
                    binding.Bind(L"downsample_ratio", dsTensor);

                    auto result = session.Evaluate(binding, L"rvm");
                    auto outputs = result.Outputs();

                    // Mantiene los estados recurrentes r1..r4 directamente como tensores WinML/DirectML.
                    r1 = outputs.Lookup(L"r1o");
                    r2 = outputs.Lookup(L"r2o");
                    r3 = outputs.Lookup(L"r3o");
                    r4 = outputs.Lookup(L"r4o");
                    hasRecurrent = true;
                    lastRecurrentEpoch = jobEpoch;
                    lastEvalSteady = std::chrono::steady_clock::now();
                    gpuConsecutiveFails = 0;
                    cpuConsecutiveFails = 0;

                    uint32_t phaGot = 0;
                    uint32_t fgrGot = 0;
                    if (modelIsFp16) {
                        phaGot = outputs.Lookup(L"pha").as<TensorFloat16Bit>().GetAsVectorView().GetMany(0, phaBuf);
                        fgrGot = outputs.Lookup(L"fgr").as<TensorFloat16Bit>().GetAsVectorView().GetMany(0, fgrBuf);
                    } else {
                        phaGot = outputs.Lookup(L"pha").as<TensorFloat>().GetAsVectorView().GetMany(0, phaBuf);
                        fgrGot = outputs.Lookup(L"fgr").as<TensorFloat>().GetAsVectorView().GetMany(0, fgrBuf);
                    }

                    const double elapsedMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
                    lastMs.store(elapsedMs, std::memory_order_release);

                    if (phaGot >= kPlane && fgrGot >= kPlane * 3u) {
                        const float* rSrc = fgrBuf.data();
                        const float* gSrc = rSrc + kPlane;
                        const float* bSrc = gSrc + kPlane;
                        const float* aSrc = phaBuf.data();
                        uint8_t* dst = packBgra.data();

                        for (size_t i = 0; i < kPlane; ++i) {
                            const size_t o = i * 4u;
                            dst[o + 0u] = FloatToByte(bSrc[i]);
                            dst[o + 1u] = FloatToByte(gSrc[i]);
                            dst[o + 2u] = FloatToByte(rSrc[i]);
                            dst[o + 3u] = FloatToByte(aSrc[i]);
                        }

                        std::lock_guard<std::mutex> mk(matteMutex);
                        if (jobEpoch == resetEpoch.load(std::memory_order_acquire)) {
                            latestMatteBgra.swap(packBgra);
                            if (packBgra.size() != kPlane * 4u) {
                                packBgra.resize(kPlane * 4u);
                            }
                            ++matteGen;
                        }
                    }
                } catch (...) {
                    releaseSession();
                    sessionIsGpu = false;
                    usingGpu.store(false, std::memory_order_release);
                    if (attemptingGpu) {
                        ++gpuConsecutiveFails;
                        if (gpuConsecutiveFails >= 2) {
                            forceCpuFallback = true;
                        }
                    } else {
                        ++cpuConsecutiveFails;
                        if (cpuConsecutiveFails >= 3) {
                            available.store(false, std::memory_order_release);
                            busy.store(false, std::memory_order_release);
                            break;
                        }
                    }
                }
                busy.store(false, std::memory_order_release);
            }

            releaseSession();
            if (model) {
                try {
                    model.Close();
                } catch (...) {}
                model = nullptr;
            }
        } catch (...) {
            available.store(false, std::memory_order_release);
        }
        busy.store(false, std::memory_order_release);
        winrt::uninit_apartment();
    }
};

PersonSegmenter::PersonSegmenter() : impl_(std::make_unique<Impl>()) {}

PersonSegmenter::~PersonSegmenter() {
    Shutdown();
}

bool PersonSegmenter::Initialize(const std::wstring& optionalModelPath) {
    Shutdown();

    if (optionalModelPath.empty()) {
        // Producción: únicamente desde el recurso RCDATA embebido en la imagen PE (cero fallbacks a CWD / %LOCALAPPDATA%).
        const uint8_t* ptr = nullptr;
        size_t sz = 0;
        if (!FindEmbeddedModelResource(ptr, sz)) {
            return false;
        }
        impl_->ownedModelBytes.clear();
        impl_->modelDataPtr = ptr;
        impl_->modelDataSize = sz;
    } else {
        // Ruta explícita (solo pruebas): rechaza reparse points y lee una sola vez a RAM para cargar vía LoadFromStream.
        const DWORD attrs = GetFileAttributesW(optionalModelPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES ||
            (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            return false;
        }
        std::ifstream f(optionalModelPath, std::ios::binary | std::ios::ate);
        if (!f.good()) return false;
        const std::streamsize fileSz = f.tellg();
        if (fileSz <= 0 || fileSz > 128LL * 1024LL * 1024LL) return false;
        impl_->ownedModelBytes.resize(static_cast<size_t>(fileSz));
        f.seekg(0, std::ios::beg);
        if (!f.read(reinterpret_cast<char*>(impl_->ownedModelBytes.data()), fileSz).good()) {
            impl_->ownedModelBytes.clear();
            return false;
        }
        impl_->modelDataPtr = impl_->ownedModelBytes.data();
        impl_->modelDataSize = impl_->ownedModelBytes.size();
    }

    impl_->stopRequested.store(false, std::memory_order_release);
    impl_->resetEpoch.store(0, std::memory_order_release);
    impl_->usingGpu.store(false, std::memory_order_release);
    impl_->available.store(true, std::memory_order_release);
    impl_->lastMs.store(0.0, std::memory_order_release);
    impl_->pendingNchw.assign(Impl::kPlane * 3u, 0.0f);
    impl_->worker = std::thread(&Impl::WorkerMain, impl_.get());
    return true;
}

bool PersonSegmenter::InitializeFromMemory(const uint8_t* modelBytes, size_t modelSize) {
    Shutdown();
    if (!modelBytes || modelSize == 0 || modelSize > 128u * 1024u * 1024u) {
        return false;
    }
    impl_->ownedModelBytes.assign(modelBytes, modelBytes + modelSize);
    impl_->modelDataPtr = impl_->ownedModelBytes.data();
    impl_->modelDataSize = impl_->ownedModelBytes.size();
    impl_->stopRequested.store(false, std::memory_order_release);
    impl_->resetEpoch.store(0, std::memory_order_release);
    impl_->usingGpu.store(false, std::memory_order_release);
    impl_->available.store(true, std::memory_order_release);
    impl_->lastMs.store(0.0, std::memory_order_release);
    impl_->pendingNchw.assign(Impl::kPlane * 3u, 0.0f);
    impl_->worker = std::thread(&Impl::WorkerMain, impl_.get());
    return true;
}

void PersonSegmenter::Shutdown() {
    if (!impl_) return;
    impl_->stopRequested.store(true, std::memory_order_release);
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    {
        std::lock_guard<std::mutex> lk(impl_->reqMutex);
        impl_->hasPending = false;
    }
    {
        std::lock_guard<std::mutex> mk(impl_->matteMutex);
        impl_->latestMatteBgra.clear();
        impl_->matteGen = 0;
    }
    impl_->modelDataPtr = nullptr;
    impl_->modelDataSize = 0;
    impl_->ownedModelBytes.clear();
    impl_->busy.store(false, std::memory_order_release);
    impl_->usingGpu.store(false, std::memory_order_release);
    impl_->available.store(false, std::memory_order_release);
}

void PersonSegmenter::ResetTemporalState() noexcept {
    if (!impl_) return;
    impl_->resetEpoch.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lk(impl_->reqMutex);
        impl_->hasPending = false;
    }
    {
        std::lock_guard<std::mutex> mk(impl_->matteMutex);
        impl_->latestMatteBgra.clear();
    }
}

bool PersonSegmenter::IsAvailable() const noexcept {
    return impl_ && impl_->available.load(std::memory_order_acquire);
}

bool PersonSegmenter::IsBusy() const noexcept {
    return impl_ && impl_->busy.load(std::memory_order_acquire);
}

bool PersonSegmenter::IsUsingGpu() const noexcept {
    return impl_ && impl_->usingGpu.load(std::memory_order_acquire);
}

double PersonSegmenter::LastInferenceMs() const noexcept {
    return impl_ ? impl_->lastMs.load(std::memory_order_acquire) : 0.0;
}

void PersonSegmenter::SubmitBgra512x288(const uint8_t* bgra, uint32_t rowPitch, bool useGpu) {
    if (!impl_ || !bgra || rowPitch < kModelW * 4u || !impl_->available.load(std::memory_order_acquire)) {
        return;
    }
    if (impl_->busy.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lk(impl_->reqMutex);
    if (impl_->hasPending) {
        return;
    }

    constexpr size_t plane = Impl::kPlane;
    impl_->pendingNchw.resize(plane * 3u);
    float* rDst = impl_->pendingNchw.data();
    float* gDst = rDst + plane;
    float* bDst = gDst + plane;
    constexpr float kInv255 = 1.0f / 255.0f;

    for (uint32_t y = 0; y < kModelH; ++y) {
        const uint8_t* srcRow = bgra + static_cast<size_t>(y) * rowPitch;
        const size_t rowBase = static_cast<size_t>(y) * kModelW;
        for (uint32_t x = 0; x < kModelW; ++x) {
            const size_t px = static_cast<size_t>(x) * 4u;
            const size_t outIdx = rowBase + x;
            bDst[outIdx] = static_cast<float>(srcRow[px + 0u]) * kInv255;
            gDst[outIdx] = static_cast<float>(srcRow[px + 1u]) * kInv255;
            rDst[outIdx] = static_cast<float>(srcRow[px + 2u]) * kInv255;
        }
    }
    impl_->pendingEpoch = impl_->resetEpoch.load(std::memory_order_acquire);
    impl_->pendingUseGpu = useGpu;
    impl_->hasPending = true;
    impl_->cv.notify_one();
}

bool PersonSegmenter::CopyLatestMatte(std::vector<uint8_t>& outMatteBgra, uint64_t& inOutGen) const {
    if (!impl_) return false;
    std::lock_guard<std::mutex> mk(impl_->matteMutex);
    if (impl_->matteGen == 0 || impl_->matteGen == inOutGen || impl_->latestMatteBgra.empty()) {
        return false;
    }
    outMatteBgra = impl_->latestMatteBgra;
    inOutGen = impl_->matteGen;
    return true;
}

} // namespace zerocam
