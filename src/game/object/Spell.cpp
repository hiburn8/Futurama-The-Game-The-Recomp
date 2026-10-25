// The effect-instance base and the spell effect; see Spell.h.
#include "game/object/Spell.h"

#include "engine/ni/NiMath.h"
#include "game/GameTime.h"
#include "game/defs/EffectDefs.h"
#include "game/level/LevelData.h"
#include "game/level/LightManager.h"
#include "game/object/Emitter.h"
#include "game/object/NodeUpdate.h"

#include <bit>
#include <cmath>

namespace fut::game {

using ni::detail::F;

// ---- EffectInstance -------------------------------------------------------------------------------------------------
EffectInstance::EffectInstance(LevelData& lvl, Def& d) : GameObject(lvl, d) {}   // orig: 0x00065CE0

EffectInstance::~EffectInstance() = default;          // orig: 0x00065D40 (children: the Owner dtor deletes its objects)

bool EffectInstance::Tick() {                         // orig: vf08 0x00065E20
    for (GameObject* o = children.NextObject(nullptr); o; o = children.NextObject(o))   // 0x00097670(+0x38)
        (void)o->Tick();                              // vf08 (the result is not read)
    time = F(double(g_frameStep) + time);             // FLD [0x002D67E8]; FADD +0x24; FSTP
    return true;
}

// ---- SpellEffect ----------------------------------------------------------------------------------------------------
SpellEffect::SpellEffect(LevelData& lvl, SpellDef& d, const ni::NiPoint3& a, const ni::NiPoint3& b)
    : EffectInstance(lvl, d) {                        // orig: 0x00073560
    for (u32 i = 0; i < 2; ++i) {                     // 0x000735B6 (the loop over the two slots)
        points[i] = i == 0 ? a : b;                   // +0x44 + 12 i (arg 1 for slot 0, else arg 2)
        emitterDefs[i] = i == 0 ? d.emitter28 : d.emitter2C;   // D+0x28 + 4 i
        emitters[i] = nullptr;                        // +0x64 + 4 i
        kinds[i] = i == 0 ? d.value30 : d.value34;    // D+0x30 + 4 i
    }
    age = 0;                                          // +0x40
}

SpellEffect::~SpellEffect() = default;                // orig: 0x00073310 (the vector dtor 0x00166E30: [1], then [0])

bool SpellEffect::Init() {                            // orig: vf04 0x00073390
    const ni::NiPoint3 zero{};                        // esp+0x20 (zeroed before the loop)
    for (u32 i = 0; i < 2; ++i) {
        const ni::NiPoint3 up{0.0f, 1.0f, 0.0f};      // esp+0x2C (written each iteration)
        emitters[i] = emitterDefs[i]->SpawnEffect(level, points[i], up);   // +0x6C vf20(&point, &up) (no null test)
        nodes[i] = new ni::NiNode;                    // new(0xE0) 0x00105C10 (the old reference released)
        nodes[i]->local.translate = points[i];        // node +0x60
        UpdateNodeAt(nodes[i].Get(), 0.0f);           // NiAVObject::Update(0, 1) 0x001018D0
        ni::NiPointer<LMSource> light;
        if (level.lightManager)                       // DAT_002EA824 vf04 0x000C08F0 (no null test in the original)
            light = level.lightManager->AddLight(level, zero, {1.0f, 1.0f, 0.0f}, 10.0f, 0xC, false);   // 10.0f 0x41200000
        nodes[i]->AttachChild(light.Get(), true);     // vfA0(light, 1)
        if (auto* em = dynamic_cast<EmitterInstance*>(emitters[i]))   // (no null test in the original)
            em->sourceA = nodes[i].Get();             // emitter +0x40 (the reference taken)
    }                                                 // (the light's temporary released)
    age = 0;                                          // +0x40
    return true;
}

bool SpellEffect::Tick() {                            // orig: vf08 0x00073220
    const double t = double(g_frameStep) + age;       // FLD [0x002D67E8]; FADD +0x40
    age = F(t);                                       // FST +0x40
    if (t > 5.0f) return false;                       // FCOMP [0x00293C24] 5.0; TEST AH,0x41; JNE (NaN lives)
    for (u32 i = 0; i < 2; ++i) {
        if (!emitters[i]) continue;                   // +0x64 + 4 i
        ni::NiNode* n = nodes[i].Get();               // (no null test)
        ni::NiPoint3 p = n->local.translate;          // node +0x60
        if (kinds[i] == 1) {                          // DEC; JE
            const double y = double(g_frameStep) * 4.0f + p.y;   // FMUL [0x00293C1C] 4.0; FADD
            p.y = F(y);                               // FST (the sum stays on the stack)
            const double a = y - points[i].y;         // FSUB +0x48 + 12 i
            // (KNOWN_ISSUES #35: FSIN / FCOS in double, as the oracle's)
            p.x = F(std::sin(a) * 3.0f + points[i].x);   // FLD ST0; FSIN; FMUL [0x0029563C] 3.0; FADD; FSTP
            p.z = F(std::cos(a) * 3.0f + points[i].z);   // FCOS; FMUL 3.0; FADD +0x4C + 12 i; FSTP
        } else if (kinds[i] == 2) {                   // DEC; JNE
            p.y = F(double(g_frameStep) * 4.0f + p.y);   // FMUL 4.0; FADD; FSTP
        }
        n->local.translate = p;
        UpdateNodeAt(n, 0.0f);                        // NiAVObject::Update(0, 1)
    }
    return EffectInstance::Tick();                    // jmp 0x00065E20
}

// ---- SpellDef vf20 --------------------------------------------------------------------------------------------------
GameObject* SpellDef::SpawnEffect(LevelData& level, const ni::NiPoint3& a, const ni::NiPoint3& b) {
    auto* o = new SpellEffect(level, *this, a, b);    // orig: 0x00073630: new(0x7C) 0x00073560 (no null test)
    o->SetOwner(&level);                              // vf10(S)
    o->SetRotation(ni::NiMatrix3{});                  // vf24(0x002D7820, the identity: 12 dwords)
    o->SetPosition(a);                                // vf1C (a copy of a)
    return o->Init() ? o : nullptr;                   // vf04; NEG AL; SBB; AND
}

}  // namespace fut::game
