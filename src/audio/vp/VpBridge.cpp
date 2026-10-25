// xds register values -> VP PIO methods; see VpBridge.h.
#include "audio/vp/VpBridge.h"

#include <algorithm>
#include <cstring>

namespace fut::audio::vp {

namespace {

// The 32 filter bytes of an xds HRTF filter id (the default filter 0x00263A38 for the two non-table ids: the
// upload 0x0024CB6A takes the pair 0x00263A58 when the voice has no 3D source or a NULL pointer).
const u8* FilterBytes(u32 id, const xds::Tables& t) {
    if (id < xds::kHrtfFilterCount && t.hrtfFilters.size() >= (id + 1) * 32) return &t.hrtfFilters[id * 32];
    return t.hrtfDefault.size() >= 32 ? t.hrtfDefault.data() : nullptr;
}

}  // namespace

VpBridge::VpBridge(VpPio& pio, VpMemoryMap& mem, const xds::Tables& tables) : fe_(pio), mem_(mem), tables_(tables) {}

void VpBridge::InitHardware() {
    for (u32 b = 0; b < 32; ++b) W(pio::kSetSubmixHeadroom + 4 * b, b < 31 ? 1 : 0);
    W(pio::kSetHrtfSubmixes, 0x09070806);
}

// orig: 0x0024C1FA (FMT): container / sample size from the format (PCM 8 -> 0, 16 -> 5, 24 -> 0xE, 32 -> 0xF,
// Xbox ADPCM -> 0xA in bits 28-31), channels - 1 in bits 16-20, bit 27 for more than one channel; 0x0024C138:
// bits 13-15 for buffers that are not mix-in / FX; the loop bit 25 with DSBPLAY_LOOPING (O).
u32 VpBridge::FormatBits(const Buffer& b, bool loop) {
    u32 top = 0;
    if (b.formatTag == 0x69) top = 0xA;
    else if (b.bits == 16) top = 0x5;
    else if (b.bits == 24) top = 0xE;
    else if (b.bits == 32) top = 0xF;
    const u32 ch = std::max<u32>(b.channels, 1);
    return top << 28 | ((ch - 1) & 31) << 16 | (ch != 1 ? 0x08000000u : 0) | 0xE000 | (loop ? 0x02000000u : 0);
}

VoiceRegs VpBridge::Encode(const xds::VoiceRegisters& x, bool is3D, u32 fmtBits) {
    VoiceRegs r;
    for (u32 i = 0; i < 6; ++i) r.vbin |= static_cast<u32>(x.slotBins[i] & 31) << kVbinShift[i];
    r.fmt = (fmtBits & ~0x3FFu) | (x.slotBins[6] & 31) | (x.slotBins[7] & 31) << 5;
    // hardware voice 0's words: mono and stereo buffers are one voice (0x0024C213; buffers of more than two
    // channels, linked voices, are not used by the game and not bridged)
    r.vol[0] = x.volume[0], r.vol[1] = x.volume[3], r.vol[2] = x.volume[6];
    r.pitch = x.pitch;
    if (is3D) {
        r.misc = x.misc;                             // as CMcpxVoiceClient_SetFilter 0x0024CC6F left them
        r.fcb = x.fcb;
    }
    return r;
}

// orig: 0x0024CB6A: word i = A[2i] | B[2i] << 8 | A[2i+1] << 16 | B[2i+1] << 24 (A = first filter, B = second),
// word 15 = A[30] | B[30] << 8 | ITD << 25 with ITD = +A[31] (azimuth >= 0) or -B[31].
std::array<u32, 16> VpBridge::HrirWords(const xds::VoiceRegisters& x, const xds::Tables& t) {
    std::array<u32, 16> w{};
    const u8* a = FilterBytes(x.hrtf0, t);
    const u8* b = FilterBytes(x.hrtf1, t);
    if (!a || !b) return w;
    for (u32 i = 0; i < 15; ++i)
        w[i] = a[2 * i] | b[2 * i] << 8 | static_cast<u32>(a[2 * i + 1]) << 16 | static_cast<u32>(b[2 * i + 1]) << 24;
    const s32 itd = x.azimuth < 0.0f ? -static_cast<s32>(b[31]) : static_cast<s32>(a[31]);
    w[15] = (a[30] | b[30] << 8) | static_cast<u32>(itd) << 25;
    return w;
}

void VpBridge::WriteParameters(Voice& v, const xds::VoiceRegisters& x, bool all) {
    VoiceRegs r = Encode(x, v.is3D, v.fmtBits);
    r.misc |= v.miscBits;
    W(pio::kSetCurrentVoice, v.hw);
    W(pio::kVoiceLock, 1);
    if (all || r.vbin != v.last.vbin) W(pio::kVbin, r.vbin);
    if (all || r.fmt != v.last.fmt) W(pio::kFmt, r.fmt);
    for (u32 k = 0; k < 3; ++k)
        if (all || r.vol[k] != v.last.vol[k]) W(pio::kVolA + 4 * k, r.vol[k]);
    if (all || r.pitch != v.last.pitch) W(pio::kPitch, r.pitch);
    if (all || r.misc != v.last.misc) W(pio::kMisc, r.misc);
    if (all || r.fcb != v.last.fcb) W(pio::kFcb, r.fcb);
    W(pio::kVoiceLock, 0);
    if (v.is3D) {
        const auto words = HrirWords(x, tables_);
        if (!v.hrirValid || words != v.hrir) {       // double-buffered entry, then the voice's handle
            v.hrirToggle ^= 1;
            const u32 entry = v.hw * 2 + v.hrirToggle;
            W(pio::kSetCurrentHrtfEntry, entry);
            for (u32 i = 0; i < 16; ++i) W(pio::kHrir + 4 * i, words[i]);
            W(pio::kSetCurrentVoice, v.hw);
            W(pio::kHrtf, entry);
            v.hrir = words;
            v.hrirValid = true;
        }
    } else if (all) {
        W(pio::kHrtf, kNoHrtf);
    }
    v.last = r;
}

void VpBridge::Play(u32 id, const Buffer& b, bool loop, const xds::VoiceRegisters& regs) {
    Stop(id);
    const u32 first = b.is3D ? 0 : 64, last = b.is3D ? 64 : kVoices;
    u32 hw = first;
    while (hw < last && used_[hw]) ++hw;
    if (hw == last || !b.data || b.bytes == 0) return;   // out of voices (the library fails the Play)
    used_[hw] = true;
    Voice& v = voices_[id];
    v.hw = hw;
    v.is3D = b.is3D;
    v.fmtBits = FormatBits(b, loop);
    // data: one SGE per 4 KB page, the base = first SGE << 12 (the mapping starts at a page boundary here). The
    // entries stay the voice's until Stop (L: the library's SGE heap; the indices are not observable): a voice never
    // has its pages replaced by a later one's.
    const u32 pages = (b.bytes + 0xFFF) / 0x1000;
    const u32 sge = AllocateSges(pages);
    if (!sge) {                                      // (no room: the voice is not started)
        used_[hw] = false;
        voices_.erase(id);
        return;
    }
    v.phys = mem_.Map(b.data, b.bytes);
    v.sge = sge, v.pages = pages;
    for (u32 p = 0; p < pages; ++p) {
        W(pio::kSetInbufSge, sge + p);
        W(pio::kSetInbufSgeOffset, v.phys + p * 0x1000);
    }
    const u32 ch = std::max<u32>(b.channels, 1);
    u32 samples = 0;
    if (b.formatTag == 0x69) samples = b.bytes / (36 * ch) * 64;
    else samples = b.bytes / (ch * std::max<u32>(b.bits / 8, 1));
    W(pio::kSetCurrentVoice, hw);
    W(pio::kVoiceLock, 1);
    W(pio::kEnv0, 0), W(pio::kEnvA, 0), W(pio::kEnv1, 0), W(pio::kEnvF, 0);
    W(pio::kLfoEnvA, 0), W(pio::kLfoEnvB, 0), W(pio::kLfoMod, 0), W(pio::kFca, 0);
    W(pio::kBufBase, sge << 12);
    W(pio::kBufEbo, samples ? samples - 1 : 0);
    W(pio::kBufLbo, 0);
    W(pio::kVoiceLock, 0);
    WriteParameters(v, regs, true);
    W(pio::kSetAntecedentVoice, (b.is3D ? 2u : 1u) << 16 | 0xFFFF);
    W(pio::kVoiceOn, hw);
    W(pio::kVoicePause, hw);                         // run (pause bit 18 clear)
}

// orig: 0x0024CE8A (a 2D voice from 0x40, the lowest free: 0x0024A6CA; 3D from 0): the set-up words without the voice
// lock, in the library's order (O: dsound_reg_oracle --stream).
bool VpBridge::CreateStream(u32 id, const Buffer& b, const xds::VoiceRegisters& regs) {
    Stop(id);
    const u32 first = b.is3D ? 0 : 64, last = b.is3D ? 64 : kVoices;
    u32 hw = first;
    while (hw < last && used_[hw]) ++hw;
    if (hw == last) return false;                   // (0x88780032)
    used_[hw] = true;
    Voice& v = voices_[id];
    v.hw = hw;
    v.is3D = b.is3D;
    v.fmtBits = FormatBits(b, false) | kFmtStream;   // (0x0024C100(1): +0x17 |= 1)
    v.miscBits = 0x00800000;                        // (0x0024C100(1): +0x1A |= 0x80)
    VoiceRegs r = Encode(regs, v.is3D, v.fmtBits);
    r.misc |= v.miscBits;
    W(pio::kSetCurrentVoice, hw);
    W(pio::kFmt, r.fmt);
    W(pio::kMisc, r.misc);
    W(pio::kEnv0, 0), W(pio::kEnvA, 0), W(pio::kEnv1, 0), W(pio::kEnvF, 0);
    W(pio::kLfoEnvA, 0), W(pio::kLfoEnvB, 0), W(pio::kLfoMod, 0);
    W(pio::kFca, 0), W(pio::kFcb, r.fcb);
    W(pio::kVbin, r.vbin);
    for (u32 k = 0; k < 3; ++k) W(pio::kVolA + 4 * k, r.vol[k]);
    W(pio::kPitch, r.pitch);
    W(pio::kHrtf, kNoHrtf);
    v.last = r;
    return true;
}

// orig: 0x0024D621's writes (one hardware voice): the voice locked, FMT, the antecedent (list 1 for 2D, 2 for 3D:
// (list + 1) << 16 | 0xFFFF), VOICE_ON, VOICE_PAUSE with bit 18 when the stream's pause bits are set.
void VpBridge::StartStream(u32 id, bool paused) {
    auto it = voices_.find(id);
    if (it == voices_.end()) return;
    Voice& v = it->second;
    v.fmtBits |= 0x00800000;                        // (the stream's state bit 0x10, O)
    const u32 fmt = (v.fmtBits & ~0x3FFu) | (v.last.fmt & 0x3FF);
    W(pio::kSetCurrentVoice, v.hw);
    W(pio::kVoiceLock, 1);
    W(pio::kFmt, fmt);
    W(pio::kSetAntecedentVoice, (v.is3D ? 2u : 1u) << 16 | 0xFFFF);
    W(pio::kVoiceOn, v.hw);
    W(pio::kVoicePause, v.hw | (paused ? 1u << 18 : 0u));
    W(pio::kVoiceLock, 0);
    v.last.fmt = fmt;
}

// orig: 0x0024D2BC's writes: FMT with bits 23 and 25 cleared (+0x16 &= 0xFD7F), VOICE_OFF; the voice stays the
// stream's.
void VpBridge::StopStream(u32 id) {
    auto it = voices_.find(id);
    if (it == voices_.end()) return;
    Voice& v = it->second;
    v.fmtBits &= ~0x02800000u;
    const u32 fmt = (v.fmtBits & ~0x3FFu) | (v.last.fmt & 0x3FF);
    W(pio::kSetCurrentVoice, v.hw);
    W(pio::kFmt, fmt);
    W(pio::kVoiceOff, v.hw);
    v.last.fmt = fmt;
}

void VpBridge::Update(u32 id, const xds::VoiceRegisters& regs) {
    auto it = voices_.find(id);
    if (it == voices_.end()) return;
    WriteParameters(it->second, regs, false);
}

void VpBridge::Stop(u32 id) {
    auto it = voices_.find(id);
    if (it == voices_.end()) return;
    W(pio::kVoiceOff, it->second.hw);
    used_[it->second.hw] = false;
    mem_.Unmap(it->second.phys);
    for (u32 p = 0; p < it->second.pages; ++p) sgeUsed_[it->second.sge + p] = false;
    voices_.erase(it);
}

void VpBridge::SetPosition(u32 id, u32 sample) {
    auto it = voices_.find(id);
    if (it == voices_.end()) return;
    W(pio::kSetCurrentVoice, it->second.hw);         // 0x0024DB2F: SET_CURRENT_VOICE, lock, CBO 0x3D8, unlock
    W(pio::kVoiceLock, 1);
    W(pio::kBufCbo, sample);
    W(pio::kVoiceLock, 0);
}

u32 VpBridge::AllocateSges(u32 pages) {
    if (pages == 0) return 0;
    u32 run = 0;
    for (u32 i = 1; i < kModelSges; ++i) {
        run = sgeUsed_[i] ? 0 : run + 1;
        if (run == pages) {
            const u32 first = i + 1 - pages;
            for (u32 k = first; k <= i; ++k) sgeUsed_[k] = true;
            return first;
        }
    }
    return 0;
}

void VpBridge::Pause(u32 id, bool pause) {
    auto it = voices_.find(id);
    if (it == voices_.end()) return;
    W(pio::kVoicePause, it->second.hw | (pause ? 1u << 18 : 0u));
}

void VpBridge::Release(u32 id) { Stop(id); }

bool VpBridge::Ended(u32 id, const Vp& vp) const {
    auto it = voices_.find(id);
    return it == voices_.end() || vp.Ended(it->second.hw);
}

u32 VpBridge::HardwareVoice(u32 id) const {
    auto it = voices_.find(id);
    return it == voices_.end() ? kVoices : it->second.hw;
}

}  // namespace fut::audio::vp
