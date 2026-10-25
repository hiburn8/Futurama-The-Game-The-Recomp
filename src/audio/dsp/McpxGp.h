// The MCPX "GP" (global processor): the DSP56300 running the DSOUND executive and dsstdfx.bin, as Futurama uses it
// (docs/analysis/audio_dsp_port.md 2-4), plus the CPU side of the effect-state protocol (3: DownloadEffectsImage,
// Get / SetEffectData with the deferred scratch copy and its dirty hull, the flush / mailbox, the I3DL2 listener
// commit's two writes).
//
// Per 32-sample frame (48 kHz, 1,500 frames/s): the mix bins (32 x 32 words, written by the VP) are put in
// X:0xC00 + 0x20 n, the start-of-frame interrupt bit is raised and the executive runs from its frame poll (P:0x160)
// to the next one: mailbox, the 10 effects (P:0x171.., each frame, whatever their inputs), the DMA of bins 0-5 to the
// six speaker rings in scratch (AC-3 order FL C FR BL BR LFE, 0x8000 + 0x800 k) and of bins 11-30 to scratch 0xB000.
// The effects add their outputs into the bins (effect 0 -> 6-9, effect 1 6-9 -> 0 / 1 / 4 / 5).
//
// Hardware readings that no code we have settles (L), each isolated here (audio_dsp_port.md 5.3 / 11.4):
//   (1) X:0x1400-0x17FF is a read / write view of the mix buffer X:0xC00-0xFFF;
//   (2) the VP overwrites all 32 bins before each GP frame (bins the caller does not give are 0);
//   (3) DMA descriptors {next (bit 14 end), control, count, DSP address, scratch offset, base, size - 1}: control bit 1
//       direction (1 = DSP -> scratch), bit 4 offset write-back, bits 5-8 buffer (0xE circular, 0xF linear), bits 10-12
//       format (2 / 6: a 24-bit value in a 32-bit word, written zero-extended); DSP addresses 0x0000-0x17FF X,
//       0x1800-0x1FFF Y, 0x2800-0x37FF P; a list runs synchronously when started;
//   (4) X:FFFFC5 bit 1 = start of frame, bit 7 = DMA list end (write 1 to clear); X:FFFFD6 actions 1 start, 2 stop,
//       3 freeze, 4 unfreeze, status bit 4 running, bit 3 frozen;
//   (6) the GP boot ROM copies scratch words 0-0x7FF to P:0-0x7FF;
//   the timer X:FFFFB3 reads the interpreter's instruction count (it only feeds the cycle statistics X:0x7C-0x7F).
#pragma once

#include "audio/dsp/DsImage.h"
#include "audio/dsp/Dsp56300.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace fut::audio::dsp {

class McpxGp final : private Dsp56300::Peripherals {
public:
    static constexpr u32 kBins = 32, kFrameSamples = 32;
    static constexpr u32 kScratchBytes = 0x100000;
    static constexpr u32 kFramePoll = 0x160;     // executive: jclr #1,x:$ffffc5,$160 (start-of-frame wait)
    static constexpr u32 kMixBuffer = 0xC00;     // X:0xC00 + 0x20 n: bin n
    static constexpr u32 kMailbox = 0x800;       // scratch: the 6 mailbox words (command at +0x10)
    static constexpr u32 kRingBase = 0x8000, kRingBytes = 0x800;   // the six speaker rings
    static constexpr u32 kAc3Order[6] = {0, 2, 1, 4, 5, 3};        // ring k carries bin kAc3Order[k] (P:0xCC..)

    McpxGp();
    ~McpxGp() override;
    McpxGp(const McpxGp&) = delete;              // the core points back at its peripherals (this)
    McpxGp& operator=(const McpxGp&) = delete;

    // Boot (executive at scratch 0, the boot ROM copy, run to the first frame poll) and DownloadEffectsImage
    // (orig: 0x0024A3FF: header + descrambled image to scratch 0x800.., command 3, wait for the acknowledgement;
    // the GP runs silent frames meanwhile, as it would while the CPU spins).
    bool Init(const DsImage& image);
    bool Ready() const { return ready_; }
    const std::string& Error() const { return error_; }

    // ---- frames
    // Bin `bin`'s 32 samples for the next frame (24-bit two's complement; only the low 24 bits are used).
    void SetBin(u32 bin, const s32* samples);
    void ClearBins();
    // Runs one GP frame: all 32 bins are written to X:0xC00.. (bins not set are 0, L (2)), then the bins given are
    // cleared for the next frame.
    bool RunFrame();
    u64 Frames() const { return frames_; }
    // Optional fast path (off by default): while every FX-send bin (11-31) has been silent since Init, run the
    // chain without effects 2-8 (jump from effect 2's first word to effect 9's, addresses from the effect map).
    // Effects 2-8 read bins 11-23 and add into bins 0 / 1 / 4 / 5; with silent inputs from boot on they add zero
    // (O: bins 0-5, rings and EP output identical on every trace whose FX sends are silent, audio_dsp_port.md 11.3).
    // Saves ~56% of the instructions; X memory of effects 2-8 and the cycle statistics differ from the full run.
    // The first non-zero FX-send sample ends the fast path for good, and from then on the output is NOT the
    // original's: effects 2-8 start from their boot state (effect 8, the second reverb, runs its init and parameter
    // cross-fade late). Enable it only when the VP model never writes bins 11-31 (the game's case).
    void SetSkipIdleEffects(bool on) { skipIdle_ = on; }
    bool SkippingEffects() const { return skipIdle_ && !fxSendsUsed_; }
    // After a frame: bin `bin` as the effects left it (bins 0-5 are what the executive sent to the rings).
    void ReadBin(u32 bin, s32 out[32]) const;
    // After a frame: the frame's 32 samples of ring k (AC-3 order), read back from the scratch rings like the EP.
    void ReadRing(u32 k, s32 out[32]) const;

    // ---- the CPU side of the effect state (DSOUND)
    // orig: 0x0024A541 GetEffectData: the live X memory of effect `effect` from byte `offset`.
    bool GetEffectData(u32 effect, u32 offset, u32* words, u32 count) const;
    // orig: 0x0024A498 SetEffectData. flags bit 0 (deferred, what DSOUND uses): into the scratch copy of the state,
    // extending the dirty interval; otherwise straight into X memory (low 24 bits).
    bool SetEffectData(u32 effect, u32 offset, const u32* words, u32 count, u32 flags);
    // orig: 0x0024A32E flush: posts command 2 (load state) for the dirty interval. While the previous command is
    // still pending the original spins until the GP acknowledges it; here the post waits for the next frame
    // boundary at which the mailbox is idle (RunFrame posts it before running).
    void CommitEffectData();
    bool CommandPending() const;
    // orig: 0x0024A997, the listener commit's I3DL2 part after the conversion 0x0024EAB2: SetEffectData(0, 0x10,
    // live word 4 | 4), SetEffectData(0, 0x118, the 66 converted words 0x46-0x87), flush. `params` = the converter's
    // words 0x46.. (xds::DirectSound::ReverbParameters()). Returns the word 4 written.
    u32 CommitI3DL2Reverb(const u32 params[66], u32 reverbEffect = 0);

    // ---- inspection (tests)
    Dsp56300& Core() { return *core_; }
    const Dsp56300& Core() const { return *core_; }
    const u8* Scratch() const { return scratch_.data(); }
    u32 ScratchWord(u32 offset) const;

private:
    u32 PeripheralRead(u32 addr) override;
    void PeripheralWrite(u32 addr, u32 value) override;
    void RunDmaList();
    bool DmaAddress(u32 dspAddr, Dsp56300::Space& space, u32& addr);
    void SetScratchWord(u32 offset, u32 value);
    bool RunToPoll();
    void PostFlush();
    bool Fail(const std::string& why);

    std::unique_ptr<Dsp56300> core_;
    std::vector<u8> scratch_;
    const DsImage* image_ = nullptr;
    // peripherals
    u32 istat_ = 0;                              // X:FFFFC5
    u32 dmaNext_ = 0, dmaStart_ = 0, dmaStatus_ = 0, dmaConfig_ = 0;   // X:FFFFD4..D7
    std::array<u32, 128> periph_{};              // the other peripheral words (stored, read back)
    std::array<u32, kBins * kFrameSamples> staging_{};   // the next frame's bins (SetBin)
    // CPU side
    bool dirty_ = false;
    u32 dirtyStart_ = 0, dirtyLength_ = 0;       // scratch bytes
    bool flushWaiting_ = false;
    bool ready_ = false;
    bool skipIdle_ = false, fxSendsUsed_ = false;
    u32 effect2Pc_ = 0, effect9Pc_ = 0;          // P addresses of effects 2 and 9 (from the effect map)
    std::string error_;
    u64 frames_ = 0;
};

}  // namespace fut::audio::dsp
