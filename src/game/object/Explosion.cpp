// The explosion object; see Explosion.h and docs/analysis/explore_leftovers_port.md 3 (checked against the assembly of
// 0x000676C0, 0x000669E0, 0x00066E90, 0x000673D0, 0x000674D0 and 0x00066D70).
#include "game/object/Explosion.h"

#include "core/CrtString.h"
#include "engine/ni/NiClone.h"
#include "engine/ni/NiMath.h"
#include "game/GameTime.h"
#include "game/defs/EffectDefs.h"
#include "game/defs/ExplosionDef.h"
#include "game/level/Camera.h"
#include "game/level/LevelData.h"
#include "game/level/LightManager.h"
#include "game/object/NodeUpdate.h"
#include "game/sound/GameSound.h"

#include <bit>
#include <cmath>
#include <memory>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {
f32 Bits(u32 b) { return std::bit_cast<f32>(b); }

// orig: new(0x114) SpriteEmitterDef(name) 0x0006E090 (registered in the level's definition list by the Def ctor) and
// the constructor's setters, in this order for all three.
SpriteEmitterDef* NewSparks(LevelData& level, const char* name, f32 r28a, f32 r28b, f32 size, f32 r30a, f32 r30b,
                            u8 flagDC, f32 r, f32 g, f32 b) {
    auto owned = std::make_unique<SpriteEmitterDef>();
    SpriteEmitterDef* d = owned.get();
    d->name = name;
    level.defs->Add(std::move(owned));
    d->SetSpriteTexture("spark.tga", level);          // vfA4 (0x0029BDB8)
    d->SetRange40(0.0f, 0.0f);                        // vf44(0)
    d->SetRange28(r28a, r28b);                        // vf28
    d->SetSize(size, size);                           // vfB4
    d->SetGravity(10.0f);                             // vf50 (0x41200000)
    d->SetValue4C(4.0f);                              // vf4C (0x40800000)
    d->SetRange38(0.0f, Bits(0x3F4CCCCDu));           // vf3C(0.8)
    d->SetRange30(r30a, r30b);                        // vf30
    d->SetCount(40);                                  // vf48 (0x28)
    d->SetFlagA8(1);                                  // vfAC
    d->SetFlagDC(flagDC);                             // vfC8
    d->SetColor(r, g, b);                             // vfC4
    d->SetRangeB4(1.0f, 0.0f);                        // vfB8
    d->Reset();                                       // vf64
    return d;
}

// the current-only twins of vf28 / vfC4 (0x0006CA90 / 0x0006D270)
void CurrentRange28(SpriteEmitterDef& d, f32 a, f32 b) { d.current.range28 = {a, b}; }   // vf68
void CurrentColor(SpriteEmitterDef& d, f32 r, f32 g, f32 b) {                            // vfE4
    d.spriteCurrent.startColor = d.spriteCurrent.endColor = {r, g, b, 1.0f};
}
}  // namespace

// orig: vf20 0x000676C0 (thiscall(owner, NiPoint3* pos, kind, label, u8 sound), RET 0x14).
GameObject* ExplosionDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, s32 kind, const char* label,
                                bool playSound) {
    // (vf04 0x00066730 first: it queues the stream when neither a mesh nor a request exists and returns 1 always)
    auto* o = new Explosion(level, *this);            // operator_new(0x60), ctor 0x000669E0 (no null check after)
    o->SetOwner(owner);                               // vf10
    o->SetName(label);                                // 0x000968C0
    o->SetPosition(pos);                              // vf1C
    o->kind = kind;                                   // +0x4C
    o->Init();                                        // vf04
    if (playSound) {                                  // 0x0006775B
        sound::GameSound& snd = level.Sound();
        if (kind == 7) {                              // Sound_Play3D(+0x58, &pos, 0); Voice_SetVolume(v, 0.25)
            snd.SetVolume(snd.Play3D(sound, pos, false), 0.25f);
        } else if (crt::StrICmp(level.name.c_str(), "Level1-6") == 0 || crt::StrICmp(level.name.c_str(), "Level5-3") == 0 ||
                   crt::StrICmp(level.name.c_str(), "Level1-3") == 0) {   // _stricmp with S+0xC (0x0029BE94 / ..)
            snd.SetMaxDistance(snd.Play3D(sound, pos, false), 500.0f);   // 0x000677EC (0x43FA0000)
        } else {
            (void)snd.Play3D(sound, pos, false);      // 0x000677FD
        }
    }
    return o;
}

Explosion::Explosion(LevelData& lvl, ExplosionDef& d) : GameObject(lvl, d) {   // orig: 0x000669E0
    // +0x44 / +0x48 / +0x5C = 0, then the three emitters (+0x50 / +0x54 / +0x58)
    fast = NewSparks(lvl, "FastParticles", 75.0f, 115.0f, Bits(0x3EA8F5C3u), Bits(0x3E4CCCCDu), Bits(0x3F19999Au), 0,
                     1.0f, 1.0f, Bits(0x3ECCCCCDu));  // 0x0029BDC4: (75, 115), 0.33, (0.2, 0.6), colour (1, 1, 0.4)
    medium = NewSparks(lvl, "MediumParticles", 50.0f, 70.0f, Bits(0x3E8F5C29u), 0.5f, Bits(0x3F99999Au), 0, 1.0f,
                       Bits(0x3F333333u), 0.0f);      // 0x0029BDA8: (50, 70), 0.28, (0.5, 1.2), colour (1, 0.7, 0)
    slow = NewSparks(lvl, "SlowParticles", 20.0f, 45.0f, Bits(0x3E4CCCCDu), 2.5f, 3.5f, 1, Bits(0x3F4CCCCDu),
                     Bits(0x3ECCCCCDu), 0.0f);        // 0x0029BD98: (20, 45), 0.2, (2.5, 3.5), flag 1, (0.8, 0.4, 0)
}

Explosion::~Explosion() {                             // orig: 0x00066D70
    light = nullptr;                                  // +0x5C released first (the node keeps it as a child)
    if (model) node->DetachChild(model.Get());        // node vfA4(&tmp, +0x44), the temporary released
    if (shockwave) node->DetachChild(shockwave.Get());
    // (then +0x5C / +0x48 / +0x44 released, the Owner +0x38 and the GameObject base; the emitter definitions stay in
    // the level's list)
}

bool Explosion::Init() {                              // orig: vf04 0x00066E90
    if (model) return true;                           // +0x44
    auto& d = static_cast<ExplosionDef&>(*def);
    const auto slotScale = [](s32 k, bool shock) {    // doubles [0x0029BE38..0x0029BE50] / [0x00293CC0], FABS
        switch (k) {                                  // jump table 0x000673B8 (kind - 5) / the shockwave's chain
        case 5: return 0.5;
        case 6: return 2.0;
        case 8: return 6.0;
        case 9: return shock ? 1.0 : 3.0;
        case 10: return shock ? 1.0 : 6.0;
        default: return 1.0;
        }
    };
    auto place = [&](ni::NiPointer<ni::NiAVObject>& slot, ni::NiNode* mesh, double scale) {
        ni::NiPointer<ni::NiObject> c = mesh ? ni::Clone(mesh) : nullptr;   // NiObject::Clone 0x000FE120
        slot = dynamic_cast<ni::NiAVObject*>(c.Get());   // DynamicCast 0x002F9190 (NiAVObject)
        node->AttachChild(slot.Get(), true);          // vfA0(clone, 1)
        if (!slot) return;                            // (port guard: the original dereferences it)
        slot->flags &= ~ni::kNiAVHidden;              // +0xB0 &= 0xFE
        slot->local.translate = {};                   // 0x002F915C (zero)
        slot->local.rotate = ni::NiMatrix3{};         // 0x002D7820 (identity, 12 dwords)
        slot->local.scale = F(std::fabs(scale));      // FLD qword; FABS; FSTP +0x6C
        slot->UpdateProperties();                     // 0x00066F8E / 0x000672C0
        UpdateNodeAt(slot.Get(), 0.0f);               // NiAVObject::Update(0, 1)
    };
    const s32 k = kind;                               // +0x4C (no range check: 0..10 in the callers)
    place(model, k >= 0 && k < static_cast<s32>(d.meshes.size()) ? d.meshes[static_cast<size_t>(k)].Get() : nullptr,
          slotScale(k, false));                       // D+0x28 + 4 * kind
    // (DynamicCast of the clone to NiNode 0x002F9230: the result is not used)
    life = 2.0f;                                      // +0x40 (0x40000000)
    if (k != 0 && k <= 4) return true;                // JE / JLE (signed)
    const NiPoint3 P = node->local.translate;         // node +0x60 -> esp+0x18
    const NiPoint3 up{0.0f, 1.0f, 0.0f};              // esp+0x24
    life = Bits(0x3F7D70A4u);                         // 0.99
    if (k == 6 || k == 8) {                           // 0x00067109
        CurrentRange28(*fast, 105.0f, 145.0f);
        CurrentRange28(*medium, 70.0f, 90.0f);
        CurrentRange28(*slow, 30.0f, 55.0f);
    } else if (k == 5) {
        CurrentRange28(*fast, 35.0f, 65.0f);
        CurrentRange28(*medium, 20.0f, 30.0f);
        CurrentRange28(*slow, 10.0f, 25.0f);
    } else if (k == 7) {                              // each range, then its colour (0.13, 0.65, 0.1)
        const f32 r = Bits(0x3E051EB8u), g = Bits(0x3F266666u), b = Bits(0x3DCCCCCDu);
        CurrentRange28(*fast, 35.0f, 65.0f);
        CurrentColor(*fast, r, g, b);
        CurrentRange28(*medium, 20.0f, 30.0f);
        CurrentColor(*medium, r, g, b);
        CurrentRange28(*slow, 10.0f, 25.0f);
        CurrentColor(*slow, r, g, b);
    } else {                                          // 0, 9, 10
        CurrentRange28(*fast, 75.0f, 115.0f);
        CurrentRange28(*medium, 50.0f, 70.0f);
        CurrentRange28(*slow, 20.0f, 45.0f);
    }
    (void)fast->SpawnEffect(level, P, up);            // vf20(&P, &up), each followed by its own Reset
    (void)medium->SpawnEffect(level, P, up);
    (void)slow->SpawnEffect(level, P, up);
    if (Camera* cam = level.camera.get()) {           // S+0x8C (no null test in the original)
        const double dz = double(P.z) - cam->position.z, dy = double(P.y) - cam->position.y,
                     dx = double(P.x) - cam->position.x;   // FLD; FSUB (z, y, x), kept on the stack
        const double s = (dx * dx + dy * dy) + dz * dz;
        if (std::sqrt(s) < 50.0f) {                   // FSQRT; FCOMP [0x0029564C]; TEST AH,5; JP
            const f32 amp = F((50.0f - std::sqrt(s)) * double(Bits(0x3CA3D70Au)));   // FSUBR 50; FMUL [0x00294360]
            cam->SetShake(amp, 0.5f);                 // 0x00011290(amp, 0x3F000000)
        }
    }
    if (k < 9 && k != 7)                              // JGE 9 / JE 7: kinds 0, 5, 6, 8
        place(shockwave, d.meshes[4].Get(), slotScale(k, true));   // D+0x38
    if (level.lightManager) {                         // DAT_002EA824 vf04 0x000C08F0 (no null test in the original)
        light = level.lightManager->AddLight(level, {0.0f, 0.0f, 0.0f}, {1.0f, Bits(0x3F4CCCCDu), Bits(0x3DCCCCCDu)},
                                             100.0f, 0xB, false);   // [0x0029BC98] radius, type 11, not attached
        node->AttachChild(light.Get(), true);         // vfA0(+0x5C, 1)
    }
    return true;
}

bool Explosion::Tick() {                              // orig: vf08 0x000673D0
    // EffectInstance::Tick 0x00065E20: vf08 of every object of the Owner +0x38 (none are attached to it), then
    const bool alive = true;
    time = F(double(g_frameStep) + time);             // FLD [0x002D67E8]; FADD +0x24; FSTP
    if (time >= life) return false;                   // FCOMP +0x40; TEST AH,1; JE (> or ==: dead; NaN lives)
    if (!node) return false;
    if (light) light->radius = F((1.0 - double(time) / life) * 100.0f);   // 0x000BFC90: +0xCC
    if (time > Bits(0x3E570A3Du) && shockwave) {      // [0x0029BE58] 0.21; TEST AH,0x41; JNE
        node->DetachChild(shockwave.Get());           // vfA4(&tmp, +0x48), the temporary released
        shockwave = nullptr;
    }
    return alive;
}

bool Explosion::PostUpdate() {                        // orig: vf0C 0x000674D0
    // the yaw toward the scene camera: FPATAN(dz, dx) - PI ([0x002F9274] at run time), x87 extended (KNOWN_ISSUES #35:
    // the C library atan2 in double)
    const ni::NiCamera* cam = level.camera ? level.camera->sceneCamera.Get() : nullptr;   // (S+0x8C)+0x30
    const NiPoint3 c = cam ? cam->local.translate : NiPoint3{};   // +0x60 (no null test in the original)
    const double dx = double(node->local.translate.x) - c.x, dz = double(node->local.translate.z) - c.z;
    const ni::NiMatrix3 R = ni::MakeYRotation(F(std::atan2(dz, dx) - double(Bits(0x40490FDBu))));   // 0x001030B0
    if (model) model->local.rotate = R;               // +0x44 +0x30 (12 dwords; no null test in the original)
    if (shockwave) shockwave->local.rotate = R;
    UpdateNodeAt(node.Get(), time);                   // NiAVObject::Update(+0x24, 1)
    return true;
}

}  // namespace fut::game
