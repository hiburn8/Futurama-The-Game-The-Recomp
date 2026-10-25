// The XMV library's decoder object; see XmvDecoder.h and docs/analysis/movies_port.md 2.7 / 2.8.
#include "XmvDecoder.h"

#include "XmvImage.h"

#include <cstring>

namespace xmv {

namespace {

uint32_t Rd(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// The ring's "1" (Create: +0x68 = +0x6C = 1): a slot that is taken without a packet behind it.
uint8_t kMarker = 0;

}  // namespace

int32_t XmvDecoder::CreateForPackets(uint32_t flags, const uint8_t* header, const XmvImage& image,
                                     XmvPlatform& platform, GetNextPacket getNext,
                                     ReleasePreviousPacket releasePrevious,
                                     std::unique_ptr<XmvDecoder>& out) {   // orig: 0x00264400
    // the header: +0x14 width, +0x18 height, +0x20 the track count, +0x24 + 12 i the tracks, +4 the first packet's size
    const uint32_t width = Rd(header + 0x14), height = Rd(header + 0x18);
    const uint32_t tracks = uint32_t(header[0x20] | header[0x21] << 8);
    // one XMemAlloc block (the two frames' planes, then the state, the rows, the track arrays); E_OUTOFMEMORY
    // 0x8007000E when it fails. The port's Wmv2Decoder holds the planes and the per-row arrays.
    std::unique_ptr<XmvDecoder> d(new XmvDecoder(platform));
    d->image_ = &image;
    const uint32_t firstTag = tracks ? uint32_t(header[0x24] | header[0x25] << 8) : 0;
    if (!d->video_.Create(image, width, height, firstTag)) return int32_t(0x8007000Eu);
    d->flags_ = flags;                                // +0x18
    d->getNext_ = std::move(getNext);                 // +0x1C / +0x24 (the context: the game's player)
    d->releasePrevious_ = std::move(releasePrevious); // +0x20
    if (platform.QueryPerformanceFrequency() / 1000 == 0) return int32_t(0x8007000Eu);   // (UB guard: a zero divisor)
    d->qpfMs_ = platform.QueryPerformanceFrequency() / 1000;   // 0x0028BF60 (QPF / 1000, the first Create's)
    const uint32_t rate = platform.DisplayRefreshRate();   // D3DDevice_GetDisplayMode
    d->fieldPeriod_ = rate == 60 ? 0x10AEF0u : rate ? 0x3E80000u / rate : 0x10AEF0u;   // (UB guard: rate 0)
    d->framesLeft_ = 0;                               // +0x3C
    d->width_ = width, d->height_ = height;           // +0x40 / +0x44
    d->aheadTime_ = 0xFFFFFFFFu;                      // +0xF8
    d->delivered_ = true;                             // +0xE8
    d->curTime_ = 0;                                  // +0xE4
    d->tracks_ = tracks;                              // +0x48
    for (uint32_t i = 0; i < tracks; ++i) {           // +0x4C = +0x160: the descriptors as dwords (20 bytes each)
        const uint8_t* t = header + 0x24 + 12 * i;
        d->trackDesc_.push_back({uint32_t(t[0] | t[1] << 8), uint32_t(t[2] | t[3] << 8), Rd(t + 4),
                                 uint32_t(t[8] | t[9] << 8), uint32_t(t[10] | t[11] << 8)});
    }
    d->headerSkip_ = d->skip_ = (tracks * 3 + 9) * 4; // +0x28 / +0x30
    d->firstPacketSize_ = Rd(header + 4);             // +0x2C
    d->nextPacketSize_ = Rd(header + 4);              // +0x50
    d->clockTrack_ = -1;                              // +0x130
    d->streams_.assign(tracks, nullptr);              // +0x134
    d->owned_.resize(tracks);
    d->enabled_.assign(tracks, 0);                    // +0x138
    d->started_.assign(tracks, 0);                    // +0x13C
    for (auto& a : d->arrays_) a.assign(tracks, 0);
    d->freeStatus_ = &d->arrays_[0];                  // +0x14C (cleared)
    d->waitingStatus_ = &d->arrays_[1];               // +0x148 (cleared)
    d->freeCompleted_ = &d->arrays_[2];               // +0x15C
    d->waitingCompleted_ = &d->arrays_[3];            // +0x158
    d->free_ = &kMarker;                              // +0x6C = 1
    d->waiting_ = &kMarker;                           // +0x68 = 1
    out = std::move(d);
    return 0;
}

XmvDecoder::~XmvDecoder() = default;

void XmvDecoder::Close() {                            // orig: 0x00264717
    for (XmvAudioStream* s : streams_)
        if (s) s->FlushEx(0, 1);                      // IDirectSoundStream_FlushEx(s, 0, DSSTREAMFLUSHEX_ASYNC)
    for (size_t i = 0; i < streams_.size(); ++i) {    // wait while a stream still plays (vt+0x0C)
        if (!streams_[i]) continue;
        if (streams_[i]->GetStatus() & kStreamPlaying) {
            platform_.Sleep(1);
            platform_.DoWork();
            --i;                                      // (the same stream again)
        }
    }
    for (size_t i = 0; i < streams_.size(); ++i) {    // vt+4 Release
        streams_[i] = nullptr;
        owned_[i].reset();
    }
    // +0x7C (an overlapped file handle: 0 for a packet decoder, so GetOverlappedResult / CloseHandle(0) fail on the
    // console) and XMemFree +0x10 / +0xC: nothing to release in the port
}

XmvDecoder::VideoDescriptor XmvDecoder::GetVideoDescriptor() const {   // orig: 0x002647F9
    VideoDescriptor v;
    v.width = width_, v.height = height_, v.audioStreams = tracks_;
    return v;
}

void XmvDecoder::OnPacketCompleted(uint32_t context, uint32_t status, uint64_t qpc) {   // orig: 0x00264815
    if (status != 0 || context == 0) return;
    lag_ = int32_t(uint32_t((qpc - clockStart_) / qpfMs_) - context);   // +0xC8 = (QPC - start) ms - context
}

int32_t XmvDecoder::EnableAudioStream(uint32_t index, uint32_t flags, const uint8_t* mixBins,
                                      XmvAudioStream** stream) {   // orig: 0x00264866
    if (index >= tracks_) return int32_t(0x80070057u);   // (UB guard: the original reads past the descriptors)
    // a track flagged as the first of three 5.1 pairs (+0x10 == 1) with no explicit arguments enables three
    uint32_t end = index + 1;
    if (flags == 0 && !mixBins && !stream && trackDesc_[index].flags == 1) end = index + 3;
    if (end > tracks_) end = tracks_;                 // (UB guard)
    uint32_t i = index;
    for (; i < end; ++i) {
        if (streams_[i]) {                            // an enabled one is released first (vt+4)
            streams_[i] = nullptr;
            owned_[i].reset();
        }
        const Track& t = trackDesc_[i];
        XmvStreamDesc desc;
        desc.flags = flags;
        desc.maxAttachedPackets = 2;
        desc.callback = [this](uint32_t context, uint32_t status, uint64_t qpc) {   // 0x00264815, context = this
            OnPacketCompleted(context, status, qpc);
        };
        // the mix bins: the caller's, else the library's default table entry when the track's flags & 7
        desc.mixBins = mixBins;
        if (!mixBins && (t.flags & 7) && image_->Contains(0x0026C9D8u + 8 * t.flags, 8))
            desc.mixBins = image_->At(0x0026C9D8u + 8 * t.flags);   // (no file's track has such flags)
        // the WAVEFORMATEX: Xbox ADPCM (0x69) with blockAlign 36 * channels, avgBytes = blockAlign * rate / 64,
        // cbSize 2, 64 samples a block; else PCM with blockAlign = bits * channels / 8, avgBytes = blockAlign * rate
        auto put16 = [&](uint32_t v) {
            desc.format.push_back(uint8_t(v));
            desc.format.push_back(uint8_t(v >> 8));
        };
        auto put32 = [&](uint32_t v) {
            put16(v & 0xFFFF);
            put16(v >> 16);
        };
        put16(t.tag);
        put16(t.channels);
        put32(t.rate);
        if (t.tag == 0x69) {
            const uint16_t align = uint16_t(t.channels * 36);
            put32(uint32_t(uint32_t(align) * t.rate) >> 6);
            put16(align);
            put16(t.bits);
            put16(2);
            put16(64);
        } else {
            const uint16_t align = uint16_t(int32_t(uint16_t(t.bits) * uint16_t(t.channels)) / 8);
            put32(uint32_t(align) * t.rate);
            put16(align);
            put16(t.bits);
            put16(0);
        }
        owned_[i] = platform_.CreateStream(desc);     // DirectSoundCreateStream
        if (!owned_[i]) return int32_t(0x80004005u);  // (its HRESULT: the platform reports none, E_FAIL; ignored by the game)
        streams_[i] = owned_[i].get();
        enabled_[i] = 1;                              // +0x138
        if (clockTrack_ == -1) clockTrack_ = int32_t(i);   // the first enabled stream gives the clock
    }
    if (stream) *stream = streams_[i - 1];            // (AddRef'd)
    return 0;
}

XmvAudioStream* XmvDecoder::GetAudioStream(uint32_t index) const {   // orig: 0x002649E3
    return index < streams_.size() ? streams_[index] : nullptr;   // (AddRef'd; null: the original crashes there)
}

uint32_t XmvDecoder::GetCurrentTime() const {         // orig: 0x0026507F
    if (!clockStart_) return 0;
    return uint32_t((platform_.QueryPerformanceCounter() - clockStart_) / qpfMs_);
}

uint32_t XmvDecoder::Due(uint32_t timeMs) const {
    // MOV EDI,time; ADD EDI,lag; MUL 0x10000; ADD EAX,0xFFFF; ADC EDX,0; the 64-bit division by the field period
    const uint64_t t = uint64_t(uint32_t(timeMs + uint32_t(lag_)));
    return uint32_t((t * 0x10000u + 0xFFFFu) / fieldPeriod_);
}

int32_t XmvDecoder::GetNextFrame(XmvSurface* surface, uint32_t& result, uint32_t* time) {   // orig: 0x00264A02
    uint32_t due = 0xFFFFFFFFu;                       // [ebp-0x14]
    uint32_t presentTime = 0;                         // [ebp-8] (EDI)
    bool synch = false;                               // [ebp-0x10]: a stream's first packet or the clock's start
    result = kFail;
    // ---- the intake: a packet from the game when a buffer is free and none is loaded
    if (free_ && !loaded_) {
        if (nextPacketSize_ == 0) {
            if (waiting_ && audioDone_) {             // the last packet's frames decoded and its audio played
                result = kEnd;
                return 0;
            }
        } else {
            uint8_t* pkt = nullptr;
            uint32_t size = 0;
            const int32_t hr = getNext_(&pkt, &size);
            if (hr < 0) return hr;
            if (pkt) {
                loadedStatus_ = freeStatus_, freeStatus_ = nullptr;          // +0x140 = +0x14C
                loadedCompleted_ = freeCompleted_, freeCompleted_ = nullptr; // +0x150 = +0x15C
                fileOffset_ += size;                  // +0x58 (64-bit)
                loaded_ = pkt;                        // +0x60
                free_ = nullptr;                      // +0x6C
                const uint8_t* p = pkt + skip_;       // (packet 0: after the file header)
                skip_ = 0;
                nextPacketSize_ = Rd(p);              // +0x50
                p += 4;
                videoHeader_ = p;                     // +0x70
                audioMs_ += Rd(p + 4);                // +0xC0 += the packet's audio ms
                const uint8_t* audio = p + (Rd(p) & 0x7FFFFF) + 8;
                const uint8_t* sizes = p + 8;
                for (uint32_t i = 0; i < tracks_; ++i, sizes += 4) {
                    if (enabled_[i]) {
                        if (!started_[i]) {           // the stream's first packet: held until SynchPlayback
                            const int32_t h = streams_[i]->Pause(2);
                            if (h < 0) return h;
                            synch = true;
                        }
                        XmvMediaPacket mp;
                        mp.data = audio;
                        mp.size = Rd(sizes);
                        mp.completedSize = &(*loadedCompleted_)[i];
                        mp.status = &(*loadedStatus_)[i];
                        mp.context = int32_t(i) == clockTrack_ ? audioMs_ : 0;
                        const int32_t h = streams_[i]->Process(mp);   // vt+0x10(&packet, NULL)
                        if (h < 0) return h;
                        started_[i] = 1;
                    } else if (started_[i]) {
                        const int32_t h = streams_[i]->Discontinuity();   // vt+0x14
                        if (h < 0) return h;
                        started_[i] = 0;
                    }
                    audio += (Rd(sizes) + 3) & ~3u;
                }
            }
        }
    }
    // ---- the video slot: the loaded packet's frames
    if (loaded_ && !videoSlot_) {
        videoSlot_ = loaded_;                         // +0x64
        videoStatus_ = loadedStatus_, loadedStatus_ = nullptr;
        videoCompleted_ = loadedCompleted_, loadedCompleted_ = nullptr;
        loaded_ = nullptr;
        last_ = nextPacketSize_ == 0 || stop_;        // +0xAC
        if (last_) audioEnd_ = audioMs_;              // +0xC4
        const uint32_t vh = Rd(videoHeader_);
        const uint8_t* v = videoHeader_ + tracks_ * 4 + 8;
        if (vh & 0x80000000u) {                       // the extension header (the first packet): +0xA0..+0xA8
            video_.SetExtension(Rd(v));
            v += 4;
        }
        frameCursor_ = v;                             // +0x08
        framesLeft_ = (vh >> 23) & 0xFF;              // +0x3C
    }
    // ---- decode ahead, pace, swap
    uint32_t field = 0, vblank = 0, fields = 0;       // [ebp-0x24] / [ebp-0x20] / [ebp+8]
    for (;;) {
        if (aheadTime_ == 0xFFFFFFFFu && framesLeft_) {   // 0x00264CCE
            const uint32_t h = Rd(frameCursor_);
            aheadTime_ = curTime_ + (h >> 17);        // +0xF8
            const uint32_t size = (h & 0x1FFFF) * 4 + 4;
            const uint8_t* data = frameCursor_ + 4;   // +8 = the frame, +4 = 0
            (void)video_.DecodeAhead(data, size);     // 0x0026765B
            frameCursor_ = data + size;
            --framesLeft_;
        }
        platform_.GetDisplayFieldStatus(field, vblank);   // D3DDevice_GetDisplayFieldStatus
        fields = vblank - clockVBlank_;
        if (aheadTime_ != 0xFFFFFFFFu) {
            if (!delivered_ && clockStart_) {         // the frame ahead already due: the current one is skipped
                presentTime = aheadTime_ + uint32_t(lag_);
                due = Due(aheadTime_);
                if (due <= fields) delivered_ = true;
            }
            if (delivered_) {                         // 0x00264D9C: the planes swapped
                video_.Swap();
                curTime_ = aheadTime_;
                aheadTime_ = 0xFFFFFFFFu;
                delivered_ = false;
            }
        }
        if (!delivered_ && clockStart_) {             // 0x00264DFC: the current frame due: stop decoding
            presentTime = curTime_ + uint32_t(lag_);
            due = Due(curTime_);
            if (due <= fields) break;
        }
        if (!(aheadTime_ == 0xFFFFFFFFu && framesLeft_)) break;   // 0x00264E3F
    }
    // ---- the clock starts on a non-odd field once a frame is ready
    if (!clockStart_ && videoSlot_ && (aheadTime_ != 0xFFFFFFFFu || !framesLeft_) && field != 1) {
        clockStart_ = platform_.QueryPerformanceCounter();   // +0xB0
        clockVBlank_ = vblank;                        // +0xB8
        synch = true;
    }
    if (clockStart_) {
        if (synch) {
            const int32_t hr = platform_.SynchPlayback();   // DirectSoundCreate, SynchPlayback, Release
            if (hr < 0) return hr;
        }
        if (!delivered_ && surface && width_ && ((flags_ & 1) || due <= fields)) {
            // 0x00265BAB: D3DSurface_GetDesc, LockRect; YUY2 0x24 -> 0x002652A2 (the RGB converters 0x002655EA /
            // 0x002659CA for 0x1E / 0x12: not used by the game, not ported)
            const uint32_t format = surface->Format();
            int32_t pitch = 0;
            uint8_t* bits = surface->Lock(pitch);
            if (format == 0x24 && bits) video_.ConvertToYuy2(bits, pitch);
            delivered_ = true;
            if (time) *time = presentTime;
            result = kNewFrame;
        }
    }
    // ---- retire: the decoded packet waits for its audio, then its buffer goes back to the game
    if (!framesLeft_ && videoSlot_ && !waiting_) {
        waiting_ = videoSlot_;                        // +0x68
        waitingStatus_ = videoStatus_, videoStatus_ = nullptr;
        waitingCompleted_ = videoCompleted_, videoCompleted_ = nullptr;
        videoSlot_ = nullptr;
        audioDone_ = false;                           // +0x74
        if (last_) aheadTime_ = audioEnd_;            // the last frame's time becomes the audio's end
    }
    if (waiting_) {
        if (!audioDone_) {
            platform_.DoWork();                       // DirectSoundDoWork
            audioDone_ = true;
            for (uint32_t i = 0; i < tracks_; ++i)
                if ((*waitingStatus_)[i] == kPacketPending) {
                    audioDone_ = false;
                    break;
                }
        }
        if (audioDone_ && !free_) {
            free_ = waiting_;                         // +0x6C
            freeStatus_ = waitingStatus_, waitingStatus_ = nullptr;
            freeCompleted_ = waitingCompleted_, waitingCompleted_ = nullptr;
            waiting_ = nullptr;
            if (nextPacketSize_ == 0 || stop_) {
                if ((flags_ & 2) && !noLoop_ && !stop_) {   // (looping: unused by the game)
                    nextPacketSize_ = firstPacketSize_;
                    fileOffset_ = 0;
                    skip_ = headerSkip_;
                } else {
                    nextPacketSize_ = 0;
                    for (uint32_t i = 0; i < tracks_; ++i)
                        if (enabled_[i]) {
                            const int32_t h = streams_[i]->Discontinuity();
                            if (h < 0) return h;
                        }
                }
            }
            if (restart_) {
                nextPacketSize_ = firstPacketSize_;
                restart_ = false;
                fileOffset_ = 0;
                skip_ = headerSkip_;
            }
            const int32_t hr = releasePrevious_(fileOffset_, nextPacketSize_);
            if (hr < 0) return hr;
        }
    }
    if (result == kFail) result = kNoFrame;
    return 0;
}

}  // namespace xmv
