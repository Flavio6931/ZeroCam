#include "AnnexBParser.h"
#include <cstring>

namespace zerocam {

namespace {

// Busca la posición del siguiente start code Annex-B (00 00 01 o 00 00 00 01) a partir de startIdx.
bool FindStartCode(const uint8_t* data, size_t len, size_t startIdx, size_t& scPos, size_t& scLen) {
    for (size_t i = startIdx; i + 3 < len; ++i) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                scPos = i;
                scLen = 3;
                return true;
            }
            if (i + 4 <= len && data[i + 2] == 0 && data[i + 3] == 1) {
                scPos = i;
                scLen = 4;
                return true;
            }
        }
    }
    return false;
}

} // namespace

void AnnexBParser::Reset() {
    streamBuf_.clear();
    currentAu_.clear();
    lastSps_.clear();
    lastPps_.clear();
    hasVclInAu_ = false;
    hasKeyframeInAu_ = false;
    hasSpsPpsInAu_ = false;
}

void AnnexBParser::Feed(const uint8_t* bytes, size_t size, int64_t ptsUs, const AccessUnitCallback& onAu) {
    if (!bytes || size == 0) return;
    streamBuf_.insert(streamBuf_.end(), bytes, bytes + size);

    size_t firstPos = 0, firstLen = 0;
    if (!FindStartCode(streamBuf_.data(), streamBuf_.size(), 0, firstPos, firstLen)) {
        if (streamBuf_.size() > 4u * 1024u * 1024u) {
            streamBuf_.clear();
        }
        return;
    }

    size_t curStart = firstPos;
    size_t curScLen = firstLen;
    while (true) {
        size_t nextPos = 0, nextScLen = 0;
        if (!FindStartCode(streamBuf_.data(), streamBuf_.size(), curStart + curScLen, nextPos, nextScLen)) {
            break;
        }
        HandleNal(streamBuf_.data() + curStart, nextPos - curStart, ptsUs, onAu);
        curStart = nextPos;
        curScLen = nextScLen;
    }

    if (curStart > 0) {
        streamBuf_.erase(streamBuf_.begin(), streamBuf_.begin() + static_cast<ptrdiff_t>(curStart));
    }
}

void AnnexBParser::Flush(int64_t ptsUs, const AccessUnitCallback& onAu) {
    size_t firstPos = 0, firstLen = 0;
    if (FindStartCode(streamBuf_.data(), streamBuf_.size(), 0, firstPos, firstLen)) {
        HandleNal(streamBuf_.data() + firstPos, streamBuf_.size() - firstPos, ptsUs, onAu);
    }
    streamBuf_.clear();
    EmitCurrentAu(ptsUs, onAu);
}

void AnnexBParser::HandleNal(const uint8_t* nalWithStart, size_t nalLen, int64_t ptsUs, const AccessUnitCallback& onAu) {
    const size_t scLen = (nalLen >= 4 && nalWithStart[2] == 0 && nalWithStart[3] == 1) ? 4 : 3;
    if (nalLen <= scLen + 1) return;

    const uint8_t* payload = nalWithStart + scLen;
    const size_t payloadLen = nalLen - scLen;
    const uint8_t nalType = payload[0] & 0x1F;
    const bool isVcl = (nalType >= 1 && nalType <= 5);
    const bool isNewFrameStart = (nalType == 9 || nalType == 7 || nalType == 8) ||
                                 (isVcl && payloadLen > 1 && (payload[1] & 0x80) != 0);

    if (hasVclInAu_ && isNewFrameStart) {
        EmitCurrentAu(ptsUs, onAu);
    }

    static constexpr uint8_t kStartCode4[4] = { 0, 0, 0, 1 };
    if (nalType == 7) {
        lastSps_.assign(kStartCode4, kStartCode4 + 4);
        lastSps_.insert(lastSps_.end(), payload, payload + payloadLen);
        hasSpsPpsInAu_ = true;
    } else if (nalType == 8) {
        lastPps_.assign(kStartCode4, kStartCode4 + 4);
        lastPps_.insert(lastPps_.end(), payload, payload + payloadLen);
        hasSpsPpsInAu_ = true;
    }

    currentAu_.insert(currentAu_.end(), kStartCode4, kStartCode4 + 4);
    currentAu_.insert(currentAu_.end(), payload, payload + payloadLen);

    if (isVcl) {
        hasVclInAu_ = true;
        if (nalType == 5) hasKeyframeInAu_ = true;
    }
}

void AnnexBParser::EmitCurrentAu(int64_t ptsUs, const AccessUnitCallback& onAu) {
    if (!hasVclInAu_ || currentAu_.empty()) {
        return;
    }
    if (hasKeyframeInAu_ && !hasSpsPpsInAu_ && !lastSps_.empty() && !lastPps_.empty()) {
        std::vector<uint8_t> prefixed;
        prefixed.reserve(lastSps_.size() + lastPps_.size() + currentAu_.size());
        prefixed.insert(prefixed.end(), lastSps_.begin(), lastSps_.end());
        prefixed.insert(prefixed.end(), lastPps_.begin(), lastPps_.end());
        prefixed.insert(prefixed.end(), currentAu_.begin(), currentAu_.end());
        if (onAu) onAu(prefixed.data(), prefixed.size(), ptsUs, true);
    } else {
        if (onAu) onAu(currentAu_.data(), currentAu_.size(), ptsUs, hasKeyframeInAu_);
    }

    currentAu_.clear();
    hasVclInAu_ = false;
    hasKeyframeInAu_ = false;
    hasSpsPpsInAu_ = false;
}

void AnnexBParser::NormalizeAccessUnit(const uint8_t* data, size_t size, std::vector<uint8_t>& outBuffer, bool& outKeyframe) {
    outBuffer.clear();
    outKeyframe = false;
    if (!data || size < 4) return;

    bool hasSpsPps = false;
    size_t pos = 0, scLen = 0;
    while (FindStartCode(data, size, pos, pos, scLen)) {
        size_t nextPos = 0, nextSc = 0;
        const size_t nalEnd = FindStartCode(data, size, pos + scLen, nextPos, nextSc) ? nextPos : size;
        if (pos + scLen < nalEnd) {
            const uint8_t* nal = data + pos + scLen;
            const size_t nLen = nalEnd - (pos + scLen);
            const uint8_t t = nal[0] & 0x1F;
            static constexpr uint8_t kSc4[4] = { 0, 0, 0, 1 };
            if (t == 7) {
                lastSps_.assign(kSc4, kSc4 + 4);
                lastSps_.insert(lastSps_.end(), nal, nal + nLen);
                hasSpsPps = true;
            } else if (t == 8) {
                lastPps_.assign(kSc4, kSc4 + 4);
                lastPps_.insert(lastPps_.end(), nal, nal + nLen);
                hasSpsPps = true;
            } else if (t == 5) {
                outKeyframe = true;
            }
        }
        pos = nalEnd;
        if (pos >= size) break;
    }

    if (outKeyframe && !hasSpsPps && !lastSps_.empty() && !lastPps_.empty()) {
        outBuffer.reserve(lastSps_.size() + lastPps_.size() + size);
        outBuffer.insert(outBuffer.end(), lastSps_.begin(), lastSps_.end());
        outBuffer.insert(outBuffer.end(), lastPps_.begin(), lastPps_.end());
        outBuffer.insert(outBuffer.end(), data, data + size);
    } else {
        outBuffer.assign(data, data + size);
    }
}

} // namespace zerocam
