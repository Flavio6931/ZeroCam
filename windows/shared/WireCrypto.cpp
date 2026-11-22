#include "WireCrypto.h"
#include <wincrypt.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace zerocam::crypto {

namespace {

inline bool NtOk(NTSTATUS st) noexcept {
    return st >= 0;
}

struct ScopedAlg {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~ScopedAlg() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};

struct ScopedHash {
    BCRYPT_HASH_HANDLE h = nullptr;
    ~ScopedHash() { if (h) BCryptDestroyHash(h); }
};

struct ScopedKey {
    BCRYPT_KEY_HANDLE h = nullptr;
    ~ScopedKey() { if (h) BCryptDestroyKey(h); }
};

struct ScopedSecret {
    BCRYPT_SECRET_HANDLE h = nullptr;
    ~ScopedSecret() { if (h) BCryptDestroySecret(h); }
};

// Resuelve %APPDATA%\ZeroCam\host_identity.dpapi evitando reparse points.
std::wstring ResolveHostIdentityPath(bool ensureDir) {
    wchar_t appData[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    std::wstring dir = std::wstring(appData, len) + L"\\ZeroCam";
    if (ensureDir) {
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    const DWORD dirAttrs = GetFileAttributesW(dir.c_str());
    if (dirAttrs != INVALID_FILE_ATTRIBUTES && (dirAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return {};
    }
    return dir + L"\\host_identity.dpapi";
}

bool ExportEccPublicKeySec1(BCRYPT_KEY_HANDLE hKey, uint8_t out65[kWirePubKeySec1Bytes]) noexcept {
    if (!hKey || !out65) return false;
    ULONG blobLen = 0;
    if (!NtOk(BCryptExportKey(hKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &blobLen, 0))) {
        return false;
    }
    if (blobLen != sizeof(BCRYPT_ECCKEY_BLOB) + 64u) {
        return false;
    }
    std::vector<uint8_t> blob(blobLen);
    if (!NtOk(BCryptExportKey(hKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), blobLen, &blobLen, 0))) {
        return false;
    }
    const auto* hdr = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.data());
    if (hdr->cbKey != 32u) return false;
    out65[0] = 0x04u;
    std::memcpy(out65 + 1u, blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), 64u);
    return true;
}

bool ImportEccPublicKeySec1(
    BCRYPT_ALG_HANDLE hAlg,
    ULONG magic,
    const uint8_t pubSec1[kWirePubKeySec1Bytes],
    BCRYPT_KEY_HANDLE* outKey) noexcept {
    if (!hAlg || !pubSec1 || !outKey || pubSec1[0] != 0x04u) return false;
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64u]{};
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    hdr->dwMagic = magic;
    hdr->cbKey = 32u;
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), pubSec1 + 1u, 64u);
    return NtOk(BCryptImportKeyPair(hAlg, nullptr, BCRYPT_ECCPUBLIC_BLOB, outKey, blob, sizeof(blob), 0));
}

} // namespace

bool RandomBytes(uint8_t* out, size_t len) noexcept {
    if (len == 0) return true;
    if (!out) return false;
    return NtOk(BCryptGenRandom(nullptr, out, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::wstring GenerateRandomPin6() {
    constexpr uint32_t kBound = 1000000u;
    constexpr uint32_t kLimit = (0xFFFFFFFFu / kBound) * kBound;
    uint32_t val = 0;
    for (int tries = 0; tries < 64; ++tries) {
        uint8_t raw[4]{};
        if (!RandomBytes(raw, sizeof(raw))) {
            break;
        }
        std::memcpy(&val, raw, sizeof(val));
        if (val < kLimit) {
            val %= kBound;
            wchar_t buf[16]{};
            swprintf_s(buf, L"%06u", val);
            return std::wstring(buf);
        }
    }
    // Respaldo defensivo en el caso improbable de agotamiento de muestreo
    wchar_t buf[16]{};
    swprintf_s(buf, L"%06u", val % kBound);
    return std::wstring(buf);
}

bool ConstantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len) noexcept {
    if (!a || !b) return false;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff = static_cast<uint8_t>(diff | static_cast<uint8_t>(a[i] ^ b[i]));
    }
    return diff == 0;
}

std::string BytesToHexUpper(const uint8_t* data, size_t len) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 2u);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0xFu]);
        out.push_back(kHex[data[i] & 0xFu]);
    }
    return out;
}

bool HexToBytes(const std::string& hex, std::vector<uint8_t>& out) {
    out.clear();
    if ((hex.size() & 1u) != 0u) return false;
    out.reserve(hex.size() / 2u);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2u) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1u]);
        if (hi < 0 || lo < 0) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

bool Sha256(const uint8_t* data, size_t len, uint8_t out32[32]) noexcept {
    if (!out32 || (len > 0 && !data)) return false;
    ScopedAlg alg;
    if (!NtOk(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return false;
    }
    ScopedHash hash;
    if (!NtOk(BCryptCreateHash(alg.h, &hash.h, nullptr, 0, nullptr, 0, 0))) {
        return false;
    }
    if (len > 0 && !NtOk(BCryptHashData(hash.h, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0))) {
        return false;
    }
    return NtOk(BCryptFinishHash(hash.h, out32, 32u, 0));
}

bool HmacSha256(
    const uint8_t* key,
    size_t keyLen,
    const uint8_t* data,
    size_t dataLen,
    uint8_t out32[32]) noexcept {
    if (!out32 || (keyLen > 0 && !key) || (dataLen > 0 && !data)) return false;
    ScopedAlg alg;
    if (!NtOk(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
        return false;
    }
    uint8_t emptyKey = 0;
    ScopedHash hash;
    if (!NtOk(BCryptCreateHash(
            alg.h,
            &hash.h,
            nullptr,
            0,
            keyLen > 0 ? const_cast<PUCHAR>(key) : &emptyKey,
            static_cast<ULONG>(keyLen),
            0))) {
        return false;
    }
    if (dataLen > 0 && !NtOk(BCryptHashData(hash.h, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen), 0))) {
        return false;
    }
    return NtOk(BCryptFinishHash(hash.h, out32, 32u, 0));
}

bool Pbkdf2HmacSha256(
    const uint8_t* password,
    size_t passLen,
    const uint8_t* salt,
    size_t saltLen,
    uint32_t iterations,
    uint8_t* out,
    size_t outLen) noexcept {
    if (!out || outLen == 0 || iterations == 0 || (passLen > 0 && !password) || (saltLen > 0 && !salt)) {
        return false;
    }
    ScopedAlg alg;
    if (!NtOk(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
        return false;
    }
    uint8_t emptyBuf = 0;
    return NtOk(BCryptDeriveKeyPBKDF2(
        alg.h,
        passLen > 0 ? const_cast<PUCHAR>(password) : &emptyBuf,
        static_cast<ULONG>(passLen),
        saltLen > 0 ? const_cast<PUCHAR>(salt) : &emptyBuf,
        static_cast<ULONG>(saltLen),
        static_cast<ULONGLONG>(iterations),
        out,
        static_cast<ULONG>(outLen),
        0));
}

bool HkdfSha256(
    const uint8_t* salt,
    size_t saltLen,
    const uint8_t* ikm,
    size_t ikmLen,
    const uint8_t* info,
    size_t infoLen,
    uint8_t* okm,
    size_t okmLen) noexcept {
    if (!okm || okmLen == 0 || okmLen > 255u * 32u) return false;
    uint8_t zeroSalt[32]{};
    const uint8_t* actualSalt = (salt && saltLen > 0) ? salt : zeroSalt;
    const size_t actualSaltLen = (salt && saltLen > 0) ? saltLen : sizeof(zeroSalt);

    uint8_t prk[32]{};
    if (!HmacSha256(actualSalt, actualSaltLen, ikm, ikmLen, prk)) {
        return false;
    }

    uint8_t prevT[32]{};
    size_t prevLen = 0;
    size_t written = 0;
    uint8_t counter = 1;
    std::vector<uint8_t> blockInput;
    blockInput.reserve(32u + infoLen + 1u);

    bool ok = true;
    while (written < okmLen) {
        blockInput.clear();
        if (prevLen > 0) {
            blockInput.insert(blockInput.end(), prevT, prevT + prevLen);
        }
        if (info && infoLen > 0) {
            blockInput.insert(blockInput.end(), info, info + infoLen);
        }
        blockInput.push_back(counter);

        uint8_t t[32]{};
        if (!HmacSha256(prk, sizeof(prk), blockInput.data(), blockInput.size(), t)) {
            ok = false;
            break;
        }
        const size_t take = std::min<size_t>(sizeof(t), okmLen - written);
        std::memcpy(okm + written, t, take);
        std::memcpy(prevT, t, sizeof(t));
        prevLen = sizeof(t);
        written += take;
        ++counter;
        SecureZeroMemory(t, sizeof(t));
    }

    if (!blockInput.empty()) {
        SecureZeroMemory(blockInput.data(), blockInput.size());
    }
    SecureZeroMemory(prevT, sizeof(prevT));
    SecureZeroMemory(prk, sizeof(prk));
    return ok;
}

std::string ComputeFingerprintHex(const uint8_t pubSec1[kWirePubKeySec1Bytes]) {
    uint8_t digest[32]{};
    if (!Sha256(pubSec1, kWirePubKeySec1Bytes, digest)) {
        return {};
    }
    return BytesToHexUpper(digest, 8u);
}

std::wstring FormatFingerprintDisplay(const std::string& fpHex16) {
    if (fpHex16.size() != 16u) return L"----";
    std::wstring out;
    out.reserve(19u);
    for (size_t i = 0; i < 16u; ++i) {
        if (i > 0 && (i % 4u) == 0u) out.push_back(L'-');
        out.push_back(static_cast<wchar_t>(fpHex16[i]));
    }
    return out;
}

bool DpapiEncryptToBase64(const std::string& plaintextUtf8, const char* entropyTag, std::wstring& outB64) {
    outB64.clear();
    if (plaintextUtf8.empty()) {
        return true;
    }

    DATA_BLOB inBlob{};
    inBlob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintextUtf8.data()));
    inBlob.cbData = static_cast<DWORD>(plaintextUtf8.size());

    DATA_BLOB entropyBlob{};
    DATA_BLOB* pEntropy = nullptr;
    if (entropyTag && entropyTag[0] != '\0') {
        entropyBlob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(entropyTag));
        entropyBlob.cbData = static_cast<DWORD>(std::strlen(entropyTag));
        pEntropy = &entropyBlob;
    }

    DATA_BLOB outBlob{};
    if (!CryptProtectData(&inBlob, L"ZeroCamSecret", pEntropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        return false;
    }

    DWORD b64Chars = 0;
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    bool ok = false;
    if (CryptBinaryToStringW(outBlob.pbData, outBlob.cbData, flags, nullptr, &b64Chars) && b64Chars > 0) {
        std::wstring buf(b64Chars, L'\0');
        if (CryptBinaryToStringW(outBlob.pbData, outBlob.cbData, flags, buf.data(), &b64Chars)) {
            while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
            outB64 = std::move(buf);
            ok = true;
        }
    }
    SecureZeroMemory(outBlob.pbData, outBlob.cbData);
    LocalFree(outBlob.pbData);
    return ok;
}

bool DpapiDecryptFromBase64(const std::wstring& b64, const char* entropyTag, std::string& outUtf8) {
    outUtf8.clear();
    if (b64.empty()) {
        return true;
    }

    DWORD rawLen = 0;
    if (!CryptStringToBinaryW(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64, nullptr, &rawLen, nullptr, nullptr) ||
        rawLen == 0) {
        return false;
    }
    std::vector<BYTE> raw(rawLen);
    if (!CryptStringToBinaryW(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64, raw.data(), &rawLen, nullptr, nullptr)) {
        return false;
    }

    DATA_BLOB inBlob{ rawLen, raw.data() };
    DATA_BLOB entropyBlob{};
    DATA_BLOB* pEntropy = nullptr;
    if (entropyTag && entropyTag[0] != '\0') {
        entropyBlob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(entropyTag));
        entropyBlob.cbData = static_cast<DWORD>(std::strlen(entropyTag));
        pEntropy = &entropyBlob;
    }

    DATA_BLOB outBlob{};
    if (!CryptUnprotectData(&inBlob, nullptr, pEntropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        return false;
    }

    outUtf8.assign(reinterpret_cast<const char*>(outBlob.pbData), outBlob.cbData);
    SecureZeroMemory(outBlob.pbData, outBlob.cbData);
    LocalFree(outBlob.pbData);
    return true;
}

// ============================================================================
// EcdsaIdentityKey
// ============================================================================

EcdsaIdentityKey::~EcdsaIdentityKey() {
    Reset();
}

void EcdsaIdentityKey::Reset() noexcept {
    if (keyHandle_) {
        BCryptDestroyKey(keyHandle_);
        keyHandle_ = nullptr;
    }
    if (algHandle_) {
        BCryptCloseAlgorithmProvider(algHandle_, 0);
        algHandle_ = nullptr;
    }
    SecureZeroMemory(pubSec1_, sizeof(pubSec1_));
}

bool EcdsaIdentityKey::ImportPrivateBlob(const uint8_t* blob, size_t blobLen) noexcept {
    Reset();
    if (!blob || blobLen != sizeof(BCRYPT_ECCKEY_BLOB) + 96u) return false;
    if (!NtOk(BCryptOpenAlgorithmProvider(&algHandle_, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
        Reset();
        return false;
    }
    if (!NtOk(BCryptImportKeyPair(
            algHandle_,
            nullptr,
            BCRYPT_ECCPRIVATE_BLOB,
            &keyHandle_,
            const_cast<PUCHAR>(blob),
            static_cast<ULONG>(blobLen),
            0))) {
        Reset();
        return false;
    }
    if (!ExportEccPublicKeySec1(keyHandle_, pubSec1_)) {
        Reset();
        return false;
    }
    return true;
}

bool EcdsaIdentityKey::ExportPrivateBlob(std::vector<uint8_t>& outBlob) const {
    outBlob.clear();
    if (!keyHandle_) return false;
    ULONG sz = 0;
    if (!NtOk(BCryptExportKey(keyHandle_, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &sz, 0)) || sz == 0) {
        return false;
    }
    outBlob.resize(sz);
    if (!NtOk(BCryptExportKey(keyHandle_, nullptr, BCRYPT_ECCPRIVATE_BLOB, outBlob.data(), sz, &sz, 0))) {
        SecureZeroMemory(outBlob.data(), outBlob.size());
        outBlob.clear();
        return false;
    }
    return true;
}

bool EcdsaIdentityKey::GenerateEphemeral() {
    Reset();
    if (!NtOk(BCryptOpenAlgorithmProvider(&algHandle_, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
        Reset();
        return false;
    }
    if (!NtOk(BCryptGenerateKeyPair(algHandle_, &keyHandle_, 256u, 0)) ||
        !NtOk(BCryptFinalizeKeyPair(keyHandle_, 0))) {
        Reset();
        return false;
    }
    if (!ExportEccPublicKeySec1(keyHandle_, pubSec1_)) {
        Reset();
        return false;
    }
    return true;
}

bool EcdsaIdentityKey::LoadOrCreatePersistent() {
    static constexpr const char* kHostKeyEntropy = "ZeroCamHostIdentityP256V2";
    const std::wstring path = ResolveHostIdentityPath(true);

    if (!path.empty()) {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0) {
            std::ifstream ifs(path, std::ios::binary | std::ios::ate);
            if (ifs.good()) {
                const std::streamsize sz = ifs.tellg();
                if (sz > 0 && sz <= 8192) {
                    std::vector<BYTE> enc(static_cast<size_t>(sz));
                    ifs.seekg(0, std::ios::beg);
                    if (ifs.read(reinterpret_cast<char*>(enc.data()), sz).good()) {
                        DATA_BLOB inBlob{ static_cast<DWORD>(enc.size()), enc.data() };
                        DATA_BLOB ent{ static_cast<DWORD>(std::strlen(kHostKeyEntropy)), reinterpret_cast<BYTE*>(const_cast<char*>(kHostKeyEntropy)) };
                        DATA_BLOB outBlob{};
                        if (CryptUnprotectData(&inBlob, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
                            const bool imported = ImportPrivateBlob(outBlob.pbData, outBlob.cbData);
                            SecureZeroMemory(outBlob.pbData, outBlob.cbData);
                            LocalFree(outBlob.pbData);
                            if (imported) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    if (!GenerateEphemeral()) {
        return false;
    }

    if (!path.empty()) {
        std::vector<uint8_t> rawBlob;
        if (ExportPrivateBlob(rawBlob)) {
            DATA_BLOB inBlob{ static_cast<DWORD>(rawBlob.size()), rawBlob.data() };
            DATA_BLOB ent{ static_cast<DWORD>(std::strlen(kHostKeyEntropy)), reinterpret_cast<BYTE*>(const_cast<char*>(kHostKeyEntropy)) };
            DATA_BLOB outBlob{};
            if (CryptProtectData(&inBlob, L"ZeroCamHostId", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
                const DWORD attrs = GetFileAttributesW(path.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
                    if (ofs.good()) {
                        ofs.write(reinterpret_cast<const char*>(outBlob.pbData), static_cast<std::streamsize>(outBlob.cbData));
                        ofs.flush();
                    }
                }
                SecureZeroMemory(outBlob.pbData, outBlob.cbData);
                LocalFree(outBlob.pbData);
            }
            SecureZeroMemory(rawBlob.data(), rawBlob.size());
        }
    }
    return true;
}

bool EcdsaIdentityKey::ExportPublicSec1(uint8_t out65[kWirePubKeySec1Bytes]) const noexcept {
    if (!keyHandle_ || !out65) return false;
    std::memcpy(out65, pubSec1_, kWirePubKeySec1Bytes);
    return true;
}

std::string EcdsaIdentityKey::FingerprintHex() const {
    if (!keyHandle_) return {};
    return ComputeFingerprintHex(pubSec1_);
}

std::wstring EcdsaIdentityKey::FingerprintDisplay() const {
    return FormatFingerprintDisplay(FingerprintHex());
}

bool EcdsaIdentityKey::SignP1363(const uint8_t* msg, size_t msgLen, uint8_t outSig64[kWireEcdsaSigBytes]) const noexcept {
    if (!keyHandle_ || !outSig64 || (msgLen > 0 && !msg)) return false;
    uint8_t hash[32]{};
    if (!Sha256(msg, msgLen, hash)) return false;
    ULONG sigLen = 0;
    const NTSTATUS st = BCryptSignHash(keyHandle_, nullptr, hash, sizeof(hash), outSig64, kWireEcdsaSigBytes, &sigLen, 0);
    return NtOk(st) && sigLen == kWireEcdsaSigBytes;
}

bool EcdsaIdentityKey::VerifyP1363(
    const uint8_t pubSec1[kWirePubKeySec1Bytes],
    const uint8_t* msg,
    size_t msgLen,
    const uint8_t sig64[kWireEcdsaSigBytes]) noexcept {
    if (!pubSec1 || !sig64 || (msgLen > 0 && !msg)) return false;
    uint8_t hash[32]{};
    if (!Sha256(msg, msgLen, hash)) return false;

    ScopedAlg alg;
    if (!NtOk(BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
        return false;
    }
    ScopedKey pubKey;
    if (!ImportEccPublicKeySec1(alg.h, BCRYPT_ECDSA_PUBLIC_P256_MAGIC, pubSec1, &pubKey.h)) {
        return false;
    }
    return NtOk(BCryptVerifySignature(
        pubKey.h,
        nullptr,
        hash,
        sizeof(hash),
        const_cast<PUCHAR>(sig64),
        kWireEcdsaSigBytes,
        0));
}

// ============================================================================
// EcdhP256KeyPair
// ============================================================================

EcdhP256KeyPair::~EcdhP256KeyPair() {
    Reset();
}

void EcdhP256KeyPair::Reset() noexcept {
    if (keyHandle_) {
        BCryptDestroyKey(keyHandle_);
        keyHandle_ = nullptr;
    }
    if (algHandle_) {
        BCryptCloseAlgorithmProvider(algHandle_, 0);
        algHandle_ = nullptr;
    }
    SecureZeroMemory(pubSec1_, sizeof(pubSec1_));
}

bool EcdhP256KeyPair::Generate() noexcept {
    Reset();
    if (!NtOk(BCryptOpenAlgorithmProvider(&algHandle_, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0))) {
        Reset();
        return false;
    }
    if (!NtOk(BCryptGenerateKeyPair(algHandle_, &keyHandle_, 256u, 0)) ||
        !NtOk(BCryptFinalizeKeyPair(keyHandle_, 0))) {
        Reset();
        return false;
    }
    if (!ExportEccPublicKeySec1(keyHandle_, pubSec1_)) {
        Reset();
        return false;
    }
    return true;
}

bool EcdhP256KeyPair::ImportRawForTest(const uint8_t privD32[32], const uint8_t pubX32[32], const uint8_t pubY32[32]) noexcept {
    Reset();
    if (!privD32 || !pubX32 || !pubY32) return false;
    if (!NtOk(BCryptOpenAlgorithmProvider(&algHandle_, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0))) {
        Reset();
        return false;
    }
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96u]{};
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    hdr->dwMagic = BCRYPT_ECDH_PRIVATE_P256_MAGIC;
    hdr->cbKey = 32u;
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), pubX32, 32u);
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB) + 32u, pubY32, 32u);
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB) + 64u, privD32, 32u);
    const NTSTATUS st = BCryptImportKeyPair(algHandle_, nullptr, BCRYPT_ECCPRIVATE_BLOB, &keyHandle_, blob, sizeof(blob), 0);
    SecureZeroMemory(blob, sizeof(blob));
    if (!NtOk(st)) {
        Reset();
        return false;
    }
    pubSec1_[0] = 0x04u;
    std::memcpy(pubSec1_ + 1u, pubX32, 32u);
    std::memcpy(pubSec1_ + 33u, pubY32, 32u);
    return true;
}

bool EcdhP256KeyPair::ExportPublicSec1(uint8_t out65[kWirePubKeySec1Bytes]) const noexcept {
    if (!keyHandle_ || !out65) return false;
    std::memcpy(out65, pubSec1_, kWirePubKeySec1Bytes);
    return true;
}

bool EcdhP256KeyPair::ComputeSharedSecretBE(const uint8_t peerPubSec1[kWirePubKeySec1Bytes], uint8_t outSecret32[32]) const noexcept {
    if (!algHandle_ || !keyHandle_ || !peerPubSec1 || !outSecret32) return false;
    ScopedKey peerKey;
    if (!ImportEccPublicKeySec1(algHandle_, BCRYPT_ECDH_PUBLIC_P256_MAGIC, peerPubSec1, &peerKey.h)) {
        return false;
    }
    ScopedSecret secret;
    if (!NtOk(BCryptSecretAgreement(keyHandle_, peerKey.h, &secret.h, 0))) {
        return false;
    }
    uint8_t rawLe[32]{};
    ULONG derivedLen = 0;
    const NTSTATUS st = BCryptDeriveKey(secret.h, BCRYPT_KDF_RAW_SECRET, nullptr, rawLe, sizeof(rawLe), &derivedLen, 0);
    if (!NtOk(st) || derivedLen != 32u) {
        SecureZeroMemory(rawLe, sizeof(rawLe));
        return false;
    }
    // Windows CNG devuelve BCRYPT_KDF_RAW_SECRET en little-endian; se invierte a big-endian para empatar con JCA.
    for (size_t i = 0; i < 32u; ++i) {
        outSecret32[i] = rawLe[31u - i];
    }
    SecureZeroMemory(rawLe, sizeof(rawLe));
    return true;
}

// ============================================================================
// Wire v2 Key Schedule
// ============================================================================

std::vector<uint8_t> BuildServerHelloSignedBytes(
    uint8_t authMode,
    const uint8_t clientNonce[kWireNonceBytes],
    const uint8_t clientEphPub[kWirePubKeySec1Bytes],
    const uint8_t serverNonce[kWireNonceBytes],
    const uint8_t serverEphPub[kWirePubKeySec1Bytes],
    const uint8_t serverIdPub[kWirePubKeySec1Bytes]) {
    static constexpr char kPrefix[] = "ZCAM-V2-SH";
    std::vector<uint8_t> msg;
    msg.reserve(10u + 1u + 16u + 65u + 16u + 65u + 65u);
    msg.insert(msg.end(), kPrefix, kPrefix + 10u);
    msg.push_back(authMode);
    msg.insert(msg.end(), clientNonce, clientNonce + kWireNonceBytes);
    msg.insert(msg.end(), clientEphPub, clientEphPub + kWirePubKeySec1Bytes);
    msg.insert(msg.end(), serverNonce, serverNonce + kWireNonceBytes);
    msg.insert(msg.end(), serverEphPub, serverEphPub + kWirePubKeySec1Bytes);
    msg.insert(msg.end(), serverIdPub, serverIdPub + kWirePubKeySec1Bytes);
    return msg;
}

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
    WireV2DerivedKeys& outKeys) {
    if (authMode != kWireAuthModeUsbLocal && authMode != kWireAuthModeWifiPin) {
        return false;
    }
    if (authMode == kWireAuthModeWifiPin && pinUtf8.empty()) {
        return false;
    }

    uint8_t pinKey[32]{};
    if (authMode == kWireAuthModeWifiPin) {
        static constexpr char kSaltPrefix[] = "ZCAM-V2-PIN-SALT";
        uint8_t saltPre[16u + kWireNonceBytes + kWireNonceBytes]{};
        std::memcpy(saltPre, kSaltPrefix, 16u);
        std::memcpy(saltPre + 16u, clientNonce, kWireNonceBytes);
        std::memcpy(saltPre + 32u, serverNonce, kWireNonceBytes);
        uint8_t pinSalt[32]{};
        const bool saltOk = Sha256(saltPre, sizeof(saltPre), pinSalt);
        const bool kdfOk = saltOk && Pbkdf2HmacSha256(
            reinterpret_cast<const uint8_t*>(pinUtf8.data()),
            pinUtf8.size(),
            pinSalt,
            sizeof(pinSalt),
            100000u,
            pinKey,
            sizeof(pinKey));
        SecureZeroMemory(pinSalt, sizeof(pinSalt));
        if (!kdfOk) {
            SecureZeroMemory(pinKey, sizeof(pinKey));
            return false;
        }
    }

    static constexpr char kTransPrefix[] = "ZCAM-V2-TRANSCRIPT";
    std::vector<uint8_t> transcript;
    transcript.reserve(18u + 1u + 16u + 65u + 16u + 65u + 65u + 64u);
    transcript.insert(transcript.end(), kTransPrefix, kTransPrefix + 18u);
    transcript.push_back(authMode);
    transcript.insert(transcript.end(), clientNonce, clientNonce + kWireNonceBytes);
    transcript.insert(transcript.end(), clientEphPub, clientEphPub + kWirePubKeySec1Bytes);
    transcript.insert(transcript.end(), serverNonce, serverNonce + kWireNonceBytes);
    transcript.insert(transcript.end(), serverEphPub, serverEphPub + kWirePubKeySec1Bytes);
    transcript.insert(transcript.end(), serverIdPub, serverIdPub + kWirePubKeySec1Bytes);
    transcript.insert(transcript.end(), sigEcdsa, sigEcdsa + kWireEcdsaSigBytes);

    if (!Sha256(transcript.data(), transcript.size(), outKeys.transcriptHash)) {
        SecureZeroMemory(pinKey, sizeof(pinKey));
        return false;
    }

    uint8_t ikm[64]{};
    std::memcpy(ikm, ecdhSecretBE, 32u);
    std::memcpy(ikm + 32u, pinKey, 32u);
    SecureZeroMemory(pinKey, sizeof(pinKey));

    static constexpr char kHkdfInfo[] = "ZCAM-V2-KEYS";
    uint8_t okm[120]{};
    const bool hkdfOk = HkdfSha256(
        outKeys.transcriptHash,
        sizeof(outKeys.transcriptHash),
        ikm,
        sizeof(ikm),
        reinterpret_cast<const uint8_t*>(kHkdfInfo),
        12u,
        okm,
        sizeof(okm));
    SecureZeroMemory(ikm, sizeof(ikm));
    if (!hkdfOk) {
        SecureZeroMemory(okm, sizeof(okm));
        return false;
    }

    std::memcpy(outKeys.c2sKey, okm, 32u);
    std::memcpy(outKeys.s2cKey, okm + 32u, 32u);
    std::memcpy(outKeys.c2sIvBase, okm + 64u, 12u);
    std::memcpy(outKeys.s2cIvBase, okm + 76u, 12u);
    std::memcpy(outKeys.authKey, okm + 88u, 32u);
    SecureZeroMemory(okm, sizeof(okm));
    return true;
}

bool ComputeHandshakeProof(
    const uint8_t authKey[kWireProofBytes],
    const uint8_t transcriptHash[32],
    const char* label,
    uint8_t outProof32[kWireProofBytes]) noexcept {
    if (!authKey || !transcriptHash || !label || !outProof32) return false;
    const size_t labelLen = std::strlen(label);
    if (labelLen > 32u) return false;
    uint8_t buf[64]{};
    std::memcpy(buf, transcriptHash, 32u);
    std::memcpy(buf + 32u, label, labelLen);
    return HmacSha256(authKey, kWireProofBytes, buf, 32u + labelLen, outProof32);
}

// ============================================================================
// Aes256GcmCipher
// ============================================================================

Aes256GcmCipher::~Aes256GcmCipher() {
    Reset();
}

void Aes256GcmCipher::Reset() noexcept {
    if (keyHandle_) {
        BCryptDestroyKey(keyHandle_);
        keyHandle_ = nullptr;
    }
    if (algHandle_) {
        BCryptCloseAlgorithmProvider(algHandle_, 0);
        algHandle_ = nullptr;
    }
    SecureZeroMemory(ivBase_, sizeof(ivBase_));
    seq_ = 0;
}

bool Aes256GcmCipher::Init(const uint8_t key32[kWireAeadKeyBytes], const uint8_t ivBase12[kWireAeadIvBytes]) noexcept {
    Reset();
    if (!key32 || !ivBase12) return false;
    if (!NtOk(BCryptOpenAlgorithmProvider(&algHandle_, BCRYPT_AES_ALGORITHM, nullptr, 0))) {
        Reset();
        return false;
    }
    if (!NtOk(BCryptSetProperty(
            algHandle_,
            BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>((std::wcslen(BCRYPT_CHAIN_MODE_GCM) + 1u) * sizeof(wchar_t)),
            0))) {
        Reset();
        return false;
    }
    if (!NtOk(BCryptGenerateSymmetricKey(
            algHandle_,
            &keyHandle_,
            nullptr,
            0,
            const_cast<PUCHAR>(key32),
            kWireAeadKeyBytes,
            0))) {
        Reset();
        return false;
    }
    std::memcpy(ivBase_, ivBase12, kWireAeadIvBytes);
    seq_ = 0;
    return true;
}

void Aes256GcmCipher::BuildNonce(const uint8_t ivBase[kWireAeadIvBytes], uint64_t seq, uint8_t outNonce[kWireAeadIvBytes]) noexcept {
    std::memcpy(outNonce, ivBase, kWireAeadIvBytes);
    for (int i = 0; i < 8; ++i) {
        const uint8_t seqByte = static_cast<uint8_t>((seq >> ((7 - i) * 8)) & 0xFFu);
        outNonce[4 + i] = static_cast<uint8_t>(outNonce[4 + i] ^ seqByte);
    }
}

bool Aes256GcmCipher::EncryptNext(
    const uint8_t* aad,
    size_t aadLen,
    const uint8_t* plaintext,
    size_t plaintextLen,
    std::vector<uint8_t>& outCtAndTag) {
    outCtAndTag.clear();
    if (!keyHandle_ || seq_ >= kWireMaxAeadSeq || plaintextLen > kMaxWirePayloadBytes) {
        return false;
    }

    uint8_t nonce[kWireAeadIvBytes]{};
    BuildNonce(ivBase_, seq_, nonce);

    uint8_t tag[kWireAeadTagBytes]{};
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = sizeof(nonce);
    authInfo.pbAuthData = (aad && aadLen > 0) ? const_cast<PUCHAR>(aad) : nullptr;
    authInfo.cbAuthData = static_cast<ULONG>(aadLen);
    authInfo.pbTag = tag;
    authInfo.cbTag = sizeof(tag);

    outCtAndTag.resize(plaintextLen + kWireAeadTagBytes);
    uint8_t dummy = 0;
    ULONG outLen = 0;
    const NTSTATUS st = BCryptEncrypt(
        keyHandle_,
        (plaintextLen > 0 && plaintext) ? const_cast<PUCHAR>(plaintext) : &dummy,
        static_cast<ULONG>(plaintextLen),
        &authInfo,
        nullptr,
        0,
        (plaintextLen > 0) ? outCtAndTag.data() : &dummy,
        static_cast<ULONG>(plaintextLen),
        &outLen,
        0);
    if (!NtOk(st) || outLen != plaintextLen) {
        outCtAndTag.clear();
        return false;
    }
    std::memcpy(outCtAndTag.data() + plaintextLen, tag, kWireAeadTagBytes);
    ++seq_;
    return true;
}

bool Aes256GcmCipher::DecryptNext(
    const uint8_t* aad,
    size_t aadLen,
    const uint8_t* ctAndTag,
    size_t ctAndTagLen,
    std::vector<uint8_t>& outPlaintext) {
    outPlaintext.clear();
    if (!keyHandle_ || seq_ >= kWireMaxAeadSeq || !ctAndTag ||
        ctAndTagLen < kWireAeadTagBytes || ctAndTagLen > kMaxWirePayloadBytes + kWireAeadTagBytes) {
        return false;
    }

    const size_t ptLen = ctAndTagLen - kWireAeadTagBytes;
    uint8_t nonce[kWireAeadIvBytes]{};
    BuildNonce(ivBase_, seq_, nonce);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce;
    authInfo.cbNonce = sizeof(nonce);
    authInfo.pbAuthData = (aad && aadLen > 0) ? const_cast<PUCHAR>(aad) : nullptr;
    authInfo.cbAuthData = static_cast<ULONG>(aadLen);
    authInfo.pbTag = const_cast<PUCHAR>(ctAndTag + ptLen);
    authInfo.cbTag = kWireAeadTagBytes;

    outPlaintext.resize(ptLen);
    uint8_t dummy = 0;
    ULONG outLen = 0;
    const NTSTATUS st = BCryptDecrypt(
        keyHandle_,
        (ptLen > 0) ? const_cast<PUCHAR>(ctAndTag) : &dummy,
        static_cast<ULONG>(ptLen),
        &authInfo,
        nullptr,
        0,
        (ptLen > 0) ? outPlaintext.data() : &dummy,
        static_cast<ULONG>(ptLen),
        &outLen,
        0);
    if (!NtOk(st) || outLen != ptLen) {
        outPlaintext.clear();
        return false;
    }
    ++seq_;
    return true;
}

} // namespace zerocam::crypto
