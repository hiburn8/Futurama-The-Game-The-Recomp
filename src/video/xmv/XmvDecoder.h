// The XMV library's decoder object as the game drives it (default.xbe section XMV: XMVDecoder_CreateDecoderForPackets
// 0x00264400, XMVDecoder_CloseDecoder 0x00264717, GetVideoDescriptor 0x002647F9, the audio stream callback
// 0x00264815, EnableAudioStream 0x00264866, GetAudioStream 0x002649E3, GetNextFrame 0x00264A02, GetCurrentTime
// 0x0026507F; the XDK names are L, docs/analysis/movies_port.md 2.7 / 2.8). The packet ring, the audio intake into the
// DirectSound streams, the frame pacing on the audio clock and the display fields, the end; the frames themselves are
// Wmv2Decoder's (bit-exact). The platform calls the library makes (QueryPerformanceCounter, D3DDevice_GetDisplayMode /
// GetDisplayFieldStatus, DirectSoundCreateStream / SynchPlayback / DoWork, Sleep) go through XmvPlatform.
#pragma once

#include "Wmv2Decoder.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace xmv {

class XmvImage;

// XMEDIAPACKET as the library fills it (the status / completed-size words are the decoder's per-slot arrays).
struct XmvMediaPacket {
    const uint8_t* data = nullptr;    // pvBuffer
    uint32_t size = 0;                // dwMaxSize
    uint32_t* completedSize = nullptr;
    uint32_t* status = nullptr;       // pdwStatus (XMEDIAPACKET_STATUS_PENDING 0x8000000A until it completes)
    uint32_t context = 0;             // pContext (the clock stream: the cumulative audio ms at the packet's end)
};

constexpr uint32_t kPacketPending = 0x8000000Au;   // XMEDIAPACKET_STATUS_PENDING
constexpr uint32_t kStreamPlaying = 0x00010000u;   // DSSTREAMSTATUS_PLAYING (GetStatus; L: the XDK name)

// IDirectSoundStream as the library calls it.
class XmvAudioStream {
public:
    virtual ~XmvAudioStream() = default;
    virtual int32_t Pause(uint32_t flags) = 0;                    // IDirectSoundStream_Pause (2: until SynchPlayback)
    virtual int32_t Process(const XmvMediaPacket& packet) = 0;    // vt+0x10 (XMediaObject Process, no output packet)
    virtual int32_t Discontinuity() = 0;                          // vt+0x14
    virtual uint32_t GetStatus() = 0;                             // vt+0x0C
    virtual int32_t FlushEx(uint64_t timestamp, uint32_t flags) = 0;   // IDirectSoundStream_FlushEx
    virtual void SetVolume(int32_t hundredthsDb) = 0;             // IDirectSoundStream_SetVolume (the game's)
};

// DSSTREAMDESC as EnableAudioStream fills it.
struct XmvStreamDesc {
    uint32_t flags = 0;
    uint32_t maxAttachedPackets = 2;
    std::vector<uint8_t> format;      // the WAVEFORMATEX (+ the ADPCM extra word) bytes
    const uint8_t* mixBins = nullptr; // the library's default table entry (0x0026C9D8 + 8 * flags) or null
    // lpfnCallback 0x00264815 with lpvContext = the decoder: (packet context, status, the QueryPerformanceCounter at
    // the completion: the port reads it where the stream completes the packet, the original inside the callback, L)
    std::function<void(uint32_t context, uint32_t status, uint64_t qpc)> callback;
};

// The D3D surface GetNextFrame converts into (D3DSurface_GetDesc's format, D3DSurface_LockRect's bits and pitch).
class XmvSurface {
public:
    virtual ~XmvSurface() = default;
    virtual uint32_t Format() = 0;
    virtual uint8_t* Lock(int32_t& pitch) = 0;
};

class XmvPlatform {
public:
    virtual ~XmvPlatform() = default;
    virtual uint64_t QueryPerformanceCounter() = 0;
    virtual uint64_t QueryPerformanceFrequency() = 0;
    virtual uint32_t DisplayRefreshRate() = 0;                        // D3DDevice_GetDisplayMode's RefreshRate
    // D3DDevice_GetDisplayFieldStatus: the field (D3DFIELD_ODD 1, L) and the VBlank count.
    virtual void GetDisplayFieldStatus(uint32_t& field, uint32_t& vblankCount) = 0;
    // DirectSoundCreateStream (null: failed).
    virtual std::unique_ptr<XmvAudioStream> CreateStream(const XmvStreamDesc& desc) = 0;
    // DirectSoundCreate + IDirectSound_SynchPlayback + Release: the streams paused with flag 2 start together.
    virtual int32_t SynchPlayback() = 0;
    virtual void DoWork() = 0;                                        // DirectSoundDoWork
    virtual void Sleep(uint32_t ms) = 0;
};

class XmvDecoder {
public:
    enum Result : uint32_t { kNoFrame = 0, kNewFrame = 1, kEnd = 2, kFail = 3 };
    // The game's packet callbacks (0x000EDC30 / 0x000EDEF0, stdcall with the context the player passed).
    using GetNextPacket = std::function<int32_t(uint8_t** packet, uint32_t* nextSize)>;
    using ReleasePreviousPacket = std::function<int32_t(uint64_t offset, uint32_t nextSize)>;

    struct VideoDescriptor {          // XMVVIDEO_DESC as 0x002647F9 writes it (+8, the frame rate slot: never written)
        uint32_t width = 0, height = 0, unwritten = 0, audioStreams = 0;
    };

    // orig: 0x00264400 (flags, the file header's first bytes, context, getNext, releasePrevious, &decoder): a negative
    // HRESULT (no decoder) or 0.
    static int32_t CreateForPackets(uint32_t flags, const uint8_t* header, const XmvImage& image, XmvPlatform& platform,
                                    GetNextPacket getNext, ReleasePreviousPacket releasePrevious,
                                    std::unique_ptr<XmvDecoder>& out);
    // orig: 0x00264717 (the decoder deleted by the caller after it).
    void Close();
    ~XmvDecoder();
    VideoDescriptor GetVideoDescriptor() const;                       // orig: 0x002647F9
    // orig: 0x00264866 (decoder, index, flags, mix bins, &stream): a negative HRESULT when a stream fails.
    int32_t EnableAudioStream(uint32_t index, uint32_t flags, const uint8_t* mixBins, XmvAudioStream** stream);
    XmvAudioStream* GetAudioStream(uint32_t index) const;             // orig: 0x002649E3 (AddRef'd: not counted)
    // orig: 0x00264A02 (decoder, surface, &result, &time): `surface` null without one. Returns the HRESULT (0, or a
    // failed callback's / stream call's).
    int32_t GetNextFrame(XmvSurface* surface, uint32_t& result, uint32_t* time);
    uint32_t GetCurrentTime() const;                                  // orig: 0x0026507F (ms since the clock start)

private:
    XmvDecoder(XmvPlatform& platform) : platform_(platform) {}
    void OnPacketCompleted(uint32_t context, uint32_t status, uint64_t qpc);   // orig: 0x00264815
    uint32_t Due(uint32_t timeMs) const;              // ceil((time + lag) * 65536 / period) (the low dword)
    // A packet slot of the ring and its per-track status / completed-size words (+0x140.. / +0x150..).
    struct Slot {
        uint8_t* packet = nullptr;
        std::vector<uint32_t>* status = nullptr;
        std::vector<uint32_t>* completed = nullptr;
    };

    XmvPlatform& platform_;
    const XmvImage* image_ = nullptr;                 // (the section: the default mix-bin table 0x0026C9D8)
    Wmv2Decoder video_;
    uint32_t flags_ = 0;                              // +0x18
    GetNextPacket getNext_;                           // +0x1C (context +0x24)
    ReleasePreviousPacket releasePrevious_;           // +0x20
    uint32_t headerSkip_ = 0;                         // +0x28 ((3 * tracks + 9) * 4)
    uint32_t firstPacketSize_ = 0;                    // +0x2C (the header's +4)
    uint32_t skip_ = 0;                               // +0x30 (the next packet's header bytes)
    bool restart_ = false;                            // +0x34 (no writer in the library's game-reachable code)
    uint32_t framesLeft_ = 0;                         // +0x3C
    uint32_t width_ = 0, height_ = 0;                 // +0x40 / +0x44
    uint32_t tracks_ = 0;                             // +0x48
    struct Track { uint32_t tag, channels, rate, bits, flags; };   // +0x4C -> +0x160, 20 bytes each
    std::vector<Track> trackDesc_;
    uint32_t nextPacketSize_ = 0;                     // +0x50
    uint64_t fileOffset_ = 0;                         // +0x58
    uint8_t* loaded_ = nullptr;                       // +0x60 (just loaded)
    uint8_t* videoSlot_ = nullptr;                    // +0x64 (its frames being decoded)
    uint8_t* waiting_ = nullptr;                      // +0x68 (its audio playing; Create: 1, kMarker)
    uint8_t* free_ = nullptr;                         // +0x6C (a buffer the game may fill; Create: 1, kMarker)
    const uint8_t* videoHeader_ = nullptr;            // +0x70
    bool audioDone_ = false;                          // +0x74 (the waiting packet's audio completed)
    bool stop_ = false, noLoop_ = false;              // +0x78 / +0x79 (no writer in the game-reachable code)
    bool last_ = false;                               // +0xAC
    uint64_t clockStart_ = 0;                         // +0xB0 (QPC; 0 = not started)
    uint32_t clockVBlank_ = 0;                        // +0xB8
    uint32_t fieldPeriod_ = 0;                        // +0xBC (ms in 16.16)
    uint32_t audioMs_ = 0;                            // +0xC0 (the cumulative audio time of the packets taken)
    uint32_t audioEnd_ = 0;                           // +0xC4
    int32_t lag_ = 0;                                 // +0xC8 (written by the clock stream's completions)
    uint32_t curTime_ = 0;                            // +0xE4
    bool delivered_ = true;                           // +0xE8 (the current frame was presented or skipped)
    uint32_t aheadTime_ = 0xFFFFFFFFu;                // +0xF8 (-1: no frame decoded ahead)
    const uint8_t* frameCursor_ = nullptr;            // +0x08 (the next frame header)
    int32_t clockTrack_ = -1;                         // +0x130
    std::vector<XmvAudioStream*> streams_;            // +0x134
    std::vector<std::unique_ptr<XmvAudioStream>> owned_;
    std::vector<uint32_t> enabled_, started_;         // +0x138 / +0x13C
    // the ring's word arrays: +0x14C / +0x15C (free), +0x140 / +0x150 (loaded), +0x144 / +0x154 (video), +0x148 /
    // +0x158 (waiting); Create gives the free and the waiting slots one array each
    std::array<std::vector<uint32_t>, 4> arrays_;
    std::vector<uint32_t>* freeStatus_ = nullptr;     // +0x14C
    std::vector<uint32_t>* freeCompleted_ = nullptr;  // +0x15C
    std::vector<uint32_t>* loadedStatus_ = nullptr;   // +0x140
    std::vector<uint32_t>* loadedCompleted_ = nullptr;   // +0x150
    std::vector<uint32_t>* videoStatus_ = nullptr;    // +0x144
    std::vector<uint32_t>* videoCompleted_ = nullptr;   // +0x154
    std::vector<uint32_t>* waitingStatus_ = nullptr;  // +0x148
    std::vector<uint32_t>* waitingCompleted_ = nullptr;   // +0x158
    uint64_t qpfMs_ = 1;                              // 0x0028BF60 (QPF / 1000, cached once)
};

}  // namespace xmv
