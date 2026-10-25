// Decompiled from default.xbe 0x000CA6D0-0x000CB2C0; see SoundEvents.h.
#include "game/sound/SoundEvents.h"

#include "core/CrtString.h"

#include <cstdio>
#include <cstring>

namespace fut::game {
namespace {

int StrICmp(const char* a, const char* b) {           // orig: _stricmp (0x001684D1), ASCII folding
    for (;; ++a, ++b) {
        int ca = static_cast<unsigned char>(*a), cb = static_cast<unsigned char>(*b);
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || ca == 0) return ca - cb;
    }
}

constexpr const char* kDelimiters = " ,\t\n";        // orig: the dword 0x0A092C20 on the stack

}  // namespace

void SoundEventTable::LoadLookup(ni::NiUDSFileObject* file) {   // orig: 0x000CA7E0
    if (file_.Get() != file) file_ = file;
    if (!file) return;
    config_ = std::make_unique<ConfigFile>();         // orig: 0x00141F10, then 0x00142920 parses the file's buffer
    config_->Parse(file->data);
}

u32 SoundEventTable::Register(const char* name, u32 sound, bool is3D, f32 volume, f32 param28,
                              f32 param2C) {          // orig: 0x000CAE00
    for (u32 i = 0; i < events_.size(); ++i) {
        SoundEvent* ev = events_[i].get();
        if (ev && StrICmp(ev->name.c_str(), name) == 0) {
            ev->sounds.push_back(sound);
            ev->is3D = is3D;
            ev->param28 = param28;
            ev->param2C = param2C;
            ev->volume = volume;
            return i;
        }
    }
    auto ev = std::make_unique<SoundEvent>();
    ev->name = name;
    ev->sounds.push_back(sound);
    ev->is3D = is3D;
    ev->param28 = param28;
    ev->param2C = param2C;
    ev->volume = volume;
    events_.push_back(std::move(ev));
    return static_cast<u32>(events_.size() - 1);
}

s32 SoundEventTable::Lookup(const char* name, const LoadSound& loadSound) {   // orig: 0x000CAFF0
    if (!config_) return -1;
    s32 result = -1;
    char file2D[256] = {}, file3D[256] = {};          // orig: stack buffers (uninitialised; kept across entries)
    for (s32 i = 0; i < config_->Count(); ++i) {
        const char* key = config_->Key(i);
        if (!key || StrICmp(key, name) != 0) continue;
        char* value = config_->MutableValue(i);
        f32 volume = 1.0f, param28 = 0.5f, param2C = 0.0f;
        if (std::strlen(value) < 11) return -1;
        auto load = [&](const char* path) { return loadSound ? loadSound(path, 0) : 0u; };
        if (crt::StrNICmp(value, "PlaySound ", 10) == 0) {
            s32 field = 0;
            for (char* tok = std::strtok(value + 10, kDelimiters); tok;
                 tok = std::strtok(nullptr, kDelimiters), ++field) {
                switch (field) {
                    case 0: std::snprintf(file2D, sizeof file2D, "%s", tok); break;   // orig: strcpy
                    case 1: std::sscanf(tok, "%f", &volume); break;
                    case 2: std::sscanf(tok, "%f", &param28); break;
                    case 3: std::sscanf(tok, "%f", &param2C); break;
                    default: break;
                }
            }
            result = static_cast<s32>(Register(name, load(file2D), false, volume, param28, param2C));
        } else if (crt::StrNICmp(value, "PlaySound3D ", 12) == 0) {
            s32 field = 0;
            for (char* tok = std::strtok(value + 12, kDelimiters); tok;
                 tok = std::strtok(nullptr, kDelimiters), ++field) {
                if (field == 0) std::snprintf(file3D, sizeof file3D, "%s", tok);
                else if (field == 1) std::sscanf(tok, "%f", &volume);
                else if (field == 2) std::sscanf(tok, "%f", &param28);
            }
            result = static_cast<s32>(Register(name, load(file3D), true, volume, param28, param2C));
        } else if (crt::StrNICmp(value, "PlayFootstepSound", 17) == 0) {
            result = static_cast<s32>(Register(name, 0, true, 1.0f, 0.5f, 0.0f));
            events_[static_cast<size_t>(result)]->footstep = 1;
        }
    }
    return result;
}

void SoundEventTable::Clear() {                       // orig: 0x000CA980 (the sound system's part not ported)
    config_.reset();
    file_ = nullptr;
    events_.clear();
}

}  // namespace fut::game
