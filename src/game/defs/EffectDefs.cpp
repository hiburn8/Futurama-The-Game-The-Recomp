// Decompiled from default.xbe (effect library 0x000BB5A0 / 0x000BF1C0, definitions 0x0004EA20-0x00073CA0); see
// EffectDefs.h.
#include "game/defs/EffectDefs.h"

#include "game/GameTime.h"
#include "game/level/LevelData.h"

#include <bit>
#include <cstring>
#include <vector>

namespace fut::game {
namespace {

using ni::detail::F;

f32 FromBits(u32 bits) {
    f32 f;
    std::memcpy(&f, &bits, 4);
    return f;
}
u32 ToBits(f32 f) {
    u32 u;
    std::memcpy(&u, &f, 4);
    return u;
}

// orig: ImpactDef vtbl+0x1C (0x0004EC90) on one controller chain.
void CollectControllers(ni::NiTimeController* c, f32& minTime, f32& maxTime, u32& frequencyBits, bool clamp) {
    for (; c; c = c->nextControllerPtr.Get()) {
        frequencyBits = ToBits(c->frequency);
        if (c->loKeyTime < minTime) minTime = c->loKeyTime;
        if (maxTime < c->hiKeyTime) maxTime = c->hiKeyTime;
        c->flags = static_cast<u16>((c->flags & 0xFFF9) | ((clamp ? 2 : 0) << 1));   // orig: SetCycleType 0x0013EBA0
    }
}

void CollectTimeRange(ni::NiAVObject* obj, f32& minTime, f32& maxTime, u32& frequencyBits, bool clamp) {   // 0x0004EC90
    if (!obj) return;
    CollectControllers(obj->controller.Get(), minTime, maxTime, frequencyBits, clamp);
    for (const auto& p : obj->properties)
        if (p) CollectControllers(p->controller.Get(), minTime, maxTime, frequencyBits, clamp);
    if (auto* node = dynamic_cast<ni::NiNode*>(obj))
        for (u32 i = 0; i < node->Children().size(); ++i)
            CollectTimeRange(node->Children()[i].Get(), minTime, maxTime, frequencyBits, clamp);
}

enum class EffectKind { Impact, Splash, Shockwave, Liquid, Ripple };
constexpr int kNoTimeRange = -1;

struct EffectSpec {
    int index;
    EffectKind kind;
    const char* name;                                  // the definition's name
    const char* source;                                // effects-root object / texture name
    u32 a, b;                                          // float bits (Impact / Splash / Shockwave), else integers
    u8 flag;
    int timeRange;                                     // ComputeTimeRange argument, or kNoTimeRange
};

// orig: the cases of 0x000BB5A0 for these classes (jump table 0x000BF02C). Generated from the disassembly
// (extracted/analysis/decomp_raw/effect_cases.txt) and verified against the original.
constexpr EffectSpec kSpecs[] = {
    {24, EffectKind::Ripple, "RippleCroc", "wave.tga", 0u, 0u, 0, kNoTimeRange},
    {25, EffectKind::Liquid, "Liquid_Lava", nullptr, 0u, 0u, 0, kNoTimeRange},
    {26, EffectKind::Liquid, "Liquid_Slime", nullptr, 1u, 0u, 0, kNoTimeRange},
    {27, EffectKind::Liquid, "Liquid_Swamp", nullptr, 4u, 0u, 0, kNoTimeRange},
    {28, EffectKind::Liquid, "Liquid_Alien", nullptr, 6u, 0u, 0, kNoTimeRange},
    {29, EffectKind::Liquid, "Liquid_Sand", nullptr, 5u, 0u, 0, kNoTimeRange},
    {30, EffectKind::Liquid, "Liquid_Oil", nullptr, 3u, 0u, 0, kNoTimeRange},
    {31, EffectKind::Liquid, "Liquid_Water", nullptr, 2u, 0u, 0, kNoTimeRange},
    {44, EffectKind::Shockwave, "Shockwave", "Highpriest_Shockwave", 0x3f800000u, 0x3f200000u, 0, kNoTimeRange},
    {45, EffectKind::Shockwave, "Shockwave_Sungod", "Sungod_shockwave", 0x3f800000u, 0x40055555u, 0, kNoTimeRange},
    {47, EffectKind::Impact, "Nibbler_Boom", "Cage_Explosion", 0x3f800000u, 0x3e31c433u, 1, 1},
    {48, EffectKind::Impact, "Smack_Impact", "Bender_leela_hit", 0x3f800000u, 0x3e31c433u, 1, 1},
    {49, EffectKind::Impact, "Shotgun_Impact", "Shotgun_impact", 0x3f800000u, 0x3eb33333u, 1, kNoTimeRange},
    {50, EffectKind::Impact, "Pistol_Impact", "Pistol_1_impact", 0x3f800000u, 0x3eb33333u, 1, kNoTimeRange},
    {51, EffectKind::Impact, "Tommygun_Impact", "Tommygun_impact", 0x3f800000u, 0x3eb33333u, 1, kNoTimeRange},
    {52, EffectKind::Impact, "Tank_Impact", "Tank_projectile_impact", 0x3f800000u, 0x3eb33333u, 1, kNoTimeRange},
    {53, EffectKind::Impact, "Igner_Impact", "Ignergun_impact", 0x3f800000u, 0x3eb33333u, 1, 1},
    {54, EffectKind::Impact, "Destructor_Impact", "Destructor_projectile_impact", 0x3f800000u, 0x3eb33333u, 1, 1},
    {55, EffectKind::Splash, "Shoot_Splash", "Shoot_Splash", 0x3f800000u, 0x3e4ccccdu, 0, kNoTimeRange},
    {56, EffectKind::Splash, "Lava_Splash", "Lava_Death_Splash", 0x3f800000u, 0x3f000000u, 0, kNoTimeRange},
    {57, EffectKind::Splash, "Water_Splash", "Water_Death_Splash_mb", 0x3f800000u, 0x3f19999au, 0, kNoTimeRange},
    {58, EffectKind::Splash, "Mud_Splash", "Swamp_Water_Death_Splash", 0x3f800000u, 0x3f19999au, 0, kNoTimeRange},
    {59, EffectKind::Splash, "Slime_Splash", "Lava_Death_Splash", 0x3f800000u, 0x3f19999au, 0, kNoTimeRange},
    {60, EffectKind::Splash, "Oil_Splash", "Water_Death_Splash", 0x3f800000u, 0x3f19999au, 0, kNoTimeRange},
    {61, EffectKind::Splash, "Quicksand_Splash", "Lava_Death_Splash", 0x3f800000u, 0x3f19999au, 0, kNoTimeRange},
    {66, EffectKind::Impact, "Bender_SpinFlame", "Swirl_Group", 0x3f800000u, 0x3f800000u, 1, 0},
    {67, EffectKind::Impact, "Bender_Spin", "Spin:Animation (Translational)", 0x3f800000u, 0x3f800000u, 1, 1},
    {68, EffectKind::Impact, "Bender_SpinAir", "Spin_air", 0x3fcccccdu, 0x3f800000u, 1, 1},
    {69, EffectKind::Impact, "Bender_Shockwave", "Eplosion_shockwave", 0x3f800000u, 0x3e555476u, 1, 1},
    {70, EffectKind::Impact, "Rotating_Gloria", "Starz", 0x3f800000u, 0x3fb00000u, 1, 1},
    {71, EffectKind::Impact, "Weasel_Wind", "Weasel_wind", 0x3ee353f8u, 0x3f2aacdau, 1, kNoTimeRange},
    {72, EffectKind::Impact, "Leela_Wind", "Leela_wind:Animation (Translational)", 0x3fb33333u, 0x3fe00000u, 1, 1},
    {76, EffectKind::Impact, "Bender_Crush_Top", "crushed_top", 0x3fb33333u, 0x3fe00000u, 1, 1},
    {77, EffectKind::Impact, "Bender_Crush_Side", "crush_side", 0x3fb33333u, 0x3fe00000u, 1, 1},
    {78, EffectKind::Impact, "Bender_Grinded", "Bender_Grinded", 0x3fb33333u, 0x3fe00000u, 1, 1},
    {79, EffectKind::Impact, "Bender_Second_Grinded", "Bender_Second_Grinded", 0x3fb33333u, 0x3fe00000u, 1, 1},
    {80, EffectKind::Impact, "Bender_Explode", "Bender_Explode", 0x3fb33333u, 0x3fe00000u, 1, 1},
    {82, EffectKind::Impact, "Rock_Effect", "Rock_Effect", 0x3fb33333u, 0x3fe00000u, 1, 1},
    {84, EffectKind::Impact, "Leela_JPL", "Punch_JPL:Animation (Translational)", 0x3f800000u, 0x3f000000u, 0, 1},
    {85, EffectKind::Impact, "Leela_SPL", "Punch_SPL:Animation (Translational)", 0x3f800000u, 0x3f000000u, 0, 1},
    {86, EffectKind::Impact, "Leela_UPL", "Punch_UPL:Animation (Translational)", 0x3f800000u, 0x3f000000u, 0, 1},
    {87, EffectKind::Impact, "Leela_JPR", "Punch_JPR:Animation (Translational)", 0x3f800000u, 0x3f000000u, 0, 1},
    {88, EffectKind::Impact, "Leela_SPR", "Punch_SPR:Animation (Translational)", 0x3f800000u, 0x3f000000u, 0, 1},
    {89, EffectKind::Impact, "Leela_UPR", "Punch_UPR:Animation (Translational)", 0x3f800000u, 0x3f000000u, 0, 1},
    {90, EffectKind::Impact, "Leela_ChargeKick", "Leela_kick_L_chargekick", 0x3f800000u, 0x3f000000u, 0, 1},
    {91, EffectKind::Impact, "Leela_ChargePunch", "Leela_punch_chargepunch:Animation (Translational)", 0x3f800000u,
     0x3f000000u, 0, 1},
    {92, EffectKind::Impact, "Leela_RunKick", "Leela_kick_runningkick:Animation (Translational)", 0x3f800000u,
     0x3f000000u, 0, 1},
    {93, EffectKind::Impact, "Leela_JumpKick", "Leela_kick_Jumpkick:Animation (Translational)", 0x3f800000u,
     0x3f000000u, 0, 1},
    {94, EffectKind::Impact, "Leela_MKL", "Leela_kick_MKL:Animation (Translational)", 0x3f800000u, 0x3f000000u, 0, 1},
    {95, EffectKind::Impact, "Leela_HKL", "Leela_kick_HKL:Animation (Translational)", 0x3f800000u, 0x3f000000u, 0, 1},
    {96, EffectKind::Impact, "Leela_MKR", "Leela_kick_MKR:Animation (Translational)", 0x3f800000u, 0x3f000000u, 0, 1},
    {97, EffectKind::Impact, "Leela_HKR", "Leela_kick_HKR:Animation (Translational)", 0x3f800000u, 0x3f000000u, 0, 1},
    {98, EffectKind::Impact, "Chicken_shield_on", "Chicken_shield_on", 0x3f800000u, 0x3f000000u, 0, 1},
    {99, EffectKind::Impact, "Chicken_shield_off", "Chicken_shield_off", 0x3f800000u, 0x3f000000u, 0, 1},
    {100, EffectKind::Impact, "Nibbler_Teleport", "Nibbler_teleport_effect", 0x3f800000u, 0x3f000000u, 1, 1},
};


// Particle emitter cases: the setter calls after the emitter's constructor, in the original's order.
enum class EmitterKind { Sprite, Mesh };
struct EmitterSpec {
    int index;
    EmitterKind kind;
    const char* name;                                  // the definition's name
    const char* texture;                               // sprite texture
    std::vector<EmitterCall> calls;
};

// orig: the sprite / mesh-piece emitter cases of 0x000BB5A0. Generated from the disassembly
// (extracted/analysis/decomp_raw/effect_cases.txt) with the shared tails expanded: sprite cases 17-22 end at
// 0x000BC917 / 0x000BCA31; mesh cases add their last mesh at 0x000BEFFB (12 and 81 first run 0x000BEFD1); cases
// 32-38 build "effect_sunpot%d" with the mesh "SunPot1_Piece" / "SunPot_%d_Piece".
const EmitterSpec kEmitterSpecs[] = {
    {0, EmitterKind::Sprite, "effect_pickupflare", "pickupflare.tga",
     {{Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Gravity, {0u}},
      {Op::Size, {0x3dcccccdu, 0x40200000u}}, {Op::RangeB4, {0x3f333333u, 0u}}, {Op::Range28, {0u, 0u}},
      {Op::Range38Max, {0u}}, {Op::Range40Max, {0u}}, {Op::Count, {0x1u}}, {Op::FlagA8, {0x1u}},
      {Op::Range30, {0x3f800000u, 0x3f800000u}}}},
    {1, EmitterKind::Sprite, "effect_smoke", "smoke.tga",
     {{Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Gravity, {0xc0c00000u}},
      {Op::Size, {0x3dcccccdu, 0x3fc00000u}}, {Op::RangeB4, {0x3f333333u, 0u}}, {Op::Range28, {0u, 0x40400000u}},
      {Op::Range38Max, {0x3f333333u}}, {Op::Range40Max, {0u}}, {Op::Count, {0x6u}}}},
    {2, EmitterKind::Sprite, "effect_smokepuff", "smoke.tga",
     {{Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Gravity, {0u}},
      {Op::Size, {0x3dcccccdu, 0x3fc00000u}}, {Op::RangeB4, {0x3f333333u, 0u}}, {Op::Range28, {0u, 0x40400000u}},
      {Op::Range38Max, {0x3f800000u}}, {Op::Range40Max, {0u}}, {Op::Count, {0x6u}}}},
    {3, EmitterKind::Sprite, "effect_sparks", "spark_blue.tga",
     {{Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Gravity, {0x41a00000u}},
      {Op::Value4C, {0x3dcccccdu}}, {Op::SizeBoth, {0x3e4ccccdu}}, {Op::RangeB4, {0x3f800000u, 0u}},
      {Op::Range28, {0x40a00000u, 0x41700000u}}, {Op::Range38Max, {0x3f4ccccdu}}, {Op::Range40, {0u, 0x3ecccccdu}},
      {Op::Count, {0x1eu}}, {Op::FlagA8, {0x1u}}, {Op::FlagDC, {0x1u}}}},
    {4, EmitterKind::Sprite, "effect_bigsmoke", "smoke.tga",
     {{Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Gravity, {0xc0c00000u}},
      {Op::Size, {0x3fc00000u, 0x40600000u}}, {Op::RangeB4, {0x3f800000u, 0u}}, {Op::Range28, {0u, 0x41100000u}},
      {Op::Range38Max, {0x3f800000u}}, {Op::Range40, {0u, 0x3f000000u}}, {Op::Count, {0x14u}},
      {Op::Range30, {0x3f800000u, 0x40400000u}}}},
    {5, EmitterKind::Sprite, "effect_fire", "fire2.tga",
     {{Op::Color, {0x3f800000u, 0x3f800000u, 0x3f333333u}}, {Op::Gravity, {0u}},
      {Op::Size, {0x3f800000u, 0x40400000u}}, {Op::RangeB4, {0x3f000000u, 0u}}, {Op::Range28, {0u, 0x40400000u}},
      {Op::Range38Max, {0x3f800000u}}, {Op::Range40Max, {0u}}, {Op::Range30, {0x3e4ccccdu, 0x3f333333u}},
      {Op::FlagA8, {0x1u}}, {Op::Count, {0xau}}}},
    {6, EmitterKind::Sprite, "effect_firetrail", "fire2.tga",
     {{Op::Color, {0x3f800000u, 0x3f800000u, 0x3f333333u}}, {Op::Gravity, {0u}},
      {Op::Size, {0x40000000u, 0x40c00000u}}, {Op::RangeB4, {0x3f000000u, 0u}}, {Op::Range28, {0u, 0x40400000u}},
      {Op::Range38Max, {0x3f800000u}}, {Op::Range40, {0u, 0x3f000000u}}, {Op::Range30, {0x3e4ccccdu, 0x3f333333u}},
      {Op::FlagA8, {0x1u}}, {Op::Count, {0x14u}}, {Op::Flag54, {0x1u}}}},
    {7, EmitterKind::Sprite, "skeleton_particles", "skel_part.tga",
     {{Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Gravity, {0x3f800000u}},
      {Op::Size, {0x3e4ccccdu, 0x3f4ccccdu}}, {Op::RangeB4, {0x3f800000u, 0u}}, {Op::Range28, {0u, 0x3e4ccccdu}},
      {Op::Range38Max, {0x40000000u}}, {Op::Range30, {0x3e4ccccdu, 0x3e4ccccdu}}, {Op::FlagA8, {0x1u}},
      {Op::Range40, {0u, 0x3f800000u}}, {Op::Count, {0x28u}}, {Op::Flag54, {0x1u}}}},
    {8, EmitterKind::Mesh, "effect_wood", nullptr,
     {{Op::Gravity, {0x41a00000u}}, {Op::Range28, {0x40e00000u, 0x41a00000u}}, {Op::Range38Max, {0x3f800000u}},
      {Op::Range40Max, {0u}}, {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x5u}},
      {Op::Mesh, {}, "Woodsplinter_1"}, {Op::Mesh, {}, "Woodsplinter_2"}, {Op::Mesh, {}, "Woodsplinter_3"},
      {Op::Mesh, {}, "Woodsplinter_4"}}},
    {9, EmitterKind::Mesh, "effect_parcel", nullptr,
     {{Op::Gravity, {0x41a00000u}}, {Op::Range28, {0x40e00000u, 0x41a00000u}}, {Op::Range38Max, {0x3f800000u}},
      {Op::Range40Max, {0u}}, {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x9u}},
      {Op::Mesh, {}, "Parcel_particle"}}},
    {10, EmitterKind::Mesh, "effect_glass", nullptr,
     {{Op::Gravity, {0x41200000u}}, {Op::Range28, {0u, 0x40a00000u}}, {Op::Range38Max, {0x3f800000u}},
      {Op::Range40Max, {0u}}, {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x5u}},
      {Op::Mesh, {}, "Glasspiece_1"}, {Op::Mesh, {}, "Glasspiece_2"}, {Op::Mesh, {}, "Glasspiece_3"},
      {Op::Mesh, {}, "Glasspiece_4"}}},
    {11, EmitterKind::Sprite, "effect_SteamEmitter", "smoke.tga",
     {{Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Gravity, {0xbf800000u}},
      {Op::Size, {0x3f000000u, 0x40200000u}}, {Op::RangeB4, {0x3f666666u, 0x3e4ccccdu}},
      {Op::Range28, {0x41700000u, 0x41a00000u}}, {Op::Range38Max, {0x3e99999au}}, {Op::Range40, {0u, 0x3f000000u}},
      {Op::Range30, {0x3e99999au, 0x3f19999au}}, {Op::Count, {0x14u}}, {Op::Flag55, {0x1u}},
      {Op::Value58, {0x3f800000u}}, {Op::Value5C, {0x40a00000u}}}},
    {12, EmitterKind::Mesh, "effect_SparkFountain", nullptr,
     {{Op::Texture, {}, "U_bluespawn.tga"}, {Op::Value60, {0x3f4ccccdu}}, {Op::Gravity, {0x435c0000u}},
      {Op::Range28Both, {0x42200000u}}, {Op::Range38, {0x3f000000u, 0x3f800000u}},
      {Op::Range40, {0x40066666u, 0x409ccccdu}}, {Op::Range30, {0x3e947ae1u, 0x3ea8f5c3u}}, {Op::Count, {0x18u}},
      {Op::Flag55, {0x1u}}, {Op::Flag54, {0x1u}}, {Op::ValueBC, {0x1u}}, {Op::Mesh, {}, "Bluespark"}}},
    {13, EmitterKind::Mesh, "effect_PigBlood", nullptr,
     {{Op::Gravity, {0x41f00000u}}, {Op::Range28, {0x41500000u, 0x41f00000u}},
      {Op::Range38, {0x3e4ccccdu, 0x3f800000u}}, {Op::Range40, {0u, 0u}}, {Op::Range30, {0x3fc66666u, 0x3fce147bu}},
      {Op::Count, {0x10u}}, {Op::Flag55, {0u}}, {Op::Flag54, {0x1u}}, {Op::ValueBC, {0x1u}},
      {Op::Mesh, {}, "Pigblood"}}},
    {17, EmitterKind::Sprite, "effect_Smoke_XXL", "smoke.tga",
     {{Op::Range28, {0x429a0000u, 0x42c80000u}}, {Op::Range30, {0x40933333u, 0x40d66666u}},
      {Op::Range38, {0u, 0x3f800000u}}, {Op::Range40, {0u, 0u}}, {Op::Value4C, {0x4129999au}}, {Op::Gravity, {0u}},
      {Op::Size, {0x3ff33333u, 0x40d00000u}}, {Op::RangeB4, {0x3f6e147bu, 0u}},
      {Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Count, {0x32u}}, {Op::FlagA8, {0u}},
      {Op::Flag55, {0u}}, {Op::Flag54, {0u}}, {Op::Value58, {0u}}, {Op::Value5C, {0u}}}},
    {18, EmitterKind::Sprite, "effect_Smoke_XL", "smoke.tga",
     {{Op::Range28, {0x41d80000u, 0x428a0000u}}, {Op::Range30, {0x40933333u, 0x40d66666u}},
      {Op::Range38, {0u, 0x3f800000u}}, {Op::Range40, {0u, 0u}}, {Op::Value4C, {0x4129999au}}, {Op::Gravity, {0u}},
      {Op::Size, {0x3ff33333u, 0x40966666u}}, {Op::RangeB4, {0x3f6e147bu, 0u}},
      {Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Count, {0x1du}}, {Op::FlagA8, {0u}},
      {Op::Flag55, {0u}}, {Op::Flag54, {0u}}, {Op::Value58, {0u}}, {Op::Value5C, {0u}}}},
    {19, EmitterKind::Sprite, "effect_Smoke_L", "smoke.tga",
     {{Op::Range28, {0x41d80000u, 0x42100000u}}, {Op::Range30, {0x40600000u, 0x40a00000u}},
      {Op::Range38, {0u, 0x3f800000u}}, {Op::Range40, {0u, 0u}}, {Op::Value4C, {0x4129999au}}, {Op::Gravity, {0u}},
      {Op::Size, {0x3ff33333u, 0x40966666u}}, {Op::RangeB4, {0x3f6e147bu, 0u}},
      {Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Count, {0xdu}}, {Op::FlagA8, {0u}},
      {Op::Flag55, {0u}}, {Op::Flag54, {0u}}, {Op::Value58, {0u}}, {Op::Value5C, {0u}}}},
    {20, EmitterKind::Sprite, "effect_Smoke_M", "smoke.tga",
     {{Op::Range28, {0x41900000u, 0x41d80000u}}, {Op::Range30, {0x40600000u, 0x40a00000u}},
      {Op::Range38, {0u, 0x3f800000u}}, {Op::Range40, {0u, 0u}}, {Op::Value4C, {0x4129999au}}, {Op::Gravity, {0u}},
      {Op::Size, {0x3ff33333u, 0x406ccccdu}}, {Op::RangeB4, {0x3f6e147bu, 0u}},
      {Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Count, {0xau}}, {Op::FlagA8, {0u}},
      {Op::Flag55, {0u}}, {Op::Flag54, {0u}}, {Op::Value58, {0u}}, {Op::Value5C, {0u}}}},
    {21, EmitterKind::Sprite, "effect_Smoke_S", "smoke.tga",
     {{Op::Range28, {0x41400000u, 0x41840000u}}, {Op::Range30, {0x3ff33333u, 0x40200000u}},
      {Op::Range38, {0u, 0x3f800000u}}, {Op::Range40, {0u, 0u}}, {Op::Value4C, {0x4129999au}}, {Op::Gravity, {0u}},
      {Op::Size, {0x3ff33333u, 0x4019999au}}, {Op::RangeB4, {0x3f6e147bu, 0u}},
      {Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Count, {0x7u}}, {Op::FlagA8, {0u}},
      {Op::Flag55, {0u}}, {Op::Flag54, {0u}}, {Op::Value58, {0u}}, {Op::Value5C, {0u}}}},
    {22, EmitterKind::Sprite, "effect_Smoke_XS", "smoke.tga",
     {{Op::Range28, {0x41400000u, 0x41840000u}}, {Op::Range30, {0x3fa66666u, 0x3fe66666u}},
      {Op::Range38, {0u, 0x3f800000u}}, {Op::Range40, {0u, 0u}}, {Op::Value4C, {0x4129999au}}, {Op::Gravity, {0u}},
      {Op::Size, {0x3f800000u, 0x3fc00000u}}, {Op::RangeB4, {0x3f6e147bu, 0u}},
      {Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Count, {0x7u}}, {Op::FlagA8, {0u}},
      {Op::Flag55, {0u}}, {Op::Flag54, {0u}}, {Op::Value58, {0u}}, {Op::Value5C, {0u}}}},
    {23, EmitterKind::Sprite, "effect_fart", "smoke.tga",
     {{Op::Range28, {0x41400000u, 0x41840000u}}, {Op::Range30, {0x3dcccccdu, 0x3f000000u}},
      {Op::Range38, {0u, 0x3f800000u}}, {Op::Range40, {0u, 0u}}, {Op::Value4C, {0x4129999au}},
      {Op::Gravity, {0xbf000000u}}, {Op::Size, {0x3f800000u, 0x3fc00000u}}, {Op::RangeB4, {0x3f6e147bu, 0u}},
      {Op::Color, {0x3ef00000u, 0x3ec80000u, 0x3e5bf488u}}, {Op::Count, {0x4u}}, {Op::FlagA8, {0u}},
      {Op::Flag55, {0u}}, {Op::Flag54, {0u}}, {Op::Value58, {0u}}, {Op::Value5C, {0u}}}},
    {32, EmitterKind::Mesh, "effect_sunpot1", nullptr,
     {{Op::Gravity, {0x41a00000u}}, {Op::Range28, {0x40e00000u, 0x41a00000u}}, {Op::Range38Max, {0x3f800000u}},
      {Op::Range40Max, {0u}}, {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x5u}},
      {Op::Mesh, {}, "SunPot1_Piece"}}},
    {33, EmitterKind::Mesh, "effect_sunpot2", nullptr,
     {{Op::Gravity, {0x41a00000u}}, {Op::Range28, {0x40e00000u, 0x41a00000u}}, {Op::Range38Max, {0x3f800000u}},
      {Op::Range40Max, {0u}}, {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x5u}},
      {Op::Mesh, {}, "SunPot_2_Piece"}}},
    {34, EmitterKind::Mesh, "effect_sunpot3", nullptr,
     {{Op::Gravity, {0x41a00000u}}, {Op::Range28, {0x40e00000u, 0x41a00000u}}, {Op::Range38Max, {0x3f800000u}},
      {Op::Range40Max, {0u}}, {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x5u}},
      {Op::Mesh, {}, "SunPot_3_Piece"}}},
    {35, EmitterKind::Mesh, "effect_sunpot4", nullptr,
     {{Op::Gravity, {0x41a00000u}}, {Op::Range28, {0x40e00000u, 0x41a00000u}}, {Op::Range38Max, {0x3f800000u}},
      {Op::Range40Max, {0u}}, {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x5u}},
      {Op::Mesh, {}, "SunPot_4_Piece"}}},
    {36, EmitterKind::Mesh, "effect_sunpot5", nullptr,
     {{Op::Gravity, {0x41a00000u}}, {Op::Range28, {0x40e00000u, 0x41a00000u}}, {Op::Range38Max, {0x3f800000u}},
      {Op::Range40Max, {0u}}, {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x5u}},
      {Op::Mesh, {}, "SunPot_5_Piece"}}},
    {37, EmitterKind::Mesh, "effect_sunpot6", nullptr,
     {{Op::Gravity, {0x41a00000u}}, {Op::Range28, {0x40e00000u, 0x41a00000u}}, {Op::Range38Max, {0x3f800000u}},
      {Op::Range40Max, {0u}}, {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x5u}},
      {Op::Mesh, {}, "SunPot_6_Piece"}}},
    {38, EmitterKind::Mesh, "effect_sunpot7", nullptr,
     {{Op::Gravity, {0x41a00000u}}, {Op::Range28, {0x40e00000u, 0x41a00000u}}, {Op::Range38Max, {0x3f800000u}},
      {Op::Range40Max, {0u}}, {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x5u}},
      {Op::Mesh, {}, "SunPot_7_Piece"}}},
    {39, EmitterKind::Mesh, "effect_glass_bottle", nullptr,
     {{Op::Gravity, {0x41200000u}}, {Op::Range28, {0u, 0x40a00000u}}, {Op::Range38Max, {0x3f800000u}},
      {Op::Range40Max, {0u}}, {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x8u}},
      {Op::Mesh, {}, "Glassbottlesplinter_1"}}},
    {42, EmitterKind::Sprite, "effect_summon", "fire2.tga",
     {{Op::Color, {0x3f800000u, 0x3f800000u, 0x3f800000u}}, {Op::Gravity, {0xbfc00000u}},
      {Op::Size, {0x3f000000u, 0x3f99999au}}, {Op::RangeB4, {0x3f800000u, 0u}}, {Op::Range28, {0u, 0x3e4ccccdu}},
      {Op::Range38Max, {0x40000000u}}, {Op::Range30, {0x3f000000u, 0x3f000000u}}, {Op::FlagA8, {0x1u}},
      {Op::Range40, {0u, 0x40200000u}}, {Op::Count, {0x4bu}}, {Op::Flag54, {0x1u}}}},
    {43, EmitterKind::Sprite, "effect_warp", "fire2.tga",
     {{Op::Color, {0u, 0u, 0x3f800000u}}, {Op::Gravity, {0xbfc00000u}}, {Op::Size, {0x3f000000u, 0x3f99999au}},
      {Op::RangeB4, {0x3f800000u, 0u}}, {Op::Range28, {0u, 0x3e4ccccdu}}, {Op::Range38Max, {0x40000000u}},
      {Op::Range30, {0x3f000000u, 0x3f000000u}}, {Op::FlagA8, {0x1u}}, {Op::Range40, {0u, 0x40200000u}},
      {Op::Count, {0x4bu}}, {Op::Flag54, {0x1u}}}},
    {46, EmitterKind::Sprite, "Punch_Particles", "spark.tga",
     {{Op::Range40Max, {0u}}, {Op::Range28, {0x3f800000u, 0x41a00000u}}, {Op::SizeBoth, {0x3e4ccccdu}},
      {Op::Gravity, {0x41200000u}}, {Op::Value4C, {0x40000000u}}, {Op::Range38Max, {0x3f800000u}},
      {Op::Range30, {0u, 0x3e99999au}}, {Op::Count, {0x50u}}}},
    {62, EmitterKind::Mesh, "effect_generic", nullptr,
     {{Op::Gravity, {0x41a00000u}}, {Op::Range28, {0x40400000u, 0x41200000u}},
      {Op::RangeC4, {0x3e99999au, 0x3f19999au}}, {Op::Range38Max, {0x3f800000u}}, {Op::Range40Max, {0u}},
      {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x2u}}, {Op::Mesh, {}, "Generic_particle"}}},
    {63, EmitterKind::Mesh, "effect_metal", nullptr,
     {{Op::Gravity, {0x41a00000u}}, {Op::Range28, {0x40400000u, 0x41200000u}},
      {Op::RangeC4, {0x3e99999au, 0x3f19999au}}, {Op::Range38Max, {0x3f800000u}}, {Op::Range40Max, {0u}},
      {Op::Range30, {0x40000000u, 0x40000000u}}, {Op::Count, {0x2u}}, {Op::Mesh, {}, "Metal_particle"}}},
    {64, EmitterKind::Mesh, "effect_scrap", nullptr,
     {{Op::Gravity, {0x41a00000u}}, {Op::Range28, {0x41200000u, 0x41a00000u}},
      {Op::RangeC4, {0x3f800000u, 0x3f800000u}}, {Op::Range38Max, {0x3f800000u}}, {Op::Range40Max, {0u}},
      {Op::Range30, {0x40c00000u, 0x40c00000u}}, {Op::Count, {0xau}}, {Op::Mesh, {}, "Bolt"},
      {Op::Mesh, {}, "Spring"}, {Op::Mesh, {}, "Nut"}}},
    {65, EmitterKind::Mesh, "effect_pigparts", nullptr,
     {{Op::Gravity, {0x42200000u}}, {Op::Range28, {0x41200000u, 0x42480000u}},
      {Op::RangeC4, {0x3f800000u, 0x3f800000u}}, {Op::Range38Max, {0x3f800000u}}, {Op::Range40Max, {0u}},
      {Op::Range30, {0x40c00000u, 0x40c00000u}}, {Op::Count, {0xeu}}, {Op::Mesh, {}, "pig_2"},
      {Op::Mesh, {}, "pig_3"}, {Op::Mesh, {}, "pig_4"}, {Op::Mesh, {}, "pig_5"}, {Op::Mesh, {}, "pig_6"},
      {Op::Mesh, {}, "pig_7"}, {Op::Mesh, {}, "pig_8"}, {Op::Mesh, {}, "pig_9"}, {Op::Mesh, {}, "pig_10"},
      {Op::Mesh, {}, "pig_11"}, {Op::Mesh, {}, "pig_12"}, {Op::Mesh, {}, "pig_13"}, {Op::Mesh, {}, "pig_14"},
      {Op::Mesh, {}, "pig_15"}, {Op::FlagCC, {0u}}}},
    {81, EmitterKind::Mesh, "effect_PowerCordSparks", nullptr,
     {{Op::Texture, {}, "U_bluespawn.tga"}, {Op::Value60, {0x3f4ccccdu}}, {Op::Gravity, {0x435c0000u}},
      {Op::Range28Both, {0x42200000u}}, {Op::Range38, {0x3e4ccccdu, 0x3f19999au}},
      {Op::Range40, {0x40066666u, 0x409ccccdu}}, {Op::Range30, {0x3e947ae1u, 0x3ea8f5c3u}}, {Op::Count, {0x10u}},
      {Op::Flag55, {0x1u}}, {Op::Flag54, {0x1u}}, {Op::ValueBC, {0x1u}}, {Op::Mesh, {}, "Bluespark"}}},
};

}  // namespace

void ApplyEmitterCall(EmitterDef& d, const EmitterCall& c, const LevelData& level) {
    const auto f = [&](int i) { return FromBits(c.args[i]); };
    auto* sprite = dynamic_cast<SpriteEmitterDef*>(&d);
    auto* mesh = dynamic_cast<MeshEmitterDef*>(&d);
    switch (c.op) {
        case Op::Range28: d.SetRange28(f(0), f(1)); break;                     // orig: vtbl+0x28
        case Op::Range28Both: d.SetRange28(f(0), f(0)); break;                 // orig: vtbl+0x2C
        case Op::Range30: d.SetRange30(f(0), f(1)); break;                     // orig: vtbl+0x30
        case Op::Range38: d.SetRange38(f(0), f(1)); break;                     // orig: vtbl+0x38
        case Op::Range38Max: d.SetRange38(0.0f, f(0)); break;                  // orig: vtbl+0x3C
        case Op::Range40: d.SetRange40(f(0), f(1)); break;                     // orig: vtbl+0x40
        case Op::Range40Max: d.SetRange40(0.0f, f(0)); break;                  // orig: vtbl+0x44
        case Op::Count: d.SetCount(c.args[0]); break;                          // orig: vtbl+0x48
        case Op::Value4C: d.SetValue4C(f(0)); break;                           // orig: vtbl+0x4C
        case Op::Gravity: d.SetGravity(f(0)); break;                           // orig: vtbl+0x50
        case Op::Flag54: d.SetFlag54(static_cast<u8>(c.args[0])); break;      // orig: vtbl+0x54
        case Op::Flag55: d.SetFlag55(static_cast<u8>(c.args[0])); break;      // orig: vtbl+0x58
        case Op::Value58: d.SetValue58(f(0)); break;                           // orig: vtbl+0x5C
        case Op::Value5C: d.SetValue5C(f(0)); break;                           // orig: vtbl+0x60
        case Op::Value60: d.SetValue60(f(0)); break;                           // orig: 0x0006CEA0
        case Op::Texture: d.SetTexture(c.text, level); break;                  // orig: 0x0006F140
        case Op::FlagA8: sprite->SetFlagA8(static_cast<u8>(c.args[0])); break;   // orig: vtbl+0xAC
        case Op::Size: sprite->SetSize(f(0), f(1)); break;                     // orig: vtbl+0xB0
        case Op::SizeBoth: sprite->SetSize(f(0), f(0)); break;                 // orig: vtbl+0xB4
        case Op::RangeB4: sprite->SetRangeB4(f(0), f(1)); break;               // orig: vtbl+0xB8
        case Op::Color: sprite->SetColor(f(0), f(1), f(2)); break;             // orig: vtbl+0xC4
        case Op::FlagDC: sprite->SetFlagDC(static_cast<u8>(c.args[0])); break;   // orig: vtbl+0xC8
        case Op::Mesh: mesh->AddMesh(level.FindEffectObject(c.text)); break;   // orig: 0x000C2AF0 + vtbl+0xA4
        case Op::ValueBC: mesh->SetValueBC(c.args[0]); break;                  // orig: vtbl+0xAC
        case Op::RangeC4: mesh->SetRangeC4(f(0), f(1)); break;                 // orig: vtbl+0xB4
        case Op::FlagCC: mesh->SetFlagCC(static_cast<u8>(c.args[0])); break;  // orig: vtbl+0xB8
    }
}

ni::NiPointer<ni::NiTexturingProperty> MakeTexturing(ni::NiObjectNET* texture) {
    ni::NiPointer<ni::NiTexturingProperty> property(new ni::NiTexturingProperty);
    property->applyMode = 2;
    property->maps.resize(7);
    property->present.assign(7, false);
    ni::NiTexturingProperty::Map map;
    map.textureObj = texture;
    map.clampMode = 3;
    map.filterMode = 1;
    map.ps2K = 0xFFB5;
    property->SetMap(0, map);
    return property;
}

const std::array<const char*, 101> kEffectNames = {
    "effect_pickupflare", "effect_smoke", "effect_smokepuff", "effect_sparks", "effect_bigsmoke", "effect_fire",
    "effect_firetrail", "skeleton_particles", "effect_wood", "effect_parcel", "effect_glass", "effect_SteamEmitter",
    "effect_SparkFountain", "effect_PigBlood", "effect_BonFire", "effect_SungodHairFire", "effect_FireCircle",
    "effect_Smoke_XXL", "effect_Smoke_XL", "effect_Smoke_L", "effect_Smoke_M", "effect_Smoke_S", "effect_Smoke_XS",
    "effect_fart", "RippleCroc", "Liquid_Lava", "Liquid_Slime", "Liquid_Swamp", "Liquid_Alien", "Liquid_Sand",
    "Liquid_Oil", "Liquid_Water", "effect_sunpot1", "effect_sunpot2", "effect_sunpot3", "effect_sunpot4",
    "effect_sunpot5", "effect_sunpot6", "effect_sunpot7", "effect_glass_bottle", "fireball", "qwerty_laser",
    "spell_summon", "spell_warp", "Shockwave", "Shockwave_Sungod", "Punch_Particles", "Nibbler_Bang", "Smack_Impact",
    "Shotgun_Impact", "Pistol_Impact", "Tommygun_Impact", "Tank_Impact", "Igner_Impact", "Destructor_Impact",
    "Shoot_Splash", "Lava_Splash", "Water_Splash", "Mud_Splash", "Slime_Splash", "Oil_Splash", "Quicksand_Splash",
    "effect_generic", "effect_metal", "effect_scrap", "effect_pigparts", "Bender_SpinFlame", "Bender_Spin",
    "Bender_SpinAir", "Bender_Shockwave", "Rotating_Gloria", "Weasel_wind", "Leela_wind", "RechargeEff",
    "RespawnEffect", "ChargeUpEffect", "Bender_Crush_Top", "Bender_Crush_Side", "Bender_Grinded",
    "Bender_Second_Grinded", "Bender_Explode", "effect_PowerCordSparks", "Rock_Effect", "RainFeathers", "Leela_JPL",
    "Leela_SPL", "Leela_UPL", "Leela_JPR", "Leela_SPR", "Leela_UPR", "Leela_ChargeKick", "Leela_ChargePunch",
    "Leela_RunKick", "Leela_JumpKick", "Leela_MKL", "Leela_HKL", "Leela_MKR", "Leela_HKR", "chicken_shield_on",
    "chicken_shield_off", "effect_Teleport",
};

void ImpactDef::ComputeTimeRange(u32 clampBits) {     // orig: 0x0004EA20
    f32 minTime = 3.4028234663852886e38f, maxTime = 0.0f;   // orig: 0x7F7FFFFF / 0
    u32 slot = clampBits;                              // the argument's own stack slot
    CollectTimeRange(mesh.Get(), minTime, maxTime, slot, (clampBits & 0xFF) != 0);
    if (minTime == 3.4028234663852886e38f) minTime = 0.0f;
    unknown24 = 0;
    timeScale = F(static_cast<double>(FromBits(slot)) * timeScale);
    duration = F(static_cast<double>(maxTime) - minTime);
}

void EmitterDef::SetTexture(const char* textureName, const LevelData& level) {   // orig: 0x0006F140
    if (ni::NiObjectNET* texture = level.FindTexture(textureName)) texturing = MakeTexturing(texture);
}

void SpriteEmitterDef::SetSpriteTexture(const char* textureName, const LevelData& level) {   // orig: 0x0006E430
    spriteTexturing = MakeTexturing(level.FindTexture(textureName));   // orig: 0x0006E3F0 (vtbl+0xA8)
}

void FireDef::Init(const LevelData& level) {          // orig: ctor 0x00069460
    flames.count = 0x30;
    smoke.count = 0x20;
    for (Layer* layer : {&flames, &smoke}) layer->value1C = layer->value20 = g_frameStep;

    // orig: 0x00067F70.
    quad = new ni::NiTriShapeData;                     // orig: ctor 0x00115D80 (NiGeometryData 0x00143910)
    quad->vertexCount = 4;
    quad->vertices = {{-1.5f, -1.5f, 0.0f}, {1.5f, -1.5f, 0.0f}, {1.5f, 1.5f, 0.0f}, {-1.5f, 1.5f, 0.0f}};
    quad->bound = ni::BoundOfPoints(quad->vertices);
    quad->textureSets = 1;
    quad->uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    quad->triangleCount = 2;
    quad->triangleList = {0, 1, 2, 0, 2, 3};
    // A missing texture leaves no texturing property (the original creates one and releases it again).
    if (ni::NiObjectNET* texture = level.FindTexture("Fire4.tga")) flameTexturing = MakeTexturing(texture);
    if (ni::NiObjectNET* texture = level.FindTexture("Smoke.tga")) smokeTexturing = MakeTexturing(texture);
    alpha80 = new ni::NiAlphaProperty;                 // orig: flags 0xEC, then |1, &~0x200, &~0x100|0xE0, &~0x12|0xC
    alpha80->flags = 0xED;
    alpha7C = new ni::NiAlphaProperty;                 // orig: flags 0xEC, then |1, &~0x200, &~0x1E0, &~0x12|0xC
    alpha7C->flags = 0x0D;
    vertexColor = new ni::NiVertexColorProperty;       // orig: ctor (0, 1), then source mode 0, lighting mode 0
    zBuffer = new ni::NiZBufferProperty;               // orig: flags 3, test function 3, then |1, &~2
    zBuffer->flags = 1;
    zBuffer->testFunction = 3;
}

void FireCircleDef::InitCircle(const LevelData& level) {   // orig: ctor 0x00069AA0
    Init(level);
    flames.range0C = {1.0f, 0.0f};
    flames.range14 = {FromBits(0x3f19999au), 0.0f};
    flames.value1C = F(static_cast<double>(g_frameStep) * 6.0);
    flames.value20 = F(static_cast<double>(g_frameStep) * static_cast<double>(1.8f));
    flames.value24 = FromBits(0x41490fdbu);            // 4 pi
    smoke.count = 0;
}

void ProjectileDef::Construct(ni::NiNode* projectileModel, const Args& a) {   // orig: 0x0007BCB0
    model = projectileModel;
    value1C = a.value1C;
    value20 = a.value20;
    value24 = a.value24;
    value28 = a.value28;
    flag2C = a.flag2C;
    value38 = a.value38;
    value3C = F(static_cast<double>(a.value4C) * static_cast<double>(0.3f));
    value40 = a.value40;
    value44 = F(static_cast<double>(a.value4C) * static_cast<double>(0.6f));
    value48 = a.value48;
    value4C = a.value4C;
}

void LaserDef::SetTextures(ni::NiObjectNET* beam, ni::NiObjectNET* glow) {   // orig: 0x00079A60
    beamTexturing = beam ? MakeTexturing(beam) : ni::NiPointer<ni::NiTexturingProperty>();
    glowTexturing = glow ? MakeTexturing(glow) : ni::NiPointer<ni::NiTexturingProperty>();
}

void LaserDef::Construct(const LevelData& level, std::vector<std::unique_ptr<Def>>& defs) {   // orig: 0x00079BF0
    Args a;
    a.value1C = 100.0f;
    a.value28 = 100000.0f;
    a.value4C = 100.0f;
    a.value38 = 100.0f;
    a.value40 = 50.0f;
    a.value48 = 25.0f;
    ProjectileDef::Construct(nullptr, a);
    SetTextures(level.FindTexture("las_beam.tga"), level.FindTexture("las_glow.tga"));
    auto spark = std::make_unique<SpriteEmitterDef>();
    spark->name = "Something";
    spark->SetSpriteTexture("spark.tga", level);
    static const EmitterCall kSparkCalls[] = {
        {Op::Range40Max, {0u}}, {Op::Range28, {0u, 0x42480000u}}, {Op::SizeBoth, {0x3e4ccccdu}},
        {Op::Gravity, {0x41f00000u}}, {Op::Value4C, {0x40000000u}}, {Op::Range38Max, {0x3e99999au}},
        {Op::Range30, {0u, 0x3ecccccdu}}, {Op::Count, {4u}}, {Op::FlagDC, {1u}}, {Op::FlagA8, {1u}}};
    for (const EmitterCall& c : kSparkCalls) ApplyEmitterCall(*spark, c, level);
    sparks = spark.get();
    defs.push_back(std::move(spark));
    value64 = 0;
}

EffectLibrary::Result EffectLibrary::Create(const char* name, LevelData& level,
                                             std::vector<std::unique_ptr<Def>>& out) {
    int index = -1;                                    // orig: NiTStringMap lookup (strcmp)
    for (size_t i = 0; i < kEffectNames.size(); ++i)
        if (std::strcmp(kEffectNames[i], name) == 0) index = static_cast<int>(i);
    if (index < 0) return Result::Unknown;
    // orig: x87 products of the frame step with float constants, stored as floats.
    const auto step = [](f32 k) { return F(static_cast<double>(g_frameStep) * static_cast<double>(k)); };
    switch (index) {
        case 14: {                                     // orig: FireDef "effect_BonFire"
            auto d = std::make_unique<FireDef>();
            d->name = "effect_BonFire";
            d->Init(level);
            d->flames.count = 0x18;                                         // orig: 0x000CA2B0
            d->flames.range04 = {0.75f, 1.25f};                             // orig: 0x00067860
            d->flames.range0C = {1.0f, 0.0f};                               // orig: 0x00067880
            d->flames.range14 = {FromBits(0x3f19999au), 0.0f};              // orig: 0x000678A0
            d->flames.value1C = step(6.0f);                                 // orig: 0x000C9710
            d->flames.value20 = step(1.8f);                                 // orig: 0x000678C0
            d->flames.value24 = step(FromBits(0x41490fdbu));                // orig: 0x000678D0 (4 pi)
            d->smoke.count = 0x10;                                          // orig: 0x000678E0
            d->smoke.range04 = {5.0f, 7.0f};                                // orig: 0x000678F0
            d->smoke.range0C = {FromBits(0x3ecccccdu), 2.0f};               // orig: 0x00067910
            d->smoke.range14 = {FromBits(0x3f19999au), 0.0f};               // orig: 0x0006CA90
            d->smoke.value1C = F(static_cast<double>(g_frameStep) + g_frameStep);   // orig: 0x00067930
            d->smoke.value20 = g_frameStep;                                 // orig: 0x000C9FC0
            d->smoke.value24 = step(FromBits(0x40c90fdbu));                 // orig: 0x00067940 (2 pi)
            out.push_back(std::move(d));
            return Result::Created;
        }
        case 15: {                                     // orig: FireDef "effect_SungodHairFire"
            auto d = std::make_unique<FireDef>();
            d->name = "effect_SungodHairFire";
            d->Init(level);
            d->flames.count = 0x18;
            d->flames.range04 = {FromBits(0x3f266666u), FromBits(0x3f666666u)};
            d->flames.range0C = {1.0f, FromBits(0x3e99999au)};
            d->flames.range14 = {FromBits(0x3f19999au), 0.0f};
            d->flames.value1C = FromBits(0x3d75c28fu);
            d->flames.value20 = FromBits(0x3db851ecu);
            d->flames.value24 = step(FromBits(0x41490fdbu));
            d->smoke.count = 0;
            out.push_back(std::move(d));
            return Result::Created;
        }
        case 16: {                                     // orig: FireCircleDef "effect_FireCircle"
            auto d = std::make_unique<FireCircleDef>();
            d->name = "effect_FireCircle";
            d->InitCircle(level);
            d->flames.range0C = {1.0f, 0.0f};
            d->flames.range14 = {FromBits(0x3f19999au), 0.0f};
            d->flames.value1C = step(6.0f);
            d->flames.value20 = step(1.8f);
            d->flames.value24 = FromBits(0x41490fdbu);
            d->value94 = 3.0f;                                              // orig: 0x00067950
            d->value98 = 1.0f;                                              // orig: 0x00067970
            d->value9C = 8;                                                 // orig: 0x00067990
            out.push_back(std::move(d));
            return Result::Created;
        }
        case 40: {                                     // orig: LaserDef "fireball" with a new tail emitter
            auto owned = std::make_unique<LaserDef>();
            LaserDef* d = owned.get();
            d->name = "fireball";
            out.push_back(std::move(owned));
            d->Construct(level, out);
            d->value68[5] = 2.0f;                      // orig: +0x7C
            d->model = dynamic_cast<ni::NiNode*>(level.FindEffectObject("Lava_ball"));   // orig: NiNode DynamicCast
            d->value80 = 50.0f;
            // orig: +0xA4 is overwritten (the "Something" emitter stays in the definition list).
            auto tail = std::make_unique<SpriteEmitterDef>();
            tail->name = "effect_fireball_tail";
            tail->SetSpriteTexture("Sungod_fireparticle.tga", level);
            static const EmitterCall kTailCalls[] = {
                {Op::Range40Max, {0u}}, {Op::Range28, {0u, 0x41400000u}}, {Op::Size, {0x40000000u, 0x40800000u}},
                {Op::Gravity, {0u}}, {Op::Value4C, {0x40000000u}}, {Op::Range38Max, {0x3f800000u}},
                {Op::Range30, {0x3e4ccccdu, 0x3f19999au}}, {Op::RangeB4, {0x3f4ccccdu, 0u}}, {Op::Count, {0x14u}},
                {Op::FlagA8, {1u}}};
            for (const EmitterCall& c : kTailCalls) ApplyEmitterCall(*tail, c, level);
            d->sparks = tail.get();
            out.push_back(std::move(tail));
            return Result::Created;
        }
        case 41: {                                     // orig: two LaserDefs "qwerty_laser"; only the second is set up
            for (int i = 0; i < 2; ++i) {
                auto owned = std::make_unique<LaserDef>();
                LaserDef* d = owned.get();
                d->name = "qwerty_laser";
                out.push_back(std::move(owned));
                d->Construct(level, out);
                if (i == 0) continue;
                d->value68[0] = 1.0f;                  // orig: +0x68
                d->value68[1] = 0.0f;
                d->value68[2] = FromBits(0x3e99999au);
                d->value80 = 20.0f;
                d->value68[4] = FromBits(0x3f666666u);  // orig: +0x78
                d->value68[5] = 2.0f;
                d->value98 = 200.0f;
            }
            return Result::Created;
        }
        case 73: {                                     // orig: RechargeEffectDef ctor 0x00070710
            auto d = std::make_unique<RechargeEffectDef>();
            d->name = "RechargeEff";
            static constexpr const char* kTextures[] = {"Health_25.tga", "Health_50.tga", "Health_100.tga",
                                                        "Ammo_25.tga", "Ammo_50.tga", "Ammo_100.tga"};
            for (size_t i = 0; i < d->textures.size(); ++i) d->textures[i] = level.FindTexture(kTextures[i]);
            out.push_back(std::move(d));
            return Result::Created;
        }
        case 74: {                                     // orig: RespawnEffectDef ctor 0x000709E0
            auto d = std::make_unique<RespawnEffectDef>();
            d->name = "RespawnEffect";
            d->texture = level.FindTexture("respawn.tga");
            out.push_back(std::move(d));
            return Result::Created;
        }
        case 75: {                                     // orig: ChargeUpEffectDef ctor 0x00065F50
            auto d = std::make_unique<ChargeUpEffectDef>();
            d->name = "ChargeUpEffect";
            d->texture = level.FindTexture("chrg_fx.tga");
            d->texture2 = level.FindTexture("respawn.tga");
            out.push_back(std::move(d));
            return Result::Created;
        }
        default:
            break;
    }
    for (const EmitterSpec& e : kEmitterSpecs) {
        if (e.index != index) continue;
        std::unique_ptr<EmitterDef> d;
        if (e.kind == EmitterKind::Sprite) {
            auto sprite = std::make_unique<SpriteEmitterDef>();   // orig: ctor 0x0006E090
            sprite->SetSpriteTexture(e.texture, level);
            d = std::move(sprite);
        } else {
            d = std::make_unique<MeshEmitterDef>();    // orig: ctor 0x0006F390
        }
        d->name = e.name;
        for (const EmitterCall& c : e.calls) ApplyEmitterCall(*d, c, level);
        EmitterDef* emitter = d.get();
        out.push_back(std::move(d));
        if (index == 42 || index == 43) {              // orig: + SpellDef(name, emitter, 1, emitter, 2), 0x000731B0
            auto spell = std::make_unique<SpellDef>();
            spell->name = index == 42 ? "spell_summon" : "spell_warp";
            spell->emitter28 = spell->emitter2C = emitter;
            spell->value30 = 1;
            spell->value34 = 2;
            out.push_back(std::move(spell));
        }
        return Result::Created;
    }
    const EffectSpec* spec = nullptr;
    for (const EffectSpec& s : kSpecs)
        if (s.index == index) spec = &s;
    if (!spec) return Result::NotPorted;

    auto meshNode = [&]() { return dynamic_cast<ni::NiNode*>(level.FindEffectObject(spec->source)); };
    switch (spec->kind) {
        case EffectKind::Impact: {                     // orig: ImpactDef ctor 0x0004EB20
            ni::NiNode* node = meshNode();
            if (!node) return Result::NotCreated;
            auto d = std::make_unique<ImpactDef>();
            d->name = spec->name;
            d->mesh = node;
            d->timeScale = FromBits(spec->a);
            d->duration = FromBits(spec->b);
            d->flag = spec->flag;
            if (spec->timeRange != kNoTimeRange) d->ComputeTimeRange(static_cast<u32>(spec->timeRange));
            out.push_back(std::move(d));
            break;
        }
        case EffectKind::Splash: {                     // orig: SplashDef ctor 0x00073740
            ni::NiNode* node = meshNode();
            if (!node) return Result::NotCreated;
            auto d = std::make_unique<SplashDef>();
            d->name = spec->name;
            d->mesh = node;
            d->value1C = FromBits(spec->a);
            d->value20 = FromBits(spec->b);
            out.push_back(std::move(d));
            break;
        }
        case EffectKind::Shockwave: {                  // orig: ShockwaveDef ctor 0x0005F1F0
            ni::NiNode* node = meshNode();
            if (!node) return Result::NotCreated;
            auto d = std::make_unique<ShockwaveDef>();
            d->name = spec->name;
            d->mesh = node;
            d->value1C = FromBits(spec->a);
            d->value20 = FromBits(spec->b);
            out.push_back(std::move(d));
            break;
        }
        case EffectKind::Liquid: {                     // orig: LiquidDef ctor 0x0006A580
            auto d = std::make_unique<LiquidDef>();
            d->name = spec->name;
            d->liquid = spec->a;
            out.push_back(std::move(d));
            break;
        }
        case EffectKind::Ripple: {                     // orig: RippleDef ctor 0x00071710
            auto d = std::make_unique<RippleDef>();
            d->name = spec->name;
            d->texture = level.FindTexture(spec->source);
            d->value1C = std::bit_cast<f32>(spec->a);   // (0 for RippleCroc)
            out.push_back(std::move(d));
            break;
        }
    }
    return Result::Created;
}

}  // namespace fut::game
