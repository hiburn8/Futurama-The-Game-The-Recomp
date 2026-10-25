// Sound events, decompiled from default.xbe (0x000CA6D0-0x000CB2C0).
//
// Sounds\SoundLookup.nif holds one NiUDSFileObject: a config file (see ConfigFile) mapping event names
// ("<character>:<event>") to commands:
//     PlaySound <file> [volume [a [b]]]      PlaySound3D <file> [volume [a]]      PlayFootstepSound
// The level loader hands the file to LoadLookup (stream kind 8). Characters look their animation events up by name
// (CharacterDef::CreateEvents): every config entry with that name (_stricmp) loads its sound and adds it to the
// event of that name in a global table; the lookup returns the event's index (or -1).
//
// The original tokenises the command with strtok inside the config buffer, so a second lookup of the same name
// only sees "<command> <file>" and re-registers the event with the default parameters; every lookup also appends
// its sound again (the player picks one of the event's sounds at random). The port reproduces both.
//
// Verified against the original under emulation with every character event lookup of the shipped levels
// (tests/game/SoundEventsTests.cpp, oracle tools/difftest/sound_oracle.py).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"
#include "game/data/ConfigFile.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fut::game {

struct SoundEvent {                                    // orig: 0x34 bytes, vtable 0x002A3A7C, ctor 0x000CA6D0
    u32 footstep = 0;                                  // orig: +0x04, 1 for PlayFootstepSound events
    std::string name;                                  // orig: +0x08
    std::vector<u32> sounds;                           // orig: NiTPrimitiveArray at +0x0C (data +0x10, size +0x18)
    bool is3D = false;                                 // orig: +0x24
    f32 param28 = 0.0f;                                // orig: +0x28 (default 0.5; meaning not identified yet)
    f32 param2C = 0.0f;                                // orig: +0x2C (default 0)
    f32 volume = 0.0f;                                 // orig: +0x30 (default 1)
};

class SoundEventTable {                                // orig: globals 0x002D6884 (table), 0x002EAC18 (config)
public:
    using LoadSound = std::function<u32(const std::string& path, u32 flags)>;   // orig: App+0xF0 vtbl+0x10

    // orig: 0x000CA7E0: keeps the file (0x002EADC4) and parses its buffer in place as the lookup config.
    void LoadLookup(ni::NiUDSFileObject* file);
    // orig: 0x000CAFF0. -1 without a config, or when a matching entry's value is shorter than 11 characters.
    s32 Lookup(const char* name, const LoadSound& loadSound);
    // orig: 0x000CAE00: adds `sound` to the event `name` (_stricmp; created when missing) and sets its parameters.
    u32 Register(const char* name, u32 sound, bool is3D, f32 volume, f32 param28, f32 param2C);
    // orig: part of 0x000CA980 (sound system shutdown): drops the config, the file and every event.
    void Clear();

    const std::vector<std::unique_ptr<SoundEvent>>& Events() const { return events_; }
    const ConfigFile* Config() const { return config_.get(); }

private:
    ni::NiPointer<ni::NiUDSFileObject> file_;
    std::unique_ptr<ConfigFile> config_;
    std::vector<std::unique_ptr<SoundEvent>> events_;
};

}  // namespace fut::game
