// The Ripple effect object (vtbl 0x0029C7E0, 0x68 bytes; built by RippleDef::Spawn 0x00071B10): a textured quad on
// the xz plane that grows from a start half size to an end half size and fades in, then out. Spawned by the floating
// statics (Static::Tick 0x00094E52), the liquid part of Character::UpdateGroundContacts (0x0003930E, "Character_Ripple"),
// the laser (0x000789A8), the crocodile and the HighPriest. Decompiled from default.xbe;
// docs/analysis/inventory_batch1_port.md 4.3.
//
// The stencil property is the inline constructor's (NiStencilProperty's 0x00026B60 fields, then +0x18 = 0, +0x34 = 3, as
// the scorch marks', MarkInstance.cpp). The quad's blocks are not mirrored in the game heap (KNOWN_ISSUES #39 style).
#pragma once

#include "game/object/GameObject.h"

namespace fut::game {

class RippleDef;

class Ripple : public GameObject {
public:
    Ripple(LevelData& level, RippleDef& def);         // orig: inline in 0x00071B10 (GameObject ctor, vtbl 0x0029C7E0)
    ~Ripple() override;                               // orig: 0x00071850
    bool Init() override;                             // orig: vf04 0x00071C50
    bool Tick() override;                             // orig: vf08 0x000716C0
    bool PostUpdate() override;                       // orig: vf0C 0x000718E0

    ni::NiPointer<ni::NiTriShape> quad;               // orig: +0x38
    ni::NiPointer<ni::NiMaterialProperty> material;   // orig: +0x3C
    ni::NiPointer<ni::NiAlphaProperty> alphaProp;     // orig: +0x40
    f32 alpha = 0;                                    // orig: +0x44 (> 0 fading in, < 0 fading out; set by Init)
    f32 rate = 1.0f;                                  // orig: +0x48 (Spawn: the start alpha; Init: D+0x20)
    f32 acc = 0;                                      // orig: +0x4C (rate * dt accumulated since the last PostUpdate)
    f32 rangeX = 0, rangeZ = 0;                       // orig: +0x50 / +0x54 (the end size - the start size)
    f32 sizeX = 0, sizeZ = 0;                         // orig: +0x58 / +0x5C (the half size)
    f32 endX = 0, endZ = 0;                           // orig: +0x60 / +0x64 (the final half size)
};

}  // namespace fut::game
