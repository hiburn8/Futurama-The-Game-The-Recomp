// MCPX voice processor model; see Vp.h. Hardware hypotheses (L1-L10, docs/analysis/audio_port.md 5.4) are marked
// where they are used; nothing else in the port depends on them.
#include "audio/vp/Vp.h"

#include "audio/Wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fut::audio::vp {

// ---- memory ---------------------------------------------------------------------------------------------------------
u32 VpMemoryMap::Map(const u8* data, u32 bytes, u32 pageOffset) {
    // first fit from next_ (the base) in the gaps between the live mappings, one unmapped page after each: the
    // address space of unmapped buffers is reused
    pageOffset &= 0xFFF;
    const u64 span = ((u64(pageOffset) + bytes + 0xFFF) & ~u64(0xFFF)) + 0x1000;
    u64 at = next_;
    for (const auto& [start, r] : ranges_) {
        const u64 page = start & ~u64(0xFFF);
        if (page >= at + span) break;
        const u64 end = ((u64(start) + r.bytes + 0xFFF) & ~u64(0xFFF)) + 0x1000;
        if (end > at) at = end;
    }
    const u32 phys = static_cast<u32>(at) + pageOffset;
    ranges_[phys] = {data, bytes};
    return phys;
}

void VpMemoryMap::MapAt(u32 phys, const u8* data, u32 bytes) { ranges_[phys] = {data, bytes}; }

void VpMemoryMap::Unmap(u32 phys) { ranges_.erase(phys); }

bool VpMemoryMap::Read(u32 phys, u8* dst, u32 n) const {
    std::memset(dst, 0, n);
    auto it = ranges_.upper_bound(phys);
    if (it == ranges_.begin()) return false;
    --it;
    const u32 off = phys - it->first;
    if (off >= it->second.bytes) return false;
    const u32 k = std::min(n, it->second.bytes - off);
    std::memcpy(dst, it->second.data + off, k);
    return k == n;
}

// ---- hardware hypotheses --------------------------------------------------------------------------------------------
namespace {

// L2: the resampler's step: 2^(pitch / 4096) samples per output sample, 32.32 fixed point.
u64 PitchStep(s32 pitch) { return static_cast<u64>(std::llround(std::exp2(pitch / 4096.0) * 4294967296.0)); }

// L2: linear interpolation between two adjacent samples with `bits` fraction bits.
s32 Interpolate(s32 a, s32 b, u64 pos, u32 bits) {
    const s64 frac = static_cast<s64>((pos >> (32 - bits)) & ((1ull << bits) - 1));
    return static_cast<s32>(a + (((static_cast<s64>(b) - a) * frac) >> bits));
}

// L5: one-pole low-pass y = (1 - a) x + a y[n-1] with the library's Q16 pole a (0 = pass-through; the design
// 0x0024E6DF solves |H(w_ref)|^2 = 10^(level / 1000) for exactly this filter).
s32 OnePole(s32& y, s32 x, u32 a) {
    y += static_cast<s32>((static_cast<s64>(x) - y) * (65536 - static_cast<s64>(a & 0xFFFF)) >> 16);
    return y;
}

// L6: which channel of a stereo voice feeds slot k (even slots left, odd right).
u32 SlotChannel(u32 k, bool stereo) { return stereo ? (k & 1) : 0; }

// L7: a bin's 24-bit value from the frame's sum (sample << 8 scale) and its 3-bit headroom.
s32 BinValue(s64 acc, u32 headroom) {
    return static_cast<s32>(std::clamp<s64>(acc >> headroom, -0x800000, 0x7FFFFF));
}

}  // namespace

// L4: one HRIR tap from its byte (the library uploads the XBE table's bytes unchanged, 0x0024CB6A). The table is
// sign + magnitude (O, docs/analysis/audio_port.md 5.6): no byte 0x80 and none of 0xE0-0xFF among its 68,882 taps,
// the counts falling with the magnitude in both halves.
s32 Vp::HrirTap(u32 b) const {
    if (hw_.hrirSignMagnitude) return (b & 0x80) ? -static_cast<s32>(b & 0x7F) : static_cast<s32>(b & 0x7F);
    return static_cast<s8>(b);
}

Vp::Vp(const VpFrontEnd& fe, const VpMemory& mem, VpHardware hw) : fe_(fe), mem_(mem), hw_(hw) {
    for (u32 a = 0; a < 4096; ++a)                // L1: 1/64 dB steps
        gain_[a] = static_cast<s32>(std::lround(32768.0 * std::pow(10.0, -static_cast<double>(a) / 1280.0)));
    if (hw_.attenuationFFFMutes) gain_[0xFFF] = 0;
}

bool Vp::Ended(u32 v) const {
    const VoiceState& s = fe_.Voice(v);
    return runs_[v % kVoices].generation == s.generation && runs_[v % kVoices].ended;
}

u32 Vp::Position(u32 v) const { return static_cast<u32>(runs_[v % kVoices].pos >> 32); }

bool Vp::ReadBytes(const VoiceRegs& r, u32 byteOffset, u8* dst, u32 n) const {
    // the buffer is linear in the SGE space: address = base + offset, page = SGE[address >> 12] (H: the library
    // writes base = SGE index << 12 | page offset and one SGE per 4 KB page)
    bool ok = true;
    u32 a = r.bufBase + byteOffset;
    while (n) {
        const u32 k = std::min(n, 0x1000 - (a & 0xFFF));
        ok &= mem_.Read(fe_.SgePage(a >> 12) + (a & 0xFFF), dst, k);
        dst += k, a += k, n -= k;
    }
    return ok;
}

// One sample (16-bit scale) of channel `ch` at sample index `index` of the voice's buffer.
s32 Vp::Fetch(u32 v, Run& run, u64 index, u32 ch, const Format& f) {
    const VoiceRegs& r = fe_.Voice(v).r;
    const u32 channels = f.stereo ? 2 : 1;
    if (index > r.bufEbo) {                       // L8: past the end: the loop start, or silence
        if (!f.loop || r.bufLbo > r.bufEbo) return 0;
        index = r.bufLbo + (index - r.bufEbo - 1) % (static_cast<u64>(r.bufEbo) - r.bufLbo + 1);
    }
    const u32 i = static_cast<u32>(index);
    switch (f.container) {
    case 0: {                                     // 8-bit unsigned PCM
        u8 b = 0x80;
        ReadBytes(r, i * channels + ch, &b, 1);
        return (static_cast<s32>(b) - 128) << 8;
    }
    case 1: {                                     // 16-bit PCM
        u8 b[2] = {};
        ReadBytes(r, 2 * (i * channels + ch), b, 2);
        return static_cast<s16>(b[0] | b[1] << 8);
    }
    case 2: {                                     // Xbox ADPCM: 64 samples per 36-byte block per channel
        const s64 block = i / 64;
        if (run.adpcmBlock != block) {
            u8 raw[72] = {};
            ReadBytes(r, static_cast<u32>(block) * 36 * channels, raw, 36 * channels);
            for (u32 c = 0; c < channels; ++c) {
                const u8* groups[8];
                for (u32 g = 0; g < 8; ++g) groups[g] = raw + 4 * channels + (g * channels + c) * 4;
                // L (KNOWN_ISSUES #23): the port's decoder - header sample + nibbles 0..62, shift-add
                // reconstruction; the MCPX's per-nibble arithmetic is not known
                DecodeXboxAdpcmChannel(raw + 4 * c, groups, run.adpcm.data() + c, channels);
            }
            run.adpcmBlock = block;
        }
        return run.adpcm[(i % 64) * channels + ch];
    }
    default:
        return 0;                                 // container 3 (32-bit) is never used by the library here
    }
}

// ---- stream voices (FMT bit 24) ------------------------------------------------------------------------------------
// What the library expects of the VP (H: CMcpxStream 0x0024BBD7 / 0x0024BA21 / 0x0024B656): a stream voice plays the
// segments of SSL A, then those of SSL B, then A again ..., each SSL's `count` segments in order from its base; when
// it has played an SSL it sets that SSL's notifier (voice notifier 0 / 1) to done and raises the notifier interrupt,
// and the library then completes the SSL's packets and programs it anew. The behaviour of the hardware itself is (L):
// - a segment's samples: the low 16 bits of its length word (the library writes the sample count, 0x0024B303);
// - the format: the length word's bits 16-17 (container: 0 8-bit, 1 16-bit, 2 Xbox ADPCM, 3 32-bit) and bit 23 (two
//   channels interleaved), as the library composes them (0x0024BDA0); FMT's own format bits are not read;
// - the position is continuous across segments and SSLs (the resampler's fraction carries over); the interpolation
//   partner of a segment's last sample is the next segment's first (0 when there is none);
// - starving: an SSL the voice reaches with no segments holds the voice silent at that point (the position does not
//   advance) until the library programs it (it pauses the voice itself, 0x0024BCBB's 0x400);
// - the register values are read when used (an SSL's count when the voice enters it or plays it).
namespace {

u32 SslBase(u32 word) { return (word >> 8) & 0xFFFF; }
u32 SslCount(u32 word) { return word & 0xFF; }
u32 SegmentSamples(const SslSegment& g) { return g.length & 0xFFFF; }

}  // namespace

bool Vp::StreamSegment(u32 v, Run& run) {
    const VoiceRegs& r = fe_.Voice(v).r;
    for (u32 guard = 0; guard < 2 * 16 + 2; ++guard) {
        const u32 count = SslCount(r.ssl[run.ssl & 1]);
        if (run.segment >= count) {
            if (count == 0 && run.segment == 0) return false;   // starved: wait for the library (L)
            // the SSL is played: its notifier done, the interrupt, the other SSL from its first segment
            if (notifiers_) notifiers_->SetStatus(v, run.ssl & 1, 1);
            interrupt_ = true;
            run.ssl ^= 1;
            run.segment = 0;
            continue;
        }
        const u64 len = SegmentSamples(fe_.Segment(SslBase(r.ssl[run.ssl & 1]) + run.segment));
        if ((run.pos >> 32) < len) return true;
        run.pos -= len << 32;
        ++run.segment;
    }
    return false;
}

s32 Vp::StreamFetch(u32 v, Run& run, u32 index0, u64 index, u32 ch) {
    const VoiceRegs& r = fe_.Voice(v).r;
    u32 segIndex = index0;
    const SslSegment* g = &fe_.Segment(segIndex);
    if (index >= SegmentSamples(*g)) {                // the next segment's first samples (L)
        index -= SegmentSamples(*g);
        if (run.segment + 1 < SslCount(r.ssl[run.ssl & 1])) {
            segIndex = index0 + 1;
        } else if (SslCount(r.ssl[(run.ssl & 1) ^ 1])) {
            segIndex = SslBase(r.ssl[(run.ssl & 1) ^ 1]);
        } else {
            return 0;
        }
        g = &fe_.Segment(segIndex);
        if (index >= SegmentSamples(*g)) return 0;
    }
    const u32 container = (g->length >> 16) & 3;
    const u32 channels = (g->length >> 23) & 1 ? 2 : 1;
    const u32 i = static_cast<u32>(index);
    switch (container) {
    case 0: {
        u8 b = 0x80;
        mem_.Read(g->offset + i * channels + ch % channels, &b, 1);
        return (static_cast<s32>(b) - 128) << 8;
    }
    case 1: {
        u8 b[2] = {};
        mem_.Read(g->offset + 2 * (i * channels + ch % channels), b, 2);
        return static_cast<s16>(b[0] | b[1] << 8);
    }
    case 2: {
        const s64 key = static_cast<s64>(segIndex) << 24 | (i / 64);
        if (run.adpcmBlock != key) {
            u8 raw[72] = {};
            mem_.Read(g->offset + (i / 64) * 36 * channels, raw, 36 * channels);
            for (u32 c = 0; c < channels; ++c) {
                const u8* groups[8];
                for (u32 k = 0; k < 8; ++k) groups[k] = raw + 4 * channels + (k * channels + c) * 4;
                DecodeXboxAdpcmChannel(raw + 4 * c, groups, run.adpcm.data() + c, channels);   // (L, as Fetch)
            }
            run.adpcmBlock = key;
        }
        return run.adpcm[(i % 64) * channels + ch % channels];
    }
    default:
        return 0;
    }
}

void Vp::RenderVoice(u32 v, Run& run, std::array<std::array<s64, kFrame>, kBins>& acc) {
    const VoiceState& st = fe_.Voice(v);
    const VoiceRegs& r = st.r;
    if (run.generation != st.generation) {        // a new VOICE_ON: fresh state, CBO (or 0) as the start
        run = Run{};
        run.generation = st.generation;
        run.pos = static_cast<u64>(st.cboWritten ? r.bufCbo : 0) << 32;
    }
    if (!st.on || st.paused || run.ended) return;
    ++running_;
    const Format f = DecodeFormat(r.fmt);
    const bool stereo = f.stereo;
    const u64 step = PitchStep(PitchValue(r));    // L9: the target registers apply from the frame's start
    u8 bins[8];
    u16 att[8];
    SlotBins(r, bins);
    SlotAttenuations(r, att);
    s32 gain[8];
    for (u32 k = 0; k < 8; ++k) gain[k] = gain_[att[k]];
    // HRTF (3D voices): the HRIR entry's 31 tap pairs and the signed interaural delay (H: the upload 0x0024CB6A)
    const bool hrtf = r.hrtf != kNoHrtf;
    s32 tapL[31] = {}, tapR[31] = {};
    s32 itd = 0;
    if (hrtf) {
        const auto& w = fe_.Hrir(r.hrtf);
        for (u32 i = 0; i < 15; ++i) {
            tapL[2 * i] = HrirTap(w[i] & 0xFF);
            tapR[2 * i] = HrirTap((w[i] >> 8) & 0xFF);
            tapL[2 * i + 1] = HrirTap((w[i] >> 16) & 0xFF);
            tapR[2 * i + 1] = HrirTap((w[i] >> 24) & 0xFF);
        }
        tapL[30] = HrirTap(w[15] & 0xFF);
        tapR[30] = HrirTap((w[15] >> 8) & 0xFF);
        itd = static_cast<s32>(w[15]) >> 25;
    }
    // L4: which ear is delayed by a positive ITD; the ITD unit is one 48 kHz sample
    const u32 delayL = (itd > 0) == hw_.positiveItdDelaysLeft && itd != 0 ? static_cast<u32>(std::abs(itd)) : 0;
    const u32 delayR = itd != 0 && delayL == 0 ? static_cast<u32>(std::abs(itd)) : 0;
    const u32 filterMode = (r.misc >> 16) & 3;    // 3 = I3DL2 (FCB: direct HF low, room HF high) (O / L5)
    const u32 bits = hw_.interpolationBits;
    const bool streamVoice = (r.fmt & kFmtStream) != 0;
    for (u32 i = 0; i < kFrame; ++i) {
        u64 idx = run.pos >> 32;
        s32 s[2];
        if (streamVoice) {
            if (!StreamSegment(v, run)) return;       // starved (L): silent, the position held
            idx = run.pos >> 32;
            const u32 seg = SslBase(r.ssl[run.ssl & 1]) + run.segment;
            for (u32 c = 0; c < (stereo ? 2u : 1u); ++c)
                s[c] = Interpolate(StreamFetch(v, run, seg, idx, c), StreamFetch(v, run, seg, idx + 1, c), run.pos, bits);
        } else if (idx > r.bufEbo) {
            if (!f.loop || r.bufLbo > r.bufEbo) {  // L8: a one-shot voice stops after its last sample
                run.ended = true;
                return;
            }
            const u64 len = static_cast<u64>(r.bufEbo - r.bufLbo) + 1;   // L8: a loop restarts at LBO
            run.pos -= ((idx - r.bufLbo) / len) * (len << 32);
            idx = run.pos >> 32;
        }
        if (!streamVoice)
            for (u32 c = 0; c < (stereo ? 2u : 1u); ++c)
                s[c] = Interpolate(Fetch(v, run, idx, c, f), Fetch(v, run, idx + 1, c, f), run.pos, bits);
        if (!stereo) s[1] = s[0];
        if (levels_)
            for (u32 c = 0; c < (stereo ? 2u : 1u); ++c) (hrtf ? levels_->fetched3D : levels_->fetched2D).Add(s[c], 0x8000);
        run.pos += step;
        s32 ear[2] = {s[0], s[0]};
        if (hrtf) {
            run.fir[run.firPos & 127] = s[0];
            s64 l = 0, rr = 0;
            for (u32 j = 0; j < 31; ++j) {
                l += static_cast<s64>(tapL[j]) * run.fir[(run.firPos - j - delayL) & 127];
                rr += static_cast<s64>(tapR[j]) * run.fir[(run.firPos - j - delayR) & 127];
            }
            ++run.firPos;
            ear[0] = static_cast<s32>(l >> hw_.hrirShift);
            ear[1] = static_cast<s32>(rr >> hw_.hrirShift);
            if (levels_) levels_->hrtf.Add(ear[0], 0x8000), levels_->hrtf.Add(ear[1], 0x8000);
        }
        for (u32 k = 0; k < 8; ++k) {
            if (gain[k] == 0) continue;
            s32 x = s[SlotChannel(k, stereo)];
            if (hrtf && k < 4) x = ear[k >> 1];    // L4: slots 0, 1 = left ear, 2, 3 = right ear
            if (filterMode == 3) {                // L5: slots 0-3 the direct path, slot 4 the room (I3DL2) send
                if (k < 4) x = OnePole(run.lp[k], x, r.fcb & 0xFFFF);
                else if (k == 4) x = OnePole(run.lp[4], x, r.fcb >> 16);
            }
            const s64 y = (static_cast<s64>(x) * 256 * gain[k]) >> 15;
            acc[bins[k] & 31][i] += y;
            if (levels_) (hrtf ? levels_->slot3D : levels_->slot2D).Add(y, 0x800000);
        }
    }
}

void Vp::RenderFrame(Bins& out) {
    std::array<std::array<s64, kFrame>, kBins> acc{};
    running_ = 0;
    for (u32 v = 0; v < kVoices; ++v) RenderVoice(v, runs_[v], acc);   // L10: one pass, voices in index order
    for (u32 b = 0; b < kBins; ++b)
        for (u32 i = 0; i < kFrame; ++i) out[b][i] = BinValue(acc[b][i], fe_.Headroom(b));
    if (levels_) {
        levels_->maxVoices = std::max(levels_->maxVoices, running_);
        for (u32 b = 0; b < kBins; ++b)
            for (u32 i = 0; i < kFrame; ++i) {
                levels_->sums[b].Add(acc[b][i], 0x800000);
                levels_->bins[b].Add(out[b][i], 0x800000);
            }
    }
    ++frames_;
}

}  // namespace fut::audio::vp
