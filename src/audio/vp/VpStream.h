// The XDK library's stream voice (DSOUND CMcpxStream, vtable 0x002A605C) on the port's VP: the packets an
// IDirectSoundStream is given (Process), their mapping into the voice's two SSLs of up to 16 physical-address segments
// (SET_CURRENT_SSL / the segment words / SET_VOICE_SSL_A / _B), the voice start, the notifier-driven refill (the DPC's
// service of each started voice, vf14) and the deferred completion of the played packets (the persistent command 2
// that DirectSoundDoWork runs: each packet's status and completed size written then), the pause modes and the flush.
// docs/analysis/audio_port.md 5.8. Hand-ported from the library's code (H); the VP side of the contract (how the
// hardware walks the SSLs and sets the notifiers) is the VP model's (L, Vp.cpp).
//
// Not ported (the game never reaches them; NotPortedText where they would start): the DSSTREAMFLAGS that make the
// voice release at a flush or signal packets from the DPC (+0x88 +0xA bit 2, +0xB bits 5-6), the end-of-stream flags
// 0x1000 / 0x2000 of CMcpxStream_Stop's flags 2 / 4 (and the notifier's +0xE byte they read), the voice-end notifier
// (3) the hardware never sets for a stream in the model, streams of more than one hardware voice (more than two
// channels).
// Since 2026-10-21 (open items round 5) also the XMV library's stream (movies_port.md 2.8): pause mode 2 with the APU's
// count of synch-paused voices and IDirectSound_SynchPlayback (CMcpxAPU 0x0024AC5B / 0x0024AC82 / 0x0024AC93), the
// DSSTREAMDESC callback with each packet's context (0x0024B282), Discontinuity (0x0024B9F2), FlushEx with a flag
// (Stop 0x0024B3FC, the deferred flush, command 3 0x0024BBC7) and GetStatus (0x0024B46E).
// Since 2026-10-21 (Temporary audit) the stop wait: a stopped voice stays "stopping" (state 0x8000, GetStatus PLAYING)
// until 500 ms of KeQuerySystemTime have passed since the stop (0x0024D2BC stores the time, the persistent command 0
// 0x0024CA2A checks it at every DirectSoundDoWork, 0x0024C941 ends it); a flush or a restart busy-waits for it
// (0x0024D1F1). The clock is a service (SetSystemTime): the backend's real KeQuerySystemTime, the tests' emulated one.
// Not modelled: the VP voice-list unlink of 0x0024C75A (the list lock / head registers and the voice's link word: the
// VP model keeps no voice lists, L).
#pragma once

#include "audio/vp/Vp.h"

#include <deque>
#include <functional>
#include <span>
#include <vector>

namespace fut::audio::vp {

class VpBridge;

class McpxStream {
public:
    // XMEDIAPACKET (the fields the game passes: no completion event; the context only with a callback).
    struct Packet {
        const u8* buffer = nullptr;
        u32 maxSize = 0;
        u32* completedSize = nullptr;
        u32* status = nullptr;
        u32 context = 0;                                // +0x10 (the callback's packet context)
    };
    // DSSTREAMDESC lpfnCallback(lpvContext, packet context, status) (the XMV library's 0x00264815): the bound
    // stream context is the caller's; null: none (the sound layer's streams).
    using Callback = std::function<void(u32 packetContext, u32 status)>;
    // KeQuerySystemTime (100 ns units; the kernel's xboxkrnl export the library imports): one call per query, as the
    // library makes them. Unset (a harness without the clock): the stop wait is skipped (the voice stops at once).
    using SystemTime = std::function<s64()>;
    static constexpr s64 kStopWait = 5000000;           // 0x004C4B40 (0x0024CA59): 500 ms
    static constexpr u32 kPending = 0x8000000Au;       // XMEDIAPACKET_STATUS_PENDING
    static constexpr u32 kSuccess = 0;                  // XMEDIAPACKET_STATUS_SUCCESS
    static constexpr u32 kFlushed = 0x80004004u;        // XMEDIAPACKET_STATUS_FLUSHED (E_ABORT)
    static constexpr u32 kNoPacket = 0x88780032u;       // DSERR_..: no free packet entry (Process)

    // The stream of buffer `id` of `bridge` (its voice: VpBridge::CreateStream); `maxPackets` = DSSTREAMDESC's
    // dwMaxAttachedPackets; the format: WAVEFORMATEX's tag, channels, bits, block align.
    McpxStream(VpPio& pio, VpBridge& bridge, VpMemoryMap& mem, VpNotifierMemory& notifiers, u32 id, u32 maxPackets,
               u16 formatTag, u16 channels, u16 bits, u16 blockAlign);
    ~McpxStream();

    u32 Process(const Packet& p);                       // CDirectSoundStream_Process 0x00247716 -> 0x0024BE8F
    void Pause(u32 mode);                               // CMcpxStream_Pause 0x0024BF5D (DSSTREAMPAUSE_*)
    void Service();                                     // vf14 0x0024BCBB: the DPC's notifier service
    void DoWork();                                      // the queued deferred commands (DirectSoundDoWork, 0x0024AB4A)
    void Flush(bool keepFlags);                         // CMcpxStream_Flush 0x0024BAE6
    u32 Voice() const;                                  // the hardware voice (kVoices: none)
    void SetCallback(Callback cb) { callback_ = std::move(cb); }
    void SetSystemTime(SystemTime t) { systemTime_ = std::move(t); }
    // (port) called between two queries of the busy wait 0x0024D1F1, which the library runs with the APU's lock
    // released: the backend lets its audio thread (the hardware) run meanwhile.
    void SetWaitYield(std::function<void()> y) { waitYield_ = std::move(y); }
    u32 Discontinuity();                                // CMcpxStream_Discontinuity 0x0024B9F2
    u32 FlushEx(u32 flags);                             // CDirectSoundStream_FlushEx 0x002477DE (timestamp 0)
    u32 GetStatus() const;                              // CMcpxStream_GetStatus 0x0024B46E (DSSTREAMSTATUS_*)
    // CMcpxAPU_SynchPlayback 0x0024AC93 (IDirectSound_SynchPlayback): every stream paused by mode 2 un-paused at
    // once (one VOICE_LOCK around their VOICE_PAUSE writes); the APU's count of such voices (+0x774) is the bridge's.
    static void SynchPlayback(VpPio& pio, VpBridge& bridge, std::span<McpxStream* const> streams);

private:
    struct Entry {
        Packet p;
        u32 offset = 0;                                 // +0x20: bytes already mapped into segments
        u32 status = kPending;                          // +0x24: the status to report
        u32 phys = 0;                                   // (port) the packet's mapping (MmLockUnlockBufferPages)
    };
    using List = std::deque<u32>;                       // entry indices (the original's LIST_ENTRY rings)

    u32 BytesToSamples(u32 bytes) const;                // 0x0024B303
    u32 SamplesToBytes(u32 samples) const;              // 0x0024B31C
    bool AllocateVoice();                               // 0x0024CE8A (the voice of a 2D stream exists from creation)
    u32 Fill();                                         // 0x0024BBD7: completions, then the SSLs refilled
    bool FillSsl(u32 ssl);                              // 0x0024B8E8
    u32 MapPacket(u32 ssl, u32 entry);                  // 0x0024B4F5
    void StartSsl(u32 ssl);                             // 0x0024B656
    void CompleteSsl(u32 ssl, u32 status);              // 0x0024BA21
    void MoveToDone(List& from, u32 status);            // 0x0024B954 (another list: to +0xC0 with the status)
    void SignalDone();                                  // 0x0024B954(+0xC0): every done packet completed
    void Signal();                                      // 0x0024B9CA (command 2)
    void SetPauseState(u32 state);                      // 0x0024CA84
    void StartVoice();                                  // 0x0024D621
    void StopVoice();                                   // 0x0024D2BC(0)
    void Stop(u32 flags);                               // CMcpxStream_Stop 0x0024B3FC
    void StopWait();                                    // command 0 0x0024CA2A
    void StopWaitEnd();                                 // 0x0024C941
    void WaitStopped();                                 // 0x0024D1F1
    // The deferred commands (vf18 0x0024B7BB queues at the APU list's tail unless queued, vf1C 0x0024B7EA cancels;
    // 0, 2 and 3 are persistent: DirectSoundDoWork 0x0024AB4A runs them in queue order and keeps them queued).
    void Queue(u32 command);
    void Cancel(u32 command);
    bool Queued(u32 command) const;

    VpPio& pio_;
    VpBridge& bridge_;
    VpMemoryMap& mem_;
    VpNotifierMemory& notifiers_;
    u32 id_;
    u16 formatTag_, channels_, bits_, blockAlign_;
    u32 segmentFormat_ = 0;                             // +0x8C: the segment length word's high half (0x0024BDA0)
    u16 state_ = 0;                                     // +0x12
    std::vector<Entry> entries_;                        // +0xC8 (0x30 bytes each)
    List free_, queue_, done_;                          // +0xB8, +0xB0, +0xC0
    List ssl_[2];                                       // +0x90, +0xA0: the packets mapped into SSL A / B
    u32 sslSegments_[2] = {0, 0};                       // +0x98, +0xA8
    u32 sslBytes_[2] = {0, 0};                          // +0x9C, +0xAC
    u32 current_ = 0;                                   // +0x198: the SSL the voice plays
    u32 active_ = 0;                                    // +0x19C: SSLs programmed
    // the queued commands in the APU list's order (+0xD8 + 0x20 * n entries): 0 the stop wait, 2 the packets'
    // completion (Signal), 3 the deferred flush (0x0024BBC7)
    std::deque<u32> commands_;
    s64 stopTime_ = 0;                                  // +0x80: KeQuerySystemTime at the stop
    bool inList_ = false;                               // +0x65 < 3: the voice is in a VP list (0x0024D621 .. 0x0024C75A)
    Callback callback_;                                 // DSSTREAMDESC +0x0C / +0x10 (owner +0xBC / +0xC0)
    SystemTime systemTime_;
    std::function<void()> waitYield_;
};

}  // namespace fut::audio::vp
