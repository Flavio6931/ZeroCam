#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfobjects.h>
#include <devicetopology.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include "../shared/ZeroCamIpc.h"

namespace zerocam {

class ZeroCamMediaSource;

// Flujo de video de la cámara virtual compatible con D3D11 zero-copy y fallback NV12 en CPU.
class ZeroCamMediaStream final : public IMFMediaStream2, public IKsControl, public IMFGetService {
public:
    explicit ZeroCamMediaStream(ZeroCamMediaSource* parent);
    ~ZeroCamMediaStream();

    // Inicializa descriptores de flujo, cola de eventos y canal IPC.
    HRESULT Initialize();

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    IFACEMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    IFACEMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFMediaStream
    IFACEMETHODIMP GetMediaSource(IMFMediaSource** ppMediaSource) override;
    IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) override;
    IFACEMETHODIMP RequestSample(IUnknown* pToken) override;

    // IMFMediaStream2
    IFACEMETHODIMP SetStreamState(MF_STREAM_STATE value) override;
    IFACEMETHODIMP GetStreamState(MF_STREAM_STATE* value) override;

    // IKsControl
    IFACEMETHODIMP KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned) override;
    IFACEMETHODIMP KsMethod(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned) override;
    IFACEMETHODIMP KsEvent(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned) override;

    // IMFGetService
    IFACEMETHODIMP GetService(REFGUID guidService, REFIID riid, LPVOID* ppvObject) override;

    // Control interno desde ZeroCamMediaSource
    HRESULT Start(const PROPVARIANT* pvarStartPosition);
    HRESULT Stop();
    HRESULT Pause();
    HRESULT Shutdown();
    HRESULT SetD3DManager(IUnknown* pManager);
    HRESULT SetAllocator(IUnknown* pAllocator);
    HRESULT GetStreamAttributes(IMFAttributes** ppAttributes);

private:
    // Construye un IMFMediaType NV12 válido para las resoluciones anunciadas.
    HRESULT CreateNv12MediaType(UINT32 width, UINT32 height, UINT32 fpsNum, UINT32 fpsDen, IMFMediaType** ppType) const;
    // Asegura apertura de texturas DXGI compartidas en el dispositivo D3D11 de FrameServer.
    bool EnsureSharedTextures(ID3D11Device* dev, ZeroCamSharedMemory* ipc);
    // Asegura una textura local NV12 del pool de FrameServer para copiar VRAM->VRAM sin bloquear el ring.
    bool EnsureLocalPool(ID3D11Device* dev, UINT32 width, UINT32 height);
    // Asegura una textura NV12 oscura BT.709 en VRAM para standby cuando FrameServer usa allocator D3D11.
    bool EnsureStandbyGpuTex(ID3D11Device* dev, UINT32 width, UINT32 height);
    // Asegura textura staging/upload NV12 en D3D11 para subir frames escalados desde CPU.
    bool EnsureCpuUploadTex(ID3D11Device* dev, UINT32 width, UINT32 height, bool& outIsStaging);
    // Asegura un ID3D11VideoProcessor para reescalar NV12->NV12 en VRAM cuando el consumidor pide otra resolución.
    bool EnsureGpuScaleProcessor(ID3D11Device* dev, UINT32 srcW, UINT32 srcH, UINT32 dstW, UINT32 dstH);
    // Intenta producir un IMFSample zero-copy GPU; retorna nullptr si debe usar fallback CPU/standby.
    HRESULT TryCreateGpuSample(ZeroCamSharedMemory* ipc, IMFSample** ppSample, bool& usedGpu);
    // Inicializa el IMFVideoSampleAllocatorEx provisto por FrameServer con el MediaType negociado.
    HRESULT EnsureAllocatorInitialized();
    // Produce un IMFSample NV12 en memoria del sistema o vía allocator externo (fallback o patrón standby).
    HRESULT CreateCpuOrStandbySample(ZeroCamSharedMemory* ipc, IMFSample** ppSample);
    // Sincroniza cadencia de entrega para evitar busy-wait si el consumidor solicita sin vsync.
    void PaceNextFrame(UINT32 fpsNum, UINT32 fpsDen, uint64_t currentSeq);

    std::atomic<ULONG> refCount_{ 1 };
    std::mutex mutex_;
    ZeroCamMediaSource* parent_ = nullptr; // Referencia débil mantenida viva por AddRef cuando es necesario.
    Microsoft::WRL::ComPtr<IMFMediaSource> parentKeepAlive_;
    Microsoft::WRL::ComPtr<IMFMediaEventQueue> eventQueue_;
    Microsoft::WRL::ComPtr<IMFStreamDescriptor> streamDescriptor_;
    Microsoft::WRL::ComPtr<IMFAttributes> streamAttributes_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiManager_;
    Microsoft::WRL::ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    bool allocatorInitialized_ = false;
    UINT32 allocW_ = 0;
    UINT32 allocH_ = 0;
    HANDLE d3dDeviceHandle_ = nullptr;

    SharedIpcChannel ipc_;
    bool ipcOpened_ = false;
    bool consumerRegistered_ = false;
    bool isShutdown_ = false;
    MF_STREAM_STATE streamState_ = MF_STREAM_STATE_STOPPED;

    UINT32 negotiatedWidth_  = 1280;
    UINT32 negotiatedHeight_ = 720;
    UINT32 negotiatedFpsNum_ = 30;
    UINT32 negotiatedFpsDen_ = 1;

    uint32_t cachedGeneration_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> cachedDevice_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTextures_[kRingSlots];
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> sharedMutexes_[kRingSlots];
    bool sharedIsKeyed_ = false;

    static constexpr uint32_t kLocalPoolCount = 3;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> localPool_[kLocalPoolCount];
    UINT32 localPoolW_ = 0;
    UINT32 localPoolH_ = 0;
    uint32_t localPoolIdx_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> standbyDevice_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> standbyGpuTex_;
    UINT32 standbyW_ = 0;
    UINT32 standbyH_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> cpuUploadDevice_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cpuUploadTex_;
    UINT32 cpuUploadW_ = 0;
    UINT32 cpuUploadH_ = 0;
    bool cpuUploadIsStaging_ = false;
    std::vector<uint8_t> scaledNv12Scratch_;

    Microsoft::WRL::ComPtr<ID3D11Device> scaleDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> scaleVideoDev_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> scaleVideoCtx_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> scaleEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> scaleVp_;
    UINT32 scaleSrcW_ = 0;
    UINT32 scaleSrcH_ = 0;
    UINT32 scaleDstW_ = 0;
    UINT32 scaleDstH_ = 0;

    int64_t nextSampleTimeHns_ = 0;
    uint64_t lastSeqDelivered_ = 0;
    ULONGLONG lastDeliverTickMs_ = 0;
};

} // namespace zerocam
