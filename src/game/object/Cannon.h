// The level2-5 cannons: the object Bender owns at P+0x7A8 in level2-5 (FsGetCannonCtrl 155; the script's class 11:
// Activate / Deactivate / SetAmmo / GetAmmo / SetActiveAmmo / GetActiveAmmo). Activate puts the player behind one of
// the stage's gun nodes ("Gun_KA" / "Gun_KA1" / "Gun" / "Gun_2"): the player node hidden, the static camera behind
// the gun; while active the player's step turns the gun with the stick and fires its two "fire" nodes' lasers with A.
// Decompiled from default.xbe (0x000334D0, 0x00033980, 0x00032FB0, 0x00032ED0, 0x00032E00, 0x00032E20, 0x00032E40,
// 0x00032E50 and the dispatcher's class-11 cases 0x000B6495..); docs/analysis/natives_batch_a_port.md 9.
#pragma once

#include "core/Types.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiObjects.h"

#include <array>

namespace fut::game {

class LevelData;
class LaserDef;
class Player;

class CannonCtrl {                                    // orig: new(0xB0), ctor 0x000334D0(player)
public:
    CannonCtrl(LevelData& level, Player& player);    // orig: 0x000334D0
    ~CannonCtrl();                                    // orig: 0x00032E50
    CannonCtrl(const CannonCtrl&) = delete;
    CannonCtrl& operator=(const CannonCtrl&) = delete;

    bool IsActive() const { return gun.Get() != nullptr; }   // orig: 0x00032E40 (+8 != 0)
    void Activate(const char* name);                  // orig: 0x00033980 (class 11:3)
    void Deactivate();                                // orig: 0x00032ED0 (class 11:4)
    void Update();                                    // orig: 0x00032FB0 (from the player's step while active)
    void SetAmmo(s32 n) { ammo = n < -1 ? -1 : n; }   // orig: 0x00032E00 (class 11:5; -1: unlimited)
    void SetActiveAmmo(s32 t) {                       // orig: 0x00032E20 (class 11:7; 0 red laser, 1 blue, 2 none)
        if (t >= 0 && t <= 2) activeAmmo = t;
    }

    LevelData& level;
    Player& player;                                   // orig: +0x0C
    std::array<ni::NiPointer<ni::NiAVObject>, 2> fire;   // orig: +0x00 / +0x04 ("<gun>fire_1" / "_2", else "Fire_")
    ni::NiPointer<ni::NiAVObject> gun;                // orig: +0x08 (the active gun's node; null: inactive)
    ni::NiMatrix3 baseKA, baseKA1;                    // orig: +0x10 / +0x40 (the guns' rest rotations, kept once)
    u8 haveKA = 0, haveKA1 = 0;                       // orig: +0x70 / +0x71
    f32 pitch = 0, yaw = 0;                           // orig: +0x74 / +0x78 (the gun's X / Y angles)
    f32 pitchKA = 0, pitchKA1 = 0, yawKA = 0, yawKA1 = 0;   // orig: +0x7C / +0x80 / +0x84 / +0x88 (per gun, kept)
    f32 cooldown = 0;                                 // orig: +0x8C (0.3 s after a shot; the rumble)
    s32 ammo = -1;                                    // orig: +0x90 (-1: unlimited)
    LaserDef* redLaser = nullptr;                     // orig: +0x94 "Cannon_Laser" (owned by the definition list)
    LaserDef* blueLaser = nullptr;                    // orig: +0x98 "Cannon_ShieldDeactivator"
    std::array<f32, 3> colour{1.0f, 0.25f, 0.25f};    // orig: +0x9C..+0xA4 (written into the red laser per shot)
    s32 activeAmmo = 0;                               // orig: +0xA8
    u32 sound = 0;                                    // orig: +0xAC (the gun's shot sample; 0: the default laser)
};

}  // namespace fut::game
