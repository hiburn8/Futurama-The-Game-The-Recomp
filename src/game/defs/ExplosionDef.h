// The explosion definition, decompiled from default.xbe (0x00065CA0-0x00067850).
//
// Level loading (Session stream kind 3) creates one ExplosionDef per level. It streams the five explosion meshes one
// after another (effects\explosns\Ground_Explosion, Air_Explosion, Big_Air_Explosion32, Bomb_Explosion,
// Explosion_shockwave): each file's first top-level object becomes model slot k, hidden and renamed
// "ExpDef::Mesh<k>"; the remaining slots 5-10 then share the first mesh. Spawning explosions (vf20 0x000676C0) is in
// game/object/Explosion.cpp.
//
// Verified against the original under emulation (tests/game/ExplosionDefTests.cpp, oracle
// tools/difftest/explosion_oracle.py).
#pragma once

#include "engine/stream/StreamManager.h"
#include "game/defs/Defs.h"

#include <array>
#include <string>

namespace fut::game {

class EffectDef : public ObjectDef {                   // orig: ctor 0x00065CA0 (vtable 0x0029BB7C)
public:
    std::string_view ClassName() const override { return "EffectDef"; }   // orig: 0x00065DD0
    bool IsType(u32 t) const override { return (t & 0x20) == t; }   // orig: 0x00065DB0
    const char* TypeName() const override { return "EffectDef"; }
};

class ExplosionDef : public EffectDef, public stream::Client {   // orig: 0x5C bytes, ctor 0x00067570, vtable 0x0029BE64
public:
    // orig: 0x00067570 (name "ExplosionDef", slots cleared) then slot 1 (0x00066730): queues the stream and loads
    // the explosion sound.
    explicit ExplosionDef(DefContext& ctx);
    ~ExplosionDef() override;                         // orig: 0x00067630 (ReleaseMeshes 0x000667E0: the sound)
    std::string_view ClassName() const override { return "ExplosionDef"; }

    // orig: the stream callback 0x00066820. `stream` is the file queued by the previous call (ignored on the
    // first). Returns the next file to stream, or nullptr when the sequence is complete.
    const char* OnStreamLoaded(ni::NiStream* stream);
    bool OnStream(stream::Request& r) override;       // orig: the client at +0x18 (0x000668A0)
    // orig: vf20 0x000676C0 (thiscall(owner, NiPoint3* pos, kind, label, u8 sound), RET 0x14; Explosion.cpp): a new
    // explosion object of mesh slot `kind` at pos (the sound plays when `sound`).
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, s32 kind, const char* label,
                      bool sound);
    std::shared_ptr<stream::Request> request;         // orig: +0x24

    std::array<ni::NiPointer<ni::NiNode>, 11> meshes;  // orig: +0x28
    s32 state = 0;                                     // orig: +0x54, the next file to stream
    u32 sound = 0;                                     // orig: +0x58, "Sounds/Level1/Generic/Xplo-1.wav"
    std::function<void(u32)> releaseSound_;           // (the DefContext's Sound_Release)
};

}  // namespace fut::game
