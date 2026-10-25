// RIFF WAVE files of the disc (Phase 3 audio). Sound effects are Xbox ADPCM (format 0x69: 36-byte blocks per channel,
// 64 samples per block, mono or stereo, 11-48 kHz); the music and a few effects are 16-bit PCM. The Xbox decodes
// ADPCM in the audio hardware (MCPX); the port decodes to 16-bit PCM on load.
#pragma once

#include "core/Types.h"

#include <span>
#include <string>
#include <vector>

namespace fut::audio {

struct WavInfo {
    u16 format = 0;                  // 1 PCM, 0x69 Xbox ADPCM
    u16 channels = 0;
    u32 sampleRate = 0;
    u16 blockAlign = 0, bitsPerSample = 0;
    std::span<const u8> data;        // the "data" chunk (inside the file bytes)
};

struct Pcm16 {
    u16 channels = 0;
    u32 sampleRate = 0;
    std::vector<s16> samples;        // interleaved
    u32 Frames() const { return channels ? static_cast<u32>(samples.size() / channels) : 0; }
};

bool ParseWav(std::span<const u8> file, WavInfo& out, std::string* error = nullptr);
// Decodes a WAVE data chunk of the given format (1 PCM 8 / 16 bit, 0x69 Xbox ADPCM) to interleaved 16-bit PCM.
bool DecodeData(u16 format, u16 channels, u16 bitsPerSample, std::span<const u8> data, std::vector<s16>& out);
bool DecodeWav(std::span<const u8> file, Pcm16& out, std::string* error = nullptr);

// One Xbox ADPCM block of one channel (4-byte header + 32 bytes; stereo blocks interleave 4-byte groups, handled
// by DecodeWav). Writes 64 samples to out[0], out[stride], ... Hypothesis (no hardware oracle): the header sample
// is the first output sample and nibbles 0..62 give the other 63; nibble 63 is unused.
void DecodeXboxAdpcmChannel(const u8* header, const u8* const* groups, s16* out, size_t stride);

}  // namespace fut::audio
