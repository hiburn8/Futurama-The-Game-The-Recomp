// MCPX VP front end (PIO methods -> voice state); see VpFrontEnd.h.
#include "audio/vp/VpFrontEnd.h"

namespace fut::audio::vp {

Format DecodeFormat(u32 f) {
    Format o;
    o.bin6 = f & 31;
    o.bin7 = (f >> 5) & 31;
    o.samplesPerBlock = (f >> 16) & 31;
    o.linked = (f >> 22) & 1;
    o.loop = (f >> 25) & 1;
    o.stereo = (f >> 27) & 1;
    o.sampleSize = (f >> 28) & 3;
    o.container = (f >> 30) & 3;
    return o;
}

const u32 kVbinShift[6] = {0, 5, 10, 16, 21, 26};   // (O: bit 15 is skipped)

void SlotBins(const VoiceRegs& r, u8 bins[8]) {
    for (u32 i = 0; i < 6; ++i) bins[i] = static_cast<u8>((r.vbin >> kVbinShift[i]) & 31);
    bins[6] = static_cast<u8>(r.fmt & 31);
    bins[7] = static_cast<u8>((r.fmt >> 5) & 31);
}

// VOLA = (v7[3:0] | v1 << 4) << 16 | v0 << 4 | v6[3:0]; VOLB likewise with v7[7:4], v3, v6[7:4], v2; VOLC with
// v7[11:8], v5, v6[11:8], v4 (H: the library's packing 0x0024C46A).
void SlotAttenuations(const VoiceRegs& r, u16 a[8]) {
    u32 v6 = 0, v7 = 0;
    for (u32 k = 0; k < 3; ++k) {
        const u32 w = r.vol[k];
        a[2 * k] = static_cast<u16>((w >> 4) & 0xFFF);
        a[2 * k + 1] = static_cast<u16>((w >> 20) & 0xFFF);
        v6 |= (w & 0xF) << (4 * k);
        v7 |= ((w >> 16) & 0xF) << (4 * k);
    }
    a[6] = static_cast<u16>(v6);
    a[7] = static_cast<u16>(v7);
}

s32 PitchValue(const VoiceRegs& r) { return static_cast<s32>(r.pitch) >> 16; }

VpFrontEnd::VpFrontEnd() { Reset(); }

void VpFrontEnd::Reset() {
    voices_.fill(VoiceState{});
    hrir_.assign(kHrirEntries, {});
    sge_.assign(kModelSges, 0);
    ssl_.assign(kSslSegments, {});
    headroom_.fill(0);
    hrtfHeadroom_ = hrtfSubmixes_ = 0;
    current_ = currentHrir_ = currentSge_ = antecedent_ = currentSsl_ = 0;
    writes_ = unknown_ = 0;
}

void VpFrontEnd::Write(u32 offset, u32 value) {
    if (offset >= 0x20000 && offset < 0x30000) offset -= 0x20000;   // accept APU-relative offsets too
    ++writes_;
    VoiceState& v = voices_[current_ % kVoices];
    if (offset >= pio::kHrir && offset < pio::kHrir + 0x40) {
        hrir_[currentHrir_ % kHrirEntries][(offset - pio::kHrir) / 4] = value;
        return;
    }
    if (offset >= pio::kSslSegmentOffset && offset < pio::kSslSegmentOffset + 0x200) {   // 64 segments of the block
        SslSegment& g = ssl_[(currentSsl_ + (offset - pio::kSslSegmentOffset) / 8) % kSslSegments];
        (offset & 4 ? g.length : g.offset) = value;
        return;
    }
    if (offset >= pio::kSetSubmixHeadroom && offset < pio::kSetSubmixHeadroom + 0x80) {
        headroom_[(offset - pio::kSetSubmixHeadroom) / 4] = value & 7;
        return;
    }
    switch (offset) {
    case pio::kVoiceListHead: case pio::kVoiceListLock: break;   // (list maintenance; no rendering effect, L)
    case pio::kSetAntecedentVoice: antecedent_ = value; break;
    case pio::kVoiceOn: {
        VoiceState& t = voices_[(value & 0xFFFF) % kVoices];
        t.on = true;
        t.paused = true;
        t.list = antecedent_ >> 16;
        t.cboWritten = false;
        ++t.generation;
        break;
    }
    case pio::kVoiceOff: case pio::kVoiceRelease: voices_[(value & 0xFFFF) % kVoices].on = false; break;
    case pio::kVoicePause: voices_[(value & 0xFFFF) % kVoices].paused = ((value >> 18) & 1) != 0; break;
    case pio::kSetCurrentHrtfEntry: currentHrir_ = value & 0xFFFF; break;
    case pio::kSetHrtfHeadroom: hrtfHeadroom_ = value & 7; break;
    case pio::kSetHrtfSubmixes: hrtfSubmixes_ = value; break;
    case pio::kSetCurrentVoice: current_ = value & 0xFFFF; break;
    case pio::kSetCurrentSsl: currentSsl_ = value & 0x3FFFC0; break;
    case pio::kVoiceSslA: v.r.ssl[0] = value; break;
    case pio::kVoiceSslB: v.r.ssl[1] = value; break;
    case pio::kVoiceLock: break;
    case pio::kVbin: v.r.vbin = value; break;
    case pio::kFmt: v.r.fmt = value; break;
    case pio::kEnv0: v.r.env0 = value; break;
    case pio::kEnvA: v.r.envA = value; break;
    case pio::kEnv1: v.r.env1 = value; break;
    case pio::kEnvF: v.r.envF = value; break;
    case pio::kMisc: v.r.misc = value; break;
    case pio::kHrtf: v.r.hrtf = value & 0xFFFF; break;
    case pio::kLfoEnvA: v.r.lfoEnvA = value; break;
    case pio::kLfoEnvB: v.r.lfoEnvB = value; break;
    case pio::kLfoMod: v.r.lfoMod = value; break;
    case pio::kVolA: v.r.vol[0] = value; break;
    case pio::kVolB: v.r.vol[1] = value; break;
    case pio::kVolC: v.r.vol[2] = value; break;
    case pio::kFca: v.r.fca = value; break;
    case pio::kFcb: v.r.fcb = value; break;
    case pio::kPitch: v.r.pitch = value; break;
    case pio::kBufBase: v.r.bufBase = value; break;
    case pio::kBufLbo: v.r.bufLbo = value; break;
    case pio::kBufCbo: v.r.bufCbo = value; v.cboWritten = true; break;
    case pio::kBufEbo: v.r.bufEbo = value; break;
    case pio::kSetInbufSge: currentSge_ = value; break;
    case pio::kSetInbufSgeOffset: sge_[currentSge_ % kModelSges] = value; break;
    default:
        // 0x2A0-0x2B0 (written 0xFFF once at init, O) and anything else: kept nowhere
        ++unknown_;
        break;
    }
}

}  // namespace fut::audio::vp
