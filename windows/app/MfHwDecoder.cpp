#include "MfHwDecoder.h"
#include <mferror.h>
#include <cstring>

namespace zerocam {

MfHwDecoder::~MfHwDecoder() {
    Shutdown();
}

bool MfHwDecoder::Initialize(D3D11Context* d3dCtx, PreviewRenderer* preview) {
    Shutdown();
    if (!d3dCtx || !d3dCtx->DxgiManager()) return false;
    d3dCtx_ = d3dCtx;
    preview_ = preview;

    HRESULT hr = CoCreateInstance(CLSID_MSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder_));
    if (FAILED(hr)) {
        return false;
    }

    // Activa baja latencia para evitar buffers B-frame innecesarios.
    Microsoft::WRL::ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(decoder_.As(&codecApi))) {
        VARIANT v{};
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }

    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(decoder_->GetAttributes(&attrs))) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        UINT32 d3dAware = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware)) && d3dAware != 0) {
            if (SUCCEEDED(decoder_->ProcessMessage(
                    MFT_MESSAGE_SET_D3D_MANAGER,
                    reinterpret_cast<ULONG_PTR>(d3dCtx_->DxgiManager())))) {
                hwAccelerated_ = true;
            }
        }
    }

    Microsoft::WRL::ComPtr<IMFMediaType> inType;
    if (FAILED(MFCreateMediaType(&inType))) {
        Shutdown();
        return false;
    }
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width_, height_);
    if (FAILED(decoder_->SetInputType(0, inType.Get(), 0))) {
        Shutdown();
        return false;
    }

    if (!ConfigureOutputType()) {
        Shutdown();
        return false;
    }

    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    initialized_ = true;
    return true;
}

void MfHwDecoder::Shutdown() {
    if (decoder_) {
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        decoder_.Reset();
    }
    d3dCtx_ = nullptr;
    preview_ = nullptr;
    initialized_ = false;
    hwAccelerated_ = false;
}

bool MfHwDecoder::ConfigureOutputType() {
    if (!decoder_) return false;
    for (DWORD i = 0; ; ++i) {
        Microsoft::WRL::ComPtr<IMFMediaType> outType;
        HRESULT hr = decoder_->GetOutputAvailableType(0, i, &outType);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) continue;

        GUID subtype{};
        if (SUCCEEDED(outType->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
            if (SUCCEEDED(decoder_->SetOutputType(0, outType.Get(), 0))) {
                MFVideoArea area{};
                UINT32 blobLen = 0;
                if (SUCCEEDED(outType->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &blobLen)) &&
                    area.Area.cx > 0 && area.Area.cy > 0) {
                    width_ = static_cast<uint32_t>(area.Area.cx);
                    height_ = static_cast<uint32_t>(area.Area.cy);
                } else {
                    UINT32 w = 0, h = 0;
                    if (SUCCEEDED(MFGetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0) {
                        width_ = w;
                        height_ = h;
                    }
                }
                return true;
            }
        }
    }
    return false;
}

bool MfHwDecoder::DecodeAccessUnit(
    const uint8_t* data,
    size_t size,
    int64_t ptsUs,
    uint32_t fpsNum,
    uint32_t fpsDen,
    uint8_t rotationCode,
    bool skipPublish) {
    if (!initialized_ || !decoder_ || !data || size == 0) return false;
    currentRotationCode_ = rotationCode & 0x3u;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer))) return false;
    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) return false;
    std::memcpy(dst, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));

    Microsoft::WRL::ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(ptsUs * 10);

    HRESULT hr = decoder_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        PullOutputs(fpsNum, fpsDen, skipPublish);
        hr = decoder_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) {
        return false;
    }

    return PullOutputs(fpsNum, fpsDen, skipPublish);
}

void MfHwDecoder::DrainAll(uint32_t fpsNum, uint32_t fpsDen) {
    if (!initialized_ || !decoder_) return;
    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    PullOutputs(fpsNum, fpsDen, false);
}

bool MfHwDecoder::PullOutputs(uint32_t fpsNum, uint32_t fpsDen, bool skipPublish) {
    while (true) {
        MFT_OUTPUT_STREAM_INFO info{};
        if (FAILED(decoder_->GetOutputStreamInfo(0, &info))) {
            return false;
        }
        const bool mftProvidesSamples =
            (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        MFT_OUTPUT_DATA_BUFFER outData{};
        Microsoft::WRL::ComPtr<IMFSample> callerSample;
        if (!mftProvidesSamples) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
            if (FAILED(MFCreateSample(&callerSample)) ||
                FAILED(MFCreateMemoryBuffer(info.cbSize > 0 ? info.cbSize : width_ * height_ * 3u / 2u, &buf))) {
                return false;
            }
            callerSample->AddBuffer(buf.Get());
            outData.pSample = callerSample.Get();
        }

        DWORD status = 0;
        HRESULT hr = decoder_->ProcessOutput(0, 1, &outData, &status);

        Microsoft::WRL::ComPtr<IMFCollection> eventsToRelease;
        eventsToRelease.Attach(outData.pEvents);
        Microsoft::WRL::ComPtr<IMFSample> outSample;
        if (mftProvidesSamples) {
            outSample.Attach(outData.pSample);
        } else {
            outSample = callerSample;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return true;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!ConfigureOutputType()) {
                return false;
            }
            continue;
        }
        if (FAILED(hr) || !outSample) {
            return false;
        }
        if (skipPublish) {
            ++decodedFrames_;
            continue;
        }

        LONGLONG sampleTimeHns = 0;
        outSample->GetSampleTime(&sampleTimeHns);

        Microsoft::WRL::ComPtr<IMFMediaBuffer> outBuf;
        if (SUCCEEDED(outSample->GetBufferByIndex(0, &outBuf))) {
            Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuf;
            if (SUCCEEDED(outBuf.As(&dxgiBuf))) {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
                UINT subIdx = 0;
                if (SUCCEEDED(dxgiBuf->GetResource(IID_PPV_ARGS(&tex))) &&
                    SUCCEEDED(dxgiBuf->GetSubresourceIndex(&subIdx))) {
                    const bool needPreview = preview_ && preview_->IsEnabled();
                    ID3D11Texture2D* pubRaw = nullptr;
                    bool alreadyRotated = false;
                    const bool pubOk = d3dCtx_->PublishGpuFrame(
                        tex.Get(), subIdx, width_, height_, sampleTimeHns, fpsNum, fpsDen,
                        currentRotationCode_, needPreview ? &pubRaw : nullptr, &alreadyRotated);
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> pubTex;
                    pubTex.Attach(pubRaw);
                    if (pubOk) {
                        ++decodedFrames_;
                        ++hwZeroCopyFrames_;
                    }
                    if (needPreview) {
                        const bool mirrorH = d3dCtx_->GetEffectsConfig().mirrorH;
                        if (pubTex) {
                            preview_->PresentNv12(
                                pubTex.Get(), 0, width_, height_,
                                alreadyRotated ? 0 : currentRotationCode_,
                                mirrorH, currentRotationCode_);
                        } else {
                            preview_->PresentNv12(
                                tex.Get(), subIdx, width_, height_,
                                currentRotationCode_, false, currentRotationCode_);
                        }
                    }
                }
            } else {
                BYTE* ptr = nullptr;
                DWORD curLen = 0;
                if (SUCCEEDED(outBuf->Lock(&ptr, nullptr, &curLen))) {
                    if (d3dCtx_->PublishCpuNv12Frame(ptr, width_, width_, height_, sampleTimeHns, fpsNum, fpsDen)) {
                        ++decodedFrames_;
                        ++cpuFallbackFrames_;
                    }
                    outBuf->Unlock();
                }
            }
        }
    }
}

} // namespace zerocam
