// The game's movie player (orig: App+0xE4, new(0x48) ctor 0x000EE0E0, dtor 0x000EE220, the device 0x000EDED0, Open
// 0x000EE2B0, the pump 0x000EDF20, Close 0x000EE110, the packet callbacks 0x000EDC30 / 0x000EDEF0, the audio language
// 0x000EDC70, the subtitles 0x000ED7E0 .. 0x000EDBF0) over the XMV library's decoder (video/xmv/XmvDecoder.h).
// docs/analysis/movies_port.md 2.
//
// The console-not-emulator split: the player is game code and always the same; what it runs on is a MovieService.
// The app's service (platform/common/MovieService.h) gives the device of the renderer, the disc's files, the clock /
// display fields and the DirectSound streams of the sound backend.
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"

#include <memory>
#include <string>
#include <vector>

namespace xmv {
class XmvDecoder;
class XmvImage;
class XmvPlatform;
}  // namespace xmv
namespace fut::ni {
class NiRenderer;
}
namespace fut::render::xd3d {
class XD3DDevice;
struct Resource;
}  // namespace fut::render::xd3d
namespace fut::text {
class LanguageDb;
}

namespace fut::game::app {

class App;

// A movie file as the player's NiUDSStream reads it: sequential reads (vf08 Read), Seek.
class MovieFile {
public:
    virtual ~MovieFile() = default;
    virtual u32 Read(void* dst, u32 bytes) = 0;       // the bytes read
    virtual void Seek(u64 offset) = 0;
};

class MovieService {
public:
    virtual ~MovieService() = default;
    // orig: 0x000EDED0's renderer +0x60 (the D3D device; null: the player fails before it opens anything).
    virtual render::xd3d::XD3DDevice* Device(ni::NiRenderer* renderer) = 0;
    // orig: NiUDSStream(path) and its open (vf04): null when the file does not open.
    virtual std::unique_ptr<MovieFile> Open(const std::string& path) = 0;
    // The XMV library's platform calls and its section of default.xbe (the decoder's tables).
    virtual xmv::XmvPlatform* Platform() = 0;
    virtual const xmv::XmvImage* Image() = 0;
};

// orig: the subtitles (new(0x14) 0x000ED7E0; the shared text object DAT_002EFBD0, its string DAT_002EFBD4).
struct MovieSubtitles {
    MovieSubtitles(App& app);                         // orig: 0x000ED7E0
    void Load(App& app, const char* name);            // orig: 0x000ED940 ("Video/%s.txt")
    void SetTime(f32 t);                              // orig: 0x000ED880
    void Draw();                                      // orig: 0x000ED8C0
    bool on = true;                                   // +0x00 (the options' subtitles, else 1; 0 without a file)
    const void* first = nullptr;                      // +0x04 (the marker list: NOT PORTED, no file on the disc)
    const void* current = nullptr;                    // +0x08
    f32 time = 0;                                     // +0x0C
    text::LanguageDb* db = nullptr;                   // +0x10 (Language\L1A1-INT.ldb)
};

class MoviePlayer {
public:
    MoviePlayer(App& app, MovieService& service);     // orig: 0x000EE0E0
    ~MoviePlayer();                                   // orig: 0x000EE220 (the device released, Close)
    bool AcquireDevice();                             // orig: 0x000EDED0
    bool Open(const char* name);                      // orig: 0x000EE2B0 ("<name>-<TV standard>")
    void Pump();                                      // orig: 0x000EDF20 (App vf20, once per App::Frame)
    void Close();                                     // orig: 0x000EE110
    bool Finished() const { return finished; }        // orig: 0x000EDC60

    bool finished = true;                             // +0x00
    ni::NiPointer<ni::NiSourceTexture> texture;       // +0x04 (over the YUY2 texture)
    ni::NiPointer<ni::NiCamera> camera;               // +0x08
    std::unique_ptr<MovieFile> stream;                // +0x0C (the NiUDSStream)
    render::xd3d::Resource* d3dTexture = nullptr;     // +0x10 (D3DFMT_YUY2 0x24, linear)
    render::xd3d::XD3DDevice* device = nullptr;       // +0x14 (AddRef'd)
    std::unique_ptr<xmv::XmvDecoder> decoder;         // +0x18
    u32 width = 0, height = 0, audioStreams = 0;      // +0x1C / +0x20 / +0x28 (XMVVIDEO_DESC; +0x24 unwritten)
    std::vector<u8> buffers[2];                       // +0x2C / +0x30 (XPhysicalAlloc(max packet size))
    u8* loaded = nullptr;                             // +0x34
    u8* freeBuffer = nullptr;                         // +0x38
    u8* inUse = nullptr;                              // +0x3C
    bool noFrameYet = true;                           // +0x41
    std::unique_ptr<MovieSubtitles> subtitles;        // +0x44

private:
    s32 GetNextPacket(u8** packet, u32* nextSize);    // orig: 0x000EDC30
    s32 ReleasePreviousPacket(u32 nextSize);          // orig: 0x000EDEF0
    s32 AudioLanguage() const;                        // orig: 0x000EDC70
    ni::NiPointer<ni::NiScreenPolygon> polygon_;      // (the camera's: the quad 0x00106DE0)
    App& app_;
    MovieService& service_;
};

}  // namespace fut::game::app
