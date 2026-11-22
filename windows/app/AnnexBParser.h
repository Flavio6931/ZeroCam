#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace zerocam {

// Segmentador incremental de flujos H.264 Annex-B que agrupa Access Units completos e inyecta SPS/PPS en keyframes.
class AnnexBParser {
public:
    using AccessUnitCallback = std::function<void(const uint8_t* data, size_t size, int64_t ptsUs, bool isKeyframe)>;

    // Reinicia el estado interno de acumulación y parámetros SPS/PPS.
    void Reset();
    // Procesa un bloque continuo de bytes H.264 Annex-B y dispara el callback por cada Access Unit completo.
    void Feed(const uint8_t* bytes, size_t size, int64_t ptsUs, const AccessUnitCallback& onAu);
    // Fuerza la emisión del último Access Unit acumulado (fin de stream o archivo).
    void Flush(int64_t ptsUs, const AccessUnitCallback& onAu);

    // Empaqueta un Access Unit ya delimitado (por ejemplo desde ZeroCam Wire v1) asegurando SPS/PPS si es IDR.
    void NormalizeAccessUnit(
        const uint8_t* data,
        size_t size,
        std::vector<uint8_t>& outBuffer,
        bool& outKeyframe);

private:
    // Procesa un NAL completo (incluyendo su start code de 4 bytes).
    void HandleNal(const uint8_t* nalWithStart, size_t nalLen, int64_t ptsUs, const AccessUnitCallback& onAu);
    // Emite el Access Unit actual anteponiendo SPS/PPS si contiene un IDR sin cabeceras previas.
    void EmitCurrentAu(int64_t ptsUs, const AccessUnitCallback& onAu);

    std::vector<uint8_t> streamBuf_;
    std::vector<uint8_t> currentAu_;
    std::vector<uint8_t> lastSps_;
    std::vector<uint8_t> lastPps_;
    bool hasVclInAu_ = false;
    bool hasKeyframeInAu_ = false;
    bool hasSpsPpsInAu_ = false;
};

} // namespace zerocam
