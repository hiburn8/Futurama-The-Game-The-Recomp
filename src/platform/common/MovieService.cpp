// The console's movie services; see MovieService.h.
#include "platform/common/MovieService.h"

#include "engine/ni/NiRenderer.h"
#include "game/sound/SoundSystem.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif

#include <algorithm>

namespace fut::platform {

namespace {

// The player's NiUDSStream over a disc file (synchronous reads at the stream's position).
class DiscMovieFile final : public game::app::MovieFile {
public:
    DiscMovieFile(const fs::Vfs& vfs, std::string path, u64 size) : vfs_(vfs), path_(std::move(path)), size_(size) {}
    u32 Read(void* dst, u32 bytes) override {
        const u64 n = std::min<u64>(bytes, pos_ < size_ ? size_ - pos_ : 0);
        if (n && !vfs_.ReadDisc(path_, pos_, dst, size_t(n))) return 0;
        pos_ += n;
        return u32(n);
    }
    void Seek(u64 offset) override { pos_ = offset; }

private:
    const fs::Vfs& vfs_;
    std::string path_;
    u64 size_ = 0, pos_ = 0;
};

// IDirectSoundStream over the sound backend's stream.
class BackendStream final : public xmv::XmvAudioStream {
public:
    explicit BackendStream(std::unique_ptr<sound::SoundStream> s) : s_(std::move(s)) {}
    int32_t Pause(uint32_t flags) override { return s_->Pause(flags); }
    int32_t Process(const xmv::XmvMediaPacket& p) override {
        return s_->Process(p.data, p.size, p.completedSize, p.status, p.context);
    }
    int32_t Discontinuity() override { return s_->Discontinuity(); }
    uint32_t GetStatus() override { return s_->GetStatus(); }
    int32_t FlushEx(uint64_t, uint32_t) override { return s_->Flush(); }
    void SetVolume(int32_t v) override { s_->SetVolume(v); }

private:
    std::unique_ptr<sound::SoundStream> s_;
};

}  // namespace

MovieService::MovieService(const fs::Vfs& vfs, sound::SoundBackend* backend, const std::vector<u8>& xbe)
    : vfs_(vfs), backend_(backend) {
    image_.LoadFromXbe(xbe);
#ifdef _WIN32
    LARGE_INTEGER f, c;
    ::QueryPerformanceFrequency(&f);
    ::QueryPerformanceCounter(&c);
    qpf_ = u64(f.QuadPart), base_ = u64(c.QuadPart);
#else
    qpf_ = SDL_GetPerformanceFrequency();
    base_ = SDL_GetPerformanceCounter();
#endif
}

render::xd3d::XD3DDevice* MovieService::Device(ni::NiRenderer* renderer) {
    return renderer ? renderer->D3DDevice() : nullptr;
}

std::unique_ptr<game::app::MovieFile> MovieService::Open(const std::string& path) {
    u64 size = 0;
    if (!vfs_.DiscFileSize(path, size)) return nullptr;
    return std::make_unique<DiscMovieFile>(vfs_, path, size);
}

uint64_t MovieService::QueryPerformanceCounter() {
#ifdef _WIN32
    LARGE_INTEGER c;
    ::QueryPerformanceCounter(&c);
    return u64(c.QuadPart);
#else
    return SDL_GetPerformanceCounter();
#endif
}

void MovieService::GetDisplayFieldStatus(uint32_t& field, uint32_t& vblankCount) {
    // (L) 59.94 fields a second since the service started, fields alternating even (2) / odd (1)
    const u64 t = QueryPerformanceCounter() - base_;
    vblankCount = uint32_t(t * 60000 / (1001 * qpf_));
    field = (vblankCount & 1) ? 1u : 2u;
}

std::unique_ptr<xmv::XmvAudioStream> MovieService::CreateStream(const xmv::XmvStreamDesc& desc) {
    if (!backend_) return nullptr;
    std::unique_ptr<sound::SoundStream> s = backend_->CreateStream(desc.format, desc.callback);
    if (!s) return nullptr;
    return std::make_unique<BackendStream>(std::move(s));
}

int32_t MovieService::SynchPlayback() { return backend_ ? backend_->SynchPlayback() : 0; }

void MovieService::DoWork() {
    if (backend_) backend_->DoStreamWork();           // (L: the streams only; the sound layer's voices run on its
}                                                     //  own DoWork, one a game step)

void MovieService::Sleep(uint32_t ms) {
#ifdef _WIN32
    ::Sleep(ms);
#else
    SDL_Delay(ms);
#endif
}

}  // namespace fut::platform
