// The game's movie player; see MoviePlayer.h and docs/analysis/movies_port.md 2.
#include "game/app/MoviePlayer.h"

#include "core/CrtString.h"
#include "engine/ni/NiCull.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiRenderer.h"
#include "game/app/App.h"
#include "game/text/LanguageDb.h"
#include "game/text/TextObject.h"
#include "render/xd3d/XD3DDevice.h"
#include "video/xmv/XmvDecoder.h"

#include <bit>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace fut::game::app {

using ni::detail::F;

namespace {

void NotPorted(const char* what) {
    static std::set<std::string> seen;
    if (seen.insert(what).second) std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// orig: DAT_002EFBD0, the subtitles' text object (made by the first player, never deleted)
std::unique_ptr<text::TextObject> g_subtitleText;

// The decoder's surface: the YUY2 texture's level 0 (D3DSurface_GetDesc / LockRect inside 0x00265BAB).
class TextureSurface final : public xmv::XmvSurface {
public:
    TextureSurface(render::xd3d::XD3DDevice& d, render::xd3d::Resource* s) : device_(d), surface_(s) {}
    uint32_t Format() override { return surface_->format; }
    uint8_t* Lock(int32_t& pitch) override {
        pitch = s32(surface_->width * 2);             // (linear 16 bits a texel: the rows back to back)
        return device_.LockRect(surface_);
    }

private:
    render::xd3d::XD3DDevice& device_;
    render::xd3d::Resource* surface_;
};

u32 Rd(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}

}  // namespace

// ---- the subtitles -------------------------------------------------------------------------------------------------

MovieSubtitles::MovieSubtitles(App& app) {            // orig: 0x000ED7E0
    on = app.options.subtitles;                       // DAT_002E79A9 (with App+0xA8; else 1)
    if (!g_subtitleText) {                            // TextObject::Create(App+0xFC, 0), 0x00098F80(renderer)
        g_subtitleText.reset(text::TextObject::Create(0, app.services.readFile, app.services.renderer));
        if (g_subtitleText) {
            g_subtitleText->SetRenderer(app.services.renderer);
            g_subtitleText->style.wordWrap = true;    // +0x68 = 1
            g_subtitleText->SetScale(std::bit_cast<f32>(0x3F59999Au), std::bit_cast<f32>(0x3FA66666u));
            g_subtitleText->style.hAlign = 0;         // +0x54 = 0
        }
    }
}

void MovieSubtitles::Load(App& app, const char* name) {   // orig: 0x000ED940
    char path[0x100];
    std::snprintf(path, sizeof path, "Video/%s.txt", name);   // 0x002A57A0
    std::vector<u8> file;
    if (!app.services.readFile || !app.services.readFile(path, file)) {   // LoadWholeFile 0x0013FBA0
        on = false;                                   // (no file on this disc: never subtitled)
        return;
    }
    db = app.text ? app.text->LoadDb("Language\\L1A1-INT.ldb") : nullptr;   // 0x002A5788
    NotPorted("MovieSubtitles::Load: the marker tables of Video/<name>.txt (0x000ED9A6..)");
    on = false;
}

void MovieSubtitles::SetTime(f32 t) {                 // orig: 0x000ED880
    time = t;
    if (on && (current || first)) NotPorted("MovieSubtitles::SetTime: the marker walk");
}

void MovieSubtitles::Draw() {                         // orig: 0x000ED8C0
    if (on && current && db) NotPorted("MovieSubtitles::Draw: the subtitle text (0x000ED8D3..)");
}

// ---- the player ----------------------------------------------------------------------------------------------------

MoviePlayer::MoviePlayer(App& app, MovieService& service) : app_(app), service_(service) {}   // orig: 0x000EE0E0

MoviePlayer::~MoviePlayer() {                         // orig: 0x000EE220
    // D3DDevice_Release(+0x14) (the device's reference: not counted in the port), then Close
    Close();
}

bool MoviePlayer::AcquireDevice() {                   // orig: 0x000EDED0
    device = service_.Device(app_.services.renderer);   // (App+0x104)+0x60
    return device != nullptr;                         // (D3DDevice_AddRef)
}

s32 MoviePlayer::GetNextPacket(u8** packet, u32* nextSize) {   // orig: 0x000EDC30
    freeBuffer = inUse;
    inUse = loaded;
    loaded = nullptr;
    *packet = inUse;
    *nextSize = Rd(inUse);                            // (the packet's first dword: the next packet's size)
    return 0;
}

s32 MoviePlayer::ReleasePreviousPacket(u32 nextSize) {   // orig: 0x000EDEF0 (the offset is not used)
    if (nextSize) {
        loaded = freeBuffer;
        freeBuffer = nullptr;
        stream->Read(loaded, nextSize);               // (stream+8)->vf08 (async in the original; L: complete by
    }                                                 //  the next GetNextPacket, which does not wait)
    return 0;
}

s32 MoviePlayer::AudioLanguage() const {              // orig: 0x000EDC70
    const std::string lang = app_.text ? app_.text->Language() : std::string("English");   // TextManager::GetLanguage
    if (crt::StrICmp(lang.c_str(), "english") == 0) return 0;
    if (crt::StrICmp(lang.c_str(), "german") == 0) return 1;
    if (crt::StrICmp(lang.c_str(), "french") == 0) return 2;
    if (crt::StrICmp(lang.c_str(), "spanish") == 0) return 3;
    return crt::StrICmp(lang.c_str(), "italian") == 0 ? 4 : 0;   // NEG; SBB; AND -4; ADD 4
}

bool MoviePlayer::Open(const char* name) {            // orig: 0x000EE2B0
    finished = true;
    char path[0x100];
    std::snprintf(path, sizeof path, "Video\\%s.xmv", name);   // 0x002A57E8
    subtitles = std::make_unique<MovieSubtitles>(app_);   // new(0x14) 0x000ED7E0
    subtitles->Load(app_, name);                      // 0x000ED940
    // new(0x6C) NiUDSStream(path, 0), +0x34 = 1 (asynchronous), SetBufferSize(0x80000), vf04 (open)
    stream = service_.Open(path);
    if (!stream || !service_.Platform() || !service_.Image()) {
        Close();
        return false;
    }
    u8 header[0x1000] = {};
    stream->Read(header, 0x1000);                     // vf08(header, 0x1000)
    stream->Seek(0);
    const s32 hr = xmv::XmvDecoder::CreateForPackets(
        0, header, *service_.Image(), *service_.Platform(),
        [this](u8** p, u32* n) { return GetNextPacket(p, n); },
        [this](u64 offset, u32 n) {
            (void)offset;
            return ReleasePreviousPacket(n);
        },
        decoder);                                     // 0x00264400(0, header, this, 0x000EDC30, 0x000EDEF0, &+0x18)
    if (hr < 0) {
        Close();
        return false;
    }
    const xmv::XmvDecoder::VideoDescriptor desc = decoder->GetVideoDescriptor();   // 0x002647F9 -> +0x1C
    width = desc.width, height = desc.height, audioStreams = desc.audioStreams;
    // two XPhysicalAlloc(max packet size, -1, 0, PAGE_READWRITE) buffers (the decoder's reads may run 8 bytes past a
    // frame: the original's page-granular blocks allow it; the port adds them, L)
    const u32 maxSize = Rd(header + 8), firstSize = Rd(header + 4);
    for (auto& b : buffers) b.assign(size_t(maxSize) + 8, 0);
    freeBuffer = buffers[0].data();                   // +0x38
    inUse = buffers[1].data();                        // +0x3C
    loaded = nullptr;                                 // +0x34
    if (firstSize) {                                  // packet 0 (the file header's packet)
        loaded = buffers[0].data();
        freeBuffer = nullptr;
        stream->Read(loaded, firstSize);
    }
    if (audioStreams >= 1 && audioStreams <= 5) {
        s32 index = 0;
        if (audioStreams == 5) {                      // five tracks: the language's (0x000EDC70, called twice)
            index = AudioLanguage();
            (void)decoder->EnableAudioStream(u32(AudioLanguage()), 0, nullptr, nullptr);   // 0x00264866
        } else {
            (void)decoder->EnableAudioStream(0, 0, nullptr, nullptr);
        }
        if (xmv::XmvAudioStream* s = decoder->GetAudioStream(u32(index))) {   // 0x002649E3 (AddRef)
            // the SFX volume (vt+0x24(0)) in hundredths of dB: FCOM 1.0 / 0.0 (0x000EE4D1..0x000EE52C)
            const f32 v = app_.Sound().CategoryVolume(0);
            s32 db;
            if (v > 1.0f) db = static_cast<s32>((1.0 - 1.0) * double(std::bit_cast<f32>(0xC47A0000u)));   // 0
            else if (v < 0.0f || v == 0.0f) db = -10000;   // 0xFFFFD8F0
            else if (std::isnan(v)) db = INT_MIN;     // (unordered: _ftol's integer indefinite; UB-avoiding)
            else db = static_cast<s32>((1.0 - double(v)) * double(std::bit_cast<f32>(0xC47A0000u)));   // _ftol
            s->SetVolume(db);                         // IDirectSoundStream_SetVolume, then Release
        } else {
            (void)AudioLanguage();                    // (the result unused)
        }
    }
    finished = false;
    noFrameYet = true;
    // D3DDevice_CreateTexture2(w, h, 1, 1, 0, D3DFMT_YUY2, 3); E_OUTOFMEMORY: Close, 0x0014CC40 (its error text)
    d3dTexture = device->CreateTexture2(width, height, 1, 0, 0x24);
    if (!d3dTexture) {
        Close();
        return false;
    }
    // 0x00118690(0, DAT_002D79A4): a source texture without pixel data (its precache: the renderer's default pixel
    // data), its renderer data's D3D texture replaced by +0x10 (AddRef'd); its pixel format descriptor 0x000EE090 /
    // 0x000EDE90, +0x48 w, +0x4C h, +0x58 = w * h * 2, +0x54 = 0 (the renderer data's: not modelled by the port's
    // texture manager)
    texture = new ni::NiSourceTexture;
    if (ni::g_precacheTextures) ni::PrecacheTexture(*texture);
    if (ni::NiRenderer* r = app_.services.renderer) r->SetSourceTextureD3D(*texture, d3dTexture);
    // the quad: new(0x24) 0x00106DE0(4, (0,0,0) (1,0,0) (1,1,0) (0,1,0), uvs (0,h) (w,h) (w,0) (0,0) in texels, no
    // colours): the FILD of the unsigned sizes (+2^32 [0x00295640] when negative)
    const f32 fw = F(double(width)), fh = F(double(height));
    const ni::NiPoint3 v[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const ni::NiPoint2 uv[4] = {{0, fh}, {fw, fh}, {fw, 0}, {0, 0}};
    polygon_ = new ni::NiScreenPolygon(4, v, uv, nullptr);
    auto* tex = new ni::NiTexturingProperty;          // new(0x3C) 0x00109DC0
    tex->applyMode = 0;                               // +0x18 = 0 (REPLACE)
    if (!tex->HasMap(0)) tex->SetBaseTexture(nullptr);   // SetBaseFilterMode(1) 0x0001D6B0 (makes the base map)
    tex->maps[0].filterMode = 1;
    tex->maps[0].clampMode = 0;                       // 0x000506A0(0)
    tex->SetBaseTexture(texture.Get());               // 0x0001D610
    auto* zb = new ni::NiZBufferProperty;             // new(0x1C) 0x0003F460 (flags 3, test function 3), +0x14 &= ~3
    zb->flags = 3, zb->testFunction = 3;
    zb->flags = u16(zb->flags & 0xFFFC);
    polygon_->AttachProperty(tex);                    // 0x00013CC0
    polygon_->AttachProperty(zb);
    polygon_->flags = u16(polygon_->flags | 5);       // +0x1C |= 5
    camera = new ni::NiCamera;                        // new(0x250) 0x001009D0
    camera->port[0] = 0.0f, camera->port[1] = 1.0f, camera->port[2] = 1.0f, camera->port[3] = 0.0f;   // +0x14C
    camera->renderer = app_.services.renderer;        // 0x00013D40(App+0x104)
    camera->UpdateProperties();
    camera->UpdateEffects();
    camera->UpdateDownwardPass(0.0f, true);           // Update(0, 1)
    camera->AttachScreenPolygon(polygon_.Get());      // 0x00019460
    Pump();                                           // the first frame, inside PlayMovie
    return true;
}

void MoviePlayer::Pump() {                            // orig: 0x000EDF20
    if (finished || !decoder) return;
    render::xd3d::Resource* surface = device->GetSurfaceLevel2(d3dTexture, 0);   // D3DTexture_GetSurfaceLevel2
    if (!surface) {
        finished = true;
        return;
    }
    u32 r = xmv::XmvDecoder::kFail;
    TextureSurface s(*device, surface);
    (void)decoder->GetNextFrame(&s, r, nullptr);      // 0x00264A02 (the HRESULT ignored)
    device->Release(surface);                         // D3DResource_Release
    ni::NiRenderer* renderer = app_.services.renderer;   // App->vf14()
    if (r == xmv::XmvDecoder::kEnd) {
        if (renderer) renderer->SetBackgroundColor(ni::NiColor{0.0f, 0.0f, 0.0f});   // vf6C(black)
        if (camera && camera->renderer) camera->renderer->ClearBuffer(camera->port, 1);   // 0x000FEF20(1)
        finished = true;
        return;
    }
    if (r == xmv::XmvDecoder::kFail) {
        finished = true;
        return;
    }
    bool draw = true;
    if (r == xmv::XmvDecoder::kNoFrame) {
        if (noFrameYet) draw = false;                 // (nothing shown yet: black)
    } else if (r == xmv::XmvDecoder::kNewFrame) {
        noFrameYet = false;
    }
    if (camera) {
        if (draw) {                                   // (no new frame: the last one drawn again)
            device->SetRenderState_Complex(render::xd3d::RS_YUVENABLE, 1);   // D3DDevice_SetRenderState_YuvEnable
            ni::VisitForCulling(*camera, nullptr);    // Click 0x00100CE0 (the quad)
            device->SetRenderState_Complex(render::xd3d::RS_YUVENABLE, 0);
        } else {
            if (renderer) renderer->SetBackgroundColor(ni::NiColor{0.0f, 0.0f, 0.0f});
            if (camera->renderer) camera->renderer->ClearBuffer(camera->port, 1);
        }
    }
    if (subtitles) {                                  // FILD (unsigned) ms; FMUL [0x002A57D8] 0.001 (double); FSTP
        const f32 t = F(double(decoder->GetCurrentTime()) * std::bit_cast<double>(0x3F50624DD2F1A9FCull));
        subtitles->SetTime(t);
        subtitles->Draw();
    }
}

void MoviePlayer::Close() {                           // orig: 0x000EE110
    if (camera) {                                     // the black clear, an extra present, a clear
        if (ni::NiRenderer* renderer = app_.services.renderer) {
            renderer->SetBackgroundColor(ni::NiColor{0.0f, 0.0f, 0.0f});
            if (camera->renderer) camera->renderer->ClearBuffer(camera->port, 1);
            renderer->DisplayFrame();                 // App->vf14()->vf40
            if (camera->renderer) camera->renderer->ClearBuffer(camera->port, 1);
        }
    }
    finished = true;
    texture = nullptr;                                // +4
    camera = nullptr;                                 // +8 (its polygon, the properties)
    polygon_ = nullptr;
    if (d3dTexture) {
        device->Release(d3dTexture);                  // D3DResource_Release
        d3dTexture = nullptr;
    }
    if (decoder) {
        decoder->Close();                             // 0x00264717
        decoder.reset();
    }
    stream.reset();                                   // +0xC
    for (auto& b : buffers) std::vector<u8>().swap(b);   // XPhysicalFree x 2
    loaded = freeBuffer = inUse = nullptr;
    subtitles.reset();                                // 0x000EDBF0, delete
}

}  // namespace fut::game::app
