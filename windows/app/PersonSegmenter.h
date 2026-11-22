#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zerocam {

/// Motor de alpha matting de retrato RVM MobileNetV3 (512x288) sobre Windows ML (DirectML GPU o CPU).
class PersonSegmenter {
public:
    static constexpr uint32_t kModelW = 512;
    static constexpr uint32_t kModelH = 288;

    PersonSegmenter();
    ~PersonSegmenter();

    PersonSegmenter(const PersonSegmenter&) = delete;
    PersonSegmenter& operator=(const PersonSegmenter&) = delete;

    /// Prepara el modelo RVM ONNX desde el recurso embebido en RAM (o ruta explícita en tests) e inicia el hilo asíncrono.
    bool Initialize(const std::wstring& optionalModelPath = L"");

    /// Prepara el modelo RVM ONNX directamente desde un búfer en RAM sin tocar disco (carga diferida al primer frame).
    bool InitializeFromMemory(const uint8_t* modelBytes, size_t modelSize);

    /// Detiene el hilo worker y libera sesiones WinML y estados recurrentes.
    void Shutdown();

    /// Reinicia la memoria temporal recurrente (r1..r4) antes de la siguiente inferencia.
    void ResetTemporalState() noexcept;

    /// Indica si el modelo RVM ONNX está disponible para inferencia.
    bool IsAvailable() const noexcept;

    /// Indica si hay una inferencia actualmente en curso (evita acumulación de cola).
    bool IsBusy() const noexcept;

    /// Indica si la sesión activa está ejecutando sobre DirectML GPU (false si está en CPU).
    bool IsUsingGpu() const noexcept;

    /// Devuelve la última latencia medida de inferencia RVM + lectura en milisegundos.
    double LastInferenceMs() const noexcept;

    /// Envía un cuadro BGRA8 512x288 de forma no bloqueante; si el worker está ocupado, descarta el frame.
    void SubmitBgra512x288(const uint8_t* bgra, uint32_t rowPitch, bool useGpu);

    /// Copia el último matte BGRA8 512x288 (RGB=fgr descontaminado, A=pha continuo 0..255) si cambió la generación.
    bool CopyLatestMatte(std::vector<uint8_t>& outMatteBgra, uint64_t& inOutGen) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zerocam
