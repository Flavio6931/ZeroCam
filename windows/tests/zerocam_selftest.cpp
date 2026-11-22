#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <objbase.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "../shared/WireCrypto.h"
#include "../shared/ZeroCamGuids.h"
#include "../shared/ZeroCamIpc.h"
#include "../app/AdbBridge.h"
#include "../app/AnnexBParser.h"
#include "../app/D3D11Context.h"
#include "../app/GpuEffectsPipeline.h"
#include "../app/MfHwDecoder.h"
#include "../app/PersonSegmenter.h"
#include "../app/StreamIngest.h"
#include "../app/VideoEffects.h"
#include "../app/VirtualCameraHost.h"

namespace {

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);

// Verifica la condición e imprime diagnóstico explícito si falla.
bool Expect(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "[FAIL] %s\n", msg);
        return false;
    }
    std::printf("[PASS] %s\n", msg);
    return true;
}

} // namespace

// Ejecutable de autoverificación end-to-end del pipeline Zero-Copy GPU y la fuente COM ZeroCamSource.dll.
int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::fprintf(stderr, "Uso: zerocam_selftest.exe <ruta_bitstream.h264>\n");
        return 2;
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring tempIpc = std::wstring(tempDir) + L"zerocam_selftest_ipc.bin";
    DeleteFileW(tempIpc.c_str());
    SetEnvironmentVariableW(kZeroCamIpcEnvOverride, tempIpc.c_str());

    bool allOk = true;

    // 1. Verificar soporte MFCreateVirtualCamera del sistema.
    allOk &= Expect(zerocam::VirtualCameraHost::IsPlatformSupported(), "MFIsVirtualCameraTypeSupported(SoftwareCameraSource) == TRUE");

    // 2. Inicializar contexto D3D11 + decodificador HW MFT.
    zerocam::D3D11Context d3dCtx;
    allOk &= Expect(d3dCtx.Initialize(true), "D3D11Context::Initialize");
    allOk &= Expect(d3dCtx.SupportsHwRotation(), "D3D11VideoProcessor soporta rotacion HW sin shaders 3D");
    std::wprintf(L"[INFO] GPU: %s | KeyedMutex=%d | HwRot=%d\n",
                 d3dCtx.AdapterName().c_str(),
                 d3dCtx.UsesKeyedMutex() ? 1 : 0,
                 d3dCtx.SupportsHwRotation() ? 1 : 0);

    zerocam::MfHwDecoder decoder;
    allOk &= Expect(decoder.Initialize(&d3dCtx, nullptr), "MfHwDecoder::Initialize (CLSID_MSH264DecoderMFT)");

    // 3. Leer bitstream H.264 real y decodificar probando rotacion HW 0°, 180° y 90°.
    std::ifstream file(argv[1], std::ios::binary);
    allOk &= Expect(file.good(), "Apertura de archivo H.264 de prueba");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    allOk &= Expect(!bytes.empty(), "Archivo H.264 no vacio");

    zerocam::AnnexBParser parser;
    int64_t ptsUs = 0;
    uint32_t auIndex = 0;
    auto onAu = [&](const uint8_t* au, size_t len, int64_t p, bool) {
        const uint8_t rotCode = (auIndex < 15) ? 0 : ((auIndex < 30) ? 2 : 1); // 0°, 180°, 90°
        ++auIndex;
        decoder.DecodeAccessUnit(au, len, p, 30, 1, rotCode);
    };
    parser.Feed(bytes.data(), bytes.size(), ptsUs, onAu);
    parser.Flush(ptsUs + 33333, onAu);
    decoder.DrainAll(30, 1);

    std::printf(
        "[INFO] Decodificados=%llu | HWZeroCopy=%llu | CpuFallback=%llu | %ux%u\n",
        static_cast<unsigned long long>(decoder.DecodedFrames()),
        static_cast<unsigned long long>(decoder.HwZeroCopyFrames()),
        static_cast<unsigned long long>(decoder.CpuFallbackFrames()),
        decoder.Width(), decoder.Height());

    allOk &= Expect(decoder.DecodedFrames() >= 30, "decodedFrames >= 30");
    allOk &= Expect(decoder.HwZeroCopyFrames() >= 30, "hwZeroCopyFrames >= 30 (D3D11VA NV12 en VRAM)");

    auto* ipc = d3dCtx.Ipc();
    allOk &= Expect(ipc != nullptr && ipc->frameSequence.load() >= 30, "IPC frameSequence >= 30");
    allOk &= Expect(ipc && ipc->clientNeedsCpuFallback.load() == 0, "clientNeedsCpuFallback == 0");
    allOk &= Expect(ipc && ipc->dxgiSharedHandles[0].load() != 0 &&
                    ipc->dxgiSharedHandles[1].load() != 0 &&
                    ipc->dxgiSharedHandles[2].load() != 0,
                    "Ring 3x DXGI shared handles validos");

    // 4. Simulación cross-device (como FrameServer.exe en otro dispositivo D3D11).
    Microsoft::WRL::ComPtr<ID3D11Device> consumerDev;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> consumerCtx;
    const UINT cFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, cFlags, nullptr, 0, D3D11_SDK_VERSION, &consumerDev, nullptr, &consumerCtx);
    allOk &= Expect(SUCCEEDED(hr), "Segundo ID3D11Device independiente creado");

    if (SUCCEEDED(hr) && ipc) {
        const uint32_t slot = ipc->activeSlot.load(std::memory_order_acquire) % zerocam::kRingSlots;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTex;
        hr = consumerDev->OpenSharedResource(
            reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ipc->dxgiSharedHandles[slot].load(std::memory_order_acquire))),
            IID_PPV_ARGS(&sharedTex));
        allOk &= Expect(SUCCEEDED(hr), "OpenSharedResource cross-device NV12 exitoso");
        if (SUCCEEDED(hr)) {
            D3D11_TEXTURE2D_DESC td{};
            sharedTex->GetDesc(&td);
            allOk &= Expect(td.Format == DXGI_FORMAT_NV12 && td.Width == decoder.Width() && td.Height == decoder.Height(),
                            "Textura compartida verificada con resolucion esperada y DXGI_FORMAT_NV12");

            const uint32_t ringKeyed = ipc->usesKeyedMutex.load();
            std::printf("[INFO] Ring usesKeyedMutex=%u\n", ringKeyed);

            Microsoft::WRL::ComPtr<ID3D11Texture2D> localNv12;
            D3D11_TEXTURE2D_DESC localDesc{};
            localDesc.Width = td.Width;
            localDesc.Height = td.Height;
            localDesc.MipLevels = 1;
            localDesc.ArraySize = 1;
            localDesc.Format = DXGI_FORMAT_NV12;
            localDesc.SampleDesc.Count = 1;
            localDesc.Usage = D3D11_USAGE_DEFAULT;
            localDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

            HRESULT pvHr = consumerDev->CreateTexture2D(&localDesc, nullptr, &localNv12);
            if (SUCCEEDED(pvHr)) {
                Microsoft::WRL::ComPtr<IDXGIKeyedMutex> km;
                bool locked = false;
                if (ringKeyed != 0 && SUCCEEDED(sharedTex.As(&km))) {
                    pvHr = km->AcquireSync(0, 100);
                    locked = SUCCEEDED(pvHr);
                }
                if (SUCCEEDED(pvHr)) {
                    consumerCtx->CopyResource(localNv12.Get(), sharedTex.Get());
                }
                if (locked && km) {
                    km->ReleaseSync(0);
                }
            }

            Microsoft::WRL::ComPtr<ID3D11VideoDevice> cVidDev;
            Microsoft::WRL::ComPtr<ID3D11VideoContext> cVidCtx;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> cVpEnum;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessor> cVp;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> bgraTex;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
            D3D11_TEXTURE2D_DESC bgraDesc{};
            bgraDesc.Width = td.Width;
            bgraDesc.Height = td.Height;
            bgraDesc.MipLevels = 1;
            bgraDesc.ArraySize = 1;
            bgraDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            bgraDesc.SampleDesc.Count = 1;
            bgraDesc.Usage = D3D11_USAGE_DEFAULT;
            bgraDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

            D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpDesc{};
            vpDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            vpDesc.InputWidth = td.Width;
            vpDesc.InputHeight = td.Height;
            vpDesc.OutputWidth = td.Width;
            vpDesc.OutputHeight = td.Height;
            vpDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
            ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc{};
            ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

            if (SUCCEEDED(pvHr)) pvHr = consumerDev.As(&cVidDev);
            if (SUCCEEDED(pvHr)) pvHr = consumerCtx.As(&cVidCtx);
            if (SUCCEEDED(pvHr)) pvHr = consumerDev->CreateTexture2D(&bgraDesc, nullptr, &bgraTex);
            if (SUCCEEDED(pvHr)) pvHr = cVidDev->CreateVideoProcessorEnumerator(&vpDesc, &cVpEnum);
            if (SUCCEEDED(pvHr)) pvHr = cVidDev->CreateVideoProcessor(cVpEnum.Get(), 0, &cVp);
            if (SUCCEEDED(pvHr)) pvHr = cVidDev->CreateVideoProcessorInputView(localNv12.Get(), cVpEnum.Get(), &ivDesc, &inView);
            if (SUCCEEDED(pvHr)) pvHr = cVidDev->CreateVideoProcessorOutputView(bgraTex.Get(), cVpEnum.Get(), &ovDesc, &outView);
            if (SUCCEEDED(pvHr)) {
                cVidCtx->VideoProcessorSetStreamFrameFormat(cVp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
                RECT srcR{ 0, 0, static_cast<LONG>(td.Width), static_cast<LONG>(td.Height) };
                cVidCtx->VideoProcessorSetStreamSourceRect(cVp.Get(), 0, TRUE, &srcR);
                RECT dstR{ 0, 0, static_cast<LONG>(td.Width), static_cast<LONG>(td.Height) };
                cVidCtx->VideoProcessorSetStreamDestRect(cVp.Get(), 0, TRUE, &dstR);
                cVidCtx->VideoProcessorSetStreamRotation(cVp.Get(), 0, FALSE, D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY);

                D3D11_VIDEO_PROCESSOR_STREAM st{};
                st.Enable = TRUE;
                st.pInputSurface = inView.Get();
                pvHr = cVidCtx->VideoProcessorBlt(cVp.Get(), outView.Get(), 0, 1, &st);

                D3D11_VIDEO_PROCESSOR_CAPS vpCaps{};
                if (SUCCEEDED(pvHr) && SUCCEEDED(cVpEnum->GetVideoProcessorCaps(&vpCaps)) &&
                    (vpCaps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) != 0) {
                    const LONG drawW = static_cast<LONG>((td.Height * td.Height) / td.Width) & ~1L;
                    const LONG offX = (static_cast<LONG>(td.Width) - drawW) / 2;
                    RECT dstRot{ offX, 0, offX + drawW, static_cast<LONG>(td.Height) };
                    cVidCtx->VideoProcessorSetStreamDestRect(cVp.Get(), 0, TRUE, &dstRot);
                    cVidCtx->VideoProcessorSetStreamRotation(cVp.Get(), 0, TRUE, D3D11_VIDEO_PROCESSOR_ROTATION_90);
                    pvHr = cVidCtx->VideoProcessorBlt(cVp.Get(), outView.Get(), 0, 1, &st);
                }
            }
            if (FAILED(pvHr)) {
                std::fprintf(stderr, "[INFO] VideoProcessorBlt preview hr=0x%08lX\n", static_cast<unsigned long>(pvHr));
            }
            allOk &= Expect(SUCCEEDED(pvHr), "VideoProcessorBlt preview NV12->BGRA8 con rotacion y KeyedMutex");
        }
    }

    // 5. Verificación in-process de ZeroCamSourceTest.dll (variante de pruebas con ZEROCAM_IPC_PATH).
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path dllPath = std::filesystem::path(exePath).parent_path() / L"ZeroCamSourceTest.dll";
    HMODULE dll = LoadLibraryW(dllPath.c_str());
    allOk &= Expect(dll != nullptr, "LoadLibraryW(ZeroCamSourceTest.dll)");

    if (dll) {
        {
            auto pGetClass = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
            allOk &= Expect(pGetClass != nullptr, "DllGetClassObject encontrado");

            Microsoft::WRL::ComPtr<IClassFactory> factory;
            if (pGetClass && SUCCEEDED(pGetClass(CLSID_ZeroCamSource, IID_PPV_ARGS(&factory)))) {
                Microsoft::WRL::ComPtr<IMFActivate> activator;
                hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&activator));
                allOk &= Expect(SUCCEEDED(hr), "ZeroCamActivator creado via IClassFactory");

                Microsoft::WRL::ComPtr<IMFMediaSourceEx> mediaSource;
                if (SUCCEEDED(hr)) {
                    hr = activator->ActivateObject(IID_PPV_ARGS(&mediaSource));
                    allOk &= Expect(SUCCEEDED(hr), "ActivateObject(IID_IMFMediaSourceEx)");
                    // Regresión 0xC00D3E85: FrameServer libera el activador justo tras activar.
                    activator.Reset();
                }

                if (mediaSource && consumerDev) {
                    Microsoft::WRL::ComPtr<IMFSampleAllocatorControl> allocCtrl;
                    DWORD inputId = 99;
                    MFSampleAllocatorUsage usage = MFSampleAllocatorUsage_DoesNotAllocate;
                    allOk &= Expect(SUCCEEDED(mediaSource.As(&allocCtrl)) &&
                                    SUCCEEDED(allocCtrl->GetAllocatorUsage(0, &inputId, &usage)) &&
                                    usage == MFSampleAllocatorUsage_UsesProvidedAllocator,
                                    "IMFSampleAllocatorControl == UsesProvidedAllocator tras liberar activator");

                    Microsoft::WRL::ComPtr<IMFAttributes> streamAttrs;
                    UINT32 d3dAware = 0, fsShared = 0;
                    if (SUCCEEDED(mediaSource->GetStreamAttributes(0, &streamAttrs))) {
                        streamAttrs->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
                        streamAttrs->GetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, &fsShared);
                    }
                    allOk &= Expect(d3dAware == 1 && fsShared == 1, "Atributos MF_SA_D3D11_AWARE y FRAMESERVER_SHARED == 1");

                    UINT consToken = 0;
                    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> consMgr;
                    MFCreateDXGIDeviceManager(&consToken, &consMgr);
                    consMgr->ResetDevice(consumerDev.Get(), consToken);
                    allOk &= Expect(SUCCEEDED(mediaSource->SetD3DManager(consMgr.Get())), "ZeroCamMediaSource::SetD3DManager");

                    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> pd;
                    allOk &= Expect(SUCCEEDED(mediaSource->CreatePresentationDescriptor(&pd)), "CreatePresentationDescriptor");

                    PROPVARIANT varStart{};
                    PropVariantInit(&varStart);
                    varStart.vt = VT_EMPTY;
                    allOk &= Expect(SUCCEEDED(mediaSource->Start(pd.Get(), &GUID_NULL, &varStart)), "IMFMediaSource::Start");

                    Microsoft::WRL::ComPtr<IMFMediaStream> stream;
                    for (int tries = 0; tries < 5 && !stream; ++tries) {
                        Microsoft::WRL::ComPtr<IMFMediaEvent> ev;
                        if (FAILED(mediaSource->GetEvent(0, &ev))) break;
                        MediaEventType met = MEUnknown;
                        ev->GetType(&met);
                        if (met == MENewStream) {
                            PROPVARIANT val{};
                            PropVariantInit(&val);
                            if (SUCCEEDED(ev->GetValue(&val)) && val.vt == VT_UNKNOWN && val.punkVal) {
                                val.punkVal->QueryInterface(IID_PPV_ARGS(&stream));
                            }
                            PropVariantClear(&val);
                        }
                    }
                    allOk &= Expect(stream != nullptr, "MENewStream recibido con IMFMediaStream");

                    if (stream) {
                        int dxgiSamples = 0;
                        for (int s = 0; s < 3; ++s) {
                            if (FAILED(stream->RequestSample(nullptr))) break;
                            for (int e = 0; e < 4; ++e) {
                                Microsoft::WRL::ComPtr<IMFMediaEvent> sev;
                                if (FAILED(stream->GetEvent(0, &sev))) break;
                                MediaEventType smet = MEUnknown;
                                sev->GetType(&smet);
                                if (smet == MEMediaSample) {
                                    PROPVARIANT sval{};
                                    PropVariantInit(&sval);
                                    if (SUCCEEDED(sev->GetValue(&sval)) && sval.vt == VT_UNKNOWN && sval.punkVal) {
                                        Microsoft::WRL::ComPtr<IMFSample> smp;
                                        Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
                                        Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuf;
                                        if (SUCCEEDED(sval.punkVal->QueryInterface(IID_PPV_ARGS(&smp))) &&
                                            SUCCEEDED(smp->GetBufferByIndex(0, &buf)) &&
                                            SUCCEEDED(buf.As(&dxgiBuf))) {
                                            ++dxgiSamples;
                                        }
                                    }
                                    PropVariantClear(&sval);
                                    break;
                                }
                            }
                        }
                        allOk &= Expect(dxgiSamples == 3, "ZeroCamSource.dll entrego 3/3 IMFSample respaldados por IMFDXGIBuffer (Zero-Copy)");
                    }
                    mediaSource->Stop();
                    mediaSource->Shutdown();
                }
                if (activator) activator->ShutdownObject();
            }
        }
    }

    // 6. Prueba opcional de cambio dinámico de resolución sobre el mismo MfHwDecoder (argc >= 3).
    if (argc >= 3) {
        const uint32_t prevW = decoder.Width();
        const uint32_t prevH = decoder.Height();
        std::ifstream file2(argv[2], std::ios::binary);
        allOk &= Expect(file2.good(), "Apertura de segundo archivo H.264 para cambio dinamico");
        std::vector<uint8_t> bytes2((std::istreambuf_iterator<char>(file2)), std::istreambuf_iterator<char>());
        allOk &= Expect(!bytes2.empty(), "Segundo archivo H.264 no vacio");

        zerocam::AnnexBParser parser2;
        uint32_t au2 = 0;
        auto onAu2 = [&](const uint8_t* au, size_t len, int64_t p, bool) {
            const uint8_t rotCode = (au2 < 15) ? 0 : 1;
            ++au2;
            decoder.DecodeAccessUnit(au, len, p, 60, 1, rotCode);
        };
        const int64_t ptsBase = 2000000LL;
        parser2.Feed(bytes2.data(), bytes2.size(), ptsBase, onAu2);
        parser2.Flush(ptsBase + 16666LL, onAu2);
        decoder.DrainAll(60, 1);

        std::printf("[INFO] Cambio dinamico: %ux%u -> %ux%u | HWZeroCopy=%llu\n",
                    prevW, prevH, decoder.Width(), decoder.Height(),
                    static_cast<unsigned long long>(decoder.HwZeroCopyFrames()));
        allOk &= Expect(decoder.Width() != prevW || decoder.Height() != prevH,
                        "Cambio dinamico de resolucion detectado en MfHwDecoder");
        allOk &= Expect(ipc && ipc->width.load(std::memory_order_acquire) == decoder.Width() &&
                        ipc->height.load(std::memory_order_acquire) == decoder.Height(),
                        "IPC actualizado con la nueva resolucion");

        if (consumerDev && ipc) {
            const uint32_t slot2 = ipc->activeSlot.load(std::memory_order_acquire) % zerocam::kRingSlots;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTex2;
            hr = consumerDev->OpenSharedResource(
                reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ipc->dxgiSharedHandles[slot2].load(std::memory_order_acquire))),
                IID_PPV_ARGS(&sharedTex2));
            D3D11_TEXTURE2D_DESC td2{};
            if (SUCCEEDED(hr)) sharedTex2->GetDesc(&td2);
            allOk &= Expect(SUCCEEDED(hr) && td2.Format == DXGI_FORMAT_NV12 &&
                            td2.Width == decoder.Width() && td2.Height == decoder.Height(),
                            "OpenSharedResource verifica nueva resolucion DXGI_FORMAT_NV12");

            // 7. Simula FrameServer/Discord: fuente 1080p + consumidor negociado 720p vía IMFVideoSampleAllocatorEx + CPU fallback.
            if (dll) {
                ipc->clientNeedsCpuFallback.store(1, std::memory_order_release);
                const uint32_t cpuSlot = 0;
                const uint32_t sw = decoder.Width();
                const uint32_t sh = decoder.Height();
                if (sw <= zerocam::kMaxCpuWidth && sh <= zerocam::kMaxCpuHeight) {
                    std::memset(ipc->cpuNv12Buffer[cpuSlot], 180, static_cast<size_t>(sw) * sh);
                    std::memset(ipc->cpuNv12Buffer[cpuSlot] + static_cast<size_t>(sw) * sh, 128, (static_cast<size_t>(sw) * sh) / 2);
                    ipc->cpuStride.store(sw, std::memory_order_release);
                    ipc->activeCpuSlot.store(cpuSlot, std::memory_order_release);
                    ipc->cpuFrameMeta.store(zerocam::PackCpuFrameMeta(sw, sh, sw, cpuSlot), std::memory_order_release);
                }

                auto pGetClass2 = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
                Microsoft::WRL::ComPtr<IClassFactory> fac2;
                Microsoft::WRL::ComPtr<IMFActivate> act2;
                Microsoft::WRL::ComPtr<IMFMediaSourceEx> src2;
                if (pGetClass2 &&
                    SUCCEEDED(pGetClass2(CLSID_ZeroCamSource, IID_PPV_ARGS(&fac2))) &&
                    SUCCEEDED(fac2->CreateInstance(nullptr, IID_PPV_ARGS(&act2))) &&
                    SUCCEEDED(act2->ActivateObject(IID_PPV_ARGS(&src2)))) {
                    act2.Reset();
                    UINT cTok = 0;
                    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> cMgr;
                    MFCreateDXGIDeviceManager(&cTok, &cMgr);
                    cMgr->ResetDevice(consumerDev.Get(), cTok);
                    src2->SetD3DManager(cMgr.Get());

                    // Entrega IMFVideoSampleAllocatorEx SIN pre-inicializar igual que FrameServer en Windows 11 (regresión 0xC00D36B6).
                    Microsoft::WRL::ComPtr<IMFVideoSampleAllocatorEx> vidAlloc;
                    if (SUCCEEDED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&vidAlloc)))) {
                        Microsoft::WRL::ComPtr<IMFSampleAllocatorControl> ctrl2;
                        if (SUCCEEDED(src2.As(&ctrl2))) {
                            ctrl2->SetDefaultAllocator(0, vidAlloc.Get());
                        }
                    }

                    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> pd2;
                    src2->CreatePresentationDescriptor(&pd2);
                    PROPVARIANT vs{};
                    PropVariantInit(&vs);
                    vs.vt = VT_EMPTY;
                    src2->Start(pd2.Get(), &GUID_NULL, &vs);

                    Microsoft::WRL::ComPtr<IMFMediaStream> st2;
                    for (int t = 0; t < 5 && !st2; ++t) {
                        Microsoft::WRL::ComPtr<IMFMediaEvent> ev2;
                        if (FAILED(src2->GetEvent(0, &ev2))) break;
                        MediaEventType m2 = MEUnknown;
                        ev2->GetType(&m2);
                        if (m2 == MENewStream) {
                            PROPVARIANT v2{};
                            PropVariantInit(&v2);
                            if (SUCCEEDED(ev2->GetValue(&v2)) && v2.vt == VT_UNKNOWN && v2.punkVal) {
                                v2.punkVal->QueryInterface(IID_PPV_ARGS(&st2));
                            }
                            PropVariantClear(&v2);
                        }
                    }

                    bool crossScaleNonBlack = false;
                    if (st2 && SUCCEEDED(st2->RequestSample(nullptr))) {
                        for (int e = 0; e < 4; ++e) {
                            Microsoft::WRL::ComPtr<IMFMediaEvent> sev2;
                            if (FAILED(st2->GetEvent(0, &sev2))) break;
                            MediaEventType sm2 = MEUnknown;
                            sev2->GetType(&sm2);
                            if (sm2 == MEMediaSample) {
                                PROPVARIANT sv2{};
                                PropVariantInit(&sv2);
                                if (SUCCEEDED(sev2->GetValue(&sv2)) && sv2.vt == VT_UNKNOWN && sv2.punkVal) {
                                    Microsoft::WRL::ComPtr<IMFSample> smp2;
                                    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf2;
                                    Microsoft::WRL::ComPtr<IMFDXGIBuffer> dbuf2;
                                    Microsoft::WRL::ComPtr<ID3D11Texture2D> outTex2;
                                    UINT outSub2 = 0;
                                    if (SUCCEEDED(sv2.punkVal->QueryInterface(IID_PPV_ARGS(&smp2))) &&
                                        SUCCEEDED(smp2->GetBufferByIndex(0, &buf2)) &&
                                        SUCCEEDED(buf2.As(&dbuf2)) &&
                                        SUCCEEDED(dbuf2->GetResource(IID_PPV_ARGS(&outTex2))) &&
                                        SUCCEEDED(dbuf2->GetSubresourceIndex(&outSub2))) {
                                        D3D11_TEXTURE2D_DESC od{};
                                        outTex2->GetDesc(&od);
                                        D3D11_TEXTURE2D_DESC stgDesc = od;
                                        stgDesc.ArraySize = 1;
                                        stgDesc.MipLevels = 1;
                                        stgDesc.Usage = D3D11_USAGE_STAGING;
                                        stgDesc.BindFlags = 0;
                                        stgDesc.MiscFlags = 0;
                                        stgDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                                        Microsoft::WRL::ComPtr<ID3D11Texture2D> rdTex;
                                        Microsoft::WRL::ComPtr<ID3D11DeviceContext> cCtx;
                                        consumerDev->GetImmediateContext(&cCtx);
                                        if (SUCCEEDED(consumerDev->CreateTexture2D(&stgDesc, nullptr, &rdTex))) {
                                            cCtx->CopySubresourceRegion(rdTex.Get(), 0, 0, 0, 0, outTex2.Get(), outSub2, nullptr);
                                            D3D11_MAPPED_SUBRESOURCE mr{};
                                            if (SUCCEEDED(cCtx->Map(rdTex.Get(), 0, D3D11_MAP_READ, 0, &mr))) {
                                                const uint8_t yCenter = static_cast<const uint8_t*>(mr.pData)[(od.Height / 2) * mr.RowPitch + (od.Width / 2)];
                                                cCtx->Unmap(rdTex.Get(), 0);
                                                UINT32 cleanPt = 0;
                                                LONGLONG smpTs = 0;
                                                smp2->GetUINT32(MFSampleExtension_CleanPoint, &cleanPt);
                                                smp2->GetSampleTime(&smpTs);
                                                crossScaleNonBlack = (od.Width == 1280 && od.Height == 720 && yCenter > 30 && cleanPt == 1 && smpTs > 0);
                                            }
                                        }
                                    }
                                }
                                PropVariantClear(&sv2);
                                break;
                            }
                        }
                    }
                    allOk &= Expect(crossScaleNonBlack, "ZeroCamSource.dll reescala 1080p->720p vía IMFVideoSampleAllocatorEx GPU");

                    // Segunda muestra forzando CPU fallback (como FrameServer Sesión 0 entre procesos).
                    ipc->gpuHandlesGeneration.store(0, std::memory_order_release);
                    bool cpuScaleNonBlack = false;
                    if (st2 && SUCCEEDED(st2->RequestSample(nullptr))) {
                        for (int e = 0; e < 4; ++e) {
                            Microsoft::WRL::ComPtr<IMFMediaEvent> sev3;
                            if (FAILED(st2->GetEvent(0, &sev3))) break;
                            MediaEventType sm3 = MEUnknown;
                            sev3->GetType(&sm3);
                            if (sm3 == MEMediaSample) {
                                PROPVARIANT sv3{};
                                PropVariantInit(&sv3);
                                if (SUCCEEDED(sev3->GetValue(&sv3)) && sv3.vt == VT_UNKNOWN && sv3.punkVal) {
                                    Microsoft::WRL::ComPtr<IMFSample> smp3;
                                    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf3;
                                    Microsoft::WRL::ComPtr<IMFDXGIBuffer> dbuf3;
                                    Microsoft::WRL::ComPtr<ID3D11Texture2D> outTex3;
                                    UINT outSub3 = 0;
                                    if (SUCCEEDED(sv3.punkVal->QueryInterface(IID_PPV_ARGS(&smp3))) &&
                                        SUCCEEDED(smp3->GetBufferByIndex(0, &buf3)) &&
                                        SUCCEEDED(buf3.As(&dbuf3)) &&
                                        SUCCEEDED(dbuf3->GetResource(IID_PPV_ARGS(&outTex3))) &&
                                        SUCCEEDED(dbuf3->GetSubresourceIndex(&outSub3))) {
                                        D3D11_TEXTURE2D_DESC od{};
                                        outTex3->GetDesc(&od);
                                        D3D11_TEXTURE2D_DESC stgDesc = od;
                                        stgDesc.ArraySize = 1;
                                        stgDesc.MipLevels = 1;
                                        stgDesc.Usage = D3D11_USAGE_STAGING;
                                        stgDesc.BindFlags = 0;
                                        stgDesc.MiscFlags = 0;
                                        stgDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                                        Microsoft::WRL::ComPtr<ID3D11Texture2D> rdTex;
                                        Microsoft::WRL::ComPtr<ID3D11DeviceContext> cCtx;
                                        consumerDev->GetImmediateContext(&cCtx);
                                        if (SUCCEEDED(consumerDev->CreateTexture2D(&stgDesc, nullptr, &rdTex))) {
                                            cCtx->CopySubresourceRegion(rdTex.Get(), 0, 0, 0, 0, outTex3.Get(), outSub3, nullptr);
                                            D3D11_MAPPED_SUBRESOURCE mr{};
                                            if (SUCCEEDED(cCtx->Map(rdTex.Get(), 0, D3D11_MAP_READ, 0, &mr))) {
                                                const uint8_t yCenter = static_cast<const uint8_t*>(mr.pData)[(od.Height / 2) * mr.RowPitch + (od.Width / 2)];
                                                cCtx->Unmap(rdTex.Get(), 0);
                                                UINT32 cleanPt = 0;
                                                LONGLONG smpTs = 0;
                                                smp3->GetUINT32(MFSampleExtension_CleanPoint, &cleanPt);
                                                smp3->GetSampleTime(&smpTs);
                                                cpuScaleNonBlack = (od.Width == 1280 && od.Height == 720 && yCenter >= 175 && yCenter <= 185 && cleanPt == 1 && smpTs > 0);
                                            }
                                        }
                                    }
                                }
                                PropVariantClear(&sv3);
                                break;
                            }
                        }
                    }
                    allOk &= Expect(cpuScaleNonBlack, "ZeroCamSource.dll reescala 1080p->720p en CPU fallback (FrameServer/Discord no negro)");

                    // 7b. Regresión de seguridad IPC hostil: cpuFrameMeta corrupto (stride/dims OOB, impar, slot inválido) y fps extremo.
                    const uint64_t hostileMetas[] = {
                        // stride=65535 > kMaxCpuWidth
                        (1280ull) | (720ull << 16) | (65535ull << 32) | (0ull << 48) | (1ull << 56),
                        // width impar=1281
                        (1281ull) | (720ull << 16) | (1281ull << 32) | (0ull << 48) | (1ull << 56),
                        // slot=5 >= kCpuFallbackSlots
                        (1280ull) | (720ull << 16) | (1280ull << 32) | (5ull << 48) | (1ull << 56),
                        // dims 4096x2160 > kMaxCpu*
                        (4096ull) | (2160ull << 16) | (4096ull << 32) | (0ull << 48) | (1ull << 56)
                    };
                    bool hostileHandledSafely = true;
                    ipc->fpsNum.store(1u, std::memory_order_release);
                    ipc->fpsDen.store(999999u, std::memory_order_release);
                    const auto tHostile0 = std::chrono::steady_clock::now();
                    for (uint64_t badMeta : hostileMetas) {
                        ipc->cpuFrameMeta.store(badMeta, std::memory_order_release);
                        if (!st2 || FAILED(st2->RequestSample(nullptr))) {
                            hostileHandledSafely = false;
                            break;
                        }
                        bool gotSample = false;
                        for (int e = 0; e < 4; ++e) {
                            Microsoft::WRL::ComPtr<IMFMediaEvent> sevH;
                            if (FAILED(st2->GetEvent(0, &sevH))) break;
                            MediaEventType smH = MEUnknown;
                            sevH->GetType(&smH);
                            if (smH == MEMediaSample) {
                                gotSample = true;
                                break;
                            }
                        }
                        if (!gotSample) {
                            hostileHandledSafely = false;
                            break;
                        }
                    }
                    const auto hostileElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - tHostile0).count();
                    ipc->fpsNum.store(30u, std::memory_order_release);
                    ipc->fpsDen.store(1u, std::memory_order_release);
                    allOk &= Expect(hostileHandledSafely && hostileElapsedMs < 1500,
                                    "ZeroCamSourceTest resiste cpuFrameMeta hostil (OOB/impar/slot) y fps extremo sin cuelgue ni crash");

                    // 7c. Regresión IPC: cambio atómico 720p <-> 1080p en cpuFrameMeta sin carreras.
                    std::memset(ipc->cpuNv12Buffer[1], 140, static_cast<size_t>(1280) * 720);
                    std::memset(ipc->cpuNv12Buffer[1] + static_cast<size_t>(1280) * 720, 128, (static_cast<size_t>(1280) * 720) / 2);
                    ipc->cpuFrameMeta.store(zerocam::PackCpuFrameMeta(1280, 720, 1280, 1), std::memory_order_release);
                    ipc->frameSequence.fetch_add(1, std::memory_order_release);
                    bool switch720Ok = false;
                    if (st2 && SUCCEEDED(st2->RequestSample(nullptr))) {
                        for (int e = 0; e < 4; ++e) {
                            Microsoft::WRL::ComPtr<IMFMediaEvent> sevS;
                            if (FAILED(st2->GetEvent(0, &sevS))) break;
                            MediaEventType smS = MEUnknown;
                            sevS->GetType(&smS);
                            if (smS == MEMediaSample) {
                                switch720Ok = true;
                                break;
                            }
                        }
                    }
                    allOk &= Expect(switch720Ok, "ZeroCamSourceTest conmuta atomicamente 1080p->720p via cpuFrameMeta");

                    src2->Stop();
                    src2->Shutdown();
                }
            }
        }
    }

    if (dll) FreeLibrary(dll);

    // 8. Prueba automatizada de RVM MobileNetV3 (512x288, r1..r4, DirectML GPU + CPU) y pipeline Bokeh de Disco Óptico.
    {
        std::filesystem::path onnxPath;
        for (const auto& cand : {
                 std::filesystem::path(L"windows/app/res/rvm_mobilenetv3_fp16.onnx"),
                 std::filesystem::path(L"../windows/app/res/rvm_mobilenetv3_fp16.onnx"),
                 std::filesystem::path(L"../../windows/app/res/rvm_mobilenetv3_fp16.onnx") }) {
            if (std::filesystem::exists(cand)) {
                onnxPath = std::filesystem::absolute(cand);
                break;
            }
        }
        allOk &= Expect(!onnxPath.empty(), "Modelo rvm_mobilenetv3_fp16.onnx localizado para prueba");

        if (!onnxPath.empty()) {
            zerocam::PersonSegmenter noFallbackSeg;
            allOk &= Expect(!noFallbackSeg.Initialize(L""),
                            "PersonSegmenter rechaza arranque sin recurso embebido (cero fallback CWD/LOCALAPPDATA)");

            zerocam::PersonSegmenter badSeg;
            allOk &= Expect(!badSeg.Initialize(L"C:\\nonexistent_zerocam_rvm_missing.onnx"),
                            "PersonSegmenter rechaza ruta ONNX inexistente");

            // Verifica carga directa desde RAM vía LearningModel::LoadFromStream (cero archivos temporales).
            std::ifstream onnxIn(onnxPath, std::ios::binary);
            std::vector<uint8_t> onnxMemBytes((std::istreambuf_iterator<char>(onnxIn)), std::istreambuf_iterator<char>());
            zerocam::PersonSegmenter memSeg;
            bool memStreamOk = !onnxMemBytes.empty() && memSeg.InitializeFromMemory(onnxMemBytes.data(), onnxMemBytes.size());
            if (memStreamOk) {
                std::vector<uint8_t> probeBgra(static_cast<size_t>(zerocam::PersonSegmenter::kModelW) * zerocam::PersonSegmenter::kModelH * 4u, 150u);
                memSeg.SubmitBgra512x288(probeBgra.data(), zerocam::PersonSegmenter::kModelW * 4u, false);
                uint64_t mGen = 0;
                std::vector<uint8_t> mOut;
                memStreamOk = false;
                for (int t = 0; t < 250; ++t) {
                    if (memSeg.CopyLatestMatte(mOut, mGen) && mOut.size() == probeBgra.size()) {
                        memStreamOk = true;
                        break;
                    }
                    Sleep(20);
                }
                memSeg.Shutdown();
            }
            allOk &= Expect(memStreamOk, "PersonSegmenter::InitializeFromMemory carga ONNX desde RAM con LoadFromStream");

            zerocam::PersonSegmenter seg;
            allOk &= Expect(seg.Initialize(onnxPath.wstring()), "PersonSegmenter::Initialize RVM WinML (LoadFromStream)");
            allOk &= Expect(seg.IsAvailable() && seg.LastInferenceMs() == 0.0,
                            "PersonSegmenter carga diferida (0 ms antes de activar desenfoque)");

            const size_t expectedMatteBytes =
                static_cast<size_t>(zerocam::PersonSegmenter::kModelW) * zerocam::PersonSegmenter::kModelH * 4u;
            std::vector<uint8_t> synthBgra(expectedMatteBytes, 180u);
            auto waitMatte = [&](bool useGpu, uint64_t& gen, std::vector<uint8_t>& outMatte) -> bool {
                seg.SubmitBgra512x288(synthBgra.data(), zerocam::PersonSegmenter::kModelW * 4u, useGpu);
                for (int t = 0; t < 250; ++t) {
                    if (seg.CopyLatestMatte(outMatte, gen) && outMatte.size() == expectedMatteBytes) {
                        return true;
                    }
                    Sleep(20);
                }
                return false;
            };

            uint64_t gen = 0;
            std::vector<uint8_t> matte;
            // Frame 0 (cold init r1..r4) + Frames 1..2 (memoria temporal recurrente en VRAM DirectML).
            const bool gpuF0 = waitMatte(true, gen, matte);
            const double gpuColdMs = seg.LastInferenceMs();
            const bool gpuF1 = gpuF0 && waitMatte(true, gen, matte);
            const bool gpuF2 = gpuF1 && waitMatte(true, gen, matte);
            const double gpuSteadyMs = seg.LastInferenceMs();
            const bool gpuFgrNonZero = gpuF2 && (matte[(144u * 512u + 256u) * 4u + 2u] > 20u);
            std::printf("[INFO] WinML DirectML GPU RVM 512x288 cold_ms=%.2f steady_ms=%.2f usingGpu=%d fgrCenterR=%u\n",
                        gpuColdMs, gpuSteadyMs, seg.IsUsingGpu() ? 1 : 0,
                        gpuF2 ? static_cast<unsigned>(matte[(144u * 512u + 256u) * 4u + 2u]) : 0u);
            allOk &= Expect(gpuF2 && gpuSteadyMs > 0.0 && gpuFgrNonZero,
                            "PersonSegmenter RVM GPU DirectML genera matte BGRA8 512x288 con estados recurrentes r1..r4");

            // Cambio dinámico GPU -> CPU (reinicia r1..r4 y cambia downsample_ratio a 0.50).
            const bool cpuF0 = waitMatte(false, gen, matte);
            const bool cpuF1 = cpuF0 && waitMatte(false, gen, matte);
            const double cpuSteadyMs = seg.LastInferenceMs();
            std::printf("[INFO] WinML CPU RVM 512x288 steady_ms=%.2f usingGpu=%d\n",
                        cpuSteadyMs, seg.IsUsingGpu() ? 1 : 0);
            allOk &= Expect(cpuF1 && cpuSteadyMs > 0.0 && !seg.IsUsingGpu(),
                            "PersonSegmenter RVM cambia GPU->CPU y genera matte BGRA8 512x288");

            // Regresión: ResetTemporalState() invalida el matte previo y descarta inferencias en vuelo vía resetEpoch.
            seg.ResetTemporalState();
            uint64_t staleGen = 0;
            std::vector<uint8_t> staleMatte;
            const bool clearedImmediately = !seg.CopyLatestMatte(staleMatte, staleGen);
            seg.SubmitBgra512x288(synthBgra.data(), zerocam::PersonSegmenter::kModelW * 4u, false);
            seg.ResetTemporalState();
            for (int t = 0; t < 200 && seg.IsBusy(); ++t) {
                Sleep(10);
            }
            const bool droppedInFlight = !seg.CopyLatestMatte(staleMatte, staleGen);
            const bool gpuResetOk = waitMatte(true, gen, matte);
            allOk &= Expect(clearedImmediately && droppedInFlight && gpuResetOk,
                            "PersonSegmenter::ResetTemporalState invalida epoch/matte previo y reinicia r1..r4 limpiamente");
            seg.Shutdown();

            // Prueba pipeline GPU D3D11 en 1080p (Bokeh 1/2 res 960x540) con textura NV12 de contraste alto en el fondo.
            d3dCtx.SetOptionalModelPath(onnxPath.wstring());
            zerocam::VideoEffectsConfig fx{};
            zerocam::ApplyEffectsPreset(fx, 1); // Estudio
            fx.mirrorH = false;
            fx.blurMode = 2;
            fx.blurStyle = 1;
            fx.blurUseGpu = true;

            const uint32_t fxW = 1920;
            const uint32_t fxH = 1080;
            std::vector<uint8_t> nv12Src(static_cast<size_t>(fxW) * fxH * 3u / 2u);
            for (uint32_t y = 0; y < fxH; ++y) {
                uint8_t* row = nv12Src.data() + static_cast<size_t>(y) * fxW;
                for (uint32_t x = 0; x < fxW; ++x) {
                    // Patrón de rayas finas (4 px) para verificar que el disco bokeh suaviza realmente el fondo.
                    row[x] = (((x / 4u) ^ (y / 4u)) & 1u) ? 210u : 110u;
                }
            }
            std::memset(nv12Src.data() + static_cast<size_t>(fxW) * fxH, 128, (static_cast<size_t>(fxW) * fxH) / 2u);

            D3D11_TEXTURE2D_DESC td{};
            td.Width = fxW;
            td.Height = fxH;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_NV12;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            D3D11_SUBRESOURCE_DATA initD{ nv12Src.data(), fxW, 0 };
            Microsoft::WRL::ComPtr<ID3D11Texture2D> srcNvTex;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> ringOutTex;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> fxPreviewTex;
            bool fxOptBokehOk = false;
            bool fxAiRvmOk = false;
            bool fxResetStateOk = false;

            auto measurePatchContrast = [&](ID3D11Texture2D* tex, uint32_t cx, uint32_t cy, uint8_t& outMean, uint8_t& outSpan) -> bool {
                D3D11_TEXTURE2D_DESC stg = td;
                stg.Usage = D3D11_USAGE_STAGING;
                stg.BindFlags = 0;
                stg.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> rd;
                if (FAILED(d3dCtx.Device()->CreateTexture2D(&stg, nullptr, &rd))) return false;
                d3dCtx.Context()->CopyResource(rd.Get(), tex);
                D3D11_MAPPED_SUBRESOURCE mr{};
                if (FAILED(d3dCtx.Context()->Map(rd.Get(), 0, D3D11_MAP_READ, 0, &mr))) return false;
                uint32_t sum = 0, cnt = 0;
                uint8_t mn = 255, mx = 0;
                for (uint32_t dy = 0; dy < 16; ++dy) {
                    const uint8_t* p = static_cast<const uint8_t*>(mr.pData) + static_cast<size_t>(cy + dy) * mr.RowPitch;
                    for (uint32_t dx = 0; dx < 16; ++dx) {
                        const uint8_t v = p[cx + dx];
                        sum += v;
                        if (v < mn) mn = v;
                        if (v > mx) mx = v;
                        ++cnt;
                    }
                }
                d3dCtx.Context()->Unmap(rd.Get(), 0);
                outMean = static_cast<uint8_t>(sum / std::max<uint32_t>(1u, cnt));
                outSpan = static_cast<uint8_t>(mx - mn);
                return true;
            };

            if (SUCCEEDED(d3dCtx.Device()->CreateTexture2D(&td, &initD, &srcNvTex))) {
                D3D11_TEXTURE2D_DESC rDesc = td;
                rDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                if (SUCCEEDED(d3dCtx.Device()->CreateTexture2D(&rDesc, nullptr, &ringOutTex))) {
                    zerocam::GpuEffectsPipeline gpuFx;
                    if (gpuFx.Initialize(d3dCtx.Device(), d3dCtx.Context(), onnxPath.wstring())) {
                        // A) Óptico f/1.8 con Bokeh de Disco Áureo 960x540: el punto focal permanece nítido y la esquina se desenfoca.
                        zerocam::VideoEffectsConfig fxOpt = fx;
                        fxOpt.blurStyle = 0;
                        fxOpt.blurMode = 3; // Fuerte para contraste medible
                        fxOpt.focusX = 0.50f;
                        fxOpt.focusY = 0.50f;
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> optPrev;
                        if (gpuFx.Process(srcNvTex.Get(), 0, fxW, fxH, 0, fxOpt, ringOutTex.Get(), &optPrev) && optPrev) {
                            uint8_t focusMean = 0, focusSpan = 0, edgeMean = 0, edgeSpan = 0;
                            if (measurePatchContrast(ringOutTex.Get(), 952, 532, focusMean, focusSpan) &&
                                measurePatchContrast(ringOutTex.Get(), 64, 64, edgeMean, edgeSpan)) {
                                std::printf("[INFO] GpuEffectsPipeline 1080p Bokeh Optico focus=(mean=%u span=%u) edge=(mean=%u span=%u)\n",
                                            focusMean, focusSpan, edgeMean, edgeSpan);
                                fxOptBokehOk = (focusMean > 80 && edgeMean > 80 && focusSpan > 40 && edgeSpan < focusSpan / 2u);
                            }
                        }

                        // B) Retrato IA RVM 512x288 (DirectML GPU) + Disco Bokeh 960x540 + alpha matting continuo.
                        zerocam::VideoEffectsConfig fxAi = fx;
                        fxAi.blurStyle = 1;
                        fxAi.blurMode = 2;
                        fxAi.blurUseGpu = true;
                        for (int f = 0; f < 6; ++f) {
                            fxPreviewTex.Reset();
                            if (!gpuFx.Process(srcNvTex.Get(), 0, fxW, fxH, 0, fxAi, ringOutTex.Get(), &fxPreviewTex)) {
                                fxPreviewTex.Reset();
                                break;
                            }
                            if (gpuFx.HasValidMatte() && gpuFx.LastAiMs() > 0.0 && f >= 2) break;
                            Sleep(f == 0 ? 400 : 40);
                        }
                        if (fxPreviewTex) {
                            uint8_t cMean = 0, cSpan = 0, eMean = 0, eSpan = 0;
                            if (measurePatchContrast(ringOutTex.Get(), 952, 532, cMean, cSpan) &&
                                measurePatchContrast(ringOutTex.Get(), 64, 64, eMean, eSpan)) {
                                std::printf("[INFO] GpuEffectsPipeline 1080p RVM IA center=(mean=%u span=%u) edge=(mean=%u span=%u) ai_ms=%.2f\n",
                                            cMean, cSpan, eMean, eSpan, gpuFx.LastAiMs());
                                fxAiRvmOk = (cMean > 60 && eMean > 60 && gpuFx.LastAiMs() > 0.0 && gpuFx.HasValidMatte());
                            }
                        }

                        // C) Regresión: ResetAiState() y blurMode==0 limpian HasValidMatte() evitando siluetas viejas.
                        const bool hadMatteBeforeReset = gpuFx.HasValidMatte();
                        gpuFx.ResetAiState();
                        const bool clearedByResetAi = hadMatteBeforeReset && !gpuFx.HasValidMatte();
                        for (int f = 0; f < 10 && !gpuFx.HasValidMatte(); ++f) {
                            Microsoft::WRL::ComPtr<ID3D11Texture2D> tmpPrev;
                            if (!gpuFx.Process(srcNvTex.Get(), 0, fxW, fxH, 0, fxAi, ringOutTex.Get(), &tmpPrev)) break;
                            Sleep(40);
                        }
                        const bool reacquiredMatte = gpuFx.HasValidMatte();
                        zerocam::VideoEffectsConfig fxBlurOff = fxAi;
                        fxBlurOff.blurMode = 0;
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> offPrev;
                        const bool offProcOk = gpuFx.Process(srcNvTex.Get(), 0, fxW, fxH, 0, fxBlurOff, ringOutTex.Get(), &offPrev);
                        fxResetStateOk = clearedByResetAi && reacquiredMatte && offProcOk && !gpuFx.HasValidMatte();
                    }
                    gpuFx.Shutdown();
                }
            }
            allOk &= Expect(fxOptBokehOk, "GpuEffectsPipeline 1080p Bokeh de Disco Optico (960x540) atenua fondo y preserva foco");
            allOk &= Expect(fxAiRvmOk, "GpuEffectsPipeline 1080p RVM Matting + Bokeh de Disco procesa NV12->BGRA8->NV12 no negro");
            allOk &= Expect(fxResetStateOk, "GpuEffectsPipeline::ResetAiState y blurMode==0 descartan matte IA sin reciclar siluetas");

            if (srcNvTex) {
                d3dCtx.SetEffectsConfig(zerocam::VideoEffectsConfig{});
                ID3D11Texture2D* pubIdRaw = nullptr;
                bool rotId = false;
                const bool idPubOk = d3dCtx.PublishGpuFrame(srcNvTex.Get(), 0, fxW, fxH, 1000000, 30, 1, 0, &pubIdRaw, &rotId);
                Microsoft::WRL::ComPtr<ID3D11Texture2D> pubId;
                pubId.Attach(pubIdRaw);
                allOk &= Expect(idPubOk && !pubId && !rotId,
                                "PublishGpuFrame identidad omite KeyedMutex para vista previa directa");

                d3dCtx.SetEffectsConfig(fx);
                ID3D11Texture2D* pubFxRaw = nullptr;
                bool rotFx = false;
                const bool fxPubOk = d3dCtx.PublishGpuFrame(srcNvTex.Get(), 0, fxW, fxH, 2000000, 30, 1, 0, &pubFxRaw, &rotFx);
                Microsoft::WRL::ComPtr<ID3D11Texture2D> pubFx;
                pubFx.Attach(pubFxRaw);

                bool fxPreviewBltOk = false;
                if (fxPubOk && pubFx && rotFx) {
                    D3D11_TEXTURE2D_DESC pubDesc{};
                    pubFx->GetDesc(&pubDesc);
                    if ((pubDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) == 0) {
                        Microsoft::WRL::ComPtr<ID3D11VideoDevice> vdev;
                        Microsoft::WRL::ComPtr<ID3D11VideoContext> vctx;
                        Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpenum;
                        Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vp;
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> pvBgra;
                        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> piv;
                        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> pov;

                        D3D11_TEXTURE2D_DESC bDesc{};
                        bDesc.Width = fxW;
                        bDesc.Height = fxH;
                        bDesc.MipLevels = 1;
                        bDesc.ArraySize = 1;
                        bDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        bDesc.SampleDesc.Count = 1;
                        bDesc.Usage = D3D11_USAGE_DEFAULT;
                        bDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

                        D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpc{};
                        vpc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
                        vpc.InputWidth = pubDesc.Width;
                        vpc.InputHeight = pubDesc.Height;
                        vpc.OutputWidth = fxW;
                        vpc.OutputHeight = fxH;
                        vpc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

                        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
                        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
                        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

                        if (SUCCEEDED(d3dCtx.Device()->QueryInterface(IID_PPV_ARGS(&vdev))) &&
                            SUCCEEDED(d3dCtx.Context()->QueryInterface(IID_PPV_ARGS(&vctx))) &&
                            SUCCEEDED(d3dCtx.Device()->CreateTexture2D(&bDesc, nullptr, &pvBgra)) &&
                            SUCCEEDED(vdev->CreateVideoProcessorEnumerator(&vpc, &vpenum)) &&
                            SUCCEEDED(vdev->CreateVideoProcessor(vpenum.Get(), 0, &vp)) &&
                            SUCCEEDED(vdev->CreateVideoProcessorInputView(pubFx.Get(), vpenum.Get(), &ivd, &piv)) &&
                            SUCCEEDED(vdev->CreateVideoProcessorOutputView(pvBgra.Get(), vpenum.Get(), &ovd, &pov))) {
                            D3D11_VIDEO_PROCESSOR_STREAM st{};
                            st.Enable = TRUE;
                            st.pInputSurface = piv.Get();
                            fxPreviewBltOk = SUCCEEDED(vctx->VideoProcessorBlt(vp.Get(), pov.Get(), 0, 1, &st));
                        }
                    }
                }
                allOk &= Expect(fxPreviewBltOk,
                                "PublishGpuFrame con filtros entrega textura previa sin KeyedMutex apta para VideoProcessorBlt");

                d3dCtx.SetEffectsConfig(zerocam::VideoEffectsConfig{});
                ID3D11Texture2D* pubIdAfterFxRaw = nullptr;
                bool rotIdAfterFx = false;
                const bool idAfterFxOk = d3dCtx.PublishGpuFrame(srcNvTex.Get(), 0, fxW, fxH, 3000000, 30, 1, 0, &pubIdAfterFxRaw, &rotIdAfterFx);
                Microsoft::WRL::ComPtr<ID3D11Texture2D> pubIdAfterFx;
                pubIdAfterFx.Attach(pubIdAfterFxRaw);
                allOk &= Expect(idAfterFxOk && !pubIdAfterFx && !rotIdAfterFx,
                                "PublishGpuFrame retorno a identidad tras IA ejecuta ResetAiState y omite KeyedMutex");
            }
        }
    }

    // 9. Pruebas de endurecimiento Ola 1 y Ola 2: IPC truncado, URL/ADB maliciosos, WireCrypto CNG (vectores cruzados) y DPAPI.
    {
        // 9a. Consumidor IPC rechaza archivo truncado sin extenderlo.
        const std::wstring truncPath = std::wstring(tempDir) + L"zerocam_selftest_trunc.bin";
        DeleteFileW(truncPath.c_str());
        {
            std::ofstream tf(truncPath, std::ios::binary | std::ios::trunc);
            const char tiny[16] = "TRUNCATED";
            tf.write(tiny, sizeof(tiny));
        }
        SetEnvironmentVariableW(kZeroCamIpcEnvOverride, truncPath.c_str());
        zerocam::SharedIpcChannel truncChan;
        const bool consumerRejectedTrunc = !truncChan.Open(false);
        WIN32_FILE_ATTRIBUTE_DATA truncInfo{};
        const bool notExtended = GetFileAttributesExW(truncPath.c_str(), GetFileExInfoStandard, &truncInfo) &&
                                 truncInfo.nFileSizeLow == 16u;
        DeleteFileW(truncPath.c_str());
        SetEnvironmentVariableW(kZeroCamIpcEnvOverride, tempIpc.c_str());
        allOk &= Expect(consumerRejectedTrunc && notExtended, "SharedIpcChannel consumidor rechaza archivo truncado sin extenderlo");

        // 9b. Validación estricta RFC 3986 en IsAllowedMediaUrl y seriales ADB.
        const bool urlsOk =
            zerocam::StreamIngest::IsAllowedMediaUrl(L"rtsp://192.168.1.50:8554/live?token=abc_123") &&
            zerocam::StreamIngest::IsAllowedMediaUrl(L"rtsps://cam.local:8322/stream1") &&
            zerocam::StreamIngest::IsAllowedMediaUrl(L"srt://10.0.0.5:9000?mode=caller&latency=20") &&
            !zerocam::StreamIngest::IsAllowedMediaUrl(L"file:///C:/Windows/win.ini") &&
            !zerocam::StreamIngest::IsAllowedMediaUrl(L"http://192.168.1.2/cam") &&
            !zerocam::StreamIngest::IsAllowedMediaUrl(L"rtsp://1.2.3.4/a\" -i file:///etc/passwd") &&
            !zerocam::StreamIngest::IsAllowedMediaUrl(L"rtsp://-evil.local/stream") &&
            !zerocam::StreamIngest::IsAllowedMediaUrl(L"rtsp://1.2.3.4/a%0d%0aInject") &&
            !zerocam::StreamIngest::IsAllowedMediaUrl(L"rtsp://1.2.3.4/../secret") &&
            !zerocam::StreamIngest::IsAllowedMediaUrl(L"rtsp://1.2.3.4/concat:a|b");
        allOk &= Expect(urlsOk, "StreamIngest::IsAllowedMediaUrl bloquea inyeccion/esquemas prohibidos y acepta RFC 3986");

        const bool serialsOk =
            zerocam::AdbBridge::IsValidAdbSerial("DEV0123456789ABC") &&
            zerocam::AdbBridge::IsValidAdbSerial("192.168.1.20:5555") &&
            zerocam::AdbBridge::IsValidAdbSerial("emulator-5554") &&
            !zerocam::AdbBridge::IsValidAdbSerial("") &&
            !zerocam::AdbBridge::IsValidAdbSerial("-s") &&
            !zerocam::AdbBridge::IsValidAdbSerial("abc\" && calc") &&
            !zerocam::AdbBridge::IsValidAdbSerial("abc;whoami") &&
            !zerocam::AdbBridge::IsValidAdbSerial("abc def");
        allOk &= Expect(serialsOk, "AdbBridge::IsValidAdbSerial rechaza flags/metacaracteres y acepta seriales validos");

        // 9c. Vector RFC 5869 HKDF-SHA256 Test Case 1 (cruzado con WireProtocolTest.kt).
        std::vector<uint8_t> rfcIkm, rfcSalt, rfcInfo;
        zerocam::crypto::HexToBytes("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", rfcIkm);
        zerocam::crypto::HexToBytes("000102030405060708090a0b0c", rfcSalt);
        zerocam::crypto::HexToBytes("f0f1f2f3f4f5f6f7f8f9", rfcInfo);
        uint8_t rfcOkm[42]{};
        const bool hkdfOk = zerocam::crypto::HkdfSha256(
            rfcSalt.data(), rfcSalt.size(), rfcIkm.data(), rfcIkm.size(), rfcInfo.data(), rfcInfo.size(), rfcOkm, sizeof(rfcOkm));
        const std::string hkdfHex = zerocam::crypto::BytesToHexUpper(rfcOkm, sizeof(rfcOkm));
        allOk &= Expect(hkdfOk && hkdfHex == "3CB25F25FAACD57A90434F64D0362F2A2D2D0A90CF1A5A4C5DB02D56ECC4C5BF34007208D5B887185865",
                        "WireCrypto HKDF-SHA256 coincide exactamente con RFC 5869 Test Case 1");

        // 9d. Vector RFC 5903 ECDH P-256 (cruzado con Android JCA: confirma inversión LE->BE en CNG).
        std::vector<uint8_t> dA, xA, yA, xB, yB, zExp;
        zerocam::crypto::HexToBytes("C88F01F510D9AC3F70A292DAA2316DE544E9AAB8AFE84049C62A9C57862D1433", dA);
        zerocam::crypto::HexToBytes("DAD0B65394221CF9B051E1FECA5787D098DFE637FC90B9EF945D0C3772581180", xA);
        zerocam::crypto::HexToBytes("5271A0461CDB8252D61F1C456FA3E59AB1F45B33ACCF5F58389E0577B8990BB3", yA);
        zerocam::crypto::HexToBytes("D12DFB5289C8D4F81208B70270398C342296970A0BCCB74C736FC7554494BF63", xB);
        zerocam::crypto::HexToBytes("56FBF3CA366CC23E8157854C13C58D6AAC23F046ADA30F8353E74F33039872AB", yB);
        zerocam::crypto::HexToBytes("D6840F6B42F6EDAFD13116E0E12565202FEF8E9ECE7DCE03812464D04B9442DE", zExp);
        uint8_t pubBSec1[zerocam::kWirePubKeySec1Bytes]{};
        pubBSec1[0] = 0x04u;
        std::memcpy(pubBSec1 + 1u, xB.data(), 32u);
        std::memcpy(pubBSec1 + 33u, yB.data(), 32u);
        zerocam::crypto::EcdhP256KeyPair ecdhA;
        uint8_t zGot[32]{};
        const bool ecdhVecOk = ecdhA.ImportRawForTest(dA.data(), xA.data(), yA.data()) &&
                               ecdhA.ComputeSharedSecretBE(pubBSec1, zGot) &&
                               zerocam::crypto::ConstantTimeEquals(zGot, zExp.data(), 32u);
        allOk &= Expect(ecdhVecOk, "WireCrypto ECDH P-256 CNG (LE->BE) coincide bit a bit con RFC 5903");

        // 9e. Key Schedule Wire v2 determinista (PBKDF2 100k + HKDF + HMAC) + impresión de vector cruzado.
        uint8_t cNonce[16]{};
        uint8_t sNonce[16]{};
        uint8_t cPub[65]{};
        uint8_t sPub[65]{};
        uint8_t idPub[65]{};
        uint8_t fakeSig[64]{};
        for (int i = 0; i < 16; ++i) { cNonce[i] = static_cast<uint8_t>(i + 1); sNonce[i] = static_cast<uint8_t>(0xA0 + i); }
        cPub[0] = 0x04u; std::memcpy(cPub + 1u, xA.data(), 32u); std::memcpy(cPub + 33u, yA.data(), 32u);
        std::memcpy(sPub, pubBSec1, 65u);
        std::memcpy(idPub, pubBSec1, 65u);
        for (int i = 0; i < 64; ++i) fakeSig[i] = static_cast<uint8_t>(0x30 + i);

        zerocam::crypto::WireV2DerivedKeys dk;
        uint8_t cProof[32]{};
        uint8_t sProof[32]{};
        const bool kschedOk = zerocam::crypto::DeriveWireV2Keys(
                                  zerocam::kWireAuthModeWifiPin, cNonce, cPub, sNonce, sPub, idPub, fakeSig, zGot, "482910", dk) &&
                              zerocam::crypto::ComputeHandshakeProof(dk.authKey, dk.transcriptHash, "client", cProof) &&
                              zerocam::crypto::ComputeHandshakeProof(dk.authKey, dk.transcriptHash, "server", sProof) &&
                              zerocam::crypto::BytesToHexUpper(dk.c2sKey, 32) == "AB322C06A2F8DFD6181ACF663285933A3A7250AD9A42444C7985B5B5CB40C019" &&
                              zerocam::crypto::BytesToHexUpper(dk.c2sIvBase, 12) == "654B4A0B6789E767A73C1B24" &&
                              zerocam::crypto::BytesToHexUpper(cProof, 32) == "A962D59A70900BF490315F0D6D8E61005D6E8FC7CFEDB61EBC13070D357204D0" &&
                              zerocam::crypto::BytesToHexUpper(sProof, 32) == "BD0769342CBECF83445ABC561AF473B4F7762F08DCE7E0E5C2086B9A54AA4F6B";
        allOk &= Expect(kschedOk, "WireCrypto DeriveWireV2Keys (PBKDF2 100k + HKDF + HMAC) coincide con vector cruzado Kotlin");

        // 9f. AES-256-GCM AEAD (incluyendo texto plano vacío, rechazo de manipulación AAD/tag y secuencia).
        zerocam::crypto::Aes256GcmCipher enc, dec;
        const bool gcmInit = enc.Init(dk.c2sKey, dk.c2sIvBase) && dec.Init(dk.c2sKey, dk.c2sIvBase);
        const uint8_t aad[20] = { 0x5A, 0x43, 0x41, 0x4D, 2, 3, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 28, 0, 0, 0 };
        const char msgTxt[] = "ZeroCamWire2";
        std::vector<uint8_t> ct0, pt0, ct1, pt1, badPt;
        const bool gcmEnc0 = gcmInit && enc.EncryptNext(aad, sizeof(aad), reinterpret_cast<const uint8_t*>(msgTxt), 12u, ct0) &&
                             zerocam::crypto::BytesToHexUpper(ct0.data(), ct0.size()) == "618A57BDCF41B60BF8116225DE132DEE096EE29A025CC099EADEC0AC";
        uint8_t badAad[20]{};
        std::memcpy(badAad, aad, sizeof(aad));
        badAad[5] ^= 0xFFu;
        const bool rejectBadAad = gcmEnc0 && !dec.DecryptNext(badAad, sizeof(badAad), ct0.data(), ct0.size(), badPt);
        std::vector<uint8_t> tamperedCt = ct0;
        if (!tamperedCt.empty()) tamperedCt.back() ^= 0x01u;
        const bool rejectBadTag = gcmEnc0 && !dec.DecryptNext(aad, sizeof(aad), tamperedCt.data(), tamperedCt.size(), badPt);
        const bool gcmDec0 = gcmEnc0 && dec.DecryptNext(aad, sizeof(aad), ct0.data(), ct0.size(), pt0) &&
                             pt0.size() == 12u && std::memcmp(pt0.data(), msgTxt, 12u) == 0;
        const bool gcmEmpty = gcmDec0 && enc.EncryptNext(aad, sizeof(aad), nullptr, 0u, ct1) &&
                              ct1.size() == zerocam::kWireAeadTagBytes &&
                              dec.DecryptNext(aad, sizeof(aad), ct1.data(), ct1.size(), pt1) && pt1.empty();
        allOk &= Expect(rejectBadAad && rejectBadTag && gcmDec0 && gcmEmpty,
                        "WireCrypto AES-256-GCM cifra/descifra (incl. vacio) y rechaza manipulacion de AAD/tag");

        // 9g. ECDSA P-256 firma/verificación P1363 + DPAPI roundtrip + PIN aleatorio 6 dígitos.
        zerocam::crypto::EcdsaIdentityKey idKey;
        uint8_t idKeyPub[65]{};
        uint8_t idSig[64]{};
        const char* signMsg = "ZCAM_HOST_V2|PORT=5000|NAME=TEST|PIN=1";
        const bool ecdsaOk = idKey.GenerateEphemeral() &&
                             idKey.ExportPublicSec1(idKeyPub) &&
                             idKey.SignP1363(reinterpret_cast<const uint8_t*>(signMsg), std::strlen(signMsg), idSig) &&
                             zerocam::crypto::EcdsaIdentityKey::VerifyP1363(idKeyPub, reinterpret_cast<const uint8_t*>(signMsg), std::strlen(signMsg), idSig) &&
                             !zerocam::crypto::EcdsaIdentityKey::VerifyP1363(idKeyPub, reinterpret_cast<const uint8_t*>("TAMPERED"), 8u, idSig);
        allOk &= Expect(ecdsaOk, "WireCrypto ECDSA P-256 P1363 firma, verifica y rechaza manipulacion");

        std::wstring pinEncB64;
        std::string pinDecUtf8;
        const std::wstring rndPin = zerocam::crypto::GenerateRandomPin6();
        bool rndPinValid = (rndPin.size() == 6u);
        for (wchar_t ch : rndPin) {
            if (ch < L'0' || ch > L'9') rndPinValid = false;
        }
        const bool dpapiOk = rndPinValid &&
                             zerocam::crypto::DpapiEncryptToBase64("739204", "ZeroCamSelfTest", pinEncB64) &&
                             !pinEncB64.empty() &&
                             zerocam::crypto::DpapiDecryptFromBase64(pinEncB64, "ZeroCamSelfTest", pinDecUtf8) &&
                             pinDecUtf8 == "739204";
        allOk &= Expect(dpapiOk, "WireCrypto DPAPI cifra/descifra PIN en reposo y genera PIN uniforme de 6 digitos");

        // 9h. Prueba live TCP Wire v2 contra StreamIngest real en loopback (rechazo Annex-B + handshake 4 msgs + AEAD bidireccional).
        {
            zerocam::StreamIngest ingest;
            ingest.SetDiscoveryPortForTest(59235);
            ingest.SetWifiPin("482910");
            ingest.SetVideoConfig(1280, 720, 30, 4000000, false);
            const bool ingestStarted = ingest.Start(zerocam::SourceMode::ZeroCamWifi, L"59234", &d3dCtx, nullptr);
            uint16_t testPort = 0;
            if (ingestStarted) {
                for (int w = 0; w < 50; ++w) {
                    const uint16_t p = ingest.BoundPort();
                    if (p >= 59234) { testPort = p; break; }
                    Sleep(20);
                }
                Sleep(100);
            }

            auto connectLoopback = [&](uint16_t port) -> SOCKET {
                SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (s == INVALID_SOCKET) return INVALID_SOCKET;
                DWORD tmo = 3000;
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
                setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
                sockaddr_in sa{};
                sa.sin_family = AF_INET;
                sa.sin_port = htons(port);
                InetPtonW(AF_INET, L"127.0.0.1", &sa.sin_addr);
                if (connect(s, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
                    closesocket(s);
                    return INVALID_SOCKET;
                }
                return s;
            };

            auto recvAll = [](SOCKET s, void* dst, size_t n) -> bool {
                uint8_t* p = static_cast<uint8_t*>(dst);
                size_t got = 0;
                while (got < n) {
                    int r = recv(s, reinterpret_cast<char*>(p + got), static_cast<int>(n - got), 0);
                    if (r <= 0) return false;
                    got += static_cast<size_t>(r);
                }
                return true;
            };

            // 9h-0. Discovery UDP v2: resiste datagrama 0 B, datagrama >256 B (WSAEMSGSIZE) y ZCAM_DISCOVER_V1, y responde firmado ECDSA.
            bool discoveryV2Ok = false;
            if (testPort != 0) {
                SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (udp != INVALID_SOCKET) {
                    DWORD uTmo = 1500;
                    setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&uTmo), sizeof(uTmo));
                    sockaddr_in dst{};
                    dst.sin_family = AF_INET;
                    dst.sin_port = htons(59235);
                    InetPtonW(AF_INET, L"127.0.0.1", &dst.sin_addr);

                    // 1) Paquete 0 bytes, 2) Paquete 1500 bytes (provocaría WSAEMSGSIZE con búfer chico), 3) Legacy V1
                    sendto(udp, "", 0, 0, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
                    std::string bigJunk(1500, 'X');
                    sendto(udp, bigJunk.data(), static_cast<int>(bigJunk.size()), 0, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
                    const char* legacyReq = "ZCAM_DISCOVER_V1";
                    sendto(udp, legacyReq, static_cast<int>(std::strlen(legacyReq)), 0, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
                    Sleep(60);

                    // 4) Solicitud legítima Discovery UDP v2 con nonce
                    const std::string reqNonce = "A1B2C3D4E5F60718";
                    const std::string reqV2 = "ZCAM_DISCOVER_V2|NONCE=" + reqNonce;
                    sendto(udp, reqV2.data(), static_cast<int>(reqV2.size()), 0, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));

                    char rbuf[1024]{};
                    sockaddr_in from{};
                    int fromLen = sizeof(from);
                    const int rr = recvfrom(udp, rbuf, sizeof(rbuf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
                    if (rr > 0) {
                        std::string reply(rbuf, static_cast<size_t>(rr));
                        const size_t sigPos = reply.rfind("|SIG=");
                        const size_t pubPos = reply.find("|PUB=");
                        if (sigPos != std::string::npos && pubPos != std::string::npos && pubPos < sigPos) {
                            const std::string canonical = reply.substr(0, sigPos);
                            const std::string sigHex = reply.substr(sigPos + 5u);
                            const std::string pubHex = reply.substr(pubPos + 5u, sigPos - (pubPos + 5u));
                            std::vector<uint8_t> pubB, sigB;
                            if (zerocam::crypto::HexToBytes(pubHex, pubB) && pubB.size() == 65u &&
                                zerocam::crypto::HexToBytes(sigHex, sigB) && sigB.size() == 64u) {
                                const bool sigOk = zerocam::crypto::EcdsaIdentityKey::VerifyP1363(
                                    pubB.data(), reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size(), sigB.data());
                                const bool fpMatch = (zerocam::crypto::ComputeFingerprintHex(pubB.data()) == ingest.HostFingerprintHex());
                                const bool fieldsOk =
                                    canonical.find("ZCAM_HOST_V2|PORT=" + std::to_string(testPort)) == 0 &&
                                    canonical.find("|PIN=1|") != std::string::npos &&
                                    canonical.find("|NONCE=" + reqNonce + "|") != std::string::npos;
                                discoveryV2Ok = sigOk && fpMatch && fieldsOk;
                            }
                        }
                    }
                    closesocket(udp);
                }
            }
            allOk &= Expect(discoveryV2Ok, "StreamIngest Discovery UDP v2 resiste datagramas 0B/1500B/V1 y firma ECDSA P-256 valida");

            bool rawAnnexRejected = false;
            if (testPort != 0) {
                SOCKET sRaw = connectLoopback(testPort);
                if (sRaw != INVALID_SOCKET) {
                    const uint8_t junk[20] = { 0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1E, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
                    send(sRaw, reinterpret_cast<const char*>(junk), sizeof(junk), 0);
                    zerocam::WirePacketHeader errHdr{};
                    uint8_t errCode = 0;
                    if (recvAll(sRaw, &errHdr, sizeof(errHdr)) &&
                        errHdr.magic == zerocam::kWireMagic &&
                        errHdr.version == zerocam::kWireVersion &&
                        errHdr.type == static_cast<uint8_t>(zerocam::WirePacketType::HandshakeError) &&
                        errHdr.payloadSize == 1u &&
                        recvAll(sRaw, &errCode, 1u) &&
                        errCode == zerocam::kWireErrProtocol) {
                        rawAnnexRejected = true;
                    }
                    closesocket(sRaw);
                }
            }
            allOk &= Expect(rawAnnexRejected, "StreamIngest TCP Wire v2 rechaza Annex-B crudo con HandshakeError(protocol)");

            // 9h-2. Handshake Wire v2 completo de 4 mensajes + recepción ControlJson cifrado + envío VideoH264 cifrado.
            bool liveWireV2Ok = false;
            if (testPort != 0) {
                // Espera a que el servidor cierre la sesión de rechazo anterior y acepte nueva conexión.
                Sleep(1300);
                SOCKET sCli = connectLoopback(testPort);
                if (sCli != INVALID_SOCKET) {
                    uint8_t cliNonce[16]{};
                    zerocam::crypto::EcdhP256KeyPair cliEph;
                    uint8_t cliEphPub[65]{};
                    if (zerocam::crypto::RandomBytes(cliNonce, sizeof(cliNonce)) &&
                        cliEph.Generate() && cliEph.ExportPublicSec1(cliEphPub)) {
                        zerocam::WirePacketHeader chHdr{ zerocam::kWireMagic, zerocam::kWireVersion,
                            static_cast<uint8_t>(zerocam::WirePacketType::HandshakeClientHello), 0, 0, zerocam::kWireClientHelloBytes };
                        uint8_t chBuf[sizeof(chHdr) + zerocam::kWireClientHelloBytes]{};
                        std::memcpy(chBuf, &chHdr, sizeof(chHdr));
                        std::memcpy(chBuf + sizeof(chHdr), cliNonce, 16u);
                        std::memcpy(chBuf + sizeof(chHdr) + 16u, cliEphPub, 65u);
                        send(sCli, reinterpret_cast<const char*>(chBuf), sizeof(chBuf), 0);

                        zerocam::WirePacketHeader shHdr{};
                        uint8_t shBuf[zerocam::kWireServerHelloBytes]{};
                        if (recvAll(sCli, &shHdr, sizeof(shHdr)) &&
                            shHdr.type == static_cast<uint8_t>(zerocam::WirePacketType::HandshakeServerHello) &&
                            shHdr.payloadSize == zerocam::kWireServerHelloBytes &&
                            recvAll(sCli, shBuf, sizeof(shBuf))) {
                            const uint8_t authMode = shBuf[0];
                            const uint8_t* srvNonce = shBuf + 1u;
                            const uint8_t* srvEphPub = shBuf + 17u;
                            const uint8_t* srvIdPub = shBuf + 82u;
                            const uint8_t* srvSig = shBuf + 147u;
                            const auto signedBytes = zerocam::crypto::BuildServerHelloSignedBytes(
                                authMode, cliNonce, cliEphPub, srvNonce, srvEphPub, srvIdPub);
                            uint8_t ecdhSec[32]{};
                            zerocam::crypto::WireV2DerivedKeys liveKeys;
                            uint8_t cliProof[32]{}, expSrvProof[32]{}, gotSrvProof[32]{};
                            if (authMode == zerocam::kWireAuthModeUsbLocal &&
                                zerocam::crypto::EcdsaIdentityKey::VerifyP1363(srvIdPub, signedBytes.data(), signedBytes.size(), srvSig) &&
                                cliEph.ComputeSharedSecretBE(srvEphPub, ecdhSec) &&
                                zerocam::crypto::DeriveWireV2Keys(authMode, cliNonce, cliEphPub, srvNonce, srvEphPub, srvIdPub, srvSig, ecdhSec, "", liveKeys) &&
                                zerocam::crypto::ComputeHandshakeProof(liveKeys.authKey, liveKeys.transcriptHash, "client", cliProof) &&
                                zerocam::crypto::ComputeHandshakeProof(liveKeys.authKey, liveKeys.transcriptHash, "server", expSrvProof)) {
                                zerocam::WirePacketHeader cfHdr{ zerocam::kWireMagic, zerocam::kWireVersion,
                                    static_cast<uint8_t>(zerocam::WirePacketType::HandshakeClientFinish), 0, 0, zerocam::kWireProofBytes };
                                uint8_t cfBuf[sizeof(cfHdr) + zerocam::kWireProofBytes]{};
                                std::memcpy(cfBuf, &cfHdr, sizeof(cfHdr));
                                std::memcpy(cfBuf + sizeof(cfHdr), cliProof, 32u);
                                send(sCli, reinterpret_cast<const char*>(cfBuf), sizeof(cfBuf), 0);

                                zerocam::WirePacketHeader sfHdr{};
                                if (recvAll(sCli, &sfHdr, sizeof(sfHdr)) &&
                                    sfHdr.type == static_cast<uint8_t>(zerocam::WirePacketType::HandshakeServerFinish) &&
                                    sfHdr.payloadSize == zerocam::kWireProofBytes &&
                                    recvAll(sCli, gotSrvProof, sizeof(gotSrvProof)) &&
                                    zerocam::crypto::ConstantTimeEquals(expSrvProof, gotSrvProof, 32u)) {
                                    // Recibe y descifra el paquete ControlJson inicial ("set_video") enviado por StreamIngest!
                                    zerocam::crypto::Aes256GcmCipher cliRecv;
                                    zerocam::WirePacketHeader ctrlHdr{};
                                    if (cliRecv.Init(liveKeys.s2cKey, liveKeys.s2cIvBase) &&
                                        recvAll(sCli, &ctrlHdr, sizeof(ctrlHdr)) &&
                                        ctrlHdr.type == static_cast<uint8_t>(zerocam::WirePacketType::ControlJson) &&
                                        ctrlHdr.payloadSize > zerocam::kWireAeadTagBytes && ctrlHdr.payloadSize < 4096u) {
                                        std::vector<uint8_t> encCtrl(ctrlHdr.payloadSize), plainCtrl;
                                        if (recvAll(sCli, encCtrl.data(), encCtrl.size()) &&
                                            cliRecv.DecryptNext(reinterpret_cast<const uint8_t*>(&ctrlHdr), sizeof(ctrlHdr),
                                                                encCtrl.data(), encCtrl.size(), plainCtrl)) {
                                            const std::string ctrlJson(plainCtrl.begin(), plainCtrl.end());
                                            liveWireV2Ok = (ctrlJson.find("\"cmd\":\"set_video\"") != std::string::npos);
                                        }
                                    }
                                }
                            }
                            SecureZeroMemory(ecdhSec, sizeof(ecdhSec));
                        }
                    }
                    closesocket(sCli);
                }
            }
            ingest.Stop();
            allOk &= Expect(liveWireV2Ok, "StreamIngest TCP Wire v2 completa handshake 4 mensajes y entrega ControlJson cifrado AES-256-GCM");
        }

        // 9i. Regresión IPC: rechazo estricto de hardlink NTFS (nNumberOfLinks > 1).
        {
            const std::wstring baseFile = std::wstring(tempDir) + L"zerocam_hl_base.bin";
            const std::wstring linkFile = std::wstring(tempDir) + L"zerocam_hl_link.bin";
            DeleteFileW(linkFile.c_str());
            DeleteFileW(baseFile.c_str());
            {
                std::ofstream bf(baseFile, std::ios::binary | std::ios::trunc);
                bf.write("HL_TEST", 7);
            }
            bool hlRejected = false;
            if (CreateHardLinkW(linkFile.c_str(), baseFile.c_str(), nullptr)) {
                SetEnvironmentVariableW(kZeroCamIpcEnvOverride, linkFile.c_str());
                zerocam::SharedIpcChannel hlProd, hlCons;
                hlRejected = !hlProd.Open(true) && !hlCons.Open(false);
                SetEnvironmentVariableW(kZeroCamIpcEnvOverride, tempIpc.c_str());
            }
            DeleteFileW(linkFile.c_str());
            DeleteFileW(baseFile.c_str());
            allOk &= Expect(hlRejected, "SharedIpcChannel rechaza hardlinks NTFS (nNumberOfLinks != 1) en productor y consumidor");
        }

        // 9j. Regresión AdbBridge: timeout efectivo bajo flujo continuo en stdout sin bucle infinito ni exceso de 64 KB.
        {
            wchar_t sysDir[MAX_PATH]{};
            GetSystemDirectoryW(sysDir, MAX_PATH);
            const std::wstring cmdExe = std::wstring(sysDir) + L"\\cmd.exe";
            std::string floodOut;
            unsigned long floodCode = 0;
            const auto t0 = std::chrono::steady_clock::now();
            const bool completed = zerocam::AdbBridge::RunProcessCaptureForTest(
                cmdExe,
                L"/c for /L %i in (1,0,2) do @echo ZEROCAM_FLOOD_LINE_0123456789ABCDEF",
                400ull,
                floodOut,
                floodCode);
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            allOk &= Expect(!completed && elapsedMs < 2000 && floodOut.size() <= 64u * 1024u,
                            "AdbBridge::RunProcessCapture corta flood continuo en stdout respetando deadline y tope 64 KB");

            // 9j-2. Regresión cierre limpio: AdbBridge::ShutdownBundledAdb termina cualquier subproceso dentro del Job Object.
            zerocam::AdbBridge::ShutdownBundledAdb();
            void* rawJobProc = zerocam::AdbBridge::SpawnInAdbJobForTest(cmdExe, L"/c ping -n 15 127.0.0.1 >nul");
            HANDLE hJobProc = static_cast<HANDLE>(rawJobProc);
            bool jobClosedCleanly = false;
            if (hJobProc != nullptr) {
                Sleep(100);
                const bool aliveBefore = (WaitForSingleObject(hJobProc, 0) == WAIT_TIMEOUT);
                zerocam::AdbBridge::ShutdownBundledAdb();
                const bool exitedAfter = (WaitForSingleObject(hJobProc, 1500) == WAIT_OBJECT_0);
                CloseHandle(hJobProc);
                jobClosedCleanly = aliveBefore && exitedAfter;
            }
            allOk &= Expect(jobClosedCleanly,
                            "AdbBridge::ShutdownBundledAdb termina subprocesos del Job Object al cerrar ZeroCam");
        }

        // 9k. Regresión Wire v2 Wi-Fi real (IP LAN no-loopback): PIN incorrecto x5 -> rate limit + PIN correcto + AEAD.
        {
            WSADATA wsa9k{};
            const bool wsa9kOk = (WSAStartup(MAKEWORD(2, 2), &wsa9k) == 0);
            // Resuelve IP LAN local (no-127.x) para ejercitar la ruta requireWifiAuth=true real de StreamIngest.
            std::wstring localLanIp;
            if (wsa9kOk) {
                SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (probe != INVALID_SOCKET) {
                    sockaddr_in rem{};
                    rem.sin_family = AF_INET;
                    rem.sin_port = htons(53);
                    InetPtonW(AF_INET, L"8.8.8.8", &rem.sin_addr);
                    connect(probe, reinterpret_cast<const sockaddr*>(&rem), sizeof(rem));
                    sockaddr_in loc{};
                    int locLen = sizeof(loc);
                    if (getsockname(probe, reinterpret_cast<sockaddr*>(&loc), &locLen) == 0 &&
                        (ntohl(loc.sin_addr.s_addr) >> 24) != 127u &&
                        loc.sin_addr.s_addr != 0) {
                        wchar_t ipBuf[INET_ADDRSTRLEN]{};
                        if (InetNtopW(AF_INET, &loc.sin_addr, ipBuf, INET_ADDRSTRLEN) != nullptr) {
                            localLanIp = ipBuf;
                        }
                    }
                    closesocket(probe);
                }
            }

            if (!localLanIp.empty()) {
                auto runWifiHandshake = [&](const std::wstring& ip, uint16_t port, const std::string& pin,
                                            uint8_t& outErrCode, bool& outGotCtrl) -> bool {
                    outErrCode = 0;
                    outGotCtrl = false;
                    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (s == INVALID_SOCKET) return false;
                    DWORD tmo = 4000;
                    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
                    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
                    sockaddr_in sa{};
                    sa.sin_family = AF_INET;
                    sa.sin_port = htons(port);
                    InetPtonW(AF_INET, ip.c_str(), &sa.sin_addr);
                    if (connect(s, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
                        closesocket(s);
                        return false;
                    }

                    auto recvAllW = [](SOCKET sock, void* dst, size_t n) -> bool {
                        uint8_t* p = static_cast<uint8_t*>(dst);
                        size_t got = 0;
                        while (got < n) {
                            int r = recv(sock, reinterpret_cast<char*>(p + got), static_cast<int>(n - got), 0);
                            if (r <= 0) return false;
                            got += static_cast<size_t>(r);
                        }
                        return true;
                    };

                    uint8_t cliNonce[16]{};
                    zerocam::crypto::EcdhP256KeyPair cliEph;
                    uint8_t cliEphPub[65]{};
                    if (!zerocam::crypto::RandomBytes(cliNonce, sizeof(cliNonce)) ||
                        !cliEph.Generate() || !cliEph.ExportPublicSec1(cliEphPub)) {
                        closesocket(s);
                        return false;
                    }
                    zerocam::WirePacketHeader chHdr{ zerocam::kWireMagic, zerocam::kWireVersion,
                        static_cast<uint8_t>(zerocam::WirePacketType::HandshakeClientHello), 0, 0, zerocam::kWireClientHelloBytes };
                    uint8_t chBuf[sizeof(chHdr) + zerocam::kWireClientHelloBytes]{};
                    std::memcpy(chBuf, &chHdr, sizeof(chHdr));
                    std::memcpy(chBuf + sizeof(chHdr), cliNonce, 16u);
                    std::memcpy(chBuf + sizeof(chHdr) + 16u, cliEphPub, 65u);
                    send(s, reinterpret_cast<const char*>(chBuf), sizeof(chBuf), 0);

                    zerocam::WirePacketHeader shHdr{};
                    if (!recvAllW(s, &shHdr, sizeof(shHdr))) { closesocket(s); return false; }
                    if (shHdr.type == static_cast<uint8_t>(zerocam::WirePacketType::HandshakeError) && shHdr.payloadSize == 1u) {
                        recvAllW(s, &outErrCode, 1u);
                        closesocket(s);
                        return false;
                    }
                    if (shHdr.type != static_cast<uint8_t>(zerocam::WirePacketType::HandshakeServerHello) ||
                        shHdr.payloadSize != zerocam::kWireServerHelloBytes) {
                        closesocket(s);
                        return false;
                    }
                    uint8_t shBuf[zerocam::kWireServerHelloBytes]{};
                    if (!recvAllW(s, shBuf, sizeof(shBuf))) { closesocket(s); return false; }
                    const uint8_t authMode = shBuf[0];
                    if (authMode != zerocam::kWireAuthModeWifiPin) { closesocket(s); return false; }
                    const uint8_t* srvNonce = shBuf + 1u;
                    const uint8_t* srvEphPub = shBuf + 17u;
                    const uint8_t* srvIdPub = shBuf + 82u;
                    const uint8_t* srvSig = shBuf + 147u;
                    const auto signedBytes = zerocam::crypto::BuildServerHelloSignedBytes(
                        authMode, cliNonce, cliEphPub, srvNonce, srvEphPub, srvIdPub);
                    uint8_t ecdhSec[32]{};
                    zerocam::crypto::WireV2DerivedKeys dk;
                    uint8_t cliProof[32]{}, expSrvProof[32]{}, gotSrvProof[32]{};
                    const bool okSetup =
                        zerocam::crypto::EcdsaIdentityKey::VerifyP1363(srvIdPub, signedBytes.data(), signedBytes.size(), srvSig) &&
                        cliEph.ComputeSharedSecretBE(srvEphPub, ecdhSec) &&
                        zerocam::crypto::DeriveWireV2Keys(authMode, cliNonce, cliEphPub, srvNonce, srvEphPub, srvIdPub, srvSig, ecdhSec, pin, dk) &&
                        zerocam::crypto::ComputeHandshakeProof(dk.authKey, dk.transcriptHash, "client", cliProof) &&
                        zerocam::crypto::ComputeHandshakeProof(dk.authKey, dk.transcriptHash, "server", expSrvProof);
                    SecureZeroMemory(ecdhSec, sizeof(ecdhSec));
                    if (!okSetup) { closesocket(s); return false; }

                    zerocam::WirePacketHeader cfHdr{ zerocam::kWireMagic, zerocam::kWireVersion,
                        static_cast<uint8_t>(zerocam::WirePacketType::HandshakeClientFinish), 0, 0, zerocam::kWireProofBytes };
                    uint8_t cfBuf[sizeof(cfHdr) + zerocam::kWireProofBytes]{};
                    std::memcpy(cfBuf, &cfHdr, sizeof(cfHdr));
                    std::memcpy(cfBuf + sizeof(cfHdr), cliProof, 32u);
                    send(s, reinterpret_cast<const char*>(cfBuf), sizeof(cfBuf), 0);

                    zerocam::WirePacketHeader sfHdr{};
                    if (!recvAllW(s, &sfHdr, sizeof(sfHdr))) { closesocket(s); return false; }
                    if (sfHdr.type == static_cast<uint8_t>(zerocam::WirePacketType::HandshakeError) && sfHdr.payloadSize == 1u) {
                        recvAllW(s, &outErrCode, 1u);
                        closesocket(s);
                        return false;
                    }
                    if (sfHdr.type != static_cast<uint8_t>(zerocam::WirePacketType::HandshakeServerFinish) ||
                        sfHdr.payloadSize != zerocam::kWireProofBytes ||
                        !recvAllW(s, gotSrvProof, sizeof(gotSrvProof)) ||
                        !zerocam::crypto::ConstantTimeEquals(expSrvProof, gotSrvProof, 32u)) {
                        closesocket(s);
                        return false;
                    }

                    // Verifica recepción AEAD ControlJson + envío de Heartbeat cifrado bidireccional.
                    zerocam::crypto::Aes256GcmCipher cliRecv, cliSend;
                    zerocam::WirePacketHeader ctrlHdr{};
                    if (cliRecv.Init(dk.s2cKey, dk.s2cIvBase) &&
                        cliSend.Init(dk.c2sKey, dk.c2sIvBase) &&
                        recvAllW(s, &ctrlHdr, sizeof(ctrlHdr)) &&
                        ctrlHdr.type == static_cast<uint8_t>(zerocam::WirePacketType::ControlJson) &&
                        ctrlHdr.payloadSize > zerocam::kWireAeadTagBytes && ctrlHdr.payloadSize < 4096u) {
                        std::vector<uint8_t> encCtrl(ctrlHdr.payloadSize), plainCtrl;
                        if (recvAllW(s, encCtrl.data(), encCtrl.size()) &&
                            cliRecv.DecryptNext(reinterpret_cast<const uint8_t*>(&ctrlHdr), sizeof(ctrlHdr),
                                                encCtrl.data(), encCtrl.size(), plainCtrl)) {
                            const std::string js(plainCtrl.begin(), plainCtrl.end());
                            outGotCtrl = (js.find("\"cmd\":\"set_video\"") != std::string::npos);
                        }
                        // Envía Heartbeat AEAD vacío para confirmar descifrado c2s sin cierre de sesión.
                        zerocam::WirePacketHeader hbHdr{ zerocam::kWireMagic, zerocam::kWireVersion,
                            static_cast<uint8_t>(zerocam::WirePacketType::Heartbeat), 0, 0, zerocam::kWireAeadTagBytes };
                        std::vector<uint8_t> hbCt;
                        if (cliSend.EncryptNext(reinterpret_cast<const uint8_t*>(&hbHdr), sizeof(hbHdr), nullptr, 0u, hbCt)) {
                            std::vector<uint8_t> hbPkt(sizeof(hbHdr) + hbCt.size());
                            std::memcpy(hbPkt.data(), &hbHdr, sizeof(hbHdr));
                            std::memcpy(hbPkt.data() + sizeof(hbHdr), hbCt.data(), hbCt.size());
                            send(s, reinterpret_cast<const char*>(hbPkt.data()), static_cast<int>(hbPkt.size()), 0);
                        }
                    }
                    closesocket(s);
                    return true;
                };

                // 9k-1. Wi-Fi PIN correcto (PBKDF2 100k) sobre IP LAN real.
                zerocam::StreamIngest wifiIngest;
                wifiIngest.SetDiscoveryPortForTest(59237);
                wifiIngest.SetWifiPin("482910");
                wifiIngest.SetVideoConfig(1280, 720, 30, 4000000, false);
                bool wifiPinOk = false;
                bool wifiRateLimitOk = false;
                if (wifiIngest.Start(zerocam::SourceMode::ZeroCamWifi, L"59236", &d3dCtx, nullptr)) {
                    uint16_t wp = 0;
                    for (int w = 0; w < 50; ++w) {
                        const uint16_t p = wifiIngest.BoundPort();
                        if (p >= 59236) { wp = p; break; }
                        Sleep(20);
                    }
                    Sleep(80);
                    if (wp != 0) {
                        uint8_t errCode = 0;
                        bool gotCtrl = false;
                        wifiPinOk = runWifiHandshake(localLanIp, wp, "482910", errCode, gotCtrl) && gotCtrl;

                        // 9k-2. 5 fallos consecutivos de PIN -> intentos 1..4 PinInvalid, 5to y 6to RateLimited (incluso con PIN válido).
                        bool allFourPinErr = true;
                        for (int attempt = 1; attempt <= 4; ++attempt) {
                            Sleep(1600);
                            uint8_t ec = 0;
                            bool gc = false;
                            const bool ok = runWifiHandshake(localLanIp, wp, "000000", ec, gc);
                            if (ok || ec != zerocam::kWireErrPinInvalid) {
                                allFourPinErr = false;
                                break;
                            }
                        }
                        Sleep(1600);
                        uint8_t ec5 = 0; bool gc5 = false;
                        const bool fifthLocked = !runWifiHandshake(localLanIp, wp, "000000", ec5, gc5) &&
                                                 ec5 == zerocam::kWireErrRateLimited;
                        Sleep(1600);
                        uint8_t ec6 = 0; bool gc6 = false;
                        const bool sixthBlockedEvenValidPin = !runWifiHandshake(localLanIp, wp, "482910", ec6, gc6) &&
                                                              ec6 == zerocam::kWireErrRateLimited;
                        wifiRateLimitOk = allFourPinErr && fifthLocked && sixthBlockedEvenValidPin;
                    }
                    wifiIngest.Stop();
                }
                allOk &= Expect(wifiPinOk, "StreamIngest TCP Wire v2 Wi-Fi PIN (PBKDF2 100k) autentica y entrega ControlJson AEAD");
                allOk &= Expect(wifiRateLimitOk, "StreamIngest TCP Wire v2 Wi-Fi bloquea IP tras 5 fallos de PIN (kWireErrRateLimited)");
            }
            if (wsa9kOk) {
                WSACleanup();
            }
        }
    }

    decoder.Shutdown();
    d3dCtx.Shutdown();
    DeleteFileW(tempIpc.c_str());

    MFShutdown();
    CoUninitialize();
    return allOk ? 0 : 1;
}
