// WAVE parsing and Xbox ADPCM decoding; see Wav.h.
#include "audio/Wav.h"

#include <algorithm>
#include <cstring>

namespace fut::audio {
namespace {

constexpr s16 kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr s8 kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

u32 U32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}
u16 U16(const u8* p) {
    u16 v;
    std::memcpy(&v, p, 2);
    return v;
}

bool Fail(std::string* error, const char* why) {
    if (error) *error = why;
    return false;
}

}  // namespace

bool ParseWav(std::span<const u8> f, WavInfo& out, std::string* error) {
    out = WavInfo();
    if (f.size() < 12 || std::memcmp(f.data(), "RIFF", 4) != 0 || std::memcmp(f.data() + 8, "WAVE", 4) != 0)
        return Fail(error, "not a RIFF WAVE file");
    bool haveFmt = false;
    for (size_t pos = 12; pos + 8 <= f.size();) {
        const u8* c = f.data() + pos;
        const u32 size = U32(c + 4);
        const size_t body = pos + 8, avail = std::min<size_t>(size, f.size() - body);
        if (std::memcmp(c, "fmt ", 4) == 0 && avail >= 16) {
            out.format = U16(c + 8);
            out.channels = U16(c + 10);
            out.sampleRate = U32(c + 12);
            out.blockAlign = U16(c + 20);
            out.bitsPerSample = U16(c + 22);
            haveFmt = true;
        } else if (std::memcmp(c, "data", 4) == 0) {
            out.data = f.subspan(body, avail);
        }
        pos = body + size + (size & 1);
    }
    if (!haveFmt) return Fail(error, "no fmt chunk");
    if (out.data.empty()) return Fail(error, "no data");
    if (out.channels == 0 || out.channels > 2) return Fail(error, "unsupported channel count");
    return true;
}

void DecodeXboxAdpcmChannel(const u8* header, const u8* const* groups, s16* out, size_t stride) {
    s32 predictor = static_cast<s16>(U16(header));
    s32 index = std::clamp<s32>(header[2], 0, 88);
    out[0] = static_cast<s16>(predictor);
    for (u32 n = 0; n < 63; ++n) {
        const u8 byte = groups[n / 8][(n % 8) / 2];
        const u8 nibble = (n & 1) ? byte >> 4 : byte & 0xF;
        const s32 step = kStepTable[index];
        s32 diff = step >> 3;
        if (nibble & 4) diff += step;
        if (nibble & 2) diff += step >> 1;
        if (nibble & 1) diff += step >> 2;
        predictor = std::clamp(nibble & 8 ? predictor - diff : predictor + diff, -32768, 32767);
        index = std::clamp(index + kIndexTable[nibble], 0, 88);
        out[(n + 1) * stride] = static_cast<s16>(predictor);
    }
}

bool DecodeData(u16 format, u16 channels, u16 bits, std::span<const u8> data, std::vector<s16>& out) {
    out.clear();
    if (channels == 0 || channels > 2) return false;
    if (format == 1) {
        if (bits == 16) {
            out.resize(data.size() / 2);
            std::memcpy(out.data(), data.data(), out.size() * 2);
        } else if (bits == 8) {
            out.resize(data.size());
            for (size_t i = 0; i < data.size(); ++i) out[i] = static_cast<s16>((data[i] - 128) << 8);
        } else {
            return false;
        }
        out.resize(out.size() / channels * channels);
        return true;
    }
    if (format != 0x69) return false;
    const size_t blockBytes = 36u * channels;
    const size_t blocks = data.size() / blockBytes;
    out.resize(blocks * 64 * channels);
    for (size_t b = 0; b < blocks; ++b) {
        const u8* blk = data.data() + b * blockBytes;
        for (u16 ch = 0; ch < channels; ++ch) {
            // Headers first (4 bytes per channel), then 4-byte groups of 8 nibbles, interleaved by channel.
            const u8* groups[8];
            for (int g = 0; g < 8; ++g) groups[g] = blk + 4u * channels + (static_cast<size_t>(g) * channels + ch) * 4;
            DecodeXboxAdpcmChannel(blk + 4u * ch, groups, &out[(b * 64) * channels + ch], channels);
        }
    }
    return true;
}

bool DecodeWav(std::span<const u8> file, Pcm16& out, std::string* error) {
    WavInfo w;
    if (!ParseWav(file, w, error)) return false;
    out = Pcm16();
    out.channels = w.channels;
    out.sampleRate = w.sampleRate;
    if (!DecodeData(w.format, w.channels, w.bitsPerSample, w.data, out.samples))
        return Fail(error, "unsupported format");
    return true;
}

}  // namespace fut::audio
