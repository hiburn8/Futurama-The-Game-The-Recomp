// GenericNPCDef / SimpleNPCDef constructors, decompiled from default.xbe (see CharacterDef.h). The GenericNPCDef
// branches were drafted from the register-tracked disassembly of 0x0004B5B0 and checked against the original under
// emulation (tests/game/NpcDefsTests.cpp, oracle tools/difftest/npc_oracle.py).
#include "game/defs/CharacterDef.h"

#include "game/defs/EffectDefs.h"
#include "game/level/LevelData.h"
#include "game/text/LanguageDb.h"

#include <cstdio>
#include <cstring>

namespace fut::game {
namespace {

using ni::detail::F;

f32 FromBits(u32 bits) {
    f32 f;
    std::memcpy(&f, &bits, 4);
    return f;
}

}  // namespace

void CharacterDef::AddSound(const char* soundPath, u32 id, DefContext& ctx) {   // orig: 0x000403F0
    sounds.push_back({ctx.loadSound ? ctx.loadSound(soundPath, 0) : 0u, id});
}

void SimpleNPCDef::Construct(DefContext& ctx) {       // orig: 0x0005F630 after the CharacterDef constructor
    if (text::StrICmp(name.c_str(), "kamikaze") == 0) type144 = 0;
    sounds148 = {};
    value160 = 100.0f;
    if (type144 == 0) {
        sounds148[0] = ctx.loadSound("sounds/level5/npc/wasp/wasp-hoverloop.wav", 0);
        sounds148[1] = ctx.loadSound("sounds/level5/npc/wasp/wasp-diveloop.wav", 0);
        sounds148[2] = ctx.loadSound("sounds/level5/npc/wasp/wasp-warning.wav", 0);
        sounds148[3] = ctx.loadSound("sounds/level5/npc/wasp/wasp-launch.wav", 0);
        value160 = 500.0f;
    }
}

void GenericNPCDef::Construct(DefContext& ctx, const LevelData& level, const std::string& levelName,
                              std::vector<std::unique_ptr<Def>>& defs) {   // orig: 0x0004B5B0
    // The defaults are the member initialisers (attack1Count / attack2Count = 1 are CharacterDef's own defaults).
    const auto Is = [&](const char* n) { return text::StrICmp(name.c_str(), n) == 0; };
    const auto FindNode = [&](const char* n) {         // orig: 0x000C2AF0 + DynamicCast to NiNode (RTTI 0x002F9230)
        return dynamic_cast<ni::NiNode*>(level.FindEffectObject(n));
    };
    const auto FindObject = [&](const char* n) {       // orig: 0x000C2AF0 + DynamicCast to NiAVObject (0x002F9190)
        return level.FindEffectObject(n);
    };
    const auto Localized = [](const char* format, const DefContext& c) {   // orig: 0x000D9DF0
        char buffer[256];
        std::snprintf(buffer, sizeof buffer, format, c.language.c_str());
        return std::string(buffer);
    };
    const auto NewLaser = [&](const char* n, const LevelData& l, std::vector<std::unique_ptr<Def>>& out) {
        auto owned = std::make_unique<LaserDef>();     // orig: ctor 0x00079BF0 (adds its spark emitter after it)
        LaserDef* d = owned.get();
        d->name = n;
        out.push_back(std::move(owned));
        d->Construct(l, out);
        return d;
    };
    const auto NewEmitter = [](const char* n, std::vector<std::unique_ptr<Def>>& out) {
        auto owned = std::make_unique<SpriteEmitterDef>();   // orig: ctor 0x0006E090
        SpriteEmitterDef* d = owned.get();
        d->name = n;
        out.push_back(std::move(owned));
        return d;
    };
    const auto NewFlare = [](const char* n, std::vector<std::unique_ptr<Def>>& out) {
        auto owned = std::make_unique<FlareDef>();     // orig: ctor 0x0006A000
        FlareDef* d = owned.get();
        d->name = n;
        out.push_back(std::move(owned));
        return d;
    };
    const auto NewProjectile = [](const char* n, ni::NiNode* m, const ProjectileDef::Args& a,
                                  std::vector<std::unique_ptr<Def>>& out) {
        auto owned = std::make_unique<ProjectileDef>();   // orig: ctor 0x0007BCB0
        ProjectileDef* d = owned.get();
        d->name = n;
        out.push_back(std::move(owned));
        d->Construct(m, a);
        return d;
    };
    const auto ApplyAll = [](EmitterDef& e, std::initializer_list<EmitterCall> calls, const LevelData& l) {
        for (const EmitterCall& c : calls) ApplyEmitterCall(e, c, l);
    };
    LaserDef* laser = nullptr;
    ProjectileDef* projectile = nullptr;
    SpriteEmitterDef* emitter = nullptr;
    FlareDef* flare = nullptr;
    ni::NiNode* mesh = nullptr;

    if (Is("turdman")) {
        value19C = FromBits(0x42480000u);
        value198 = FromBits(0x42c80000u);
        flags170[0x17D - 0x170] = 1;
        flags170[0x189 - 0x170] = 1;
        AddSound("sounds/level1/npc/mutant/attackroar_1.wav", 0xa, ctx);
        AddSound("sounds/level1/npc/mutant/attackroar_2.wav", 0xa, ctx);
        AddSound("sounds/level1/npc/mutant/attackroar_3.wav", 0xa, ctx);
        AddSound("sounds/level1/npc/mutant/attackroar_4.wav", 0xa, ctx);
        AddSound("sounds/level1/npc/mutant/Impact-flesh-1.wav", 2, ctx);
        AddSound("sounds/level1/npc/mutant/Impact-flesh-2.wav", 2, ctx);
        AddSound("sounds/level1/npc/mutant/Impact_1.wav", 2, ctx);
        AddSound("sounds/level1/npc/mutant/Impact_2.wav", 2, ctx);
        AddSound("sounds/level1/npc/mutant/Impact_3.wav", 2, ctx);
        AddSound("sounds/level1/npc/mutant/Impact_4.wav", 2, ctx);
        AddSound("sounds/level1/npc/mutant/Ent-Scream.wav", 0x5e, ctx);
        AddSound("sounds/level1/npc/mutant/Die-Grunt.wav", 0, ctx);
        AddSound("sounds/level1/npc/mutant/Die-Grunt2.wav", 0, ctx);
        AddSound("sounds/level1/npc/mutant/SlapHit_1.wav", 0x17, ctx);
        AddSound("sounds/level1/npc/mutant/SlapHit_2.wav", 0x17, ctx);
        AddSound("sounds/level1/npc/mutant/SlapHit_3.wav", 0x17, ctx);
        AddSound("sounds/level1/npc/mutant/SlapHit_4.wav", 0x17, ctx);
        AddSound("sounds/level1/npc/mutant/BreatheLoop.wav", 0x14, ctx);
        AddSound("sounds/level1/npc/mutant/shake.wav", 0x35, ctx);
        AddSound("sounds/level1/npc/mutant/Laugh-mad.wav", 0x13, ctx);
        AddSound("sounds/level1/npc/mutant/Laugh-goofy.wav", 0x13, ctx);
        AddSound("sounds/level1/npc/mutant/Laugh-goofy-2.wav", 0x13, ctx);
        AddSound("sounds/level1/sewers/watersplash.wav", 0x4a, ctx);
        value14C = FromBits(0x432f0000u);
    } else if (Is("lavaman")) {
        value19C = FromBits(0x42480000u);
        value198 = FromBits(0x42c80000u);
        flags170[0x17D - 0x170] = 1;
        flags170[0x189 - 0x170] = 1;
        AddSound("sounds/level3/npc/lavaman/attack1.wav", 0xa, ctx);
        AddSound("sounds/level3/npc/lavaman/attack2.wav", 0xa, ctx);
        AddSound("sounds/level3/npc/lavaman/die1.wav", 0, ctx);
        AddSound("sounds/level3/npc/lavaman/die2.wav", 0, ctx);
        AddSound("sounds/level3/npc/lavaman/dieSFX.wav", 0, ctx);
        AddSound("sounds/level3/npc/lavaman/dieSFX.wav", 0, ctx);
        AddSound("sounds/level3/npc/lavaman/dieSFX.wav", 0, ctx);
        AddSound("sounds/level3/npc/lavaman/dieSFX.wav", 0, ctx);
        AddSound("sounds/level3/npc/lavaman/Impact1.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/Impact2.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/Impact3.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/Impact4.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/ImpactSFX-1.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/ImpactSFX-2.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/ImpactSFX-3.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/ImpactSFX-1.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/ImpactSFX-2.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/ImpactSFX-3.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/ImpactSFX-1.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/ImpactSFX-2.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/ImpactSFX-3.wav", 2, ctx);
        AddSound("sounds/level3/npc/lavaman/Growl1.wav", 0x17, ctx);
        AddSound("sounds/level3/npc/lavaman/Growl2.wav", 0x17, ctx);
        AddSound("sounds/level3/npc/lavaman/Growl3.wav", 0x17, ctx);
        AddSound("sounds/level3/npc/lavaman/laugh1.wav", 0x13, ctx);
        AddSound("sounds/level3/npc/lavaman/laugh2.wav", 0x13, ctx);
        AddSound("sounds/level3/npc/lavaman/burn-loop.wav", 0x7c, ctx);
        AddSound("Sounds/Level3/Generic/Fall-in-lava.wav", 0x49, ctx);
        value14C = FromBits(0x43c80000u);
    } else if (Is("rockmonster")) {
        value19C = FromBits(0x42480000u);
        value198 = FromBits(0x42c80000u);
        flags170[0x17D - 0x170] = 1;
        flags170[0x189 - 0x170] = 1;
        AddSound("sounds/level2/npc/rockmonster/attack1.wav", 0xa, ctx);
        AddSound("sounds/level2/npc/rockmonster/attack2.wav", 0xa, ctx);
        AddSound("sounds/level2/npc/rockmonster/die1.wav", 0, ctx);
        AddSound("sounds/level2/npc/rockmonster/die2.wav", 0, ctx);
        AddSound("sounds/level2/npc/rockmonster/HitRock.wav", 0x1d, ctx);
        AddSound("sounds/level2/npc/rockmonster/Impact1.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/Impact2.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/Impact3.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/Impact4.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/ImpactSFX-1.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/ImpactSFX-2.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/ImpactSFX-3.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/ImpactSFX-1.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/ImpactSFX-2.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/ImpactSFX-3.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/ImpactSFX-1.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/ImpactSFX-2.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/ImpactSFX-3.wav", 2, ctx);
        AddSound("sounds/level2/npc/rockmonster/Growl1.wav", 0x17, ctx);
        AddSound("sounds/level2/npc/rockmonster/Growl2.wav", 0x17, ctx);
        AddSound("sounds/level2/npc/rockmonster/Growl3.wav", 0x17, ctx);
        AddSound("sounds/level2/npc/rockmonster/laugh1.wav", 0x13, ctx);
        AddSound("sounds/level2/npc/rockmonster/laugh2.wav", 0x13, ctx);
        AddSound("sounds/level2/npc/rockmonster/Land.wav", 0x32, ctx);
        value14C = FromBits(0x43480000u);
    } else if (Is("tentacle_dude")) {
        flag1A0 = 0;
        value19C = FromBits(0x42480000u);
        value198 = FromBits(0x42c80000u);
        value14C = FromBits(0x43160000u);
        AddSound("sounds/level1/npc/sewertentacle/attack.wav", 4, ctx);
        AddSound("sounds/level1/npc/sewertentacle/impact.wav", 2, ctx);
        AddSound("sounds/level1/npc/sewertentacle/idle-1.wav", 0x10, ctx);
        AddSound("sounds/level1/npc/sewertentacle/idle-2.wav", 0x10, ctx);
        AddSound("sounds/level1/npc/sewertentacle/idle-3.wav", 0x10, ctx);
        AddSound("sounds/level1/npc/sewertentacle/shake_loop.wav", 0x35, ctx);
        flags170[0x17C - 0x170] = 1;
    } else if (Is("toxicwarrior")) {
        value19C = FromBits(0x41c80000u);
        value198 = FromBits(0x42c80000u);
        value194 = 0.0f;
        value190 = FromBits(0x41700000u);
        value144 = 1;
        value168 = ctx.rand() % 10 + 5;                // orig: rand() 0x0016695C
        flags170[0x17E - 0x170] = 1;
        AddSound("sounds/weapons/laser shotgun fire 1.wav", 4, ctx);
        AddSound("sounds/weapons/laser shotgun fire 2.wav", 5, ctx);
        AddSound("sounds/level1/walker/rocket.wav", 6, ctx);
        AddSound("sounds/level1/npc/maskedman/damage1.wav", 2, ctx);
        AddSound("sounds/level1/npc/maskedman/damage2.wav", 2, ctx);
        AddSound("sounds/level1/npc/maskedman/damage3.wav", 2, ctx);
        AddSound("sounds/level1/npc/maskedman/Die-scream 1.wav", 0, ctx);
        AddSound("sounds/level1/npc/maskedman/Die-scream 2.wav", 0, ctx);
        AddSound("sounds/level1/npc/maskedman/breathe-loop.wav", 0x14, ctx);
        AddSound("sounds/level1/npc/maskedman/fizzle.wav", 0x48, ctx);
        AddSound("sounds/level1/npc/maskedman/laugh.wav", 0x13, ctx);
        AddSound("sounds/level1/npc/maskedman/hey-1.wav", 0x5e, ctx);
        AddSound("sounds/level1/npc/maskedman/hey-2.wav", 0x5e, ctx);
        AddSound("sounds/level1/sewers/watersplash.wav", 0x4a, ctx);
        laser = NewLaser("ENEMY-LASERSPAM", level, defs);
        weapon158 = laser;
        laser->value68[0] = 0.0f;
        laser->value68[1] = FromBits(0x3f800000u);
        laser->value68[2] = 0.0f;
        laser->value80 = FromBits(0x40080000u);
        laser->value68[4] = FromBits(0x3f666666u);
        laser->value68[5] = FromBits(0x40000000u);
        laser->flag85 = 1;
        laser->value98 = FromBits(0x43480000u);
        emitter = NewEmitter("Particles", defs);
        particles164 = emitter;
        emitter->SetSpriteTexture("Toxicman_weaponparticle.tga", level);
        ApplyAll(*emitter, {{Op::Range40Max, {0}}, {Op::Range28, {0x41a00000u, 0x43160000u}},
                  {Op::SizeBoth, {0x3ecccccdu}}, {Op::Gravity, {0x41200000u}}, {Op::Value4C, {0x40000000u}},
                  {Op::Range38Max, {0x3e19999au}}, {Op::Range30, {0x3e4ccccdu, 0x3f19999au}}, {Op::Count, {0x28u}},
                  {Op::FlagA8, {1}}, {Op::FlagDC, {1}}}, level);
        mesh = FindNode("Toxic_Rocket");
        projectile = NewProjectile("toxic_rocket", mesh, {FromBits(0x3fc00000u), 1, 0.0f, FromBits(0x40a00000u), 0.0f,
                                             FromBits(0x41a00000u), FromBits(0x42480000u), FromBits(0x420c0000u),
                                             FromBits(0x41a00000u)}, defs);
        projectile15C = projectile;
        emitter = NewEmitter("effect_toxicsmoke", defs);
        projectile15C->emitter34 = emitter;
        projectile15C->sound5C = ctx.loadSound("sounds/level1/npc/maskedman/RocketTravel-loop.wav", 0);
        emitter->SetSpriteTexture("smoke.tga", level);
        ApplyAll(*emitter, {{Op::Color, {0x3f800000u, 0x3f800000u, 0x3f333333u}}, {Op::Gravity, {0}},
                  {Op::Size, {0x3f000000u, 0x3f99999au}}, {Op::RangeB4, {0x3f000000u, 0}},
                  {Op::Range28, {0, 0x40400000u}}, {Op::Range38Max, {0x3f800000u}}, {Op::Range40, {0, 0x3f000000u}},
                  {Op::Range30, {0x3e4ccccdu, 0x3f333333u}}, {Op::FlagA8, {0}}, {Op::Color, {0, 0, 0}},
                  {Op::Count, {0x14u}}, {Op::Flag54, {1}}}, level);
    } else if (Is("cosmicray")) {
        flag1A0 = 0;
        value19C = FromBits(0x42480000u);
        value198 = FromBits(0x42c80000u);
        flags170[0x18D - 0x170] = 0;
        flags170[0x17C - 0x170] = 1;
    } else if (Is("gunturret")) {
        flags170[0x18E - 0x170] = 0;
        flags170[0x18B - 0x170] = 1;
        flags170[0x17C - 0x170] = 1;
        value144 = 2;
        value14C = FromBits(0x43960000u);
        flags170[0x183 - 0x170] = 1;
        AddSound("sounds/weapons/laser shotgun fire 1.wav", 4, ctx);
        AddSound("sounds/level1/npc/walker/impact1.wav", 2, ctx);
        AddSound("sounds/level1/npc/walker/impact2.wav", 2, ctx);
        laser = NewLaser("trooper laser", level, defs);
        weapon158 = laser;
        laser->value68[5] = FromBits(0x40400000u);
        mesh = FindNode("Trooper_laser");
        laser->model = mesh;
        laser->value80 = FromBits(0x41200000u);
        flare = NewFlare("TrooperFlare", defs);
        flare160 = flare;
        flare->SetValue(FromBits(0x3dcccccdu));
        flare->mesh = FindObject("Trooper_muzzle");
        emitter = NewEmitter("effect_toxicpart", defs);
        particles164 = emitter;
        emitter->SetSpriteTexture("Trooper_weaponparticle.tga", level);
        ApplyAll(*emitter, {{Op::Range40Max, {0}}, {Op::Range28, {0, 0x41200000u}}, {Op::SizeBoth, {0x3f000000u}},
                  {Op::Gravity, {0}}, {Op::Value4C, {0x40000000u}}, {Op::Range38Max, {0x3eb33333u}},
                  {Op::Range30, {0x3e4ccccdu, 0x3f19999au}}, {Op::Count, {0xau}}, {Op::FlagA8, {1}},
                  {Op::FlagDC, {1}}}, level);
    } else if (Is("chickenwalker")) {
        flags170[0x18D - 0x170] = 0;
        value14C = FromBits(0x43960000u);
        value144 = 5;
        flags170[0x183 - 0x170] = 0;
        AddSound("sounds/level1/npc/walker/die.wav", 0, ctx);
        AddSound("sounds/weapons/laser shotgun fire 1.wav", 4, ctx);
        AddSound("sounds/level1/npc/walker/impact1.wav", 2, ctx);
        AddSound("sounds/level1/npc/walker/impact2.wav", 2, ctx);
        laser = NewLaser("ChickenLaser", level, defs);
        weapon158 = laser;
        laser->value68[5] = FromBits(0x41700000u);
        laser->value80 = FromBits(0x42960000u);
        laser->value68[0] = FromBits(0x3dcccccdu);
        laser->value68[1] = FromBits(0x3e800000u);
        laser->value68[2] = FromBits(0x3f800000u);
        laser->value68[4] = FromBits(0x40000000u);
    } else if (Is("stormtrooper")) {
        value198 = FromBits(0x43480000u);
        value19C = FromBits(0x42480000u);
        value194 = 0.0f;
        value190 = FromBits(0x41700000u);
        value14C = FromBits(0x43960000u);
        value144 = 2;
        value168 = 0xcu;
        flags170[0x17E - 0x170] = 1;
        flags170[0x183 - 0x170] = 1;
        AddSound("sounds/weapons/laser pistol fire 1.wav", 4, ctx);
        AddSound("sounds/weapons/laser pistol fire 2.wav", 4, ctx);
        AddSound("sounds/level1/npc/stormtrooper/impact1.wav", 2, ctx);
        AddSound("sounds/level1/npc/stormtrooper/impact2.wav", 2, ctx);
        AddSound("sounds/level1/npc/stormtrooper/hoverloop.wav", 0x15, ctx);
        laser = NewLaser("trooper laser", level, defs);
        weapon158 = laser;
        laser->value68[5] = FromBits(0x40400000u);
        mesh = FindNode("Stormtrooper_laser");
        laser->model = mesh;
        laser->value80 = FromBits(0x41200000u);
        laser->value68[0] = FromBits(0x3f800000u);
        laser->value68[1] = FromBits(0x3e4ccccdu);
        laser->value68[2] = FromBits(0x3e4ccccdu);
        flare = NewFlare("TrooperFlare", defs);
        flare160 = flare;
        flare->SetValue(FromBits(0x3dcccccdu));
        flare->mesh = FindObject("Trooper_muzzle");
        emitter = NewEmitter("effect_toxicpart", defs);
        particles164 = emitter;
        emitter->SetSpriteTexture("Trooper_weaponparticle.tga", level);
        ApplyAll(*emitter, {{Op::Range40Max, {0}}, {Op::Range28, {0, 0x41200000u}}, {Op::SizeBoth, {0x3f000000u}},
                  {Op::Gravity, {0}}, {Op::Value4C, {0x40000000u}}, {Op::Range38Max, {0x3eb33333u}},
                  {Op::Range30, {0x3e4ccccdu, 0x3f19999au}}, {Op::Count, {0xau}}, {Op::FlagA8, {1}},
                  {Op::FlagDC, {1}}}, level);
    } else if (Is("goonskinny")) {
        value194 = 0.0f;
        value190 = FromBits(0x41700000u);
        value14C = FromBits(0x43480000u);
        value144 = 2;
        value168 = 0xcu;
        flags170[0x17E - 0x170] = 0;
        flags170[0x183 - 0x170] = 1;
        laser = NewLaser("trooper laser", level, defs);
        weapon158 = laser;
        laser->value68[5] = FromBits(0x40400000u);
        mesh = FindNode("Trooper_laser");
        laser->model = mesh;
        laser->value80 = FromBits(0x41000000u);
        laser->value68[0] = FromBits(0x3f800000u);
        laser->value68[1] = FromBits(0x3e4ccccdu);
        laser->value68[2] = FromBits(0x3e4ccccdu);
        flare = NewFlare("TrooperFlare", defs);
        flare160 = flare;
        flare->SetValue(FromBits(0x3dcccccdu));
        flare->mesh = FindObject("Trooper_muzzle");
        emitter = NewEmitter("effect_toxicpart", defs);
        particles164 = emitter;
        emitter->SetSpriteTexture("Trooper_weaponparticle.tga", level);
        ApplyAll(*emitter, {{Op::Range40Max, {0}}, {Op::Range28, {0, 0x41200000u}}, {Op::SizeBoth, {0x3f000000u}},
                  {Op::Gravity, {0}}, {Op::Value4C, {0x40000000u}}, {Op::Range38Max, {0x3eb33333u}},
                  {Op::Range30, {0x3e4ccccdu, 0x3f19999au}}, {Op::Count, {0xau}}, {Op::FlagA8, {1}},
                  {Op::FlagDC, {1}}}, level);
        emitter = NewEmitter("effect_toxicpart", defs);
        laser->sparks = emitter;
        emitter->SetSpriteTexture("Trooper_weaponparticle.tga", level);
        ApplyAll(*emitter, {{Op::Range40Max, {0}}, {Op::Range28, {0, 0x41200000u}}, {Op::SizeBoth, {0x3f000000u}},
                  {Op::Gravity, {0}}, {Op::Value4C, {0x40000000u}}, {Op::Range38Max, {0x3eb33333u}},
                  {Op::Range30, {0x3e4ccccdu, 0x3f19999au}}, {Op::Count, {0xau}}, {Op::FlagA8, {1}},
                  {Op::FlagDC, {1}}}, level);
        AddSound("sounds/level1/npc/goonskinny/fire.wav", 4, ctx);
        AddSound("sounds/level1/npc/goonskinny/impact1.wav", 2, ctx);
        AddSound("sounds/level1/npc/goonskinny/impact2.wav", 2, ctx);
        AddSound("sounds/level1/npc/goonskinny/death.wav", 0, ctx);   // (shared tail)
    } else if (Is("goonbox")) {
        value194 = 0.0f;
        value190 = FromBits(0x41700000u);
        value14C = FromBits(0x43480000u);
        value144 = 2;
        value168 = 0xcu;
        flags170[0x17E - 0x170] = 0;
        flags170[0x183 - 0x170] = 1;
        laser = NewLaser("trooper laser", level, defs);
        weapon158 = laser;
        laser->value68[5] = FromBits(0x40400000u);
        mesh = FindNode("Trooper_laser");
        laser->model = mesh;
        laser->value80 = FromBits(0x41000000u);
        laser->value68[0] = FromBits(0x3f800000u);
        laser->value68[1] = FromBits(0x3e4ccccdu);
        laser->value68[2] = FromBits(0x3e4ccccdu);
        flare = NewFlare("TrooperFlare", defs);
        flare160 = flare;
        flare->SetValue(FromBits(0x3dcccccdu));
        flare->mesh = FindObject("Trooper_muzzle");
        emitter = NewEmitter("effect_toxicpart", defs);
        particles164 = emitter;
        emitter->SetSpriteTexture("Trooper_weaponparticle.tga", level);
        ApplyAll(*emitter, {{Op::Range40Max, {0}}, {Op::Range28, {0, 0x41200000u}}, {Op::SizeBoth, {0x3f000000u}},
                  {Op::Gravity, {0}}, {Op::Value4C, {0x40000000u}}, {Op::Range38Max, {0x3eb33333u}},
                  {Op::Range30, {0x3e4ccccdu, 0x3f19999au}}, {Op::Count, {0xau}}, {Op::FlagA8, {1}},
                  {Op::FlagDC, {1}}}, level);
        AddSound("sounds/level1/npc/goonbox/fire.wav", 4, ctx);
        AddSound("sounds/level1/npc/goonbox/gas-fire.wav", 0xa, ctx);
        AddSound("sounds/level1/npc/goonbox/damage1.wav", 2, ctx);
        AddSound("sounds/level1/npc/goonbox/damage2.wav", 2, ctx);
        AddSound("sounds/level1/npc/goonbox/damage3.wav", 2, ctx);
        AddSound("sounds/level1/npc/goonbox/death.wav", 0, ctx);
        AddSound("sounds/level1/npc/goonbox/track_loop.wav", 0x15, ctx);   // (shared tail)
    } else if (Is("warthog")) {
        flags170[0x18B - 0x170] = 1;
        value194 = 0.0f;
        value190 = FromBits(0x41700000u);
        value14C = FromBits(0x43480000u);
        flags170[0x189 - 0x170] = 1;
        AddSound("sounds/level1/npc/warthog/attack1-1.wav", 4, ctx);
        AddSound("sounds/level1/npc/warthog/damage1.wav", 2, ctx);
        AddSound("sounds/level1/npc/warthog/damage2.wav", 2, ctx);
        AddSound("sounds/level1/npc/warthog/damage3.wav", 2, ctx);
        AddSound("sounds/level1/npc/warthog/die1.wav", 0, ctx);
        AddSound("sounds/level1/npc/warthog/die2.wav", 0, ctx);
        AddSound("sounds/level1/npc/warthog/1.wav", 0x10, ctx);
        AddSound("sounds/level1/npc/warthog/2.wav", 0x10, ctx);
        AddSound("sounds/level1/npc/warthog/3.wav", 0x10, ctx);   // (shared tail)
    } else if (Is("goonsneak")) {
        value194 = 0.0f;
        value190 = FromBits(0x41700000u);
        value14C = FromBits(0x43480000u);
        flags170[0x183 - 0x170] = 1;
        value144 = 0xbu;
        mesh = FindNode("Granate");
        projectile = NewProjectile("granate", mesh, {FromBits(0x40000000u), 0, FromBits(0x3f19999au),
                                             FromBits(0x40200000u), FromBits(0x40c00000u), FromBits(0x41700000u),
                                             FromBits(0x42c80000u), FromBits(0x42480000u),
                                             FromBits(0x41c80000u)}, defs);
        weapon158 = projectile;
        weapon158->sound60 = ctx.loadSound("sounds/level1/npc/goonsneak/GrenadeBounce.wav", 0);
        AddSound("sounds/level1/npc/goonsneak/throw.wav", 4, ctx);
        AddSound("sounds/level1/npc/goonsneak/damage1.wav", 2, ctx);
        AddSound("sounds/level1/npc/goonsneak/damage2.wav", 2, ctx);
        AddSound("sounds/level1/npc/goonsneak/damage3.wav", 2, ctx);
        AddSound("sounds/level1/npc/goonsneak/death.wav", 0, ctx);
        AddSound("sounds/level1/npc/goonsneak/laugh.wav", 0x12, ctx);
        AddSound("sounds/level1/npc/goonsneak/laugh-2.wav", 0x12, ctx);   // (shared tail)
    } else if (Is("aquafish")) {
        value14C = FromBits(0x43480000u);
        flags170[0x183 - 0x170] = 1;
    } else if (Is("kfish")) {
        value14C = FromBits(0x42c80000u);
        value174 = FromBits(0x3f800000u);
        flags170[0x17C - 0x170] = 1;
        flags170[0x188 - 0x170] = 1;
    } else if (Is("crocodile")) {
        flag1A0 = 0;
        flags170[0x18D - 0x170] = 0;
        value14C = FromBits(0x43480000u);
        flags170[0x17C - 0x170] = 1;
        AddSound("sounds/level1/npc/crocodile/idle-roar1.wav", 0x10, ctx);
        AddSound("sounds/level1/npc/crocodile/idle-roar2.wav", 0x10, ctx);
        AddSound("sounds/level1/npc/crocodile/jump-roar1.wav", 0x11, ctx);
        AddSound("sounds/level1/npc/crocodile/jump-roar2.wav", 0x11, ctx);
        value150 = FromBits(0x41000000u);
        value154 = FromBits(0x41600000u);
    } else if (Is("hooker1") || Is("hooker2") || Is("alien_pod")) {   // orig: the three share 0x0004CF45
        flag1A0 = 0;
        flags170[0x18D - 0x170] = 0;
        flags170[0x17C - 0x170] = 1;
    } else if (Is("eyestalk")) {
        value194 = 0.0f;
        value190 = FromBits(0x40a00000u);
        flags170[0x18D - 0x170] = 1;
        value14C = FromBits(0x43480000u);
        flags170[0x17C - 0x170] = 1;
        value144 = 2;
        mesh = FindNode("Canyoncreature_shot");
        if (mesh) mesh->velocity.z = 1.5f;           // orig: fabs(1.5) stored into the effects mesh (+0x6C)
        projectile = NewProjectile("eyestalk", mesh, {FromBits(0x3f400000u), 1, FromBits(0x3dcccccdu),
                                             FromBits(0x41200000u), FromBits(0x3f400000u), FromBits(0x40a00000u),
                                             FromBits(0x41700000u), FromBits(0x41700000u),
                                             FromBits(0x41700000u)}, defs);
        projectile->value24 = F(static_cast<double>(projectile->value24) * static_cast<double>(0.6f));
        weapon158 = projectile;
        emitter = NewEmitter("effect_snotpart", defs);
        weapon158->emitter30 = emitter;
        emitter->SetSpriteTexture("U_canyoncreature_snotparticle.tga", level);
        ApplyAll(*emitter, {{Op::Range40Max, {0}}, {Op::Range28, {0x41a00000u, 0x41f00000u}},
                  {Op::Size, {0x3dcccccdu, 0x3e800000u}}, {Op::Gravity, {0x42480000u}}, {Op::Value4C, {0x3f000000u}},
                  {Op::Range38Max, {0x3e800000u}}, {Op::Range30, {0x40a00000u, 0x40a00000u}}, {Op::Count, {0}},
                  {Op::FlagA8, {0}}}, level);
        emitter->spriteCurrent.flagDC = 0;   // orig: a direct store to +0x110
        AddSound("sounds/level1/npc/eyestalk/fire.wav", 4, ctx);
        AddSound("sounds/level1/npc/eyestalk/die.wav", 0, ctx);   // (shared tail)
    } else if (Is("weasel")) {
        value194 = 0.0f;
        value190 = FromBits(0x41700000u);
        value174 = FromBits(0x40000000u);
        flags170[0x189 - 0x170] = 1;
        value144 = 9;
        AddSound("sounds/level2/npc/canyoncreature/shoot-1.wav", 4, ctx);
        AddSound("sounds/level2/npc/canyoncreature/shoot-2.wav", 4, ctx);
        AddSound("sounds/level2/npc/canyoncreature/damage-1.wav", 2, ctx);
        AddSound("sounds/level2/npc/canyoncreature/damage-2.wav", 2, ctx);
        AddSound("sounds/level2/npc/canyoncreature/damage-3.wav", 2, ctx);
        AddSound("sounds/level2/npc/canyoncreature/damage-4.wav", 2, ctx);
        AddSound("sounds/level2/npc/canyoncreature/die-1.wav", 0, ctx);
        AddSound("sounds/level2/npc/canyoncreature/die-2.wav", 0, ctx);
        AddSound("sounds/level2/npc/CanyonCreature/Land.wav", 0x32, ctx);
        AddSound("sounds/level2/npc/CanyonCreature/Jump.wav", 0x33, ctx);
        AddSound("sounds/level2/npc/canyoncreature/Impact-shoulder.wav", 0x19, ctx);
        AddSound("sounds/level2/npc/canyoncreature/Impact-Spin-1.wav", 0x18, ctx);
        AddSound("sounds/level2/npc/canyoncreature/Impact-Spin-2.wav", 0x18, ctx);
        AddSound("sounds/level2/npc/canyoncreature/Impact-Butt.wav", 0x1a, ctx);
        AddSound("sounds/level2/npc/canyoncreature/laugh-1.wav", 0x12, ctx);
        AddSound("sounds/level2/npc/canyoncreature/laugh-2.wav", 0x12, ctx);
        AddSound("sounds/level2/npc/canyoncreature/warning.wav", 0x5c, ctx);
        AddSound("sounds/level2/npc/canyoncreature/Roll_loop.wav", 0x34, ctx);
        mesh = FindNode("Canyoncreature_shot");
        // orig: stored without a check (a missing mesh would fault; KNOWN_ISSUES #19).
        if (mesh) mesh->velocity.z = 1.5f;
        projectile = NewProjectile("snot", mesh, {FromBits(0x3f800000u), 1, FromBits(0x3dcccccdu),
                                             FromBits(0x41200000u), FromBits(0x3f800000u), FromBits(0x40a00000u),
                                             FromBits(0x41200000u), FromBits(0x41200000u),
                                             FromBits(0x41200000u)}, defs);
        weapon158 = projectile;
        emitter = NewEmitter("effect_snotpart", defs);
        weapon158->emitter30 = emitter;
        emitter->SetSpriteTexture("U_canyoncreature_snotparticle.tga", level);
        ApplyAll(*emitter, {{Op::Range40Max, {0}}, {Op::Range28, {0x41a00000u, 0x41f00000u}},
                  {Op::Size, {0x3dcccccdu, 0x3e800000u}}, {Op::Gravity, {0x42480000u}}, {Op::Value4C, {0x3f000000u}},
                  {Op::Range38Max, {0x3e800000u}}, {Op::Range30, {0x40a00000u, 0x40a00000u}}, {Op::Count, {0x19u}},
                  {Op::FlagA8, {0}}}, level);
        emitter->spriteCurrent.flagDC = 0;   // orig: a direct store to +0x110
    } else if (Is("junkbot")) {
        flags170[0x18A - 0x170] = 1;
        AddSound("sounds/level2/npc/junkbot/blade.wav", 0xa, ctx);
        AddSound("sounds/level2/npc/junkbot/drill.wav", 0xb, ctx);
        AddSound("sounds/level2/npc/junkbot/damage1.wav", 2, ctx);
        AddSound("sounds/level2/npc/junkbot/damage2.wav", 2, ctx);
        AddSound("sounds/level2/npc/junkbot/damage3.wav", 2, ctx);
        AddSound("sounds/level2/npc/junkbot/die.wav", 0, ctx);
        AddSound("sounds/level2/npc/junkbot/trackloop.wav", 0x15, ctx);   // (shared tail)
    } else if (Is("carnivorous_plant")) {
        flag1A0 = 0;
        flags170[0x18D - 0x170] = 0;
        flags170[0x17C - 0x170] = 1;
        AddSound("sounds/level2/npc/plant/idle-1.wav", 0x10, ctx);
        AddSound("sounds/level2/npc/plant/idle-2.wav", 0x10, ctx);
        AddSound("Sounds/Level2/NPC/plant/Chew-Bender.wav", 0x7d, ctx);
        AddSound("Sounds/Level2/NPC/plant/Chew-Bender2.wav", 0x7d, ctx);
        AddSound("Sounds/Level2/NPC/plant/Spit.wav", 0x7e, ctx);   // (shared tail)
    } else if (Is("cactusmonster")) {
        flag1A0 = 0;
        flags170[0x18D - 0x170] = 0;
        flags170[0x17C - 0x170] = 1;
        AddSound("sounds/level2/npc/cactus/whip.wav", 0xa, ctx);   // (shared tail)
    } else if (Is("flowerplant")) {
        flag1A0 = 0;
        flags170[0x18D - 0x170] = 0;
        value144 = 0xau;
        flags170[0x17C - 0x170] = 1;
        mesh = FindNode("Flowerplant_shot");
        projectile = NewProjectile("flower", mesh, {FromBits(0x3f800000u), 1, FromBits(0x3dcccccdu),
                                             FromBits(0x41200000u), FromBits(0x3f800000u), FromBits(0x40a00000u),
                                             FromBits(0x40400000u), FromBits(0x40400000u),
                                             FromBits(0x40400000u)}, defs);
        projectile->value24 = F(static_cast<double>(projectile->value24) * static_cast<double>(0.6f));
        weapon158 = projectile;
        emitter = NewEmitter("effect_snotpart", defs);
        weapon158->emitter30 = emitter;
        emitter->SetSpriteTexture("U_canyoncreature_snotparticle.tga", level);
        ApplyAll(*emitter, {{Op::Range40Max, {0}}, {Op::Range28, {0x41a00000u, 0x41f00000u}},
                  {Op::Size, {0x3dcccccdu, 0x3e800000u}}, {Op::Gravity, {0x42480000u}}, {Op::Value4C, {0x3f000000u}},
                  {Op::Range38Max, {0x3e800000u}}, {Op::Range30, {0x40a00000u, 0x40a00000u}}}, level);
        ApplyAll(*emitter, {{Op::Count, {0}}, {Op::FlagA8, {0}}}, level);   // orig: the weasel tail 0x0004D3C6
        emitter->spriteCurrent.flagDC = 0;
    } else if (Is("stormtrooper_mk2")) {
        value198 = FromBits(0x43480000u);
        value19C = FromBits(0x42480000u);
        value194 = 0.0f;
        value190 = FromBits(0x41700000u);
        value14C = FromBits(0x43960000u);
        value144 = 3;
        flags170[0x183 - 0x170] = 1;
        AddSound("sounds/level2/npc/Stormtrooper_MKII/Grenade-fire.wav", 4, ctx);
        AddSound("sounds/level2/npc/Stormtrooper_MKII/Impact1.wav", 2, ctx);
        AddSound("sounds/level2/npc/Stormtrooper_MKII/Impact2.wav", 2, ctx);
        AddSound("sounds/level2/npc/Stormtrooper_MKII/die-land.wav", 0, ctx);
        AddSound("sounds/level2/npc/Stormtrooper_MKII/die-land2.wav", 0, ctx);
        AddSound("sounds/level2/npc/Stormtrooper_MKII/HoverLoop.wav", 0x14, ctx);
        value144 = 0xbu;
        mesh = FindNode("Granate");
        projectile = NewProjectile("granate", mesh, {FromBits(0x40000000u), 0, FromBits(0x3f19999au),
                                             FromBits(0x40200000u), FromBits(0x40a00000u), FromBits(0x41900000u),
                                             FromBits(0x42c80000u), FromBits(0x42480000u),
                                             FromBits(0x41c80000u)}, defs);
        weapon158 = projectile;
        weapon158->sound60 = ctx.loadSound("sounds/level2/npc/Stormtrooper_MKII/Grenade-bounce.wav", 0);
    } else if (Is("igner_boss")) {
        value198 = FromBits(0x44160000u);
        value19C = FromBits(0x44160000u);
        flags170[0x18B - 0x170] = 1;
        flags170[0x18D - 0x170] = 0;
        flag1A0 = 0;
        value14C = FromBits(0x43c80000u);
        value144 = 0xcu;
        laser = NewLaser("Igner_Laser", level, defs);
        weapon158 = laser;
        laser->value68[5] = FromBits(0x41a00000u);
        laser->value1C = FromBits(0x43960000u);
        mesh = FindNode("Igner_shot");
        laser->model = mesh;
        laser->value80 = FromBits(0x41000000u);
        flare = NewFlare("TrooperFlare", defs);
        flare160 = flare;
        flare->SetValue(FromBits(0x3dcccccdu));
        flare->mesh = FindObject("Trooper_muzzle");
        emitter = NewEmitter("effect_toxicpart", defs);
        particles164 = emitter;
        emitter->SetSpriteTexture("Trooper_weaponparticle.tga", level);
        ApplyAll(*emitter, {{Op::Range40Max, {0}}, {Op::Range28, {0, 0x41200000u}}, {Op::SizeBoth, {0x3f000000u}},
                  {Op::Gravity, {0}}, {Op::Value4C, {0x40000000u}}, {Op::Range38Max, {0x3eb33333u}},
                  {Op::Range30, {0x3e4ccccdu, 0x3f19999au}}, {Op::Count, {0xau}}, {Op::FlagA8, {1}},
                  {Op::FlagDC, {1}}}, level);
        AddSound("sounds/level2/npc/igner/fire.wav", 4, ctx);   // (shared tail)
    } else if (Is("tank")) {
        flags170[0x18B - 0x170] = 1;
        flags170[0x182 - 0x170] = 1;
        flags170[0x18D - 0x170] = 0;
        flags170[0x18E - 0x170] = 0;
        value14C = FromBits(0x43c80000u);
        value144 = 2;
        value198 = FromBits(0x43480000u);
        AddSound("sounds/level2/npc/Walt_Tank/attack1-1.wav", 4, ctx);
        AddSound("sounds/level2/npc/Walt_Tank/attack2-1.wav", 5, ctx);
        AddSound("sounds/level2/npc/Walt_Tank/damage.wav", 2, ctx);
        AddSound("sounds/level2/npc/Walt_Tank/xplo-big-1.wav", 0, ctx);
        AddSound("sounds/level2/npc/walt_tank/moveloop.wav", 0x15, ctx);
        AddSound("sounds/level2/Bender/Hit-metal-1.wav", 0x17, ctx);
        AddSound("sounds/level2/Bender/Hit-metal-2.wav", 0x17, ctx);
        laser = NewLaser("TankFx_Laser", level, defs);
        weapon158 = laser;
        laser->value68[5] = FromBits(0x40000000u);
        mesh = FindNode("Tank_projectile");
        laser->model = mesh;
        laser->value80 = FromBits(0x41c80000u);
        laser->value1C = FromBits(0x43160000u);
        laser->sound58 = ctx.loadSound("sounds/level2/npc/Walt_Tank/Fireball-impact.wav", 0);
        laser->sound5C = ctx.loadSound("sounds/level2/npc/Walt_Tank/Fireball-burnloop.wav", 0);
        flare = NewFlare("TankFx_LaserNozzle", defs);
        flare160 = flare;
        flare->SetValue(FromBits(0x3ecccccdu));
        flare->mesh = FindObject("Tank_Muzzlefire");
    } else if (Is("owl")) {
        flag1A0 = 0;
        value14C = FromBits(0x43480000u);
        flags170[0x17C - 0x170] = 1;
        AddSound("sounds/level1/npc/owl/idle_1.wav", 0x10, ctx);
        AddSound("sounds/level1/npc/owl/idle_2.wav", 0x10, ctx);
        AddSound("sounds/level1/npc/owl/Hit.wav", 2, ctx);   // (shared tail)
        value150 = FromBits(0x41000000u);              // orig: the crocodile tail 0x0004CF13
        value154 = FromBits(0x41600000u);
    } else if (Is("statuehead")) {
        flag1A0 = 0;
        value144 = 0xdu;
        value14C = FromBits(0x43960000u);
        flags170[0x183 - 0x170] = 0;
        flags170[0x17C - 0x170] = 1;
        AddSound("sounds/level3/npc/sungod/fireball-throw.wav", 4, ctx);
        AddSound("sounds/level3/npc/statue/Fireball-leela-impact.wav", 0x17, ctx);
        AddSound("sounds/level3/npc/statue/Statue-turn.wav", 0x5a, ctx);
        laser = NewLaser("Statue_Laser", level, defs);
        weapon158 = laser;
        laser->sound58 = ctx.loadSound("sounds/level3/npc/statue/Fireball-other-impact.wav", 0);
        laser->sound5C = ctx.loadSound("sounds/level3/npc/sungod/Fireball-burnloop.wav", 0);
        laser->value68[5] = FromBits(0x3f800000u);
        mesh = FindNode("Lava_ball");
        laser->model = mesh;
        laser->value80 = FromBits(0x41200000u);
        laser->flag50 = 1;
        emitter = NewEmitter("effect_toxicpart", defs);
        laser->sparks = emitter;
        emitter->SetSpriteTexture("Sungod_fireparticle.tga", level);
        ApplyAll(*emitter, {{Op::Range40, {0, 0x3f000000u}}, {Op::Range28, {0, 0x41400000u}},
                  {Op::Size, {0x3f800000u, 0x40000000u}}, {Op::Gravity, {0x3ecccccdu}}, {Op::Value4C, {0x40000000u}},
                  {Op::Range38Max, {0x3f800000u}}, {Op::Range30, {0x3e4ccccdu, 0x3f19999au}},
                  {Op::RangeB4, {0x3ecccccdu, 0}}, {Op::Count, {0x14u}}, {Op::FlagA8, {1}}}, level);
    } else if (Is("high_priest")) {
        value144 = 0xdu;
        flags170[0x18B - 0x170] = 1;
        flags170[0x18E - 0x170] = 0;
        value194 = 0.0f;
        value190 = FromBits(0x42340000u);
        value14C = FromBits(0x43960000u);
        flags170[0x180 - 0x170] = 1;
        AddSound("sounds/level3/npc/high_priest/Attack1.wav", 0xa, ctx);
        AddSound("sounds/level3/npc/high_priest/Attack2.wav", 0xb, ctx);
        AddSound("Sounds/Level3/NPC/Skeleton2/Lightning-Fire.wav", 4, ctx);
        AddSound("sounds/level3/npc/high_priest/Summon.wav", 0x50, ctx);
        AddSound("sounds/level3/npc/high_priest/ChantOneshot.wav", 0x51, ctx);
        AddSound("sounds/level3/npc/high_priest/Death.wav", 0, ctx);
        AddSound("Sounds/Level3/Generic/Fall-in-lava.wav", 0x49, ctx);
        laser = NewLaser("priest_spell", level, defs);
        weapon158 = laser;
        laser->sound58 = ctx.loadSound("sounds/level3/npc/skeleton2/lightning-hit.wav", 0);
        laser->value68[0] = FromBits(0x3f800000u);
        laser->value68[2] = 0.0f;
        laser->value80 = FromBits(0x41c80000u);
        laser->value68[4] = FromBits(0x3f8ccccdu);
        laser->value68[5] = FromBits(0x40800000u);
        laser->value1C = FromBits(0x428c0000u);
        laser->value68[1] = 0.0f;
        // orig: vtbl+0x24 = LaserDef::SetTextures 0x00079A60 (beam, glow).
        laser->SetTextures(level.FindTexture("skeleton_beam.tga"), level.FindTexture("bolt_glow.tga"));
        laser->flag84 = 1;
    } else if (Is("skel_hth")) {
        value194 = 0.0f;
        value190 = 0.0f;
        value174 = FromBits(0x3fc00000u);
        value178 = FromBits(0x3fc00000u);
        value14C = FromBits(0x43960000u);
        flags170[0x181 - 0x170] = 1;
        value144 = 0xfu;
        AddSound("sounds/level3/npc/skeleton1/Flesh-1.wav", 0x17, ctx);
        AddSound("sounds/level3/npc/skeleton1/Flesh-2.wav", 0x17, ctx);
        AddSound("sounds/level3/npc/skeleton1/Impact-1.wav", 2, ctx);
        AddSound("sounds/level3/npc/skeleton1/Impact-2.wav", 2, ctx);
        AddSound("sounds/level3/npc/skeleton1/laugh.wav", 0x13, ctx);
        AddSound("sounds/level3/npc/skeleton1/laugh-2.wav", 0x13, ctx);
        AddSound("sounds/level3/npc/skeleton1/Attack-1.wav", 0xa, ctx);
        AddSound("sounds/level3/npc/skeleton1/Attack-2.wav", 0xa, ctx);
        AddSound("sounds/level3/npc/skeleton1/Attack-3.wav", 0xa, ctx);
        AddSound("Sounds/Level3/Generic/Fall-in-lava.wav", 0x49, ctx);   // (shared tail)
    } else if (Is("skel_ranged")) {
        value194 = 0.0f;
        value190 = 0.0f;
        value174 = FromBits(0x40200000u);
        value178 = FromBits(0x40200000u);
        value14C = FromBits(0x43960000u);
        flags170[0x181 - 0x170] = 1;
        value144 = 0xeu;
        value168 = 0xcu;
        AddSound("sounds/level3/npc/skeleton2/lightning-hit.wav", 0x17, ctx);
        AddSound("sounds/level3/npc/skeleton2/Take-Hit-1.wav", 2, ctx);
        AddSound("sounds/level3/npc/skeleton2/Take-Hit-2.wav", 2, ctx);
        AddSound("sounds/level3/npc/skeleton1/laugh.wav", 0x13, ctx);
        AddSound("sounds/level3/npc/skeleton1/laugh-2.wav", 0x13, ctx);
        AddSound("sounds/level3/npc/Kamikaze_Skeleton/Tick.wav", 0x67, ctx);
        AddSound("sounds/level3/npc/Kamikaze_Skeleton/Mad-loop.wav", 0x68, ctx);
        AddSound("sounds/level3/npc/Kamikaze_Skeleton/Madlaugh-loop.wav", 0x68, ctx);
        AddSound("Sounds/Level3/Generic/Fall-in-lava.wav", 0x49, ctx);
        laser = NewLaser("skeleton laser", level, defs);
        laser->sound58 = ctx.loadSound("sounds/level3/npc/skeleton2/lightning-hit.wav", 0);
        weapon158 = laser;
        laser->value68[0] = 0.0f;
        laser->value68[2] = FromBits(0x3f800000u);
        laser->value80 = FromBits(0x41200000u);
        // orig: the high_priest tail 0x0004DF34.
        laser->value68[4] = FromBits(0x3f8ccccdu);
        laser->value68[5] = FromBits(0x40800000u);
        laser->value1C = FromBits(0x428c0000u);
        laser->value68[1] = 0.0f;
        laser->SetTextures(level.FindTexture("skeleton_beam.tga"), level.FindTexture("bolt_glow.tga"));
        laser->flag84 = 1;
    } else if (Is("sungod")) {
        AddSound(Localized("sounds/localized/%s/Chars/Sungod/laugh-1.wav", ctx).c_str(), 0x17, ctx);
        AddSound(Localized("sounds/localized/%s/Chars/Sungod/laugh-2.wav", ctx).c_str(), 0x17, ctx);
        AddSound(Localized("sounds/localized/%s/Chars/Sungod/laugh-3.wav", ctx).c_str(), 0x17, ctx);
        flags170[0x18D - 0x170] = 0;
        flags170[0x18E - 0x170] = 0;
        value14C = FromBits(0x447a0000u);
        value144 = 0xdu;
        laser = NewLaser("fireball_sungod", level, defs);
        weapon158 = laser;
        laser->sound5C = ctx.loadSound("sounds/level3/npc/sungod/Fireball-burnloop.wav", 0);
        laser->sound58 = ctx.loadSound("sounds/level3/npc/sungod/Fireball-impact.wav", 0);
        laser->value68[5] = FromBits(0x40000000u);
        mesh = FindNode("Lava_ball");
        laser->model = mesh;
        laser->value80 = FromBits(0x41200000u);
        laser->flag50 = 1;
        emitter = NewEmitter("effect_toxicpart", defs);
        laser->sparks = emitter;
        emitter->SetSpriteTexture("Sungod_fireparticle.tga", level);
        ApplyAll(*emitter, {{Op::Range40Max, {0}}, {Op::Range28, {0, 0x41400000u}},
                  {Op::Size, {0x40000000u, 0x40800000u}}, {Op::Gravity, {0}}, {Op::Value4C, {0x40000000u}},
                  {Op::Range38Max, {0x3f800000u}}, {Op::Range30, {0x3e4ccccdu, 0x3f19999au}},
                  {Op::RangeB4, {0x3f4ccccdu, 0}}, {Op::Count, {0x14u}}, {Op::FlagA8, {1}}}, level);
    } else if (Is("mosquito")) {
        flags170[0x18D - 0x170] = 0;
        value14C = FromBits(0x43c80000u);
    } else if (Is("pirahna")) {
        flags170[0x18D - 0x170] = 0;
        flags170[0x188 - 0x170] = 1;
    } else if (Is("kamikaze")) {                      // orig: nothing (the loader builds a SimpleNPCDef for it)
    } else if (Is("patrol_guard")) {
        flags170[0x17C - 0x170] = 1;
        value194 = 0.0f;
        value190 = 0.0f;
        flags170[0x18D - 0x170] = 0;
        value14C = FromBits(0x43c80000u);
        flags170[0x18C - 0x170] = 1;
        AddSound("sounds/level5/npc/patrolguard/click1-2.wav", 0x6b, ctx);
        AddSound("sounds/level5/npc/patrolguard/click3.wav", 0x6c, ctx);
        AddSound("sounds/level5/npc/patrolguard_small/clickbeep.wav", 0x6d, ctx);
        AddSound("sounds/level5/npc/patrolguard/xplo.wav", 0x6e, ctx);
        AddSound("sounds/level5/npc/patrolguard_small/xplo.wav", 0x6f, ctx);
        AddSound("sounds/level5/npc/patrolguard/loop.wav", 0x6a, ctx);
        AddSound("sounds/level5/npc/patrolguard_small/loop.wav", 0x70, ctx);
        AddSound("sounds/level5/npc/patrolguard/AlarmLoop.wav", 0x71, ctx);   // (shared tail)
    } else if (Is("spinbot")) {
        flags170[0x18E - 0x170] = 0;
        flags170[0x189 - 0x170] = 1;
        value194 = 0.0f;
        value190 = 0.0f;
        flags170[0x18D - 0x170] = 1;
        value14C = FromBits(0x43c80000u);
        flags170[0x18C - 0x170] = 1;
        AddSound("sounds/level5/npc/patrolguard/xplo.wav", 0x6e, ctx);
        AddSound("sounds/level5/npc/patrolguard/loop.wav", 0x6a, ctx);
        AddSound("sounds/level5/npc/patrolguard/AlarmLoop.wav", 0x71, ctx);   // (shared tail)
    } else if (Is("destructor")) {
        flag1A0 = text::StrICmp(levelName.c_str(), "Level5-3") == 0 ? 1 : 0;   // orig: g_level +0x0C, the level name
        flags170[0x18E - 0x170] = 0;
        value19C = FromBits(0x43c80000u);
        value198 = FromBits(0x44480000u);
        value194 = 0.0f;
        value190 = 0.0f;
        flags170[0x18D - 0x170] = 0;
        value14C = FromBits(0x43c80000u);
        value144 = 0x10u;
        value168 = 0xcu;
        flags170[0x17E - 0x170] = 1;
        flags170[0x183 - 0x170] = 1;
        laser = NewLaser("Destructor_Laser", level, defs);
        weapon158 = laser;
        laser->value68[5] = FromBits(0x41200000u);
        laser->value1C = FromBits(0x43c80000u);
        mesh = FindNode("Destructor_projectile");
        laser->model = mesh;
        laser->value80 = FromBits(0x41700000u);
        flare = NewFlare("TrooperFlare", defs);
        flare160 = flare;
        flare->SetValue(FromBits(0x3dcccccdu));
        flare->mesh = FindObject("Trooper_muzzle");
        value148 = 0x11u;
        mesh = FindNode("Destructor_rocket");
        projectile = NewProjectile("Destructor_Rocket", mesh, {FromBits(0x40200000u), 1, 0.0f, FromBits(0x41200000u),
                                             0.0f, FromBits(0x41a00000u), FromBits(0x459c4000u),
                                             FromBits(0x42480000u), FromBits(0x41200000u)}, defs);
        projectile15C = projectile;
        projectile15C->sound5C = ctx.loadSound("sounds/level5/npc/destructor/missile-flyloop.wav", 0);
        projectile15C->flag50 = 1;
        AddSound("sounds/level5/npc/destructor/laser.wav", 4, ctx);
        AddSound("sounds/level5/npc/destructor/missile-launch.wav", 5, ctx);
        AddSound("sounds/level5/npc/destructor/impact-1.wav", 2, ctx);
        AddSound("sounds/level5/npc/destructor/impact-2.wav", 2, ctx);
        AddSound("sounds/level5/npc/destructor/impact-3.wav", 2, ctx);
        AddSound("sounds/level5/npc/destructor/grunt-1.wav", 2, ctx);
        AddSound("sounds/level5/npc/destructor/grunt-2.wav", 2, ctx);
        AddSound("sounds/level5/npc/destructor/BulletBounce-1.wav", 3, ctx);
        AddSound("sounds/level5/npc/destructor/BulletBounce-2.wav", 3, ctx);
        AddSound("sounds/level5/npc/destructor/BulletBounce-3.wav", 3, ctx);
        AddSound("sounds/level5/npc/destructor/Laugh.wav", 0x13, ctx);
        AddSound("sounds/level5/npc/destructor/DestAlarm.wav", 0x5c, ctx);   // (shared tail)
    } else if (Is("nibbler_1-2") || Is("nibbler_1-4") || Is("nibbler_1-6") || Is("nibbler_2") || Is("nibbler_3") ||
               Is("nibbler_4") || Is("nibbler_5")) {
        flag1A0 = 0;
        flags170[0x17C - 0x170] = 1;
        value14C = FromBits(0x43480000u);
        value19C = FromBits(0x41c80000u);
        value198 = FromBits(0x42480000u);
        AddSound("sounds/pickups/nibbler/loop_idle.wav", 0x14, ctx);
        AddSound("sounds/pickups/nibbler/loop_free.wav", 0x16, ctx);
        AddSound("sounds/pickups/nibbler/cagecrash.wav", 0, ctx);
        AddSound("sounds/pickups/nibbler/NibblerPling.wav", 0x79, ctx);
    }
}

}  // namespace fut::game
