// The math of the XDK 5233 DirectSound library statically linked into default.xbe (section DSOUND,
// 0x00246DA0-0x002643F4): the 3D calculator of the full-HRTF algorithm (DirectSoundUseFullHRTF), the voice register
// packing (volume / mix bins / pitch), XAudioCalculatePitch, the I3DL2 source levels and filter coefficients and the
// I3DL2 listener -> reverb state conversion. Analysis: docs/analysis/audio_port.md 2. Every function is ported
// literally in x87 order (53-bit precision, KNOWN_ISSUES #10): x87 intermediates are doubles, float stores are f32
// casts, CVTTSS2SI / _ftol truncate, FISTP / FRNDINT round to nearest. Verified bit for bit against the original
// under Unicorn (tools/difftest/dsound_math_oracle.py, tests/audio/DSoundMathTests.cpp).
//
// Transcendentals (FYL2X, FCOS, F2XM1) are the C library's double functions; the x87 instructions return extended
// results that are then rounded to double / float, so a difference would need a value within an ulp of a rounding
// boundary (not seen in the oracle's cases). Under Unicorn (QEMU 5) the same instructions are host doubles.
//
// Objects are plain structs with the original's fields in the original order; pointers between DirectSound objects
// are explicit arguments. HRTF filters are identified by index (orig: addresses, see HrtfFilter).
#pragma once

#include "core/Types.h"

#include <vector>

namespace fut::audio::xds {

// DS3DLISTENER (DirectSound settings +0x34; default 0x00263878).
struct Listener {
    u32 size = 0x40;
    f32 position[3] = {0, 0, 0};                 // +0x04
    f32 velocity[3] = {0, 0, 0};                 // +0x10
    f32 front[3] = {0, 0, 1};                    // +0x1C
    f32 top[3] = {0, 1, 0};                      // +0x28
    f32 distanceFactor = 1, rolloffFactor = 1, dopplerFactor = 1;   // +0x34 / +0x38 / +0x3C
};

// The 3D algorithm's listener block (DirectSound settings +0x58): the listener, its right vector (top x front,
// recomputed by ListenerRightVector) and the rear-speaker flag (+0x10: the console's speaker configuration has
// surround / AC3 outputs, docs/analysis/audio_port.md 2.4.6).
struct ListenerFrame {
    const Listener* listener = nullptr;          // +0x00
    f32 right[3] = {1, 0, 0};                    // +0x04
    u8 rearSpeakers = 0;                         // +0x10
};

// DSI3DL2LISTENER (default 0x00263848; the game's five presets are at 0x002D6924, 0x34 apart).
struct I3DL2Listener {
    s32 room = -1000, roomHF = -100;             // +0x00 / +0x04
    f32 roomRolloffFactor = 0;                   // +0x08
    f32 decayTime = 1.49f, decayHFRatio = 0.83f; // +0x0C / +0x10
    s32 reflections = -2602;                     // +0x14
    f32 reflectionsDelay = 0.007f;               // +0x18
    s32 reverb = 200;                            // +0x1C
    f32 reverbDelay = 0.011f;                    // +0x20
    f32 diffusion = 100, density = 100;          // +0x24 / +0x28
    f32 hfReference = 5000;                      // +0x2C
};

// DSI3DL2BUFFER (default 0x002638B8).
struct I3DL2Source {
    s32 direct = 0, directHF = 0, room = 0, roomHF = 0;   // +0x00..+0x0C
    f32 roomRolloffFactor = 0;                   // +0x10
    s32 obstructionHF = 0;                       // +0x14
    f32 obstructionLFRatio = 0;                  // +0x18
    s32 occlusionHF = 0;                         // +0x1C
    f32 occlusionLFRatio = 0.25f;                // +0x20
};

// A 3D voice's parameter block (0x7C bytes, voice settings +0xB4): DS3DBUFFER (defaults 0x002638E0), the I3DL2
// source, the rolloff curve (SetRolloffCurve stores the caller's pointer, not a copy).
struct Buffer3D {
    u32 size = 0x4C;
    f32 position[3] = {0, 0, 0};                 // +0x04
    f32 velocity[3] = {0, 0, 0};                 // +0x10
    u32 insideConeAngle = 360, outsideConeAngle = 360;   // +0x1C / +0x20
    f32 coneOrientation[3] = {0, 0, 1};          // +0x24
    s32 coneOutsideVolume = 0;                   // +0x30
    f32 minDistance = 1, maxDistance = 1e9f;     // +0x34 / +0x38
    u32 mode = 0;                                // +0x3C DS3DMODE_NORMAL / HEADRELATIVE 1 / DISABLE 2
    f32 distanceFactor = 1, rolloffFactor = 1, dopplerFactor = 1;   // +0x40 / +0x44 / +0x48
    I3DL2Source i3dl2;                           // +0x4C
    const f32* curve = nullptr;                  // +0x70
    u32 curveCount = 0;                          // +0x74
};

// HRTF filter identifiers: index k of the 32-byte filters at 0x002513A0 (31 taps + delay byte; 2,222 entries), or
// the original's other two pointer values.
constexpr u32 kHrtfNone = 0xFFFFFFFFu;           // NULL (the source ctor's zero fill)
constexpr u32 kHrtfDefault = 0xFFFFFFFEu;        // 0x00263A38, the filter of voices without 3D
constexpr u32 kHrtfFilterCount = 2222;
constexpr u32 kHrtfFilterBase = 0x002513A0;

// CFullHrtfSource (0x54 bytes, ctor 0x0024989E zero-fills): the calculator's per-voice results.
struct Source {
    f32 direction[3] = {0, 0, 0};                // +0x00 unit vector listener -> source
    f32 distance = 0;                            // +0x0C
    f32 azimuth = 0, elevation = 0;              // +0x10 / +0x14 degrees (piecewise-linear arctangent)
    f32 coneAngle = 0;                           // +0x18
    u32 dirty = 0;                               // +0x1C register-update bits (1 distance, 2 cone, 4 front/rear,
                                                 //   8 centre, 0x10 Doppler, 0x20 HRTF pair)
    s32 distanceAtt = 0, coneAtt = 0;            // +0x20 / +0x24 hundredths of dB
    s32 front = 0, rear = 0;                     // +0x28 / +0x2C
    s32 centreFront = 0, centre = 0;             // +0x30 / +0x34
    s32 doppler = 0;                             // +0x38 pitch, 1/4096 octave
    u32 hrtf0 = kHrtfNone, hrtf1 = kHrtfNone;    // +0x3C / +0x40
    u32 centreFlag = 0;                          // +0x44 slot 4 is the centre bin (set by MixBinRegisters)
};

// DSOUND data tables the calculator reads (loaded from the user's default.xbe; not distributed).
struct Tables {
    std::vector<u16> hrtfIndex;                  // 0x00262960: 61 azimuth steps x 31 elevation steps
    std::vector<u8> hrtfFilters;                 // 0x002513A0: 2,222 x 32 bytes
    std::vector<u8> hrtfDefault;                 // 0x00263A38: 32 bytes
    std::vector<f32> centreGain, centreCut;      // 0x002510C0 / 0x00251178: 45 floats each
    // The I3DL2 reverb conversion's tables (ConvertI3DL2Reverb), as float bits: 0x00263A68 4 rows of 5 (the
    // reflection gains, the delay scales / offsets), 0x00263AB8 8 signs, 0x00263AE8 / 0x00263AF8 2 rows of 4 delays.
    std::vector<u32> reverbRows, reverbSigns, reverbDelays;
    bool Loaded() const { return hrtfIndex.size() == 61 * 31; }
    // Reads the tables from default.xbe file bytes; false if the image is not the expected XBE.
    bool LoadFromXbe(const std::vector<u8>& xbe);
};

// ---- the 3D calculator (0x0024A098: stages by mask) -----------------------------------------------------------
// mask bits: 1 direction / distance, 2 azimuth / elevation (+ HRTF pair), 4 distance attenuation, 8 cone angle,
// 0x10 cone attenuation, 0x20 front / rear split, 0x40 centre, 0x80 Doppler. `bufferFlags` are the DSBCAPS flags
// (MUTE3DATMAXDISTANCE 0x20000).
void Calculate3D(u32 mask, Source& s, const ListenerFrame& lf, u32 bufferFlags, const Buffer3D& p, const Tables& t);
// orig: 0x0024984C. Recomputes right = top x front when `listenerDirty` has 0x400 (and the 3D algorithm global
// 0x00263B7C, 0 after DirectSoundUseFullHRTF, is not 2).
void ListenerRightVector(ListenerFrame& lf, u32 listenerDirty, u32 algorithm = 0);

// ---- voice registers ---------------------------------------------------------------------------------------------
// The voice settings fields the register functions read (CDirectSoundVoiceSettings, ctor 0x00248B03).
struct VoiceSettings {
    u32 flags = 0;                               // +0x08 (3D 0x10; mix-in / FX 0x182000)
    u32 sampleRate = 0;                          // +0x10 the format's rate (SetFrequency(0))
    s32 pitch = 0;                               // +0x18 1/4096 octave against 48 kHz
    s32 volume = 0;                              // +0x1C lVolume - headroom
    s32 headroom = 0;                            // +0x20 600 for 2D buffers, 0 for 3D / mix-in / FX
    u32 binCount = 0;                            // +0x24
    u8 bins[8] = {};                             // +0x28
    s32 binVolume[32] = {};                      // +0x30 per mix bin
};
// orig: 0x0024C46A. out[k] / out[3 + k] / out[6 + k]: the VOLA / VOLB / VOLC words of hardware voice k
// (`hwVoices` = 1 mono, 2 stereo); `mode` is the 3D block's DS3DMODE, `s` / `direct`, `room` the 3D results
// (read only for 3D voices with mode != DISABLE).
void VolumeRegisters(const VoiceSettings& v, u32 mode, const Source* s, s32 i3dl2Direct, s32 i3dl2Room, u32 hwVoices,
                     u32* out);
// The 8 slot attenuations (12-bit, 1/64 dB; 0xFFF = the most / unused) from the three words of one voice.
void UnpackVolumes(u32 vola, u32 volb, u32 volc, u16 slots[8]);
// orig: 0x0024C395. The 8 slot bins (the buffer's bins, then the lowest unused bin numbers) packed 5 bits each:
// out0 = slots 0-5, out1 = (out1 & ~0x3FF) | slots 6-7. Sets / clears the 3D source's centre flag.
void MixBinRegisters(const VoiceSettings& v, Source* s, u32& out0, u32& out1, u8 slots[8] = nullptr);
// The pitch-relevant fields of a voice that is the output buffer of another (settings +0xB0 -> +0x20).
struct PitchChain {
    s32 pitch;
    u32 flags, mode;
    s32 doppler;
};
// orig: 0x0024C667. (pitch + Doppler (3D, mode != DISABLE) + the output buffer's) clamped to [-32767, 8191] << 16;
// 0 for mix-in / FX buffers.
u32 PitchRegister(const VoiceSettings& v, u32 mode, s32 doppler, const PitchChain* chain);
// orig: 0x00246F71 XAudioCalculatePitch: 0 for 48000, else round(4096 * log2(f / 48000)). (f = 0 never reaches it:
// SetFrequency(0) uses the buffer's rate; the x87 would give -inf -> 0x80000000.)
s32 CalculatePitch(u32 frequency);
// orig: 0x0024C0C2: FISTP(4096 * log2(ratio)).
s32 PitchFromRatio(f32 ratio);

// ---- I3DL2 ---------------------------------------------------------------------------------------------------------
// The per-voice I3DL2 block (0x1C bytes; +0x14 / +0x18 point at the listener / source parameters).
struct I3DL2Voice {
    u32 dirty = 0;                               // 1 direct, 2 room, 4 direct HF coefficient, 8 room HF coefficient
    s32 direct = 0, room = 0;
    u32 directHFCoef = 0, roomHFCoef = 0;        // one-pole coefficients, 16-bit
};
// orig: 0x0024E875 (thiscall, distance argument)
void I3DL2SourceLevels(I3DL2Voice& o, const I3DL2Listener& l, const I3DL2Source& s, f32 distance);
// orig: 0x0024E6DF (level, level, frequency, rate) -> coefficient
u32 FilterCoefficient(s32 level, s32 hfLevel, f32 frequency, f32 rate);
// orig: 0x0024E5FD (level, level, frequency, rate) -> (coefficient, gain)
void DecayFilter(s32 level, s32 hfLevel, f32 frequency, f32 rate, f32& coefficient, f32& gain);
// orig: 0x0024E35D: 10^(level / 2000) through F2XM1 / FSCALE, as a float.
f32 Pow10Level(s32 level);
// orig: 0x0024EAB2 (thiscall on the reverb converter, DirectSound settings +0x6C). `conv` holds the converter's 137
// words: [0] the listener pointer (unused here), [1..70] effect 0's DSP state words 0..0x45 as read by the listener
// commit (0x0024A9D2, 0x118 bytes), [71..136] the computed parameters (written back to state +0x118, 0x108 bytes).
// `t`: the reverb tables (Tables::reverbRows / reverbSigns / reverbDelays; 0 while not loaded).
void ConvertI3DL2Reverb(const I3DL2Listener& l, u32* conv, const Tables& t);
// orig: 0x0024E5C6 (also called by the listener commit before the conversion when 0x00263BBC is set; it is 0).
void ReverbResetLengths(u32* conv);

// ---- helpers (literal instruction semantics) -------------------------------------------------------------------
s32 Cvtt(f32 v);                                 // CVTTSS2SI (0x0024E354)
s32 AmplitudeToLevel(f32 g);                     // orig: 0x002497EA 2000 * log10, 0 at >= 1, -10000 at <= 0
s32 PowerToLevel(f32 g);                         // orig: 0x002497A3 1000 * log10

}  // namespace fut::audio::xds
