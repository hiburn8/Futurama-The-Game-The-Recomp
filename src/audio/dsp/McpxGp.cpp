#include "audio/dsp/McpxGp.h"

#include <cstring>

namespace fut::audio::dsp {

namespace {
constexpr u32 kXWords = 0x1000, kYWords = 0x800, kPWords = 0x1000;
constexpr u32 kXView = 0x1400, kXViewWords = 0x400;          // (L 1) read view of the mix buffer
constexpr u32 kIrqFrame = 0x02, kIrqDmaEnd = 0x80;            // X:FFFFC5 (L 4)
constexpr u32 kDmaRunning = 0x10, kDmaFrozen = 0x08;          // X:FFFFD6 status (L 4)
constexpr u64 kBootLimit = 2000000, kFrameLimit = 5000000;    // instructions (a frame takes ~24,200)
constexpr u32 kDownloadFrames = 50;
}  // namespace

McpxGp::McpxGp() = default;
McpxGp::~McpxGp() = default;

bool McpxGp::Fail(const std::string& why) {
    error_ = why;
    ready_ = false;
    return false;
}

u32 McpxGp::ScratchWord(u32 offset) const {
    u32 v = 0;
    if (offset + 4 <= scratch_.size()) std::memcpy(&v, &scratch_[offset], 4);
    return v;
}

void McpxGp::SetScratchWord(u32 offset, u32 value) {
    if (offset + 4 <= scratch_.size()) std::memcpy(&scratch_[offset], &value, 4);
}

bool McpxGp::RunToPoll() {
    // run to the executive's start-of-frame poll; a pending frame interrupt lets it pass
    for (;;) {
        if (!core_->RunUntil(kFramePoll, kFrameLimit)) return Fail("GP: " + core_->FaultText());
        if (!(istat_ & kIrqFrame)) return true;
        core_->Step();
    }
}

bool McpxGp::Init(const DsImage& image) {
    ready_ = false;
    error_.clear();
    if (!image.Loaded()) return Fail("GP: no effects image");
    image_ = &image;
    core_ = std::make_unique<Dsp56300>(kXWords, kYWords, kPWords, static_cast<Dsp56300::Peripherals*>(this));
    core_->SetXView(kXView, kXViewWords, kMixBuffer);
    scratch_.assign(kScratchBytes, 0);
    istat_ = dmaNext_ = dmaStart_ = dmaStatus_ = dmaConfig_ = 0;
    periph_.fill(0);
    staging_.fill(0);
    dirty_ = flushWaiting_ = false;
    fxSendsUsed_ = false;
    frames_ = 0;
    // effect i's code is loaded at P:0x171 + (the code words of the effects before it) (2.3)
    effect2Pc_ = effect9Pc_ = 0;
    if (image.EffectCount() == 10) {
        u32 p = 0x171;
        for (u32 i = 0; i < 10; ++i) {
            if (i == 2) effect2Pc_ = p;
            if (i == 9) effect9Pc_ = p;
            p += image.Effect(i).codeSize / 4;
        }
    }

    // orig: 0x0024A11D puts the executive at scratch 0; (L 6) the GP boot ROM copies scratch words 0-0x7FF to P
    const std::vector<u8>& exe = image.Executive();
    std::memcpy(scratch_.data(), exe.data(), exe.size());
    for (u32 i = 0; i < 0x800; ++i) core_->WriteP(i, ScratchWord(4 * i));
    core_->pc = 0;
    if (!core_->RunUntil(kFramePoll, kBootLimit)) return Fail("GP boot: " + core_->FaultText());

    // orig: 0x0024A3FF DownloadEffectsImage: the header with the command slot 0, the image from 0x818, then
    // command 3 (load code + state); the CPU spins until the GP clears the slot
    const std::vector<u8>& f = image.Bytes();
    for (u32 i = 0; i < 6; ++i) SetScratchWord(kMailbox + 4 * i, i == 4 ? 0 : image.Header(i));
    std::memcpy(&scratch_[DsImage::kCodeOffset], &f[DsImage::kCodeOffset], f.size() - DsImage::kCodeOffset);
    SetScratchWord(kMailbox + 0x10, 3);
    ready_ = true;
    for (u32 i = 0; i < kDownloadFrames && CommandPending(); ++i)
        if (!RunFrame()) return false;
    if (CommandPending()) return Fail("GP: the image download was not acknowledged");
    return true;
}

// ---- frames ---------------------------------------------------------------------------------------------------------
void McpxGp::SetBin(u32 bin, const s32* samples) {
    if (bin >= kBins) return;
    for (u32 i = 0; i < kFrameSamples; ++i) staging_[bin * kFrameSamples + i] = static_cast<u32>(samples[i]) & kMask24;
}

void McpxGp::ClearBins() { staging_.fill(0); }

bool McpxGp::RunFrame() {
    if (!ready_) return false;
    if (flushWaiting_ && !CommandPending()) PostFlush();
    // (L 2) the VP has written all 32 bins of this frame
    std::memcpy(core_->XData() + kMixBuffer, staging_.data(), sizeof(u32) * staging_.size());
    if (!fxSendsUsed_)
        for (u32 i = 11 * kFrameSamples; i < staging_.size() && !fxSendsUsed_; ++i) fxSendsUsed_ = staging_[i] != 0;
    staging_.fill(0);
    istat_ |= kIrqFrame;                         // start of frame
    core_->Step();                               // the poll instruction sees the bit and falls through
    if (SkippingEffects() && effect2Pc_ && effect9Pc_) {
        if (!core_->RunUntil(effect2Pc_, kFrameLimit)) return Fail("GP: " + core_->FaultText());
        core_->pc = effect9Pc_;                  // the fast path: effects 2-8 are not run
    }
    if (!RunToPoll()) return false;
    ++frames_;
    return true;
}

void McpxGp::ReadBin(u32 bin, s32 out[32]) const {
    const u32* x = core_->XData() + kMixBuffer + (bin % kBins) * kFrameSamples;
    for (u32 i = 0; i < kFrameSamples; ++i) out[i] = Sext24(x[i]);
}

void McpxGp::ReadRing(u32 k, s32 out[32]) const {
    // the ring's descriptor (X:0x25 + 7 k) holds the written-back byte offset after this frame's 32 words
    const u32 off = core_->XData()[0x25 + 7 * (k % 6) + 4];
    const u32 start = (off + kRingBytes - 4 * kFrameSamples) % kRingBytes;
    for (u32 i = 0; i < kFrameSamples; ++i)
        out[i] = Sext24(ScratchWord(kRingBase + kRingBytes * (k % 6) + (start + 4 * i) % kRingBytes) & kMask24);
}

// ---- peripherals (L 4) ------------------------------------------------------------------------------------------------
u32 McpxGp::PeripheralRead(u32 addr) {
    switch (addr) {
    case 0xFFFFC5: return istat_;
    case 0xFFFFD4: return dmaNext_;
    case 0xFFFFD5: return dmaStart_;
    case 0xFFFFD6: return dmaStatus_;
    case 0xFFFFD7: return dmaConfig_;
    case 0xFFFFB3: return static_cast<u32>(core_->instructionCount) & kMask24;   // timer count (stand-in)
    default: return periph_[addr - 0xFFFF80];
    }
}

void McpxGp::PeripheralWrite(u32 addr, u32 value) {
    switch (addr) {
    case 0xFFFFC5: istat_ &= ~value; return;    // write 1 to clear
    case 0xFFFFD4: dmaNext_ = value; return;
    case 0xFFFFD5: dmaStart_ = value; return;
    case 0xFFFFD7: dmaConfig_ = value; return;
    case 0xFFFFD6:
        switch (value & 7) {
        case 0: break;
        case 1: dmaStatus_ |= kDmaRunning; break;
        case 2: dmaStatus_ &= ~kDmaRunning; break;
        case 3: dmaStatus_ |= kDmaFrozen; break;
        case 4: dmaStatus_ &= ~kDmaFrozen; break;
        default: core_->ExternalFault("DMA control action not modelled"); return;
        }
        if ((dmaStatus_ & (kDmaRunning | kDmaFrozen)) == kDmaRunning) RunDmaList();
        return;
    default: periph_[addr - 0xFFFF80] = value; return;
    }
}

bool McpxGp::DmaAddress(u32 dspAddr, Dsp56300::Space& space, u32& addr) {
    // (L 3) the DMA engine's DSP address space
    if (dspAddr < 0x1800) space = Dsp56300::kX, addr = dspAddr;
    else if (dspAddr < 0x2000) space = Dsp56300::kY, addr = dspAddr - 0x1800;
    else if (dspAddr >= 0x2800 && dspAddr < 0x3800) space = Dsp56300::kP, addr = dspAddr - 0x2800;
    else {
        core_->ExternalFault("DMA to an unmodelled DSP address");
        return false;
    }
    return true;
}

void McpxGp::RunDmaList() {
    // (L 3) the descriptor list from X:FFFFD4 to the end-of-list flag, run synchronously: every consumer of DMA data
    // in the program waits for the end-of-list bit first
    for (u32 nodes = 0; !(dmaNext_ & 0x4000); ++nodes) {
        if (nodes >= 1000) return core_->ExternalFault("DMA list does not end");
        Dsp56300::Space ds;
        u32 da;
        if (!DmaAddress(dmaNext_ & 0x3FFF, ds, da)) return;
        u32 d[7];
        for (u32 i = 0; i < 7; ++i) d[i] = core_->Read(ds, da + i);
        const u32 ctl = d[1], count = d[2], size = d[6] + 1;
        u32 offset = d[4];
        dmaNext_ = d[0];
        const u32 format = (ctl >> 10) & 7, buffer = (ctl >> 5) & 0xF;
        if ((ctl & 1) || ((ctl >> 2) & 3) || ((ctl >> 13) & 1) || (ctl >> 14) > 1 || (format != 2 && format != 6) ||
            (buffer != 0xE && buffer != 0xF))
            return core_->ExternalFault("DMA descriptor control word not modelled");
        const bool toScratch = (ctl >> 1) & 1;
        Dsp56300::Space ms;
        u32 ma;
        if (!DmaAddress(d[3], ms, ma)) return;
        if (buffer == 0xE && offset >= size) offset = 0;
        for (u32 i = 0; i < count; ++i) {
            u32 sa;
            if (buffer == 0xF) {
                sa = offset + 4 * i;             // linear: the offset field is the scratch address
            } else {
                sa = d[5] + offset;              // circular: base + offset, wrapping at size
                offset += 4;
                if (offset >= size) offset = 0;
            }
            if (sa + 4 > scratch_.size()) return core_->ExternalFault("DMA outside the scratch memory");
            if (toScratch) SetScratchWord(sa, core_->Read(ms, ma + i));   // 24-bit value, zero-extended (L 3)
            else core_->Write(ms, ma + i, ScratchWord(sa) & kMask24);
        }
        if ((ctl >> 4) & 1) core_->Write(ds, da + 4, offset);          // offset write-back
    }
    istat_ |= kIrqDmaEnd;
}

// ---- CPU side: effect state ------------------------------------------------------------------------------------------
bool McpxGp::CommandPending() const { return ScratchWord(kMailbox + 0x10) != 0; }

bool McpxGp::GetEffectData(u32 effect, u32 offset, u32* words, u32 count) const {
    if (!image_ || effect >= image_->EffectCount() || offset % 4) return false;
    const u32 x0 = image_->StateX(effect) + offset / 4;
    if (x0 + count > kXWords) return false;
    for (u32 i = 0; i < count; ++i) words[i] = core_->XData()[x0 + i];
    return true;
}

bool McpxGp::SetEffectData(u32 effect, u32 offset, const u32* words, u32 count, u32 flags) {
    if (!image_ || effect >= image_->EffectCount() || offset % 4) return false;
    if (!(flags & 1)) {                          // immediate: through the GP X window (low 24 bits, L)
        const u32 x0 = image_->StateX(effect) + offset / 4;
        if (x0 + count > kXWords) return false;
        for (u32 i = 0; i < count; ++i) core_->XData()[x0 + i] = words[i] & kMask24;
        return true;
    }
    // deferred: the scratch copy of the state (scratch offset = file offset), then the dirty interval as the
    // original extends it (orig: 0x0024A498; the block-below case at 0x0024A503): start = min, length += the new
    // size plus the gap after the old end, or += the distance to the old start for a block below it
    const u32 at = image_->Effect(effect).stateOffset + offset;
    const u32 size = 4 * count;
    if (at + size > scratch_.size()) return false;
    for (u32 i = 0; i < count; ++i) SetScratchWord(at + 4 * i, words[i]);
    if (!dirty_) {
        dirtyStart_ = at, dirtyLength_ = size;
    } else {
        u32 grow = size;
        if (dirtyStart_ + dirtyLength_ < at) grow += at - dirtyLength_ - dirtyStart_;
        else if (at + size < dirtyStart_) grow = dirtyStart_ - at;
        if (dirtyStart_ >= at) dirtyStart_ = at;
        dirtyLength_ += grow;
    }
    dirty_ = true;
    return true;
}

void McpxGp::PostFlush() {
    // command 2 (load state): X offset (relative to the state section at X:0x80), scratch start, words
    // (0x206 = 0x818 / 4 - 0x80: the scratch word index of X:0x80)
    SetScratchWord(kMailbox + 0x0, (dirtyStart_ >> 2) - image_->CodeWords() - 0x206);
    SetScratchWord(kMailbox + 0x8, dirtyStart_);
    SetScratchWord(kMailbox + 0xC, dirtyLength_ >> 2);
    SetScratchWord(kMailbox + 0x10, 2);
    dirty_ = flushWaiting_ = false;
}

void McpxGp::CommitEffectData() {
    if (!dirty_ || !image_) return;
    if (CommandPending()) {
        flushWaiting_ = true;                    // posted by the next RunFrame at which the mailbox is idle
        return;
    }
    PostFlush();
}

u32 McpxGp::CommitI3DL2Reverb(const u32 params[66], u32 reverbEffect) {
    u32 w4 = 0;
    GetEffectData(reverbEffect, 0x10, &w4, 1);   // the live word 4 (bit 0: initialised by the DSP)
    w4 |= 4;                                     // bit 2: parameters changed (orig: 0x0024AA01)
    SetEffectData(reverbEffect, 0x10, &w4, 1, 1);
    SetEffectData(reverbEffect, 0x118, params, 66, 1);
    CommitEffectData();
    return w4;
}

}  // namespace fut::audio::dsp
