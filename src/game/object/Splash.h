// The Splash effect object (vtbl 0x0029CA28, ctor 0x00073970, 0x54 bytes): a clone of a SplashDef's animated mesh
// played once at a position. Spawned by SplashDef vf14 0x00073CA0 and its direction variant 0x00073880 (the liquid
// part of Character::UpdateGroundContacts, the rodent's footstep in water, the laser's Shoot_Splash). Decompiled from
// default.xbe; docs/analysis/inventory_batch1_port.md 4.1.
#pragma once

#include "game/object/GameObject.h"

namespace fut::game {

class SplashDef;

class Splash : public GameObject {
public:
    Splash(LevelData& level, SplashDef& def);         // orig: 0x00073970
    ~Splash() override;                               // orig: 0x000739E0
    bool Init() override;                             // orig: vf04 0x00073A80
    bool Tick() override;                             // orig: vf08 0x00073700
    bool PostUpdate() override;                       // orig: vf0C 0x00073B90

    ni::NiPointer<ni::NiNode> clone;                  // orig: +0x3C, the clone of the definition's mesh (Init)
    ni::NiPointer<ni::NiAVObject> follow;             // orig: +0x40 (zeroed by the ctor; no writer among the Splash
                                                      //  functions, H)
    f32 speed = 0;                                    // orig: +0x44 (Init: D+0x1C; not set by the ctor)
    u8 billboard = 0;                                 // orig: +0x48 (face the camera; vf14 writes 0)
    f32 duration = 0;                                 // orig: +0x4C (Init: D+0x20; not set by the ctor)
    u32 f50 = 0;                                      // orig: +0x50 (zeroed by the ctor; no reader found)
};

}  // namespace fut::game
