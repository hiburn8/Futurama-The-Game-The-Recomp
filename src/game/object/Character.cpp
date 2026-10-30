// Characters; see Character.h.
#include "game/object/Character.h"
#include "game/object/Pickup.h"

#include "core/CrtString.h"
#include "engine/ni/NiClone.h"
#include "engine/ni/NiCull.h"
#include "engine/ni/NiKeyframeManager.h"
#include "engine/ni/NiRenderState.h"
#include "engine/ni/NiRenderer.h"
#include "game/defs/CharacterDef.h"
#include "game/defs/IngameEvent.h"
#include "game/GameTime.h"
#include "game/level/Camera.h"
#include "game/level/Dialogue.h"
#include "game/level/Hud.h"
#include "game/level/Intermission.h"
#include "game/level/LevelData.h"
#include "game/level/LightManager.h"
#include "game/object/Cannon.h"
#include "game/object/ChickenWalker.h"
#include "game/object/GenericNpc.h"
#include "game/object/NodeUpdate.h"
#include "game/object/RatRide.h"

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>

namespace fut::game {

using ni::detail::F;

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// orig: 0x00034980 thiscall(character, object): every object of the subtree, pre-order, as its own root (the
// character is unused).
void FlagPass(ni::NiAVObject* o) {
    bool selective = true, rigid = false;
    o->SetSelectiveUpdateFlags(selective, false, rigid);   // orig: vf58
    if (auto* n = dynamic_cast<ni::NiNode*>(o))
        for (size_t i = 0; i < n->Children().size(); ++i)   // (the size is re-read every step)
            if (ni::NiAVObject* c = n->Children()[i].Get()) FlagPass(c);
}

// The player classes PlayerDef::Spawn and the Player constructor choose by definition name (_stricmp 0x001684D1).
bool IsFryName(const char* n) {
    static constexpr const char* kNames[] = {"fry",     "railgunfry", "fry-1-1", "fry-1-2", "fry-1-3",
                                             "fry-1-4", "fry-1-5",    "fry-1-6", "fry-5-3"};
    for (const char* k : kNames)
        if (crt::StrICmp(n, k) == 0) return true;
    return false;
}

// orig: the AddSound (0x000403F0) lists of PlayerDef::Spawn 0x0005E6B0 per player class, in call order (each list's
// last call goes through the shared tail 0x0005EF80).
struct SpawnSound {
    const char* path;
    u32 id;
};
constexpr SpawnSound kFrySounds[] = {   // 27 calls
    {"Sounds/Weapons/Hammer swing 1.wav", 0x3C},
    {"Sounds/Weapons/Hammer swing 2.wav", 0x3C},
    {"Sounds/Weapons/Hit-glass.wav", 0x1F},
    {"Sounds/Weapons/Hit-hard-1.wav", 0x20},
    {"Sounds/Weapons/Hit-hard-2.wav", 0x20},
    {"Sounds/Weapons/Hit-metal-1.wav", 0x21},
    {"Sounds/Weapons/Hit-metal-2.wav", 0x21},
    {"Sounds/Weapons/Hit-wood-1.wav", 0x23},
    {"Sounds/Weapons/Hit-wood-2.wav", 0x23},
    {"Sounds/Weapons/Hit-bulletproof.wav", 0x1E},
    {"Sounds/Level1/Fry/Fry-grab.wav", 0x45},
    {"Sounds/Level1/Fry/Fry-jump-land.wav", 0x32},
    {"Sounds/Level1/Fry/Fry-jump-up.wav", 0x33},
    {"Sounds/Level1/Fry/Fry-climb.wav", 0x46},
    {"Sounds/Level1/Fry/Fry-die-land.wav", 0x4E},
    {"Sounds/Level1/Fry/Fry-electrified_loop.wav", 0x4D},
    {"Sounds/Weapons/Change-Weapon.wav", 0x47},
    {"Sounds/Pickups/Pickup_Ammo.wav", 0x72},
    {"Sounds/Pickups/Pickup_Ammo_Big.wav", 0x73},
    {"Sounds/Pickups/Pickup_Weapon.wav", 0x74},
    {"Sounds/Pickups/Pickup_collectible.wav", 0x75},
    {"Sounds/Pickups/Pickup_money.wav", 0x76},
    {"Sounds/Pickups/Pickup_slurm.wav", 0x77},
    {"Sounds/Pickups/Pickup_slurm_big.wav", 0x78},
    {"Sounds/Pickups/Pickup_ChargeUp.wav", 0x7A},
    {"Sounds/Pickups/Pickup_ExtraLife.wav", 0x7B},
    {"sounds/level1/sewers/watersplash.wav", 0x4A},
};
constexpr SpawnSound kLeelaSounds[] = {   // 31 calls
    {"Sounds/Level3/LEELA/Kick-rock.wav", 0x29},
    {"Sounds/Level3/LEELA/Kick-metal.wav", 0x2A},
    {"Sounds/Level3/LEELA/Kick-skeleton.wav", 0x2C},
    {"Sounds/Level3/LEELA/Kick-humaniod.wav", 0x2B},
    {"Sounds/Level3/LEELA/Punch-rock.wav", 0x25},
    {"Sounds/Level3/LEELA/Punch-metal-1.wav", 0x26},
    {"Sounds/Level3/LEELA/Punch-skeleton.wav", 0x28},
    {"Sounds/Level3/LEELA/Punch-humanoid-1.wav", 0x27},
    {"Sounds/Level3/LEELA/Punch-humanoid-2.wav", 0x27},
    {"Sounds/Level3/LEELA/Sw-Kick.wav", 0x37},
    {"Sounds/Level3/LEELA/Sw-Kick-Charge.wav", 0x38},
    {"Sounds/Level3/LEELA/Sw-Punch.wav", 0x39},
    {"Sounds/Level3/LEELA/Sw-Punch-Charge.wav", 0x3A},
    {"Sounds/Level3/LEELA/Sw-Roundhouse.wav", 0x3B},
    {"Sounds/Level3/LEELA/Sw-BackFlip.wav", 0x55},
    {"Sounds/Level3/LEELA/Sw-Roll.wav", 0x56},
    {"Sounds/Level3/LEELA/Sw-BackFlipKick.wav", 0x57},
    {"Sounds/Level3/LEELA/Leela-grab.wav", 0x45},
    {"Sounds/Level3/LEELA/Leela-jump-land.wav", 0x32},
    {"Sounds/Level3/LEELA/Leela-jump-up.wav", 0x33},
    {"Sounds/Level3/LEELA/Leela-climb.wav", 0x46},
    {"Sounds/Level3/LEELA/Leela-electrified_loop.wav", 0x4D},
    {"Sounds/Level3/LEELA/Leela-die-land.wav", 0x4E},
    {"Sounds/Pickups/Pickup_collectible.wav", 0x75},
    {"Sounds/Pickups/Pickup_goldbar.wav", 0x76},
    {"Sounds/Pickups/Pickup_pills.wav", 0x77},
    {"Sounds/Pickups/Pickup_pills_big.wav", 0x78},
    {"Sounds/Pickups/Pickup_ChargeUp.wav", 0x7A},
    {"Sounds/Pickups/Pickup_ExtraLife.wav", 0x7B},
    {"Sounds/Level3/Generic/Fall-in-lava.wav", 0x49},
    {"sounds/level3/leela/Blades-impact.wav", 0x7F},
};
constexpr SpawnSound kBenderSounds[] = {   // 43 calls
    {"Sounds/Level2/Bender/Charge-Attack.wav", 0x3D},
    {"Sounds/Level2/Bender/Flame-Start.wav", 0x3E},
    {"Sounds/Level2/Bender/Flame-Loop.wav", 0x3F},
    {"Sounds/Level2/Bender/Flame-End.wav", 0x40},
    {"Sounds/Level2/Bender/Flame-BackSpin.wav", 0x41},
    {"Sounds/Level2/Bender/Spin-Attack.wav", 0x42},
    {"Sounds/Level2/Bender/Start-Arse-Slam.wav", 0x44},
    {"Sounds/Level2/Bender/Arse-slam.wav", 0x43},
    {"Sounds/Level2/Bender/Hit-Humaniod-1.wav", 0x24},
    {"Sounds/Level2/Bender/Hit-Humaniod-2.wav", 0x24},
    {"Sounds/Level2/Bender/Hit-Humaniod-3.wav", 0x24},
    {"Sounds/Level2/Bender/Hit-Humaniod-4.wav", 0x24},
    {"Sounds/Level2/Bender/Bender-grab.wav", 0x45},
    {"Sounds/Level2/Bender/Bender-jump-land.wav", 0x32},
    {"Sounds/Level2/Bender/Bender-jump-up.wav", 0x33},
    {"Sounds/Level2/Bender/Bender-climb.wav", 0x46},
    {"Sounds/Level2/Bender/BenderSquashed.wav", 0x66},
    {"Sounds/Level2/Bender/Grinderloop-noisy.wav", 0x4C},
    {"Sounds/Level2/Bender/Bender-electrified_loop.wav", 0x4D},
    {"Sounds/Level2/Bender/Bender-die-land.wav", 0x4E},
    {"Sounds/Level2/Bender/Hit-Metal-1.wav", 0x21},
    {"Sounds/Level2/Bender/Hit-Metal-2.wav", 0x21},
    {"Sounds/Level2/Bender/Hit-Rock-1.wav", 0x22},
    {"Sounds/Level2/Bender/Hit-Rock-2.wav", 0x22},
    {"Sounds/Level2/Bender/Foot-Gravel-1.wav", 0x2D},
    {"Sounds/Level2/Bender/Foot-Gravel-2.wav", 0x2D},
    {"Sounds/Level2/Bender/Foot-Gravel-3.wav", 0x2D},
    {"Sounds/Level2/Bender/Foot-Gravel-4.wav", 0x2D},
    {"Sounds/Level2/Bender/Foot-Metal-1.wav", 0x2E},
    {"Sounds/Level2/Bender/Foot-Metal-2.wav", 0x2E},
    {"Sounds/Level2/Bender/Foot-Metal-3.wav", 0x2E},
    {"Sounds/Level2/Bender/Foot-Metal-4.wav", 0x2E},
    {"Sounds/Level2/Bender/Foot-Wood-1.wav", 0x31},
    {"Sounds/Level2/Bender/Foot-Wood-2.wav", 0x31},
    {"Sounds/Level2/Bender/Foot-Wood-3.wav", 0x31},
    {"Sounds/Level2/Bender/Foot-Wood-4.wav", 0x31},
    {"Sounds/Pickups/Pickup_collectible.wav", 0x75},
    {"Sounds/Pickups/Pickup_gems.wav", 0x76},
    {"Sounds/Pickups/Pickup_beer.wav", 0x77},
    {"Sounds/Pickups/Pickup_beer_big.wav", 0x78},
    {"Sounds/Pickups/Pickup_ChargeUp.wav", 0x7A},
    {"Sounds/Pickups/Pickup_ExtraLife.wav", 0x7B},
    {"Sounds/Level2/Generic/quicksand_obj_sink.wav", 0x4B},
};
constexpr SpawnSound kRatSounds[] = {   // 13 calls
    {"Sounds/Level4/ZoidbergOnRat/Jump.wav", 0x33},
    {"Sounds/Level4/ZoidbergOnRat/Breathe-loop.wav", 0x14},
    {"Sounds/Level4/ZoidbergOnRat/Noise-1.wav", 0x10},
    {"Sounds/Level4/ZoidbergOnRat/Crashing.wav", 0x00},
    {"Sounds/Level4/ZoidbergOnRat/Crash-Log.wav", 0x62},
    {"Sounds/Level4/ZoidbergOnRat/Crash-Root.wav", 0x63},
    {"Sounds/Level4/ZoidbergOnRat/Crash-Thorn.wav", 0x64},
    {"Sounds/Level4/ZoidbergOnRat/Crash-Land.wav", 0x65},
    {"Sounds/Level4/ZoidbergOnRat/Falling.wav", 0x69},
    {"Sounds/Level4/ZoidbergOnRat/Leaves-Bush.wav", 0x53},
    {"Sounds/Level4/ZoidbergOnRat/Leaves-Tree.wav", 0x52},
    {"Sounds/Pickups/Pickup_money.wav", 0x76},
    {"Sounds/Level4/ZoidbergOnRat/Splash.wav", 0x4A},
};

template <class T>
void Unlink(std::vector<T*>& list, T* item) {
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == item) {
            list.erase(list.begin() + i);
            return;
        }
}

}  // namespace

// ---- PhysicController ---------------------------------------------------------------------------------------------
PhysicController::PhysicController(Character& c) : character(&c) {   // orig: 0x00037730
    flags = 0x0C;                                     // orig: NiTimeController ctor 0x00140FD0
    loKeyTime = FLT_MAX;
    hiKeyTime = -FLT_MAX;
    SetActive(false);                                 // orig: 0x0013EBD0 -> 0x04
    computeScaledFirst = false;                       // orig: byte +0x34 = 0
}

void PhysicController::Update(f32 t) {                // orig: 0x000377B0
    if (character && !SkipUpdate(t) && IsActive()) {
        if (!bone) return;
        const f32 y = keepY ? bone->local.translate.y : saved.y;
        bone->local.translate = {saved.x, y, saved.z};
        applied = 1;
        return;
    }
    applied = 0;
}

void PhysicController::SetBone(const char* boneName) {   // orig: 0x00037820
    bone = GetObjectByName(character->GetNode(), boneName);   // orig: node vf54 (no reference)
    saved = bone ? bone->local.translate : ni::NiPoint3{};
}

// ---- Character ----------------------------------------------------------------------------------------------------
Character::Character(LevelData& lvl, CharacterDef& d) : GameObject(lvl, d), cdef(d) {   // orig: 0x0003D220
    // NOT PORTED: the inventory Owner (+0x38), the lip-sync player (+0x40), the shadow / animation-state fields.
    lvl.characters.push_back(this);                   // orig: append to S+0x90 (link +0x76C)
    char buf[1024];
    std::snprintf(buf, sizeof buf, "character::%s", d.name.c_str());   // orig: sprintf
    node = new ni::NiNode;                            // (replaces the GameObject's node)
    // HEAP-MIRROR: +0xB2 is heap garbage in the original; 0 here (won't do: KNOWN_ISSUES "Heap layout")
    node->name = buf;
    health = health4F8 = maxHealth = d.maxHealth;     // orig: +0x4F4 / +0x4F8 / +0x4FC = D+0x60
    heightOffset = d.boundCenter;                     // orig: +0x4D0 = D+0x9C
    boundHeight = d.boundHeight;                      // orig: +0x4D4 = D+0x94
    lvl.playerNoDamage694 = 0;                        // orig: byte DAT_002DD694 = 0 (0x0003D5C0)
}

IngameEventContext Character::IngameContext() const {
    IngameEventContext ctx;
    ctx.level = &level;
    ctx.text = level.text;
    ctx.fileExists = level.fileExists;
    ctx.soundDuration = level.soundDuration;
    if (const Intermission* im = level.intermission) {   // (the App clock, the one the intermission reads)
        ctx.queryCounter = im->queryCounter;
        ctx.counterFrequency = im->counterFrequency;
    }
    return ctx;
}

void Character::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x000354B0 (docs/analysis/combat_ai_port.md 9.6)
    if (attacker != this) (void)def->IsType(2);       // (the result is not used)
    if (!(amount > 0.0f)) return;                     // TEST AH,0x41; JNP (returns on <, ==; NaN goes on)
    if (std::bit_cast<u32>(health) == 0x47590300u && !(amount == 55555.0f)) {   // [0x00295594]; TEST AH,0x44; JNP
        if (ambientLight) {                           // (no null test in the original) the "immortal" green flash
            ambientLight->dimmer = 1.0f;              // +0xE0
            ++ambientLight->revision;                 // +0xCC
            ambientLight->ambient = {0.0f, 1.0f, 0.0f};   // +0xE4 / +0xE8 / +0xEC (L: the ambient colour)
            ++ambientLight->revision;
        }
        return;
    }
    if (!level.IsCharacter(attacker)) attacker = nullptr;   // 0x00033EE0
    if (!(health > 0.0f)) return;                     // TEST AH,0x41; JNE
    if (ambientLight) {                               // +0x30: the red hit flash
        ambientLight->dimmer = 1.0f;
        ++ambientLight->revision;
        ambientLight->ambient = {1.0f, 0.0f, 0.0f};
        ++ambientLight->revision;
    }
    const double h = double(health) - amount;         // FLD; FSUB; FST (compared unrounded)
    health = F(h);
    const IngameEventContext ctx = IngameContext();
    if (h > 0.0) {                                    // TEST AH,0x41; JNE
        cdef.ingameEvents.Trigger(0, {attacker, nullptr}, ctx);   // D+0x28
    } else {                                          // orig: 0x0003560D
        killer = attacker;                            // +0x68
        if (!def->IsType(2) && body) body->b1A = 1;   // body +0x5C (null test: a crash guard)
        if (!def->IsType(2) && def->IsType(0x200000)) {   // (IsType(2) runs a second time at 0x0003562C)
            cdef.ingameEvents.Trigger(1, {attacker, nullptr}, ctx);   // D+0x28 0x000F28F0
            level.AddKilled(static_cast<u16>(id));    // S->0x000C7790(+0x20)
        }
    }
    if (attacker && attacker->def->IsType(1) && attacker != this) {
        IngameEventArg ev{this, nullptr};             // 0x000984D0 / 0x000D8150
        if (attacker->def->IsType(2))                 // 0x000356D4: a player's weapon goes to +4 (0x000EE950)
            if (auto* pl = dynamic_cast<Player*>(attacker); pl && pl->weapon && pl->weapon->def->IsType(8))
                ev.object = pl->weapon;
        attacker->cdef.ingameEvents.Trigger(health > 0.0f ? 2 : 3, ev, ctx);
    }
    if (health < 0.0f) health = 0.0f;                 // TEST AH,5; JP
    if (!(health > 0.0f)) level.SetObjFlags(id, level.GetObjFlags(id) | 0x10u);   // TEST AH,0x41; JP
}

Character::~Character() {                             // orig: 0x0003D810
    StopAnimation();                                  // 0x0003D85E: 0x00039D90
    inventory.DeleteObjects();                        // 0x0003D865..0x0003D8D9: each object (its node detached) vf00(1)
    Unlink(level.characters, this);                   // +0x76C / +0x770 (head S+0x90)
    if (physic) {                                     // +0x560: out of the node's chain, released
        node->RemoveController(physic.Get());         // 0x000FE4C0
        physic->character = nullptr;
        physic = nullptr;
    }
    if (aim55C) {                                     // +0x55C
        node->RemoveController(aim55C.Get());
        aim55C = nullptr;
    }
    if (shadowEffect) {                               // 0x0003D9A1
        shadowEffect->DetachAllAffectedNodes();       // 0x00141A30
        if (ni::NiNode* p = shadowEffect->parent) p->DetachChild(shadowEffect.Get());   // vfA4 (released)
        shadowEffect = nullptr;
    }
    // 0x0003DAA2: the base UnloadModel (not the virtual): the model back under the definition's model root, the
    // sequences out of the manager (RemoveAllSequences clears their owner: the next character that takes the
    // definition's own sequences can add them to its manager)
    Character::UnloadModel();                         // 0x0003B7D0
    if (keyframes) {                                  // +0x558: out of the node's chain, released, then Update(0, 1)
        node->RemoveController(keyframes.Get());      // 0x000FE4C0
        keyframes = nullptr;
        UpdateNodeAt(node.Get(), 0.0f);               // 0x001018D0
    }
    // (L) the rest (agent +0x58, body +0x5C, the shadow's texture / camera +0x5A8..+0x5B0, SetOwner(0) 0x000967E0)
    // follows in the members' and GameObject's destructors.
}

void Character::SetPosition(const ni::NiPoint3& p) {  // orig: 0x000353B0
    const f32 off = type == 5 ? cdef.boundCenter : heightOffset;
    const ni::NiPoint3 q{p.x, F(double(p.y) + off), p.z};   // orig: body vf04 (FLD; FADD; FSTP)
    if (body) body->SetPosition(q);
    else bodyPosition = q;
}

void Character::SetRotation(const ni::NiMatrix3& m) { rotation = m; }   // orig: 0x00033D20

void Character::Translate(const ni::NiPoint3& d) {    // orig: 0x00035400
    const double inv = 1.0 / double(g_frameStep);     // FLD 1.0 [0x00293A98]; FDIV [0x002D67E8]
    body->AddVelocity({F(inv * d.x), F(inv * d.y), F(inv * d.z)});   // body vf50 0x000AF7D0
}

ni::NiPoint3 Character::GetWorldPosition() const {    // orig: 0x00035440
    const f32 off = type == 5 ? cdef.boundCenter : heightOffset;
    const ni::NiPoint3 b = body ? body->GetPosition() : bodyPosition;   // orig: body vf08
    return {b.x, F(double(b.y) - off), b.z};
}

void Character::AddSequence(ni::NiSequence* seq) {    // orig: 0x0003DCE0
    if (!seq || !keyframes) return;
    for (ni::NiSequence* s : sequences)
        if (s && std::strcmp(s->name.c_str(), seq->name.c_str()) == 0) return;
    // orig: g_cloneNameMode = 1; the definition's own sequence when only the definition holds it (the first
    // instance, outside the definition's stream callback), else a clone (0x0012F050).
    ni::NiPointer<ni::NiSequence> s = seq->RefCount() == 1 ? ni::NiPointer<ni::NiSequence>(seq) : ni::CloneSequence(*seq);
    sequenceRefs.push_back(s);
    sequences.push_back(s.Get());                     // orig: name map SetAt 0x00037B40, array Add
    keyframes->AddSequence(s.Get(), true);            // orig: 0x0012FFB0
    byAnimation.fill(nullptr);
    for (ni::NiSequence* q : sequences)
        for (u32 a = 0; a < kAnimationCount; ++a)
            if (q && crt::StrICmp(q->name.c_str(), AnimationName(a)) == 0) {
                byAnimation[a] = q;
                break;
            }
}

void Character::AddDefSequences() {                   // orig: 0x0003DEA0
    byAnimation.fill(nullptr);
    sequences.reserve(cdef.animations.size());        // orig: (+0x52C).SetSize(D+0xCC)
    for (const auto& a : cdef.animations)
        if (a) AddSequence(a->sequence.Get());
}

void Character::RenderShadow(bool draw) {             // orig: 0x0003B960
    if (!model || !shadowCamera || !draw) {
        if (shadowEffect) shadowEffect->switchOn = false;   // 0x0003C05E: +0x5A0 +0xC0 = 0
        return;
    }
    if (shadowEffect) shadowEffect->switchOn = true;  // +0x5A0 +0xC0 = 1
    if (envEffect) envEffect->switchOn = false;       // +0x5A4 +0xC0 = 0: the environment map is off for the pass
    const ni::NiPoint3 p = GetWorldPosition();        // vf34
    ni::NiCamera& cam = *shadowCamera;
    cam.local.translate = {p.x, F(double(p.y) + 1000.0f), p.z};   // +0x60 ([0x00295D64] = 1000.0f)
    cam.UpdateDownwardPass(0.0f, true);               // Update(0, 1)
    NiCameraLookAt(cam, p, {1.0f, 0.0f, 0.0f});       // 0x000FFF40(p, DAT_002D77E0 = (1, 0, 0))
    cam.UpdateDownwardPass(0.0f, true);
    double v = 1.05f;                                 // [0x00295D5C]
    if (shadowTexture) {
        // the node's bound radius over the distance to the camera, times [0x00295D60] = 1.55f (x87: unrounded)
        const ni::NiPoint3& E = cam.world.translate;  // +0xA0
        const double dz = double(p.z) - E.z, dy = double(p.y) - E.y, dx = double(p.x) - E.x;
        v = double(node->worldBound.radius) / std::sqrt((dx * dx + dy * dy) + dz * dz) * 1.55f;
    }
    f32 f[6];
    std::memcpy(f, cam.frustum, sizeof f);            // +0x134 (6 dwords)
    f[0] = F(-v), f[1] = F(v), f[2] = F(v), f[3] = F(-v);
    cam.SetFrustum(f);                                // 0x00011960
    if (shadowTexture) {
        ni::NiRenderer* r = level.renderer;           // App->vf14() (AddRef; the original does not test it)
        if (r) {
            r->SetRenderTarget(shadowTexture.Get());  // vf3C
            ni::NiAccumulator* saved = r->GetAccumulator();   // renderer +8 (AddRef'd)
            const ni::NiPoint3 fog = level.camera ? level.camera->fogColor : ni::NiPoint3{};   // (S+0x8C) 0x00034500
            r->SetBackgroundColor({1.0f, 1.0f, 1.0f});   // vf6C
            cam.renderer = r;                         // 0x00013D40 (+0x1A8)
            r->ClearBuffer(cam.port, 5);              // 0x000FEF20(5): the colour and depth of the target
            cam.port[0] = 0.02f, cam.port[1] = 0.98f;   // 0x000119E0 (0x3CA3D70A, 0x3F7AE148)
            cam.port[2] = 0.98f, cam.port[3] = 0.02f;
            r->SetAccumulator(shadowAccumulator.get());   // 0x00153490(+0x5B4)
            const u32 outlineMode = ni::g_outlineMode;   // DAT_002D7B58: 0 for the Click (0x0003BBD3), restored after
            ni::g_outlineMode = 0;
            ni::VisitForCulling(cam, node.Get());     // +0x15C = the node (0x00013D00); Click 0x00100CE0
            ni::g_outlineMode = outlineMode;
            r->DisplayFrame();                        // 0x000FEF50: vf40 (a rendered texture is not presented)
            r->SetAccumulator(saved);                 // 0x00153490
            r->SetRenderTarget(nullptr);              // vf3C(0): the back buffer
            r->SetBackgroundColor({fog.x, fog.y, fog.z});   // vf6C
            // (+0x15C released)
        }
    }
    // 0x0003BC7C: the effect's model projection: the texture centred under the shadow camera, k / radius wide
    // (renderer_lighting_port.md 5.2; the x87 order kept: the products of two floats are exact in the 53-bit
    // register, q and the sums are rounded where stored)
    const ni::NiCamera& c = *shadowCamera;
    const double rk = double(node->worldBound.radius) * (shadowTexture ? 3.1f : 0.5f);   // [0x00295D58] / [0x00293AA0]
    const f32 nuz = F(-double(c.worldUp.z)), nuy = F(-double(c.worldUp.y)), nux = F(-double(c.worldUp.x));   // FCHS
    const double q = 1.0 / rk;                        // FDIVR 1.0f (kept in the register)
    const f32 s = F(q);                               // FST
    const double e2 = double(s) * nuz, e1 = double(s) * nuy, e0 = double(s) * nux;
    const double f2 = q * c.worldRight.z;             // (the unrounded q)
    const double f1 = double(s) * c.worldRight.y, f0 = double(s) * c.worldRight.x;
    const ni::NiPoint3& C = c.world.translate;        // +0xA0
    const f32 tA = F(((f0 * C.x + f1 * C.y) + f2 * C.z) - 0.5);   // (f0 also stored: FST)
    const f32 tB = F(((e0 * C.x + e1 * C.y) + e2 * C.z) - 0.5);
    ni::NiTextureEffect& fx = *shadowEffect;
    const ni::NiPoint3& E = fx.world.translate;       // +0xA0
    const f32 dz = F(double(p.z) - E.z), dy = F(double(p.y) - E.y), dx = F(double(p.x) - E.x);
    // +0xE0 (the original's 16-byte chunk k = the port's column k): rows s R, -s U and DAT_002F915C.. (L: the zero
    // vector, BSS)
    fx.modelProjection.m[0][0] = F(f0), fx.modelProjection.m[0][1] = F(f1), fx.modelProjection.m[0][2] = F(f2);
    fx.modelProjection.m[1][0] = F(e0), fx.modelProjection.m[1][1] = F(e1), fx.modelProjection.m[1][2] = F(e2);
    fx.modelProjection.m[2][0] = 0.0f, fx.modelProjection.m[2][1] = 0.0f, fx.modelProjection.m[2][2] = 0.0f;
    fx.modelProjectionTranslate = {F(-double(tA)), F(-double(tB)), 0.0f};   // +0x110
    fx.clippingPlaneEnabled = true;                   // +0x170
    const ni::NiPoint3& D = c.worldDir;               // +0x110
    const f32 k = F(((double(dy) * D.y + double(dz) * D.z) + double(dx) * D.x) - 1.0);   // FSUB 1.0f
    fx.SetModelPlane({D, k});                         // 0x00141DF0, 0x000344C0
    fx.UpdateDownwardPass(0.0f, true);                // Update(0, 1)
    if (envEffect) envEffect->switchOn = true;        // 0x0003BE96: +0x5A4 +0xC0 = 1
    // 0x0003BEA7: the character's node leaves its parent while the room's effects are updated
    ni::NiNode* parent = node->parent;                // +0x14
    if (parent) parent->DetachChild(node.Get());      // vfA4 (the returned pointer released)
    const ni::NiPoint3 at = GetWorldPosition();       // vf34
    const ni::NiPoint3 probe{at.x, F(double(at.y) + 1.0f), at.z};
    ni::NiRoom* room = level.roomGroup ? level.roomGroup->FindRoomContaining(probe) : nullptr;   // S+0x20 0x0011F1D0
    if (room != shadowRoom) {
        if (shadowRoom) {                             // the affected nodes from a copy (0x000342E0), each updated
            const std::vector<ni::NiNode*> nodes = fx.affectedNodes;
            for (ni::NiNode* n : nodes) {
                if (!n) continue;
                fx.DetachAffectedNode(n);             // 0x001419F0
                n->UpdateEffects();                   // 0x00102190
            }
        }
        if (room) {
            fx.AttachAffectedNode(room);              // 0x001419B0
            fx.DetachAffectedNode(node.Get());        // 0x001419F0
            room->UpdateEffects();
        }
        shadowRoom = room;                            // +0x780
    }
    if (parent) parent->AttachChild(node.Get(), true);   // vfA0(node, 1)
}

bool MasterPropertyAccumulator::RegisterObject(ni::NiGeometry& geometry) {   // orig: 0x0003B080
    ni::NiRenderer* r = camera ? camera->renderer : nullptr;   // (+8)+0x1A8
    if (!r) return false;
    // 0x00037A70(renderer +0xC, +0xC): each listed property exchanged with the state's slot of its type (the state
    // is the geometry's own: shared, changed in place), and back after the draw
    auto swap = [this](ni::NiPropertyState* ps) {
        if (!ps) return;
        for (auto& p : properties) {
            if (!p) break;
            const u32 t = ni::PropertyType(*p);
            if (t >= ni::NiPropertyState::kCount) continue;
            ni::NiPointer<ni::NiProperty> old(ps->slot[t].Get());
            ps->slot[t] = p.Get();
            p = old.Get();
        }
    };
    ni::NiPropertyState* ps = r->propertyState;
    swap(ps);
    r->SetAccumulator(nullptr);                       // renderer +8 released and cleared
    ni::NiDynamicEffectState* effects = r->effectState;   // +0x10 saved and cleared (the display sets it again)
    r->effectState = nullptr;
    if (auto* shape = dynamic_cast<ni::NiTriShape*>(&geometry)) r->DisplayTriShape(*shape, *camera);   // vf84
    else NotPorted("MasterPropertyAccumulator: the display of a geometry class other than NiTriShape");
    r->effectState = effects;
    r->SetAccumulator(this);                          // +8 = this
    swap(ps);
    return true;
}

void Character::BuildModel() {                        // orig: 0x0003DF70
    if (model) return;
    model = cdef.AcquireModel(modelVariant);          // orig: 0x00040450
    if (model) {
        model->flags &= ~ni::kNiAVHidden;
        node->AttachChild(model.Get(), true);         // orig: vfA0
        if (cdef.envEffect) {
            // (unreachable: the stream callback releases D+0x118; NiClone does not copy texture effects)
            envEffect = dynamic_cast<ni::NiTextureEffect*>(ni::Clone(cdef.envEffect.Get()).Get());
        } else {
            envEffect = level.envTextureEffect;       // orig: S+0xE4 (0x00034250)
        }
        if (envEffect) {
            if (auto* m = dynamic_cast<ni::NiNode*>(model.Get())) envEffect->AttachAffectedNode(m);   // 0x001419B0
            envEffect->UpdateProperties();            // 0x0003E074 (the effect's: it is no geometry's parent)
            envEffect->UpdateEffects();
            envEffect->UpdateDownwardPass(0.0f, true);   // orig: Update(0, 1): revision + 1
            envEffect->switchOn = false;
        }
        if (sequences.empty()) AddDefSequences();     // orig: +0x538 == 0
        node->UpdateProperties();                     // 0x0003E0A8
        node->UpdateEffects();
        node->UpdateDownwardPass(0.0f, true);
    }
    if (flag60) AddModelCollisionMeshes();            // 0x0003E0CA: the BSP collision meshes (0 for the players)
    if (body) body->SetStepHeight(cdef.stepHeight);  // orig: body vf38(D+0xAC)
    // orig: 0x0003E0E1, the animation UnloadModel saved ("none" at first: no sequence of that name).
    if (ni::NiSequence* s = FindSequence(animName.c_str())) {   // 0x00039BA0 (called twice by the original)
        PlayAnimation(s, true, true, true, 0.0f);     // push 0; 1; 1; 1 -> 0x0003A130
        if (curSeq) curSeq->offset = animOffset;      // seq+0xA8 = +0x668
    }
    if (model) {
        ni::NiAVObject* t = GetObjectByName(node.Get(), "tip");   // orig: 0x00295D98 / 0x00295EC8
        if (!t) t = GetObjectByName(node.Get(), "Tip");
        if (t) tips.push_back(dynamic_cast<ni::NiNode*>(t));   // orig: 0x0003CFF0
    }
    FlagPass(node.Get());                             // orig: 0x00034980
}

bool Character::Init() {                              // orig: 0x00038070
    if (stream::Manager* m = stream::Manager::Current()) m->Note("INIT " + cdef.name);
    if (model) return true;
    CreateAmbientLight();                             // orig: vf44
    keyframes = new ni::NiKeyframeManager(node.Get());   // orig: 0x00130D20 (+0x4C = 0)
    keyframes->cumulativeAnimations = false;
    if (ni::NiPointer<ni::NiUDSFileObject> face = cdef.lipSyncFile) {   // orig: 0x00034530
        BuildModel();                                 // orig: vf64
        (void)lip.SetUp(face.Get(), model.Get());     // orig: (+0x40)->0x0009D880(file, model) (result unused)
        cdef.lipSyncFile = nullptr;
    }
    if (ni::NiPointer<ni::NiUDSFileObject> ev = cdef.ingameEventFile) {   // orig: 0x00034550
        const IngameEventContext ctx = IngameContext();
        // orig: (D+0x28)->0x000F2A80(ev, IsType(2) ? this+0x40 (the lip-sync player) : 0)
        if (ctx.text) cdef.ingameEvents.Parse(&ev->data, def->IsType(2) ? &this->lip : nullptr, ctx);
        cdef.ingameEventFile = nullptr;
    }
    level.AttachObject(*this);                        // orig: S->vf04 (detach, first free slot again)
    if (def->IsType(2)) {
        BuildModel();
        physic = new PhysicController(*this);         // orig: 0x00037730
        ni::NiTimeController* c = node->controller.Get();   // appended at the end of the chain (0x0013EDD0)
        if (c) {
            while (c->nextControllerPtr) c = c->nextControllerPtr.Get();
            c->nextControllerPtr = physic.Get();
        } else {
            node->controller = physic.Get();
        }
    }
    if (def->IsType(2)) {                             // orig: 0x000382D2: the projected shadow
        shadowEffect = new ni::NiTextureEffect;       // +0x5A0 = new(0x1A0) 0x00119F00 (an effect id)
        shadowTexture = ni::CreateRenderedTexture(128, 128, level.renderer);   // +0x5AC / +0x5A8 released first
        shadowEffect->sourceTextureObj = shadowTexture.Get();   // 0x00013D00 (+0x15C)
        shadowEffect->textureFiltering = 1;           // +0x160
        shadowEffect->coordGenType = 0;               // +0x16C
        shadowEffect->UpdateProjection();             // 0x0011A290
        shadowEffect->textureType = 1;                // +0x168 (a projected shadow)
        shadowEffect->textureClamping = 0;            // +0x164
        shadowAccumulator = std::make_unique<MasterPropertyAccumulator>();   // new(0x14) 0x00038000
        auto& props = shadowAccumulator->properties;  // (AddHead 0x00013A50 each: the last one first)
        auto* vc = new ni::NiVertexColorProperty;     // new(0x20) vtbl 0x00293AD0: flags 0, +0x18 = 0, +0x1C = 0
        props.insert(props.begin(), vc);
        auto* mat = new ni::NiMaterialProperty;       // new(0x5C) 0x00114B10
        mat->emissive = {0.8f, 0.8f, 0.8f};           // +0x40..+0x48 = 0x3F4CCCCD (O: the pass's emissive)
        ++mat->revision;
        mat->alpha = 0.75f;                           // +0x50
        ++mat->revision;
        props.insert(props.begin(), mat);
        props.insert(props.begin(), new ni::NiTexturingProperty);   // new(0x3C) 0x00109DC0, +0x18 = 0: no maps
        auto* shade = new ni::NiShadeProperty;        // new(0x18) vtbl 0x00295B34 (L: NiShadeProperty), flags 1 & ~1
        shade->flags = 0;                             // (O: the pass draws with SHADEMODE flat)
        props.insert(props.begin(), shade);
        shadowCamera = new ni::NiCamera;              // new(0x250) 0x001009D0
        shadowCamera->local.translate = {0.0f, 5000.0f, 0.0f};   // +0x60 (0x459C4000)
        shadowCamera->UpdateDownwardPass(0.0f, true);   // Update(0, 1)
        f32 f[6];
        std::memcpy(f, shadowCamera->frustum, sizeof f);
        f[4] = 100.0f;                                // 0x42C80000
        f[5] = 10000.0f;                              // 0x461C4000
        shadowCamera->SetFrustum(f);                  // 0x00011960
    }
    health4F8 = health;                               // orig: +0x4F8 = +0x4F4
    node->UpdateProperties();                         // 0x00101EA0
    node->UpdateEffects();                            // 0x00102190
    node->UpdateDownwardPass(0.0f, true);
    static constexpr const char* kRootBones[] = {
        "Fry_ROOTBONE:Animation (Translational)", "Leela_BONEROOT:Animation (Translational)",
        "B_ROOTBONE:Animation (Translational)", "rootbone_zoidberg:Animation (Translational)"};
    type = static_cast<u32>(cdef.control);            // orig: D+0xE8 when no root bone is found
    for (u32 i = 0; i < 4; ++i)
        if (GetObjectByName(node.Get(), kRootBones[i])) {   // orig: node vf54
            type = i + 1;
            break;
        }
    if (physic && type >= 1 && type <= 4) physic->SetBone(type == 4 ? "parent" : kRootBones[type - 1]);
    if (level.lightManager) level.lightManager->AddTarget({0, 0, 0}, node.Get());   // orig: DAT_002EA824 vf00
    return true;
}

// ---- the view files -----------------------------------------------------------------------------------------------
ViewFile::ViewFile(std::string p, const char* owner, s32 w, ni::NiRenderer* renderer)
    : path(std::move(p)), which(w), fpView(std::strcmp(owner, "FPView") == 0) {   // orig: 0x00045F60
    // the camera's frustum: a = DAT_002DD040 (pi / 180: its static initialiser 0x0019A220, F(pi_f * 1 / 180) =
    // 0x3C8EFA36) * 45 [0x00297410] * 0.5 [0x00293AA0]; c = F(FCOS a); t = FSIN a / c (kept); (-t, t, 0.75 t, -0.75 t,
    // 0.1, 100) ([0x00293A94], 0x3DCCCCCD, 0x42C80000)
    const double a = double(std::bit_cast<f32>(0x3C8EFA36u)) * 45.0f * 0.5f;
    const f32 c = F(std::cos(a));                     // (KNOWN_ISSUES #35: the oracle's FCOS / FSIN are double)
    const double t = std::sin(a) / double(c);
    const double ty = 0.75f * t;
    const f32 frustum[6] = {F(-t), F(t), F(ty), F(-ty), 0.1f, 100.0f};
    camera = new ni::NiCamera;                        // new(0x250) 0x001009D0
    camera->renderer = renderer;                      // (+0x1A8) = App->vf14()
    camera->SetFrustum(frustum);                      // 0x00011960
    std::memcpy(collectKey.data(), "BD_COLLECT", 11); // (11 bytes from 0x00297404)
    std::memcpy(killsKey.data(), "BD_KILLS", 9);      // (9 bytes from 0x002973F8)
    // (the request: vf02 0x00046210, called by the Player constructor)
    if (stream::Manager* m = stream::Manager::Current()) request = m->Create(*this, owner);
}

ViewFile::~ViewFile() {                               // orig: 0x000460F0
    request.reset();                                  // +0x04
    root = nullptr;                                   // +0x08 (the node +0x10 still holds it)
    if (camera) {
        camera->renderer = nullptr;                   // (+0x0C)+0x1A8
        cameraScene = nullptr;                        // (+0x0C)+0x15C
    }
    // (+0x20: the text object deleted, TextObject::~TextObject 0x00099730: none in the port)
    node = nullptr;                                   // +0x10 (0x000461A9: the root and its lights die here)
    camera = nullptr;                                 // +0x0C
}

bool ViewFile::OnStream(stream::Request& r) {         // orig: vf00 0x000462B0
    if (r.state == stream::State::New) {
        r.SetPath(path);                              // sprintf(+0x14 ? 0x00297418 : 0x00297434); 0x000D1F30
        return true;
    }
    // the file's top-level object 0 as a NiNode (RTTI 0x002F9230) -> +0x08
    ni::NiStream* st = r.Stream();
    ni::NiObject* first = nullptr;
    if (st && !st->TopLevelLinks().empty()) first = st->ResolveLink(st->TopLevelLinks()[0]);
    root = dynamic_cast<ni::NiNode*>(first);
    node = new ni::NiNode;                            // new(0xE0) 0x00105C10 -> +0x10
    node->local.translate = {0, 0, 0};                // [0x002F915C] (NiPoint3::ZERO)
    node->local.scale = F(std::fabs(1.0));            // FLD QWORD [0x00293CC0]; FABS; FSTP
    if (root) {                                       // (port guard: the original goes on with a null root)
        node->AttachChild(root.Get(), true);          // vfA0
        // the root's z-buffer property (GetProperty(11) 0x00101DA0; else a new one, flags 3, function 3, at the
        // head of its list 0x00013A50): test and write off
        auto* z = dynamic_cast<ni::NiZBufferProperty*>(GetProperty(*root, 11));
        if (!z) {
            z = new ni::NiZBufferProperty;            // new(0x1C), vtable 0x00296C80
            z->flags = 3, z->testFunction = 3;
            AttachProperty(*root, z);
        }
        z->flags = u16(z->flags & 0xFC);              // AND byte +0x14, 0xFC
    }
    // the node turned by -pi / 2 about Y: FLD [0x002F9274] (pi); FMUL [0x00297414] (-0.5); FSTP
    node->local.rotate = ni::MakeYRotation(F(double(std::bit_cast<f32>(0x40490FDBu)) * -0.5f));   // 0x001030B0
    node->UpdateDownwardPass(0.0f, true);             // NiAVObject::Update(0, 1)
    node->UpdateProperties();                         // 0x00101EA0
    node->UpdateEffects();                            // 0x00102190
    cameraScene = node;                               // (+0x0C)+0x15C = +0x10
    camera->local.translate = {std::bit_cast<f32>(0xC0000000u), 0, 0};   // (-2, 0, 0)
    camera->UpdateDownwardPass(0.0f, true);
    if (root) {                                       // (no test in the original)
        root->UpdateProperties();
        root->UpdateEffects();
    }
    request.reset();                                  // +0x04 released
    return false;
}

// ---- Player -------------------------------------------------------------------------------------------------------
Player::Player(LevelData& lvl, CharacterDef& d, const ni::NiPoint3& pos, const ni::NiMatrix3& rot, std::string cls)
    : Character(lvl, d), className(std::move(cls)) {  // orig: 0x0005D6F0
    // NOT PORTED: the input / action map (+0x7BC), the HUD, the AI navigation agent, the level extras.
    spawnY = pos.y;                                   // orig: +0xB84 (raw copy)
    lvl.players.push_back(this);                      // orig: append to S+0x98 (link +0xC4C)
    if (!lvl.player) lvl.player = this;               // orig: S+0x94
    // orig: +0x5C = new(0x48) 0x000AF0D0(D+0x90, D+0x94, D+0x98, 10.0) in the level's physics world, owner = this.
    body = std::make_unique<phys::CharBody>(d.boundWidth, d.boundHeight, d.boundDepth,
                                            lvl.physic ? &lvl.physic->world : nullptr);
    body->owner = this;                               // orig: vf28 SetOwner
    body->shape.cc = 1;                               // orig: shape+0xCC
    Character::SetPosition(pos);                      // orig: direct calls
    Character::SetRotation(rot);
    startY = GetWorldPosition().y;                    // orig: +0xB80
    // orig: 0x0005D8E4 / 0x0005D8EF: +0x7B8 = 0, then = Input_GetActivePad 0x0009B720 (the pad a script's button
    // trigger 0x000B74F0 reads before the player's first SampleInput vfFC, which sets it again)
    {
        const input::InputSystem* in = lvl.input;
        pad = in ? (in->activePad ? in->activePad : in->pads[0]) : nullptr;
    }
    const char* n = d.name.c_str();                   // orig: _stricmp chain
    type = IsFryName(n) ? 1 : crt::StrICmp(n, "leela") == 0 ? 2 : crt::StrICmp(n, "bender") == 0 ? 3
         : crt::StrICmp(n, "rodent") == 0                    ? 4 : 0;
    // orig: 0x0005D8FB..0x0005DB73, the HUD (S+0x118, 0x000E5B60 by name): "timer" hidden; then Fry (and a name of
    // none of the classes) "weapons" shown / "charge" hidden, Leela and Bender the opposite, the rodent both hidden
    // and "timer" shown (0x0029A708 / 0x0029A45C / 0x0029A698)
    if (lvl.hud) {
        lvl.hud->SetActive("timer", false);
        const bool weapons = type == 1 || type == 0, charge = type == 2 || type == 3;
        lvl.hud->SetActive("weapons", weapons);
        lvl.hud->SetActive("charge", charge);
        if (type == 4) lvl.hud->SetActive("timer", true);
    }
    // orig: the per-class extras: Bender in level2-5 (_stricmp(S+0xC, "level2-5") 0x0029A6A4) first the cannons
    // (+0x7A8 = new(0xB0) 0x000334D0(P), Cannon.h), then every Bender the first-person view
    if (type == 3 && crt::StrICmp(lvl.name.c_str(), "level2-5") == 0)
        cannon = std::make_unique<CannonCtrl>(lvl, *this);
    if (type == 3)
        view = std::make_unique<ViewFile>("Cockpit\\Bender_vision.nif", "FPView", 0, lvl.renderer);   // FPView(0)
    if (type == 4) {                                  // orig: 0x0005DC69: +0x7AC = new(0x274) 0x00061240, Start
        ride = std::make_unique<RatRide>(lvl, *this);
        ride->Start();                                // 0x00062EE0 (the camera presets, the spline start)
    }
    if (type == 1 && crt::StrICmp(lvl.name.c_str(), "level1-6") == 0) {   // orig: 0x0005DCB2
        chicken = std::make_unique<ChickenWalker>(lvl, *this);   // +0x7B0 = new(0x17C) 0x00043D80, then 0x000431A0
    }
    else if (type == 2)
        view = std::make_unique<ViewFile>("Cockpit\\Leela_vision.nif", "FPView", 1, lvl.renderer);   // FPView(1)
    agent = std::make_unique<AiAgent>(lvl, *this);    // orig: 0x0005DE4E (+0x58 = new(0x8A8) 0x0002E810)
    // orig: Player::Reset 0x00057680 (the parts with load-time state):
    // HEAP-MIRROR: the new player block's and its body's never written bytes are heap garbage in the original (#33 /
    // #41); the port's defaults here (won't do: KNOWN_ISSUES "Heap layout")
    node->flags &= ~ni::kNiAVHidden;
    health = maxHealth;                               // orig: +0x4F4 = +0x4FC
    if (lvl.hud) {                                    // (Reset's tail 0x000578E8: the HUD's weapon box emptied)
        lvl.hud->Command("weapons", "select", "");    // 0x000E5BC0 (0x0029A45C, 0x0029A464, 0x00295254)
        lvl.hud->Command("weapons", "ammo", -1.0f);   // 0x000E5B90 (0x00297214, 0xBF800000)
    }
}

// ---- the held state (docs/analysis/object_gaps.md 16) -------------------------------------------------------------
ni::NiPoint3 Character::TopPoint() const {                // orig: 0x000345B0
    const f32 h = F(double(boundHeight) + heightOffset);   // FLD +0x4D4; FADD +0x4D0; FSTP
    const ni::NiPoint3 p = GetWorldPosition();            // vf34
    return {p.x, F(double(h) + p.y), p.z};
}

void Player::Grab(Character* by, ni::NiNode* tip) {   // orig: 0x00055370 (16.3)
    if (ladder || ledge || locked || !by || !tip) return;   // +0xB8C / +0xB8D / +0xB8E
    releaseDizzy = 0;                                 // +0x7A5
    hasControl = 0;                                   // +0x630
    attacking504 = attacking505 = 0;                  // +0x504 / +0x505
    pendingWeapon = nullptr;                          // +0x524
    stowedWeapon = weapon;                            // +0xC1C = +0x528
    (void)SetWeapon(nullptr);                         // vfB4(0)
    StopAnimation();                                  // 0x00039D90
    ClearOverlay();                                   // 0x000363C0
    heldBy = by;
    if (!level.IsCharacter(by)) {                     // 0x00033EE0
        heldBy = nullptr;
        return;
    }
    if (type == 3) {                                  // Bender: hung at the tip, its rotation undone
        tip->AttachChild(node.Get(), true);           // vfA0(node, 1)
        const ni::NiPoint3 top = heldBy->TopPoint();      // 0x000345B0
        SetPosition(top);                             // vf1C
        SetRotation(ni::NiMatrix3{});                 // vf24(identity 0x002D7820)
        const ni::NiMatrix3 R = ni::InverseOrZero(tip->local.rotate);   // 0x00103BB0 (tip +0x30)
        node->local.translate = {std::bit_cast<f32>(0xC0533333u), 0.0f, 0.0f};   // (-3.3, 0, 0)
        node->local.rotate = R;                       // rep movsd (12 dwords)
    } else {
        const ni::NiPoint3 up{0.0f, 1.0f, 0.0f};          // 0x002D77EC
        const ni::NiPoint3 me = GetWorldPosition();       // vf34 (this first)
        const ni::NiPoint3 at = heldBy->GetWorldPosition();
        ni::NiPoint3 d{F(double(at.x) - me.x), 0.0f, F(double(at.z) - me.z)};   // 0x00011530; then y = 0
        if (ni::Unitize(d) == 0.0f) d.x = 1.0f;       // FCOMP [0x00293AB8]; TEST AH,0x44; JP (only ==)
        const ni::NiPoint3 c = ni::Cross(up, d);          // 0x00011700 (ECX = up)
        ni::NiMatrix3 M;                              // +0x570: column 0 = d, column 1 = up, column 2 = -c (FCHS)
        const f32 dr[3] = {d.x, d.y, d.z}, ur[3] = {up.x, up.y, up.z}, cr[3] = {-c.x, -c.y, -c.z};
        for (int r = 0; r < 3; ++r) M.m[r][0] = dr[r], M.m[r][1] = ur[r], M.m[r][2] = cr[r];
        SetRotation(M);                               // vf24(+0x570)
        tip->AttachChild(node.Get(), true);           // vfA0(node, 1)
        ni::NiPoint3 t{};
        ni::NiMatrix3 R;                              // identity 0x002D7820
        if (crt::StrICmp(heldBy->def->name.c_str(), "turdman") == 0) {   // 0x00297780
            grabTurd = 1;                             // +0x7A7
            t = {std::bit_cast<f32>(0x3F5ABC62u), std::bit_cast<f32>(0xC0B029C7u), std::bit_cast<f32>(0xBB7CF80Eu)};
        } else {
            if (heldBy->IsAnimPlaying(0x31)) {        // the third attack
                SetPosition(heldBy->GetWorldPosition());   // vf1C(holder vf34)
                t = {std::bit_cast<f32>(0x40733333u), -1.0f, std::bit_cast<f32>(0xBE99999Au)};   // (3.8, -1, -0.3)
                const ni::NiPoint3 a{0.0f, 0.0f, 1.0f}, b{0.0f, 1.0f, 0.0f};   // 0x002D77F8 / 0x002D77EC
                const ni::NiPoint3 e = ni::Cross(b, a);   // 0x00011700 (ECX = b)
                const f32 ar[3] = {a.x, a.y, a.z}, br[3] = {b.x, b.y, b.z}, er[3] = {-e.x, -e.y, -e.z};
                for (int r = 0; r < 3; ++r) R.m[r][0] = ar[r], R.m[r][1] = br[r], R.m[r][2] = er[r];
            } else {
                SetPosition(heldBy->GetWorldPosition());   // vf1C(holder vf34); t = 0, R = identity
            }
            grabStart = 1;                            // +0x7A6 (after the node's transform)
        }
        node->local.translate = t;
        node->local.rotate = R;
    }
    UpdateNodeAt(heldBy->GetNode(), heldBy->animTime);   // 0x0005584F: vf30()->Update(holder +0x78, 1)
}

void Player::Release(bool leftover) {                 // orig: 0x00055880 (16.3)
    if (!heldBy && !leftover) return;
    if (!level.IsCharacter(heldBy)) heldBy = nullptr; // 0x00033EE0
    hasControl = 1;                                   // +0x630
    if (ni::NiNode* dn = level.GetNode()) dn->AttachChild(node.Get(), true);   // S->vf10()->vfA0(node, 1)
    const ni::NiPoint3 pos = GetWorldPosition();          // vf34
    if (type == 3) {
        releaseDizzy = 1;                             // +0x7A5 (UpdateAnimation's dizzy)
        node->local.translate = pos;
        node->local.rotate = rotation;                // +0x570 (12 dwords)
    } else {
        const ni::NiPoint3 u{0.0f, 1.0f, 0.0f};           // 0x002D77EC
        ni::NiPoint3 d{1.0f, 0.0f, 0.0f};                 // (no holder: x = 1, z = 0)
        if (heldBy) {
            const ni::NiPoint3 X = ni::MulVector(heldBy->rotation, {1.0f, 0.0f, 0.0f});   // holder vf38, 0x002D77E0
            d = IsAnimPlaying(0xD7) ? ni::NiPoint3{F(double(X.x) * -1.0f), F(double(X.y) * -1.0f), F(double(X.z) * -1.0f)}
                                    : X;              // FMUL [0x00293BF0] (turdman's grab faces the other way)
        }
        d.y = 0.0f;
        (void)ni::Unitize(d);                         // (FSTP ST0)
        const double A = double(u.y) * d.z - double(u.z) * d.y;   // kept on the x87 stack
        const f32 B = F(double(u.z) * d.x - double(u.x) * d.z);
        const f32 C = F(double(u.x) * d.y - double(u.y) * d.x);
        const ni::NiPoint3 c2{F(-A), -B, -C};             // FCHS of the unrounded A, then FSTP; B and C reloaded, FCHS
        for (int r = 0; r < 3; ++r) {                 // +0x570: column 0 = d, column 1 = u, column 2 = c2
            const f32 dr[3] = {d.x, d.y, d.z}, ur[3] = {u.x, u.y, u.z}, cr[3] = {c2.x, c2.y, c2.z};
            rotation.m[r][0] = dr[r];
            rotation.m[r][1] = ur[r];
            rotation.m[r][2] = cr[r];
        }
        node->local.translate = pos;
        node->local.rotate = rotation;                // rep movsd (12 dwords)
    }
    if (heldBy && type == 3) {                        // 0x00055AC0: Bender takes the holder's attack damage
        if (const AnimEntry* e = heldBy->curSeq ? heldBy->EntryOf(heldBy->curSeq) : nullptr)   // 0x0003FD20
            Damage(F(e->hasDamage ? e->damage : 0.0f), heldBy);   // AnimEntry::GetDamage 0x0003EE90; vf7C
    }
    if (type == 1 && stowedWeapon) {                  // Fry takes his weapon out again
        (void)SetWeapon(stowedWeapon);                // vfB4(+0xC1C)
        stowedWeapon = nullptr;
    }
    heldBy = nullptr;
}

void Player::HeldUpdate(u8& flag) {                   // orig: vf180 0x00055D60 (16.3)
    if (!level.IsCharacter(heldBy)) {                 // 0x00033EE0: the holder is gone
        Release(true);
        return;
    }
    const f32 ht = heldBy->animTime;                  // holder +0x78
    if (heldHurt || IsAnimPlaying(0xBF)) {
        PlayAnimation(SeqById(0xBF), false, true, false, 0.0f);   // 0x0003A130(0x00033E00(0xBF), 0, 1, 0, 0)
        if (curSeq && heldHurt) curSeq->offset = -ht; // FLD; FCHS; FSTP
        heldHurt = 0;
        flag = 0;
        return;
    }
    if (grabTurd || IsAnimPlaying(0xD7)) {
        grabTurd = 0;
        if (!IsAnimPlaying(0xD7)) {
            PlayAnimById(0xD7, true, false, true, 0.0f);   // 0x0003CE70(0xD7, 1, 0, 1, 0)
            if (curSeq) {
                animStart = ht;                       // +0x80 = holder +0x78 (raw)
                curSeq->offset = -ht;
            }
        }
        flag = 0;
        return;
    }
    if (grabStart || IsAnimPlaying(0xD8) || IsAnimPlaying(0xDB)) {
        if (grabStart) {
            PlayAnimById(0xD8, true, false, true, 0.0f);   // "tentacle_attack1"
            if (curSeq) {
                animStart = ht;
                curSeq->offset = -ht;
            }
        }
        grabStart = 0;
        flag = 0;
    }
}

void Player::BuildModel() {                           // orig: vf64 0x0003DF70 / 0x00081710 (Bender)
    Character::BuildModel();                          // 0x0003DF70
    if (className != "Bender" || !model) return;      // (Bender's vtable 0x0029D980; +0x564)
    for (const char* n : {"Ciggar_null", "Beer_Bottle"})   // 0x0029DB50 / 0x0029DB44 (model vf54, twice)
        if (ni::NiAVObject* o = GetObjectByName(model.Get(), n)) o->flags |= ni::kNiAVHidden;   // +0xB0 |= 1
    if (AnimEntry* e = cdef.FindAnimationById(0xBC)) {   // CharacterDef::FindAnimationById 0x0003F280 (twice)
        e->hasDamage = true;                          // AnimEntry::SetDamage 0x0003EE60: the block +0x40 (new(8))
        e->damage = 50.0f;                            // 0x42480000
    }
}

void Player::OnStageLoaded(Stage& stage) {            // orig: 0x0004F8D0
    if (ride && ride->started) ride->AddStage(stage); // 0x000611E0, 0x000628E0
}

// ---- the inventory, healing, ammo (docs/analysis/pickup_port.md 3.3 / 3.4) ---------------------------------------
void Character::Inventory::AttachObject(GameObject& obj) {   // orig: 0x000352D0
    if (!obj.def->IsType(4)) return;
    holder.OnPickup(obj);                             // vfC8
    obj.GetNode()->local.translate = {};              // (the node is not re-parented: it stays in its room)
    obj.GetNode()->local.rotate = {};                 // identity 0x002D7820
}

void Character::Inventory::DetachObject(GameObject& obj) {   // orig: 0x00035350
    ni::NiNode* n = obj.GetNode();
    if (n && n->parent) n->parent->DetachChild(n);
}

ni::NiNode* Character::Inventory::GetNode() const { return holder.node.Get(); }   // orig: 0x0003D800

f32 Character::Say(const char* textId, const char* dbName, s32 priority) {   // orig: 0x000358D0
    if (!level.text) return 0.0f;
    const text::LanguageDb* db = dbName ? FindLanguageDb(level, dbName)              // 0x000C33F0
                                        : FindLanguageDbWithId(level, *level.text, textId);   // the S+0xEC walk
    if (!db) return 0.0f;
    // orig: the builder 0x000D7FF0 (database, the lip list S+0xE8, the sound map S+0x100, directories ""), then
    // NewDialogue, NewSegment(0), AddLip(this + 0x40), AddSound, CloseSegment(-1), Build
    DialogueBuilder b(level, level.text, db);
    b.NewDialogue();
    b.NewSegment(0.0f);
    (void)b.AddLip(&lip, textId);
    (void)b.AddSound(textId);
    b.CloseSegment(-1.0f);
    ni::NiPointer<Dialogue> d = b.Build();
    if (!d) return 0.0f;
    d->priority = priority;                           // +0x18
    (void)level.dialogue->Play(d.Get());              // DAT_002EBDAC->0x000D7BE0 (the result ignored)
    return d->duration;                               // +0x14
}

bool Character::Heal(f32 amount) {                    // orig: 0x000357E0
    u32 bits;
    std::memcpy(&bits, &health, 4);
    if (bits == 0x47590300u) return false;            // dword compare: 55555.0f
    if (health == maxHealth) return false;            // TEST AH,0x44; JP (NaN: continue)
    health = F(double(amount) + health);              // FADD; FST
    if (health > maxHealth) health = maxHealth;       // TEST AH,0x41; JNE
    if (ambientLight) {                               // +0x30
        ++ambientLight->revision;
        ambientLight->dimmer = 1.0f;
        ambientLight->ambient = {0.5f, 0.5f, 1.0f};
        ++ambientLight->revision;
        // (0x000345B0: the top point, result unused)
    }
    if (health > 0.0f) level.SetObjFlags(id, level.GetObjFlags(id) & ~0x10u);   // not dead any more
    return true;
}

void Player::OnPickup(GameObject& obj) {              // orig: vfC8 0x00051250 (inventory_batch1_port.md 5.10)
    // the IngameEvent arg (0x000984D0) with the picked-up object in its first word (0x000D8150), category 8
    cdef.ingameEvents.Trigger(8, {&obj, nullptr}, IngameContext());   // D+0x28 0x000F28F0
    if (obj.def->IsType(8)) {                         // a weapon
        if (type != 1 || chicken) return;             // 0x000512B5: Fry without the walker object (P+0x7B0): else 0
        auto* W = dynamic_cast<Weapon*>(&obj);
        if (!W) return;                               // (the original treats every IsType(8) object as a weapon)
        switch (W->WeaponType()) {                    // vf7C, called again for each test
        case WeaponDef::kPistol: (void)AddAmmo(0, 50); break;     // vf8C
        case WeaponDef::kShotgun: (void)AddAmmo(1, 100); break;
        case WeaponDef::kMachineGun: (void)AddAmmo(3, 100); break;
        case WeaponDef::kRailgun: (void)AddAmmo(2, 100); break;
        default: break;
        }
        if (weapon) attacking504 = 1;                 // +0x504
        pendingWeapon = W;                            // +0x524
        attacking505 = 1;                             // +0x505
        if (level.hud) {                              // 0x00051339: the HUD S+0x118 (0x0029A45C)
            level.hud->Command("weapons", "select", W->def->name.c_str());   // 0x000E5BC0 (0x0029A464, W+0x10 +0x10)
            const s32 slot = pendingWeapon->ammoSlot; // (+0x524)+0x74
            level.hud->Command("weapons", "ammo",     // 0x000E5B90 (0x00297214): FILD ammo[slot], else -1
                               slot != -1 ? static_cast<f32>(ammo[static_cast<size_t>(slot)]) : -1.0f);
        }
    } else if (obj.def->IsType(4) && obj.def->name.empty()) {
        return;                                       // 0x000513FA (a nameless pickup: no HUD)
    }
    // (the other pickups' "ammo_energy" / "ammo_plasma" / "slurmcan" / "slurm_sixpack" compares: results unused)
    // 0x00051480: the definition's name, unless DAT_002EAA48, as the HUD's ("items", "show", name) (0x0029A418 /
    // 0x0029A420)
    if (!obj.def->name.empty() && !level.quickLoading && level.hud)
        level.hud->Command("items", "show", obj.def->name.c_str());
}

bool Player::AddAmmo(s32 slot, s32 n) {               // orig: vf8C 0x000504D0
    if (chicken && chicken->active) {                 // 0x00042690: the walker's rockets
        chicken->AddRockets(static_cast<u32>(n));     // 0x00042700
        return true;
    }
    s32& a = ammo[static_cast<size_t>(slot)];
    if (a == 999) return false;
    a += n;
    if (a > kMaxAmmo[slot]) a = kMaxAmmo[slot];
    return true;
}

bool Player::AddCharge(f32 v) {                       // orig: vf15C 0x00050420
    u32 bits;
    std::memcpy(&bits, &charges, 4);
    if (bits == 0x40800000u) return false;            // dword compare with 4.0f
    charges = F(double(v) + charges);
    if (charges > 4.0f) charges = 4.0f;               // TEST AH,0x41; JNE
    return true;
}

ni::NiPoint3 Player::CharacterPoint() const {         // orig: 0x00054A50
    if (firstPerson) return EyePoint();               // +0xB88: 0x00054A80
    return Character::CharacterPoint();               // 0x0003D7C0
}

Player::~Player() {                                   // orig: 0x00050830
    Character::UnloadModel();                         // 0x0005086D: 0x0003B7D0 (not the virtual; a no-op after the class dtor's)
    Unlink(level.players, this);                      // +0xC4C next / +0xC50 prev (head S+0x98)
    if (level.player == this) level.player = level.players.empty() ? nullptr : level.players.front();   // S+0x94 = S+0x98
    // +0xC28, +0xC20 and +0xC38 (released, +0xC20 / +0xC38 detached from their parents first): the lock-on marker's
    // objects that vf154 0x00058FA0 builds (+0xC38: no writer found in the port's reached code)
    markerMaterial = nullptr;                         // +0xC28
    if (marker && marker->parent) (void)marker->parent->DetachChild(marker.Get());   // +0xC20
    marker = nullptr;
    cannon.reset();                                   // 0x000509A2: +0x7A8 0x00032E50, operator delete
    ride.reset();                                     // +0x7AC vf00(1)
    chicken.reset();                                  // +0x7B0 vf08(1)
    view.reset();                                     // +0x7B4 0x000460F0, operator delete
    // (then DAT_002DC9F4, a heap statistic of the bytes freed: not kept; the Character dtor 0x0003D810)
}

GameObject* PlayerDef::Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                             const char* label) {     // orig: 0x0005E6B0
    if (stream::Manager* m = stream::Manager::Current()) m->Note("SPAWN " + name + (model ? " 1" : " 0"));
    RequestStream();                                  // orig: vf04 0x0003FE00 (always true)
    const char* n = name.c_str();
    const char* cls = IsFryName(n) ? "Fry"
                    : crt::StrICmp(n, "leela") == 0  ? "Leela"
                    : crt::StrICmp(n, "bender") == 0 ? "Bender"
                    : crt::StrICmp(n, "rodent") == 0 ? "ZoidbergRat"
                                                     : "Player";
    auto* o = new Player(level, *this, pos, rot, cls);
    std::span<const SpawnSound> list;                 // (the generic Player class adds none)
    if (std::strcmp(cls, "Fry") == 0) list = kFrySounds;
    else if (std::strcmp(cls, "Leela") == 0) list = kLeelaSounds;
    else if (std::strcmp(cls, "Bender") == 0) list = kBenderSounds;
    else if (std::strcmp(cls, "ZoidbergRat") == 0) list = kRatSounds;
    DefContext ctx;
    ctx.loadSound = level.loadSound ? level.loadSound : [](const std::string&, u32) { return 0u; };
    for (const SpawnSound& e : list) AddSound(e.path, e.id, ctx);   // (appended again on every spawn)
    o->SetOwner(owner);                               // orig: vf10
    if (model) o->Init();                             // orig: vf04
    if (label) o->SetName(label);                     // orig: 0x000968C0 (after Init; id 0: no trigger lookup)
    return o;
}

}  // namespace fut::game
