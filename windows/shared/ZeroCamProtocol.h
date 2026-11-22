#pragma once
#include <cstdint>

namespace zerocam {

// Constantes del protocolo binario cifrado ZeroCam Wire v2 compartido entre PC y Android.
inline constexpr uint32_t kWireMagic               = 0x4D41435Au; // 'ZCAM' little-endian
inline constexpr uint8_t  kWireVersion             = 2;
inline constexpr uint16_t kDefaultStreamPort       = 5000;
inline constexpr uint16_t kStreamPortFallbackCount = 11; // Rango automático 5000..5010 si el puerto base está ocupado
inline constexpr uint16_t kDiscoveryUdpPort        = 5001;
inline constexpr uint32_t kMaxWirePayloadBytes     = 8u * 1024u * 1024u;
inline constexpr const char* kDiscoveryMagicV2     = "ZCAM_DISCOVER_V2";
inline constexpr const char* kDiscoveryHostV2      = "ZCAM_HOST_V2";

// Tamaños fijos criptográficos de ZeroCam Wire v2 (ECC P-256 + AES-256-GCM).
inline constexpr uint32_t kWireNonceBytes          = 16u;
inline constexpr uint32_t kWirePubKeySec1Bytes     = 65u;
inline constexpr uint32_t kWireEcdsaSigBytes       = 64u;
inline constexpr uint32_t kWireProofBytes          = 32u;
inline constexpr uint32_t kWireAeadKeyBytes        = 32u;
inline constexpr uint32_t kWireAeadIvBytes         = 12u;
inline constexpr uint32_t kWireAeadTagBytes        = 16u;
inline constexpr uint32_t kWireClientHelloBytes    = kWireNonceBytes + kWirePubKeySec1Bytes; // 81 B
inline constexpr uint32_t kWireServerHelloBytes    = 1u + kWireNonceBytes + kWirePubKeySec1Bytes + kWirePubKeySec1Bytes + kWireEcdsaSigBytes; // 211 B
inline constexpr uint64_t kWireMaxAeadSeq          = (1ull << 48);

// Modo de autenticación negociado en ServerHello (0=USB loopback local sin PIN, 1=Wi-Fi con PIN obligatorio).
inline constexpr uint8_t kWireAuthModeUsbLocal     = 0u;
inline constexpr uint8_t kWireAuthModeWifiPin      = 1u;

// Códigos de rechazo en paquetes HandshakeError (1 byte).
inline constexpr uint8_t kWireErrPinInvalid        = 1u;
inline constexpr uint8_t kWireErrRateLimited       = 2u;
inline constexpr uint8_t kWireErrProtocol          = 3u;
inline constexpr uint8_t kWireErrPinRequired       = 4u;

// Tipos de paquete soportados en ZeroCam Wire v2.
enum class WirePacketType : uint8_t {
    VideoH264             = 1,
    VideoHevc             = 2,
    ControlJson           = 3,
    Heartbeat             = 4,
    HandshakeClientHello  = 16,
    HandshakeServerHello  = 17,
    HandshakeClientFinish = 18,
    HandshakeServerFinish = 19,
    HandshakeError        = 20
};

// Banderas de paquete de video (bits 0..1 estado frame, bits 2..3 rotación horaria requerida: 0=0°, 1=90°, 2=180°, 3=270°).
enum WirePacketFlags : uint16_t {
    kWireFlagKeyframe     = 1u << 0,
    kWireFlagConfigSpsPps = 1u << 1,
    kWireFlagRotShift     = 2,
    kWireFlagRotMask      = 0x3u << 2
};

// Extrae el código de rotación (0..3 -> 0°/90°/180°/270° CW) desde las banderas del paquete.
inline uint8_t ExtractRotationCode(uint16_t flags) noexcept {
    return static_cast<uint8_t>((flags >> kWireFlagRotShift) & 0x3u);
}

#pragma pack(push, 1)
// Cabecera binaria fija de 20 bytes (también utilizada como AAD en AES-256-GCM bajo Wire v2).
struct WirePacketHeader {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t flags;
    uint64_t ptsUs;
    uint32_t payloadSize;
};
#pragma pack(pop)

static_assert(sizeof(WirePacketHeader) == 20, "WirePacketHeader debe medir exactamente 20 bytes.");

} // namespace zerocam
