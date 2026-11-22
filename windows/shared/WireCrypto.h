#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <string>
#include <vector>
#include "ZeroCamProtocol.h"

namespace zerocam::crypto {

/// Genera bytes aleatorios criptográficamente seguros mediante BCryptGenRandom (CSPRNG del SO).
bool RandomBytes(uint8_t* out, size_t len) noexcept;

/// Genera un PIN numérico uniforme de 6 dígitos (000000..999999) con muestreo sin sesgo.
std::wstring GenerateRandomPin6();

/// Compara dos bloques de bytes en tiempo constante para evitar ataques de temporización.
bool ConstantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len) noexcept;

/// Codifica bytes en hexadecimal mayúsculas sin separadores.
std::string BytesToHexUpper(const uint8_t* data, size_t len);

/// Decodifica hexadecimal (mayúsculas o minúsculas) en un vector de bytes.
bool HexToBytes(const std::string& hex, std::vector<uint8_t>& out);

/// Calcula SHA-256 sobre un búfer en memoria mediante Windows CNG.
bool Sha256(const uint8_t* data, size_t len, uint8_t out32[32]) noexcept;

/// Calcula HMAC-SHA256 sobre un búfer en memoria mediante Windows CNG.
bool HmacSha256(
    const uint8_t* key,
    size_t keyLen,
    const uint8_t* data,
    size_t dataLen,
    uint8_t out32[32]) noexcept;

/// Deriva una clave con PBKDF2-HMAC-SHA256 mediante BCryptDeriveKeyPBKDF2.
bool Pbkdf2HmacSha256(
    const uint8_t* password,
    size_t passLen,
    const uint8_t* salt,
    size_t saltLen,
    uint32_t iterations,
    uint8_t* out,
    size_t outLen) noexcept;

/// Deriva material de claves con RFC 5869 HKDF-SHA256 (Extract + Expand).
bool HkdfSha256(
    const uint8_t* salt,
    size_t saltLen,
    const uint8_t* ikm,
    size_t ikmLen,
    const uint8_t* info,
    size_t infoLen,
    uint8_t* okm,
    size_t okmLen) noexcept;

/// Calcula la huella FP (16 caracteres hex = 64 bits iniciales de SHA-256) de una clave pública SEC1 de 65 B.
std::string ComputeFingerprintHex(const uint8_t pubSec1[kWirePubKeySec1Bytes]);

/// Formatea una huella FP de 16 hex en bloques legibles XXXX-XXXX-XXXX-XXXX para la UI.
std::wstring FormatFingerprintDisplay(const std::string& fpHex16);

/// Cifra una cadena UTF-8 en reposo usando Windows DPAPI (ámbito usuario actual) y la codifica en Base64.
bool DpapiEncryptToBase64(const std::string& plaintextUtf8, const char* entropyTag, std::wstring& outB64);

/// Descifra una cadena Base64 protegida previamente con Windows DPAPI.
bool DpapiDecryptFromBase64(const std::wstring& b64, const char* entropyTag, std::string& outUtf8);

/// Clave persistente de identidad ECDSA P-256 del equipo Windows (protegida con DPAPI en %APPDATA%\ZeroCam).
class EcdsaIdentityKey {
public:
    EcdsaIdentityKey() = default;
    ~EcdsaIdentityKey();

    EcdsaIdentityKey(const EcdsaIdentityKey&) = delete;
    EcdsaIdentityKey& operator=(const EcdsaIdentityKey&) = delete;

    /// Carga la identidad persistente DPAPI del PC o genera y guarda una nueva si no existe.
    bool LoadOrCreatePersistent();

    /// Genera una identidad ECDSA P-256 efímera en memoria (útil para pruebas automatizadas).
    bool GenerateEphemeral();

    /// Libera los handles CNG internos.
    void Reset() noexcept;

    bool IsValid() const noexcept { return keyHandle_ != nullptr; }

    /// Exporta la clave pública en formato SEC1 sin comprimir (65 B: 0x04 || X[32] || Y[32]).
    bool ExportPublicSec1(uint8_t out65[kWirePubKeySec1Bytes]) const noexcept;

    /// Devuelve la huella FP en 16 hex mayúsculas o cadena vacía si no está inicializada.
    std::string FingerprintHex() const;

    /// Devuelve la huella FP formateada XXXX-XXXX-XXXX-XXXX para mostrar al usuario.
    std::wstring FingerprintDisplay() const;

    /// Firma un mensaje con ECDSA P-256 + SHA-256 entregando formato raw IEEE P1363 (64 B: r[32]||s[32]).
    bool SignP1363(const uint8_t* msg, size_t msgLen, uint8_t outSig64[kWireEcdsaSigBytes]) const noexcept;

    /// Verifica una firma raw IEEE P1363 (64 B) sobre un mensaje contra una clave pública SEC1 de 65 B.
    static bool VerifyP1363(
        const uint8_t pubSec1[kWirePubKeySec1Bytes],
        const uint8_t* msg,
        size_t msgLen,
        const uint8_t sig64[kWireEcdsaSigBytes]) noexcept;

private:
    bool ImportPrivateBlob(const uint8_t* blob, size_t blobLen) noexcept;
    bool ExportPrivateBlob(std::vector<uint8_t>& outBlob) const;

    BCRYPT_ALG_HANDLE algHandle_ = nullptr;
    BCRYPT_KEY_HANDLE keyHandle_ = nullptr;
    uint8_t pubSec1_[kWirePubKeySec1Bytes]{};
};

/// Par de claves efímero ECDH P-256 para acuerdo de secreto compartido con Android JCA.
class EcdhP256KeyPair {
public:
    EcdhP256KeyPair() = default;
    ~EcdhP256KeyPair();

    EcdhP256KeyPair(const EcdhP256KeyPair&) = delete;
    EcdhP256KeyPair& operator=(const EcdhP256KeyPair&) = delete;

    /// Genera un nuevo par efímero ECDH P-256 en Windows CNG.
    bool Generate() noexcept;

    /// Importa un par privado P-256 fijo exclusivamente para pruebas de vectores cruzados deterministas.
    bool ImportRawForTest(const uint8_t privD32[32], const uint8_t pubX32[32], const uint8_t pubY32[32]) noexcept;

    /// Libera los handles CNG.
    void Reset() noexcept;

    /// Exporta la clave pública efímera en formato SEC1 sin comprimir (65 B).
    bool ExportPublicSec1(uint8_t out65[kWirePubKeySec1Bytes]) const noexcept;

    /// Calcula el secreto compartido ECDH P-256 (32 B en big-endian, compatible con Android JCA).
    bool ComputeSharedSecretBE(const uint8_t peerPubSec1[kWirePubKeySec1Bytes], uint8_t outSecret32[32]) const noexcept;

private:
    BCRYPT_ALG_HANDLE algHandle_ = nullptr;
    BCRYPT_KEY_HANDLE keyHandle_ = nullptr;
    uint8_t pubSec1_[kWirePubKeySec1Bytes]{};
};

/// Conjunto de claves derivadas del handshake Wire v2; limpia automáticamente su memoria al destruirse.
struct WireV2DerivedKeys {
    uint8_t c2sKey[kWireAeadKeyBytes]{};
    uint8_t s2cKey[kWireAeadKeyBytes]{};
    uint8_t c2sIvBase[kWireAeadIvBytes]{};
    uint8_t s2cIvBase[kWireAeadIvBytes]{};
    uint8_t authKey[kWireProofBytes]{};
    uint8_t transcriptHash[32]{};

    ~WireV2DerivedKeys() {
        SecureZeroMemory(c2sKey, sizeof(c2sKey));
        SecureZeroMemory(s2cKey, sizeof(s2cKey));
        SecureZeroMemory(c2sIvBase, sizeof(c2sIvBase));
        SecureZeroMemory(s2cIvBase, sizeof(s2cIvBase));
        SecureZeroMemory(authKey, sizeof(authKey));
        SecureZeroMemory(transcriptHash, sizeof(transcriptHash));
    }
};

/// Construye el bloque exacto de 238 B firmado por ServerHello en Wire v2.
std::vector<uint8_t> BuildServerHelloSignedBytes(
    uint8_t authMode,
    const uint8_t clientNonce[kWireNonceBytes],
    const uint8_t clientEphPub[kWirePubKeySec1Bytes],
    const uint8_t serverNonce[kWireNonceBytes],
    const uint8_t serverEphPub[kWirePubKeySec1Bytes],
    const uint8_t serverIdPub[kWirePubKeySec1Bytes]);

/// Deriva c2sKey, s2cKey, c2sIvBase, s2cIvBase, authKey y transcriptHash con PBKDF2 (100k) + HKDF-SHA256.
bool DeriveWireV2Keys(
    uint8_t authMode,
    const uint8_t clientNonce[kWireNonceBytes],
    const uint8_t clientEphPub[kWirePubKeySec1Bytes],
    const uint8_t serverNonce[kWireNonceBytes],
    const uint8_t serverEphPub[kWirePubKeySec1Bytes],
    const uint8_t serverIdPub[kWirePubKeySec1Bytes],
    const uint8_t sigEcdsa[kWireEcdsaSigBytes],
    const uint8_t ecdhSecretBE[32],
    const std::string& pinUtf8,
    WireV2DerivedKeys& outKeys);

/// Calcula la prueba HMAC-SHA256 de ClientFinish ("client") o ServerFinish ("server").
bool ComputeHandshakeProof(
    const uint8_t authKey[kWireProofBytes],
    const uint8_t transcriptHash[32],
    const char* label,
    uint8_t outProof32[kWireProofBytes]) noexcept;

/// Cifrador/descifrador AES-256-GCM con contador monotónico de 64 bits (< 2^48) y AAD por paquete.
class Aes256GcmCipher {
public:
    Aes256GcmCipher() = default;
    ~Aes256GcmCipher();

    Aes256GcmCipher(const Aes256GcmCipher&) = delete;
    Aes256GcmCipher& operator=(const Aes256GcmCipher&) = delete;

    /// Inicializa la clave AES-256-GCM y el IV base de 12 bytes reiniciando el contador de secuencia a 0.
    bool Init(const uint8_t key32[kWireAeadKeyBytes], const uint8_t ivBase12[kWireAeadIvBytes]) noexcept;

    /// Libera los handles CNG y borra de memoria el IV base.
    void Reset() noexcept;

    bool IsReady() const noexcept { return keyHandle_ != nullptr; }
    uint64_t Sequence() const noexcept { return seq_; }

    /// Calcula nonce[12] = ivBase XOR (0x00000000 || seq64_be).
    static void BuildNonce(const uint8_t ivBase[kWireAeadIvBytes], uint64_t seq, uint8_t outNonce[kWireAeadIvBytes]) noexcept;

    /// Cifra el siguiente paquete devolviendo ciphertext || tag[16] e incrementando seq_ de forma monotónica.
    bool EncryptNext(
        const uint8_t* aad,
        size_t aadLen,
        const uint8_t* plaintext,
        size_t plaintextLen,
        std::vector<uint8_t>& outCtAndTag);

    /// Descifra y autentica el siguiente paquete (ciphertext || tag[16]) incrementando seq_ solo si es auténtico.
    bool DecryptNext(
        const uint8_t* aad,
        size_t aadLen,
        const uint8_t* ctAndTag,
        size_t ctAndTagLen,
        std::vector<uint8_t>& outPlaintext);

private:
    BCRYPT_ALG_HANDLE algHandle_ = nullptr;
    BCRYPT_KEY_HANDLE keyHandle_ = nullptr;
    uint8_t ivBase_[kWireAeadIvBytes]{};
    uint64_t seq_ = 0;
};

} // namespace zerocam::crypto
