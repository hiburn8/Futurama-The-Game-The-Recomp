// The XDK library's CMcpxStream on the port's VP; see VpStream.h. Each function names the original it follows.
#include "audio/vp/VpStream.h"

#include "audio/vp/VpBridge.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace fut::audio::vp {

namespace {

// State word +0x12 (H, from the tests the library makes on it).
constexpr u16 kAllocated = 0x0001;                     // a hardware voice (0x0024CE8A)
constexpr u16 kStarted = 0x0002;                       // VOICE_ON written (0x0024D621); 3 = playing
constexpr u16 kPaused = 0x0004;                        // DSSTREAMPAUSE_PAUSE
constexpr u16 kPauseQueue = 0x0020;                    // Process only queues (Pause mode 3 before the start)
constexpr u16 kPausedNoActivate = 0x0040;              // Pause mode 2
constexpr u16 kStarved = 0x0400;                       // paused by the library: no SSL left to play
constexpr u16 kDiscontinuity = 0x0800;
constexpr u16 kEndPending = 0x1000, kEnded = 0x2000;   // (CMcpxStream_Stop's flag 4; not reached by the game)
constexpr u16 kStopping = 0x8000;                      // VOICE_OFF written, until 500 ms have passed (0x0024CA2A)
constexpr u16 kPauseBits = kPaused | kPausedNoActivate | kStarved;   // 0x444

void NotPortedText(const char* what) {
    static int count = 0;
    if (count++ < 8) std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

}  // namespace

McpxStream::McpxStream(VpPio& pio, VpBridge& bridge, VpMemoryMap& mem, VpNotifierMemory& notifiers, u32 id,
                       u32 maxPackets, u16 formatTag, u16 channels, u16 bits, u16 blockAlign)
    : pio_(pio), bridge_(bridge), mem_(mem), notifiers_(notifiers), id_(id), formatTag_(formatTag),
      channels_(std::max<u16>(channels, 1)), bits_(bits), blockAlign_(std::max<u16>(blockAlign, 1)) {
    // 0x0024BDA0 (vf10, the format): the segment word's container (bits 16-17: 8-bit 0, 16-bit 1, Xbox ADPCM 2,
    // 32-bit 3), channels - 1 (bits 18-22) and bit 23 for more than one channel
    if (formatTag_ == 0x69) segmentFormat_ = (segmentFormat_ & ~0x10000u) | 0x20000u;
    else if (formatTag_ == 1 && bits_ == 16) segmentFormat_ = (segmentFormat_ & ~0x20000u) | 0x10000u;
    else if (formatTag_ == 1 && bits_ == 32) segmentFormat_ |= 0x30000u;
    else if (formatTag_ == 1 && bits_ == 8) segmentFormat_ &= ~0x30000u;
    segmentFormat_ = (segmentFormat_ & ~0x7C0000u) | ((static_cast<u32>(channels_) - 1) << 18 & 0x7C0000u);
    segmentFormat_ = channels_ > 1 ? segmentFormat_ | 0x800000u : segmentFormat_ & ~0x800000u;
    if (channels_ > 2) NotPortedText("a stream of more than one hardware voice (CMcpxStream, > 2 channels)");
    // 0x0024B355: the packet entries, all free
    entries_.resize(maxPackets);
    for (u32 i = 0; i < maxPackets; ++i) free_.push_back(i);
    state_ |= kAllocated;                               // (the 2D stream's voice: VpBridge::CreateStream, by the caller)
}

McpxStream::~McpxStream() {
    // CMcpxStream's dtor 0x0024BD43: the commands cancelled, Flush(0) (which waits for a stopping voice, 0x0024D1F1),
    // the voice released (0x0024B77B: the caller's VpBridge::Release)
    commands_.clear();
    Flush(false);
}

u32 McpxStream::Voice() const { return bridge_.HardwareVoice(id_); }

void McpxStream::Queue(u32 command) {                   // orig: vf18 0x0024B7BB -> 0x0024B0EB -> 0x0024AA41
    if (!Queued(command)) commands_.push_back(command);
}

void McpxStream::Cancel(u32 command) {                  // orig: vf1C 0x0024B7EA -> 0x0024AF0E
    std::erase(commands_, command);
}

bool McpxStream::Queued(u32 command) const {           // (the entry's flag bit 1)
    return std::find(commands_.begin(), commands_.end(), command) != commands_.end();
}

// orig: 0x0024B303 / 0x0024B31C: bytes <-> samples by the block align (Xbox ADPCM: 64 samples per block).
u32 McpxStream::BytesToSamples(u32 bytes) const {
    const u32 n = bytes / blockAlign_;
    return formatTag_ == 0x69 ? n << 6 : n;
}

u32 McpxStream::SamplesToBytes(u32 samples) const {
    return formatTag_ == 0x69 ? (samples >> 6) * blockAlign_ : samples * blockAlign_;
}

// orig: 0x0024CE8A (the voice allocation and set-up): a 2D stream's voice is allocated with the stream (0x0024B355
// unless DSSTREAMDESC bit 18), so Process finds it.
bool McpxStream::AllocateVoice() {
    if (state_ & kAllocated) return true;
    NotPortedText("a stream whose voice is allocated at Process (DSSTREAMDESC flag 0x40000, 0x0024CE8A)");
    return false;
}

// orig: CDirectSoundStream_Process 0x00247716 (GetStatus 0x0024B46E: READY = a free entry, else 0x88780032), then
// 0x0024BE8F.
u32 McpxStream::Process(const Packet& p) {
    if (free_.empty()) return kNoPacket;
    if (p.completedSize) *p.completedSize = 0;          // 0x0024B264
    if (p.status) *p.status = kPending;
    if (!(state_ & kEnded)) {
        state_ &= ~kDiscontinuity;
        Cancel(3);                                      // commands 3 and 1 cancelled (vf1C; 1 is never queued here)
        Cancel(1);
    }
    if (!AllocateVoice()) return 0x80004005u;
    const u32 e = free_.front();
    free_.pop_front();
    Entry& en = entries_[e];
    en.p = p;
    en.offset = 0;
    en.status = kPending;
    // MmLockUnlockBufferPages(buffer, size, lock): (port) the packet mapped where the VP reads it
    en.phys = p.buffer && p.maxSize ? mem_.Map(p.buffer, p.maxSize, static_cast<u32>(reinterpret_cast<uintptr_t>(p.buffer)))
                                    : 0;
    queue_.push_back(e);
    if (!(state_ & kPauseQueue)) Fill();
    return 0;
}

// orig: 0x0024BBD7. Bit 0 of the result: an SSL was completed; bit 1: an SSL was programmed.
u32 McpxStream::Fill() {
    u32 r = 0;
    bool again = true;
    while (again) {
        u32 s = current_;
        for (u32 k = 0; k < 2; ++k) {                   // the played SSLs, the current one first
            if (notifiers_.Status(Voice(), s) != 1) break;   // (0x0024B2E5: 1 = done)
            CompleteSsl(s, kSuccess);
            r |= 1;
            if (state_ & kEndPending) NotPortedText("CMcpxStream end of stream (0x0024BC20: the notifier's +0xE byte)");
            s = (s - 1) & 1;
        }
        if (state_ & kEnded) break;
        s = (active_ + current_) & 1;                   // the next SSL to program
        for (u32 k = 0; k < 2; ++k) {
            if (!FillSsl(s)) {
                again = false;
                break;
            }
            r |= 2;
            s = (s - 1) & 1;
        }
    }
    // (DSSTREAMDESC +0xB bit 6, packets signalled from the DPC: not the game's)
    return r;
}

// orig: 0x0024B8E8: an empty SSL takes the queued packets (up to 16 segments) and starts.
bool McpxStream::FillSsl(u32 ssl) {
    bool mapped = false;
    if (sslSegments_[ssl] != 0) return false;
    while (!queue_.empty()) {
        if (!MapPacket(ssl, queue_.front())) break;
        mapped = true;
        if (sslSegments_[ssl] >= 16) break;
    }
    if (mapped) StartSsl(ssl);
    return mapped;
}

// orig: 0x0024B4F5: the packet's unmapped rest as segments of at most 0xFFFF samples over physically contiguous
// runs (0x0024FDAD: one run here, a packet is one mapping), until the SSL has 16; a packet mapped to its end moves to
// the SSL's list. Returns the bytes mapped.
u32 McpxStream::MapPacket(u32 ssl, u32 e) {
    Entry& en = entries_[e];
    const u32 remaining = en.p.maxSize - en.offset;
    const u32 maxBytes = SamplesToBytes(0xFFFF);
    const u32 voice = Voice();
    u32 mapped = 0;
    if (remaining > 0) {
        while (sslSegments_[ssl] < 16) {
            const u32 chunk = std::min(remaining - mapped, maxBytes);
            const u32 phys = en.phys + en.offset + mapped;   // (the run is the whole chunk)
            const u32 samples = BytesToSamples(chunk);
            const u32 bytes = SamplesToBytes(samples);
            if (samples == 0) break;
            const u32 seg = (ssl + voice * 2) * 16 + sslSegments_[ssl];
            pio_.Write(pio::kSetCurrentSsl, seg & 0x3FFFC0);
            pio_.Write(pio::kSslSegmentOffset + (seg & 0x3F) * 8, phys);
            pio_.Write(pio::kSslSegmentLength + (seg & 0x3F) * 8, (segmentFormat_ & 0xFFFF0000u) | samples);
            mapped += bytes;
            sslBytes_[ssl] += bytes;
            ++sslSegments_[ssl];
            if (mapped >= remaining) break;
        }
    }
    if (mapped >= remaining) {
        queue_.erase(std::find(queue_.begin(), queue_.end(), e));
        ssl_[ssl].push_back(e);
        return mapped;
    }
    en.offset += mapped;
    return mapped;
}

// orig: 0x0024B656: SET_VOICE_SSL_x = base | count, the SSL's notifier pending, the counts, command 2 queued, the
// voice started (once) and not starved.
void McpxStream::StartSsl(u32 ssl) {
    const u32 voice = Voice();
    pio_.Write(pio::kSetCurrentVoice, voice);
    pio_.Write(ssl == 0 ? pio::kVoiceSslA : pio::kVoiceSslB,
               ((ssl + voice * 2) << 12 & 0xFFFF00u) | (sslSegments_[ssl] & 0xFF));
    notifiers_.SetStatus(voice, ssl, 0x80);
    if (active_++ == 0) current_ = ssl;
    Queue(2);                                           // vf18(2, 0, 0, 0)
    StartVoice();
    SetPauseState(state_ & ~kStarved);
}

// orig: 0x0024BA21: the SSL emptied (count 0), its packets to the done list with `status`, the other SSL current.
void McpxStream::CompleteSsl(u32 ssl, u32 status) {
    if (sslSegments_[ssl] == 0) return;
    const u32 voice = Voice();
    pio_.Write(pio::kSetCurrentVoice, voice);
    pio_.Write(ssl == 0 ? pio::kVoiceSslA : pio::kVoiceSslB, (ssl + voice * 2) << 12 & 0xFFFF00u);
    MoveToDone(ssl_[ssl], status);
    sslSegments_[ssl] = 0;
    sslBytes_[ssl] = 0;
    --active_;
    current_ = (current_ - 1) & 1;
}

// orig: 0x0024B954 with another list than +0xC0: each entry to the done list, its status set.
void McpxStream::MoveToDone(List& from, u32 status) {
    while (!from.empty()) {
        const u32 e = from.front();
        from.pop_front();
        entries_[e].status = status;
        done_.push_back(e);
    }
}

// orig: 0x0024B954(+0xC0, PENDING): each done entry freed and completed (0x0024B72F: its pages unlocked, then
// 0x0024B282: *pdwCompletedSize = dwMaxSize, *pdwStatus = its status, then the DSSTREAMDESC callback (stream
// context, packet +0x10, status) if there is one; no completion event in the game's packets).
void McpxStream::SignalDone() {
    while (!done_.empty()) {
        const u32 e = done_.front();
        done_.pop_front();
        free_.push_back(e);
        Entry& en = entries_[e];
        if (en.phys) mem_.Unmap(en.phys);
        en.phys = 0;
        if (en.p.completedSize) *en.p.completedSize = en.p.maxSize;
        if (en.p.status) *en.p.status = en.status;
        if (callback_) callback_(en.p.context, en.status);
    }
}

// orig: 0x0024B9CA (command 2, run by DirectSoundDoWork while queued): cancelled once the voice is not playing.
void McpxStream::Signal() {
    if ((state_ & (kAllocated | kStarted)) != (kAllocated | kStarted)) Cancel(2);
    SignalDone();
}

// orig: 0x0024AB4A's walk: the queued entries taken off the APU list in order; each one still queued (an earlier one
// may have cancelled it) goes back to the list's tail (all three are persistent) and runs (vf20 0x0024BFEB). Entries
// queued meanwhile wait for the next DoWork.
void McpxStream::DoWork() {
    const std::deque<u32> pass = commands_;
    for (const u32 c : pass) {
        if (!Queued(c)) continue;
        Cancel(c);
        commands_.push_back(c);
        switch (c) {
        case 0: StopWait(); break;                      // 0x0024CA2A
        case 2: Signal(); break;                        // 0x0024B9CA
        case 3:                                         // 0x0024BBC7: the flush once neither started nor stopping
            if (!(state_ & (kStopping | kStarted))) Flush(false);
            break;
        default: break;
        }
    }
}

// orig: command 0 0x0024CA2A: KeQuerySystemTime; a stopping voice (0x8001) ends its wait once 500 ms have passed since
// the stop (+0x80 + 0x4C4B40, a signed 64-bit compare); a voice not stopping cancels the command.
void McpxStream::StopWait() {
    const s64 now = systemTime_ ? systemTime_() : 0;
    if ((state_ & (kStopping | kAllocated)) == (kStopping | kAllocated)) {
        if (now >= stopTime_ + kStopWait) StopWaitEnd();
    } else {
        Cancel(0);
    }
}

// orig: 0x0024C941: (under the APU's lock, the VP's list processing held by 0x00250249(1) / (0)) a voice still
// stopping leaves its VP list (0x0024C75A(0): the list registers and the voice's link word, not modelled, L; +0x65 =
// 0xFF, the stopping bit cleared), then the voice's vf14 (Service: nothing for a stopped stream).
void McpxStream::StopWaitEnd() {
    if ((state_ & (kStopping | kAllocated)) == (kStopping | kAllocated)) {
        inList_ = false;
        state_ &= ~kStopping;
    }
    Service();
}

// orig: 0x0024D1F1: an allocated voice waits for its stop (command 0's test run in a loop; each run queries the
// clock).
void McpxStream::WaitStopped() {
    if (!(state_ & kAllocated)) return;
    while (state_ & kStopping) {
        StopWait();
        if ((state_ & kStopping) && waitYield_) waitYield_();
    }
}

// orig: CMcpxStream_Discontinuity 0x0024B9F2: with packets queued or mapped (0x0024B33D) the stream ends after them
// (state 0x800: the service stops the voice instead of starving it); else Stop_Ex(0, 0, 1) at once.
u32 McpxStream::Discontinuity() {
    if (!queue_.empty() || active_ != 0) {
        state_ |= kDiscontinuity;
        return 0;
    }
    Stop(1);                                            // Stop_Ex 0x0024B8AF: timestamp 0 -> Stop(flags)
    return 0;
}

// orig: CDirectSoundStream_FlushEx 0x002477DE with a zero timestamp: a flag -> Stop_Ex 0x0024B8AF -> Stop(flags);
// none -> Flush(0) at once.
u32 McpxStream::FlushEx(u32 flags) {
    if (flags) Stop(flags);
    else Flush(false);
    return 0;
}

// orig: CMcpxStream_Stop 0x0024B3FC: a playing voice stopped (flag 1: no envelope release), the flush deferred to
// DirectSoundDoWork (command 3) unless flag 4.
void McpxStream::Stop(u32 flags) {
    if ((state_ & (kAllocated | kStarted)) != (kAllocated | kStarted)) return;
    if (flags & 6) {
        NotPortedText("CMcpxStream_Stop flags 2 / 4 (0x0024C9A6: the voice released, the end of stream 0x1000)");
        return;
    }
    StopVoice();                                        // 0x0024D2BC(0) (command 0: the 500 ms wait)
    if (!(flags & 4)) Queue(3);                         // vf18(3, 0, 0, 0)
}

// orig: CMcpxStream_GetStatus 0x0024B46E: READY with a free packet entry, PLAYING / PAUSED / STARVED of a playing
// voice, PLAYING while stopping (0x8001), 0x80000 once ended (0x2000).
u32 McpxStream::GetStatus() const {
    u32 r = free_.empty() ? 0u : 1u;
    if (state_ & kEnded) r |= 0x80000;
    if ((state_ & (kAllocated | kStarted)) == (kAllocated | kStarted)) {
        if (state_ & kPauseBits) {
            r |= 0x20000;
            if (state_ & kStarved) r |= 0x40000;
        } else {
            r |= 0x10000;
        }
    } else if ((state_ & (kStopping | kAllocated)) == (kStopping | kAllocated)) {
        r |= 0x10000;
    }
    return r;
}

// orig: CMcpxAPU_SynchPlayback 0x0024AC93: nothing without a synch-paused voice; else the voices of every owner whose
// state is allocated, started and synch-paused only (0x447 == 0x43) collected, the synch bit cleared on every owner,
// the count cleared, and under one VOICE_LOCK each collected voice's VOICE_PAUSE written without the pause bit (the
// high half of the previous word kept: 0 here).
void McpxStream::SynchPlayback(VpPio& pio, VpBridge& bridge, std::span<McpxStream* const> streams) {
    if (bridge.synchPaused == 0) return;
    std::vector<McpxStream*> owners(streams.begin(), streams.end());
    std::sort(owners.begin(), owners.end(),             // (the APU walks its voice table: hardware voice order)
              [](const McpxStream* a, const McpxStream* b) { return a->Voice() < b->Voice(); });
    std::vector<u32> voices;
    for (McpxStream* s : owners) {
        if ((s->state_ & 0x447) == 0x43) voices.push_back(s->Voice());
        s->state_ &= ~kPausedNoActivate;
    }
    bridge.synchPaused = 0;
    // (the FIFO's free space waited for; the first word of the pool block is written even when no voice was
    // collected: its contents are not known then, 0 here)
    pio.Write(pio::kSetCurrentVoice, voices.empty() ? 0u : voices[0]);
    pio.Write(pio::kVoiceLock, 1);
    u32 w = 0;
    for (u32 v : voices) {
        w = (w & 0xFFFF0000u) | v;
        pio.Write(pio::kVoicePause, w);
    }
    pio.Write(pio::kVoiceLock, 0);
}

// orig: vf14 0x0024BCBB (the DPC 0x0024AEB6 -> 0x0024A834 calls it for every started voice): the voice-end notifier
// (3) flushes; else the SSLs refilled, and with nothing left to play the voice is paused as starved.
void McpxStream::Service() {
    if (!(state_ & kStarted)) return;                   // (only started voices are in the APU's lists)
    const u32 voice = Voice();
    if (notifiers_.Status(voice, 3) == 1) {
        notifiers_.SetStatus(voice, 3, 0x80);
        if (state_ & kEndPending) state_ = static_cast<u16>((state_ & ~kEndPending) | kEnded);
        Flush(true);
        return;
    }
    if ((state_ & (kAllocated | kStarted)) != (kAllocated | kStarted)) return;
    const u32 r = Fill();
    if ((r & 3) == 1 && active_ == 0) {
        if (state_ & (kEnded | kDiscontinuity)) {
            StopVoice();                                // 0x0024B4C8 (+0x198 = 0)
            current_ = 0;
        } else if (!(state_ & kStarved)) {
            SetPauseState(kStarved);                    // (the pushed 0x400 alone: the other pause bits cleared)
        }
    }
}

// orig: CMcpxStream_Pause 0x0024BF5D.
void McpxStream::Pause(u32 mode) {
    switch (mode) {
    case 0:                                             // DSSTREAMPAUSE_RESUME
        if (state_ & kPauseQueue) {
            state_ &= ~kPauseQueue;
            Fill();
        }
        SetPauseState(state_ & ~(kPaused | kPausedNoActivate));
        break;
    case 1: SetPauseState((state_ & ~kPausedNoActivate) | kPaused); break;   // DSSTREAMPAUSE_PAUSE
    case 2: SetPauseState((state_ & ~kPaused) | kPausedNoActivate); break;
    case 3:
        if ((state_ & (kAllocated | kStarted)) == (kAllocated | kStarted)) SetPauseState(state_ & ~(kPaused | kPausedNoActivate));
        else state_ |= kPauseQueue;
        break;
    default: break;                                     // (E_FAIL)
    }
}

// orig: 0x0024CA84: the pause bits 0x444 replaced; VOICE_PAUSE when the voice is playing and its paused-ness changes.
void McpxStream::SetPauseState(u32 s) {
    const u32 next = s & kPauseBits, cur = state_ & kPauseBits;
    if (cur == next) return;
    if ((next & kPausedNoActivate) != (cur & kPausedNoActivate)) {
        // the APU's count of synch-paused voices (one hardware voice per stream here): 0x0024AC5B adds it (at most
        // 0x1D in all, else 0x88780032 and the count unchanged; the state is set either way) / 0x0024AC82 removes it
        if (next & kPausedNoActivate) {
            if (bridge_.synchPaused + 1 <= 0x1D) ++bridge_.synchPaused;
        } else {
            --bridge_.synchPaused;
        }
    }
    if ((cur != 0) != (next != 0) && (state_ & (kAllocated | kStarted)) == (kAllocated | kStarted))
        bridge_.Pause(id_, next != 0);                 // 0x140 = voice | 0x40000 when paused
    state_ = static_cast<u16>((state_ & 0xFBBB) | next);
}

// orig: 0x0024D621 (once): a stopping voice's wait first (0x0024D1F1), the voice's notifiers pending, VOICE_ON with
// the pause bit of the state (the voice joins the 2D / 3D list: +0x65).
void McpxStream::StartVoice() {
    if (state_ & kStarted) return;
    WaitStopped();
    state_ |= kStarted;
    inList_ = true;
    const u32 voice = Voice();
    for (u32 n = 0; n < VpNotifierMemory::kPerVoice; ++n) notifiers_.SetStatus(voice, n, 0x80);   // 0x0024C0E4
    bridge_.StartStream(id_, (state_ & kPauseBits) != 0);
}

// orig: 0x0024D2BC(0): not paused-no-activate; a playing voice stopped: state &= 0x3111, FMT bits 23 / 25 cleared
// and, in a VP list (+0x65 < 3, always for a started voice here), the stopping bit 0x8000 and VOICE_OFF; then the stop
// time (KeQuerySystemTime into +0x80) and command 0 queued. A voice not playing and not stopping cancels command 0.
// Without the clock service the stopping bit is not kept (the voice stops at once: the harnesses without the clock).
void McpxStream::StopVoice() {
    SetPauseState(state_ & ~kPausedNoActivate);
    if ((state_ & (kAllocated | kStarted)) != (kAllocated | kStarted)) {
        if (!(state_ & kStopping)) Cancel(0);
        return;
    }
    state_ &= 0x3111;
    if (inList_) {
        state_ |= kStopping;
        bridge_.StopStream(id_);
    }
    if (systemTime_) stopTime_ = systemTime_();
    else state_ &= ~kStopping;
    Queue(0);                                           // vf18(0, 0, 0, 0)
}

// orig: CMcpxStream_Flush 0x0024BAE6 (at passive level): both SSLs and the queue flushed (XMEDIAPACKET_STATUS_FLUSHED),
// the voice stopped (and its stop waited for, 0x0024D1F1), the packets signalled at once; `keepFlags` false clears the
// end-of-stream flags.
void McpxStream::Flush(bool keepFlags) {
    if (state_ & kAllocated) {
        for (u32 c = 0; c < 6; ++c) Cancel(c);          // (commands 0-5 cancelled)
        for (u32 s = 0; s < 2; ++s) CompleteSsl(s, kFlushed);
        current_ = 0;
        StopVoice();
        WaitStopped();
        MoveToDone(queue_, kFlushed);
        Signal();                                       // 0x0024B9CA
    }
    if (!keepFlags) state_ &= 0xCFFF;
}

}  // namespace fut::audio::vp
