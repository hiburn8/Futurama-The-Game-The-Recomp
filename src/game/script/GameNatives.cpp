// The level scripts' game natives; see GameNatives.h. The cases follow the dispatcher 0x000B0CD0: each pops its
// arguments (the stack pointer moves down by the table's pop size first), calls the game, and writes its results.
#include "game/script/GameNatives.h"

#include "core/CrtString.h"
#include "core/memory/MemoryManager.h"
#include "engine/ni/NiCollision.h"
#include "game/GameGlobals.h"
#include "game/GameTime.h"
#include "game/defs/CharacterDef.h"
#include "game/defs/Defs.h"
#include "game/defs/EffectDefs.h"
#include "game/defs/IngameEvent.h"
#include "game/level/Camera.h"
#include "game/level/Dialogue.h"
#include "game/level/Hud.h"
#include "game/level/Intermission.h"
#include "game/level/LevelAnimation.h"
#include "game/level/LevelData.h"
#include "game/level/LightManager.h"
#include "game/level/ScriptTriggers.h"
#include "game/level/Stage.h"
#include "game/level/TriggerManager.h"
#include "game/object/Cannon.h"
#include "game/object/Character.h"
#include "game/object/ChickenWalker.h"
#include "game/object/Destructable.h"
#include "game/object/Emitter.h"
#include "game/object/Fire.h"
#include "game/object/GenericNpc.h"
#include "game/object/Pickup.h"
#include "game/object/RatRide.h"
#include "game/object/SimpleNpc.h"
#include "game/object/Static.h"
#include "game/script/ScriptNatives.h"
#include "game/sound/GameSound.h"
#include "game/text/LanguageDb.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <optional>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fut::script {

namespace {

constexpr NativeInfo kNatives[] = {
#include "game/script/NativeTable.inc"
};

game::LevelData* g_level = nullptr;

u32 Get(VmAddr a) {
    u32 v;
    std::memcpy(&v, mem::FromVmAddr(a), 4);
    return v;
}
f32 GetF(VmAddr a) {
    f32 v;
    std::memcpy(&v, mem::FromVmAddr(a), 4);
    return v;
}
const char* Str(VmAddr a) { return a ? static_cast<const char*>(mem::FromVmAddr(a)) : nullptr; }
void Set(VmAddr a, u32 v) { std::memcpy(mem::FromVmAddr(a), &v, 4); }
void SetByte(VmAddr a, u8 v) { std::memcpy(mem::FromVmAddr(a), &v, 1); }   // (the low-byte results, 0x000B19CB)
void SetF(VmAddr a, f32 v) { std::memcpy(mem::FromVmAddr(a), &v, 4); }

using ni::detail::F;
using game::ScriptTrigger;

game::Stage* g_selStage = nullptr;                    // orig: DAT_002E9E04 (natives 322 / 324)
// (port) the Tank behind native 143's stand-in handle 0x07F10000 (the original returns the Tank pointer itself; L:
// one tank per level, alive while its script uses it)
game::Tank* g_tank = nullptr;

// The AI natives act on the selected agent (orig: the AI manager S+0x114, +0x10; script_triggers.md 5).
struct AiSelection {
    game::AiAgent* agent = nullptr;
    game::AiSequence* seq = nullptr;                  // orig: M+0x14 (AISelectSequence)
    game::AiGroup* group = nullptr;                   // orig: M+0x18 (native 363)
} g_ai;

// orig: native 330's statics (0x000B0810): the last pair's points 0x002E9FE0 / 0x002E9FD4 (zero at the first call,
// guard bits 0x002E9FEC) and its result byte 0x002E9FD0.
struct LineOfSightCache {
    ni::NiPoint3 a, b;
    u8 clear = 0;
} g_losCache;

std::string Lower(const char* s) {
    std::string r = s ? s : "";
    for (char& ch : r)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
    return r;
}

game::ScriptTriggers& Trig() { return *g_level->scriptTriggers; }

void NotPortedText(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& t : seen)
        if (t == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// The definitions FsPartCopyDef created, by the VM word the script holds (orig: the definition object itself; the
// port keeps the definition in the DefRegistry and gives the script a VM block of the original size).
std::map<VmAddr, game::EmitterDef*> g_partDefs;

game::EmitterDef* PartDef(VmAddr p) {
    const auto it = g_partDefs.find(p);
    return it == g_partDefs.end() ? nullptr : it->second;
}

// The fire-circle definitions natives 296 / 297 gave the script, by the VM word it holds (orig: the definition itself;
// the port gives the script a VM block of the original size per definition, as FsPartCopyDef's: made with the
// definition by native 297, and on the first native 296 of a definition that exists (that block is the port's, L)).
std::map<VmAddr, game::FireCircleDef*> g_circleDefs;

game::FireCircleDef* CircleDef(VmAddr p) {
    const auto it = g_circleDefs.find(p);
    return it == g_circleDefs.end() ? nullptr : it->second;
}

VmAddr CircleDefWord(game::FireCircleDef* d, bool created) {
    if (!d) return 0;
    for (const auto& [w, c] : g_circleDefs)
        if (c == d) return w;
    void* obj = mem::OperatorNew(0xA0);               // orig: new(0xA0) (native 297), the word the script keeps
    Set(mem::ToVmAddr(obj), 0x0029BED4);              // (the vtable word: the trace shows the pointer as hex)
    if (created) {                                    // orig: Def::Def 0x000962E0, the name copy
        const u32 len = static_cast<u32>(d->name.size()) + 1;
        std::memcpy(mem::OperatorNew(len), d->name.c_str(), len);
    }
    g_circleDefs[mem::ToVmAddr(obj)] = d;
    return mem::ToVmAddr(obj);
}

// Class 16 (0x000B5097, table 0x000B6E50): slot 0 is the address of the script variable holding the fire-circle
// definition; the case calls the definition's accessor (no class test) with the other slot or writes its result at
// slot 0 (the floats through the x87: FSTP).
bool CallFireCircleDef(const NativeInfo& n, VmAddr a) {
    const VmAddr obj = Get(Get(a));
    if (!obj) {                                       // orig: Script_NullObjectError 0x000BA270
        NotPortedText("class-16 native on a null object (Script_NullObjectError 0x000BA270)");
        return true;
    }
    game::FireCircleDef* d = CircleDef(obj);
    if (!d) {
        NotPortedText("class-16 native on a definition natives 296 / 297 did not return");
        return true;
    }
    switch (n.method) {
    case 3: d->value94 = GetF(a + 4); return true;     // SetRadius 0x00067950 (raw dword)
    case 4: SetF(a, d->value94); return true;          // GetRadius 0x00067960
    case 5: d->value98 = GetF(a + 4); return true;     // SetHeight 0x00067970
    case 6: SetF(a, d->value98); return true;          // GetHeight 0x00067980
    case 7: d->value9C = Get(a + 4); return true;      // SetDensity 0x00067990
    case 8: Set(a, d->value9C); return true;           // GetDensity 0x000679A0
    default: return false;
    }
}

// orig: 0x000C31D0 (this = the level object): the first object of the level's own list whose label equals `label`
// (strcmp); it counts only when its definition is an emitter definition - a first match of another class ends the
// search.
game::GameObject* FindEmitterByLabel(const char* label) {
    if (!label) return nullptr;
    for (game::GameObject* o = g_level->NextObject(nullptr); o; o = g_level->NextObject(o))
        if (o->label && std::strcmp(o->label->c_str(), label) == 0)
            return o->def && o->def->IsType(0x4000) ? o : nullptr;
    return nullptr;
}

// Class 14 (0x000B52DB): slot 0 is the address of the script variable holding the definition (the compiler pushes
// `addr global[..]`); the case reads the definition through it and calls its virtual setter with the other slots.
bool CallParticleDef(const NativeInfo& n, VmAddr a) {
    const VmAddr obj = Get(Get(a));
    if (!obj) return true;                            // orig: Script_NullObjectError 0x000BA270 (size 0: nothing)
    game::EmitterDef* d = PartDef(obj);
    if (!d) {
        NotPortedText("class-14 native on a definition FsPartCopyDef did not create");
        return true;
    }
    auto* sprite = dynamic_cast<game::SpriteEmitterDef*>(d);
    switch (n.method) {
    case 6: d->SetRange28(GetF(a + 4), GetF(a + 8)); return true;          // PresetVel: vtbl+0x28 0x0006C8B0
    case 8: d->SetRange30(GetF(a + 4), GetF(a + 8)); return true;          // PresetLifeTime: vtbl+0x30
    case 9: d->SetRange38(0.0f, GetF(a + 4)); return true;                 // PresetSpread: vtbl+0x3C -> vf38(0, a)
    case 12: d->SetRange40(GetF(a + 4), GetF(a + 8)); return true;         // PresetEmitTime: vtbl+0x40
    case 13: d->SetCount(Get(a + 4)); return true;                         // PresetAmount: vtbl+0x48 (dword)
    case 15: d->SetGravity(GetF(a + 4)); return true;                      // PresetGravity: vtbl+0x50
    case 16: d->SetFlag54(static_cast<u8>(Get(a + 4))); return true;       // PresetContinuous: vtbl+0x54 (byte)
    case 17: d->SetFlag55(static_cast<u8>(Get(a + 4))); return true;       // PresetInfinite: vtbl+0x58 (byte)
    case 18: d->SetValue58(GetF(a + 4)); return true;                      // PresetDamage: vtbl+0x5C
    case 19: d->SetValue5C(GetF(a + 4)); return true;                      // PresetDamageDist: vtbl+0x60
    case 22:                                                               // PresetSize: vtbl+0xB0 0x0006CBC0
        if (sprite) sprite->SetSize(GetF(a + 4), GetF(a + 8));
        else NotPortedText("PresetSize on a mesh emitter definition (vtbl+0xB0)");
        return true;
    case 25:                                                               // PresetColor: vtbl+0xC4 0x0006D140
        if (sprite) sprite->SetColor(GetF(a + 4), GetF(a + 8), GetF(a + 12));
        else NotPortedText("PresetColor on a mesh emitter definition (vtbl+0xC4)");
        return true;
    default:
        return false;                                 // the other class-14 methods: NOT PORTED
    }
}

// (port) native 155's stand-in handle for the player's cannon object (orig: P+0x7A8 itself; L: the scripts only store
// it and pass it back, as native 143's tank handle)
constexpr u32 kCannonHandle = 0x07F00000u;

game::CannonCtrl* PlayerCannon() { return g_level && g_level->player ? g_level->player->cannon.get() : nullptr; }

// Class 11 (0x000B6495): slot 0 is the address of the script variable holding native 155's value; a null object goes to
// Script_NullObjectError 0x000BA270 (nothing is called), else the cannon's method with the other slot, or its result
// written at slot 0.
bool CallCannon(const NativeInfo& n, VmAddr a) {
    const VmAddr obj = Get(Get(a));
    if (!obj) return true;                            // orig: Script_NullObjectError 0x000BA270 (returns false)
    game::CannonCtrl* c = PlayerCannon();
    if (obj != kCannonHandle || !c) {
        NotPortedText("class-11 native on an object native 155 did not return");
        return true;
    }
    switch (n.method) {
    case 3: c->Activate(Str(Get(a + 4))); return true;   // Activate(name) 0x00033980
    case 4: c->Deactivate(); return true;             // Deactivate 0x00032ED0
    case 5: c->SetAmmo(static_cast<s32>(Get(a + 4))); return true;   // SetAmmo 0x00032E00
    case 6: Set(a, static_cast<u32>(c->ammo)); return true;          // GetAmmo (inline: +0x90)
    case 7: c->SetActiveAmmo(static_cast<s32>(Get(a + 4))); return true;   // SetActiveAmmo 0x00032E20
    case 8: Set(a, static_cast<u32>(c->activeAmmo)); return true;    // GetActiveAmmo (inline: +0xA8)
    default: return false;
    }
}

// orig: 0x000C2730(0): the first player (S+0x98).
game::Player* FirstPlayer() { return g_level->players.empty() ? nullptr : g_level->players.front(); }

// orig: Session::FindWaypointAnyStage 0x000C2A40: the first stage's waypoint with that name.
const game::StageWaypoint* FindWaypointAnyStage(const char* name) {
    for (const auto& st : g_level->stages)
        if (const game::StageWaypoint* wp = st->FindWaypoint(name)) return wp;
    return nullptr;
}

// The trigger constructors (script_triggers.md 2.3; allocation order 2.9).
VmAddr CreateTrigger(const NativeInfo& n, VmAddr a) {
    game::ScriptTriggers& tr = Trig();
    game::TriggerManager& tm = *g_level->triggers;
    auto foc = [&](VmAddr name) { return name ? tm.FindOrCreate(Str(name)) : nullptr; };
    switch (n.method) {
    case 212:                                         // FsTriggerNULL() 0x000B0440
    case 213: {                                       // FsTriggerNULL(b) 0x000B03F0
        ScriptTrigger& t = tr.Create(ScriptTrigger::Null, 0xC, 0x002A20C4);
        t.state = n.method == 213 && Get(a) != 0 ? 1 : 0;
        Set(t.vm + 4, t.state);
        return t.vm;
    }
    case 195:                                         // FsTriggerSphere(actor, centre, radius) 0x000B8A90
    case 196: {                                       // FsTriggerSphere(actor, centre): radius -1
        ScriptTrigger& t = tr.Create(ScriptTrigger::Sphere, 0x30, 0x002A2458);
        t.name0 = tr.CopyName(Str(Get(a)));
        t.name1 = tr.CopyName(Str(Get(a + 4)));
        t.radius = n.method == 195 ? GetF(a + 8) : -1.0f;
        tr.Refresh(t);                                // orig: vf08 0x000B7380 (non-virtual)
        return t.vm;
    }
    case 198: {                                       // FsTriggerBox(actor, box) 0x000B8C40
        ScriptTrigger& t = tr.Create(ScriptTrigger::Box, 0x70, 0x002A2478);
        t.name0 = tr.CopyName(Str(Get(a)));
        t.name1 = tr.CopyName(Str(Get(a + 4)));
        t.rec0 = foc(t.name0);
        t.rec1 = foc(t.name1);
        tr.Refresh(t);                                // orig: vf08 0x000B76D0
        return t.vm;
    }
    case 207: {                                       // FsTriggerButton(action) 0x000B8B80
        ScriptTrigger& t = tr.Create(ScriptTrigger::Button, 0x10, 0x002A24E0);
        t.action = Get(a);
        Set(t.vm + 0xC, t.action);
        return t.vm;
    }
    case 208: {                                       // FsTriggerPickup(label) 0x000B8BB0
        ScriptTrigger& t = tr.Create(ScriptTrigger::Pickup, 0x10, 0x002A2468);
        t.name0 = tr.CopyName(Str(Get(a)));
        return t.vm;
    }
    case 209: {                                       // FsTriggerAtSight(watcher, target) 0x000B8D80
        ScriptTrigger& t = tr.Create(ScriptTrigger::AtSight, 0x14, 0x002A24F0);
        t.rec0 = foc(Get(a));
        t.rec1 = foc(Get(a + 4));
        return t.vm;
    }
    case 210: {                                       // FsTriggerDestructable(name) 0x000B8E10
        ScriptTrigger& t = tr.Create(ScriptTrigger::Destructable, 0x10, 0x002A2500);
        t.rec0 = foc(Get(a));
        return t.vm;
    }
    case 211: {                                       // FsTriggerAnimation(anim, clip) 0x000B8EA0
        ScriptTrigger& t = tr.Create(ScriptTrigger::Animation, 0x14, 0x002A2490);
        if (Get(a) && Get(a + 4)) {                   // (both or neither)
            t.name0 = tr.CopyName(Str(Get(a)));
            t.name1 = tr.CopyName(Str(Get(a + 4)));
        }
        return t.vm;
    }
    case 214:                                         // FsTriggerHealth(name, health): flags 1, 1
    case 215: {                                       // FsTriggerHealth(name, health, f65, f64) 0x000B8F70
        ScriptTrigger& t = tr.Create(ScriptTrigger::Health, 0x70, 0x002A24A0);
        t.name0 = tr.CopyName(Str(Get(a)));
        t.flag65 = n.method == 215 ? static_cast<u8>(Get(a + 8)) : 1;   // (zero-extended bytes, 0x000B2D73)
        t.flag64 = n.method == 215 ? static_cast<u8>(Get(a + 12)) : 1;
        t.radius = -1.0f;
        t.rec0 = foc(t.name0);
        tr.Refresh(t);                                // orig: vf08 0x000B7A40
        t.health = std::bit_cast<f32>(Get(a + 4));    // (raw dword, after the refresh)
        return t.vm;
    }
    default: return 0;
    }
}

void NotPorted(const NativeInfo& n) {
    static std::vector<const NativeInfo*> seen;
    for (const NativeInfo* s : seen)
        if (s == &n) return;
    seen.push_back(&n);
    std::fprintf(stderr, "NOT PORTED native %u:%u %s\n", n.cid, n.method, n.name[0] ? n.name : "(unnamed)");
}

game::TriggerRec* Record(VmAddr name) {               // orig: TriggerManager::FindOrCreate 0x000DB390
    return g_level && g_level->triggers ? g_level->triggers->FindOrCreate(Str(name)) : nullptr;
}

// The game natives ported so far. `a` = the first argument's address (the stack pointer after the pop), `r` = where
// the results go (the same address). Returns false for a native that is not ported.
bool Call(const NativeInfo& n, VmAddr a) {
    if (n.cid == 11) return CallCannon(n, a);         // the cannon class (table 0x000B6F94)
    if (n.cid == 14) return CallParticleDef(n, a);    // the particle-definition class (table 0x000B6ED4)
    if (n.cid == 16) return CallFireCircleDef(n, a);  // the fire-circle definition class (table 0x000B6E50)
    if (n.cid == 10 && n.method == 3) {               // FireWeapon(tank, mode, x, y, z) 0x000B6789 -> 0x00092350
        const VmAddr h = Get(a);                      // the object variable: its first dword is the object
        const u32 obj = h ? Get(h) : 0;
        if (!obj || obj != 0x07F10000u || !g_tank) return true;   // (Script_NullObjectError 0x000BA270: NOT PORTED)
        const ni::NiPoint3 p{GetF(a + 8), GetF(a + 12), GetF(a + 16)};
        g_tank->FireWeapon(static_cast<s32>(Get(a + 4)), p);
        return true;
    }
    if (n.cid != 0) return false;
    game::Camera* cam = g_level ? g_level->camera.get() : nullptr;
    switch (n.method) {
    // ---- explore_gaps_port.md 3 ----
    case 64: {                                        // FsSetTransform(name, from) 0x000DB410 (TM; result dropped)
        game::TriggerManager& tm = *g_level->triggers;
        game::TriggerRec* src = Record(Get(a + 4));   // FindOrCreate(from) first (EDI)
        game::TriggerRec* dst = Record(Get(a));       // then FindOrCreate(name) (ESI)
        if (src && dst) {
            const ni::NiMatrix3 R = tm.Rotation(*src);    // 0x000DB090
            const ni::NiPoint3 p = tm.Position(*src);     // 0x000DA7E0
            tm.SetRotation(*dst, R);                  // 0x000DB1E0
            tm.SetPosition(*dst, p);                  // 0x000DA8C0
        }
        return true;
    }
    case 65:                                          // FsSetTranslate(name, from) 0x000DB520 (TM; result dropped)
    case 69: {                                        // FsSetRotate(name, from) 0x000DB490 (TM; result dropped)
        game::TriggerManager& tm = *g_level->triggers;
        game::TriggerRec* src = Record(Get(a + 4));   // FindOrCreate(from) first (EDI)
        game::TriggerRec* dst = Record(Get(a));       // then FindOrCreate(name) (ESI)
        if (src && dst) {
            if (n.method == 69) tm.SetRotation(*dst, tm.Rotation(*src));   // 0x000DB090, 0x000DB1E0
            else tm.SetPosition(*dst, tm.Position(*src));                  // 0x000DA7E0, 0x000DA8C0
        }
        return true;
    }
    case 66:                                          // FsSetTranslate(name, x, y, z) 0x000DB580 (TM; result dropped)
    case 68:                                          // FsSetRelTranslate(name, x, y, z) 0x000DB4F0 (TM; dropped)
        if (game::TriggerRec* rec = Record(Get(a))) {   // FindOrCreate(name)
            const ni::NiPoint3 v{GetF(a + 4), GetF(a + 8), GetF(a + 12)};
            if (n.method == 66) g_level->triggers->SetPosition(*rec, v);   // 0x000DA8C0
            else g_level->triggers->Translate(*rec, v);                    // 0x000DA960
        }
        return true;
    case 289:                                         // FsCreateAirExplosion(name) 0x000C2F70 (thiscall S)
        game::ExplodeAtCharacterRecord(*g_level, Str(Get(a)));
        return true;
    case 80:                                          // FsDecNibblersPickedUp 0x000B0460
    case 81:                                          // FsIncNibblersPickedUp 0x000B0490
        if (n.method == 80 && !(g_level->nibblersPickedUp > 0)) return true;   // S+0x18C (JLE)
        if (game::Player* p = g_level->player; p && p->def) {   // (S+0x94)+0x10 (the player is not tested)
            g_level->nibblersPickedUp += n.method == 80 ? -1 : 1;
            p->cdef.ingameEvents.ResetIndexed(p->IngameContext());   // (D+0x28)->0x000F2980 (tail jump)
        }
        return true;
    case 129:                                         // (b) 0x000B0520: the pad's rumble off, P+0xA04 disabled = b
        if (input::InputSystem* in = g_level->input; in && in->activePad)   // DAT_002E7044
            (void)in->activePad->SetRumble(0.0f, 0.0f, true);   // Pad::SetRumble 0x0009B740(0, 0, 1)
        if (g_level->player) g_level->player->rumble.disabled = static_cast<u8>(Get(a));   // 0x0005EFF0 (MOVZX)
        return true;
    case 145: {                                       // FsIsLanguage(name) 0x000B01C0 (low byte, 0x000B19CB)
        const char* name = Str(Get(a));
        const char* lang = g_level->text ? g_level->text->Language().c_str() : nullptr;   // TextManager +0x18
        SetByte(a, name && lang && crt::StrICmp(name, lang) == 0 ? 1 : 0);
        return true;
    }
    case 159: {                                       // FsPlayerIsRolling 0x000B0560 (low byte)
        game::Player* p = g_level->player;
        SetByte(a, p && (p->IsAnimPlaying(0x61) || p->IsAnimPlaying(0x5E) || p->IsAnimPlaying(0x5F) ||
                         p->IsAnimPlaying(0x60)) ? 1 : 0);   // 0x00033E20
        return true;
    }
    case 84: {                                        // GetPlayerVelocity 0x000B04C0 (a vector: P+0x4E4)
        ni::NiPoint3 v{};                             // no player: zero (0x002F915C) and a debug print
        if (game::Player* p = g_level->player) v = p->velBefore;
        SetF(a, v.x);
        SetF(a + 4, v.y);
        SetF(a + 8, v.z);
        return true;
    }
    case 106:                                         // FsReleaseCharacterFromHold(name) 0x00036AC0
    case 107: {                                       // FsCharacterIsClimbing(name) 0x00036B10 (low byte)
        game::TriggerRec* rec = Get(a) ? Record(Get(a)) : nullptr;   // FindOrCreate (S+0xC0)
        auto* c = rec && rec->kind == 5 ? static_cast<game::Character*>(rec->target) : nullptr;
        game::Player* p = c && c->def->IsType(2) ? static_cast<game::Player*>(c) : nullptr;   // (+0x10)->vf0C(2)
        if (n.method == 106) {
            if (p && p->ladder) {                     // 0x00050310 (tail jump): off the ladder
                p->ladder = 0;                        // +0xB8C
                p->b93 = p->b94 = 0;                  // +0xB93 / +0xB94
                p->ladderGate = 0.0f;                 // +0xBE4 = 0
            }
        } else {
            SetByte(a, p && (p->OnLedge() || p->OnLadder()) ? 1 : 0);   // vf13C, then vf138
        }
        return true;
    }
    case 111: {                                       // FsSetScaleCharacter(name, scale) 0x00036F10
        game::TriggerRec* rec = Get(a) ? Record(Get(a)) : nullptr;
        if (!rec || rec->kind != 5 || !rec->target) return true;
        auto* c = static_cast<game::Character*>(rec->target);
        const f32 lo = std::bit_cast<f32>(0x3E99999Au);   // 0.3 [0x00293B18]
        f32 s = GetF(a + 4);                          // FLD
        if (s < lo) s = lo;                           // FCOM; TEST AH,5; JP (not below, or NaN: the next compare)
        else if (s > 20.0f) s = 20.0f;                // FCOM [0x00295AB4]; TEST AH,0x41; JNE (only '>')
        ni::NiNode* nd = c->GetNode();
        c->heightOffset = F(double(c->heightOffset) / nd->local.scale * s);   // FLD +0x4D0; FDIV [node+0x6C]; FMUL
        nd->local.scale = std::fabs(s);               // FABS; FSTP node+0x6C
        return true;
    }
    case 119: {                                       // FsCharacterCurrAnimLength(name) 0x000370D0 (fstp)
        double r = 0.0;                               // FLD [0x00293AB8]
        game::TriggerRec* rec = Get(a) ? Record(Get(a)) : nullptr;
        if (rec && rec->kind == 5 && rec->target)
            if (ni::NiSequence* s = static_cast<game::Character*>(rec->target)->curSeq)   // +0xA4
                r = game::SequenceDuration(s);        // 0x00036210 (tail jump)
        SetF(a, F(r));
        return true;
    }
    case 340:                                         // AIInstantAttack 0x00031A10("PLAYER") (thiscall S+0x114)
        if (g_ai.agent)                               // +0x10: the selected agent
            for (game::Character* c : g_level->characters)   // S+0x90 (0x000C26E0 walk)
                if (c->label && crt::StrICmp(c->label->c_str(), "PLAYER") == 0) {   // +0x34 (0x00293BD4)
                    g_ai.agent->Engage(*c);           // 0x0002CC80
                    break;
                }
        return true;
    case 78: Set(a, static_cast<u32>(g_level->pickupValue)); return true;   // FsGetCollPickedUp: inline S+0x188
    case 320: SetByte(a, 0); return true;             // (name) 0x000B1A4E: the result's low byte 0 (no call)
    case 77: {                                        // (def, waypoint, label, speed) 0x00076880: a thrown pickup
        const char* defName = Str(Get(a));
        const char* wpName = Str(Get(a + 4));
        if (!defName) return true;
        game::TriggerRec* rec = Record(Get(a + 4));   // FindOrCreate (S+0xC0)
        if (!rec || rec->kind != 2 || !rec->target) return true;   // a stage waypoint
        const auto* wp = static_cast<const game::StageWaypoint*>(rec->target);
        auto* pd = g_level->defs ? dynamic_cast<game::PickupDef*>(g_level->defs->Find(defName)) : nullptr;
        if (!pd || !pd->IsType(4)) return true;       // Session::FindDef 0x000C3F10; vf0C(4)
        const ni::NiMatrix3 M = ni::MakeYRotation(wp->yaw);   // 0x001030B0(+0xC)
        // the function's static sin / cos (guard 0x002E0878) of [0x0029CDA4] * [0x0029CDB4] (65 degrees), FSTP each
        static const f32 sn = F(std::sin(double(std::bit_cast<f32>(0x3C8EFA35u)) * 65.0f));
        static const f32 cs = F(std::cos(double(std::bit_cast<f32>(0x3C8EFA35u)) * 65.0f));
        const f32 speed = GetF(a + 12);
        const double k = double(cs) * speed;          // FLD cs; FMUL speed (kept)
        // (the first chunk of M: words 0 / 1 / 2 = M[0][0] / M[1][0] / M[2][0], c and s; y replaced by sn speed; the
        // port used M[0][2] = -s until 2026-10-19: every thrown pickup flew mirrored, O: v3 level1-1 PE_Keycard2)
        const ni::NiPoint3 v{F(double(M.m[0][0]) * k), F(double(sn) * speed), F(double(M.m[2][0]) * k)};
        for (const auto& st : g_level->stages) {      // Session::NextStage 0x000C26C0, 0x000CD9F0 (its waypoints)
            bool here = false;
            for (const auto& w : st->waypoints) here = here || &w == wp;
            if (!here) continue;
            const ni::NiPoint3 pos = wp->position;
            const ni::NiMatrix3 R;                    // the identity 0x002D7820
            const bool keyed = pd->value > 0;         // def+0x2C (SETG)
            if (keyed && wpName) {                    // (S+0x1FC)->0x00077730(waypoint): taken already
                auto [it, inserted] = g_level->nibblerStates.try_emplace(wpName, u8{0});
                if (!inserted && it->second) return true;
            }
            auto* o = dynamic_cast<game::Pickup*>(pd->Spawn(*g_level, st.get(), pos, R, Str(Get(a + 8))));   // vf14
            if (!o) return true;
            if (keyed) o->keyName = wpName ? wpName : "";   // 0x000765B0 (+0x70 = strdup)
            o->rising = 1;                            // +0x64
            o->fallDt = 0;                            // 0x00076550: +0x5C / +0x58 = 0, +0x4C = v, +0x60 = 0, +0x38 = 2
            o->airTime = 0;
            o->velocity = v;
            o->bounces = 0;
            o->state = game::Pickup::kFalling;
            return true;
        }
        return true;
    }
    case 138:                                         // FsRearmDestructor(name) 0x00036BF0 -> vf114 (tail jump)
    case 139: {                                       // FsFireDestructorMissile(name, target) 0x00036B80 -> 0x000823E0
        game::TriggerRec* rec = Get(a) ? Record(Get(a)) : nullptr;   // FindOrCreate (S+0xC0)
        if (!rec || rec->kind != 5 || !rec->target) return true;
        auto* c = static_cast<game::Character*>(rec->target);
        game::TriggerRec* t = nullptr;
        if (n.method == 139) {                        // (the target's record before the class test)
            if (!Get(a + 4) || !(t = Record(Get(a + 4)))) return true;
        }
        if (crt::StrICmp(c->def->name.c_str(), "destructor") != 0) return true;   // D+0x10 (0x00295A94)
        auto* d = static_cast<game::Destructor*>(c);
        if (n.method == 138) d->Rearm();
        else d->FireMissile(*t);
        return true;
    }
    case 306:                                         // (name, x, y, z) 0x000C4710: a "fireball" at name + (x, y, z)
    case 307: {                                       // FsCreateLaser(name) 0x000C47A0: a "qwerty_laser" at name
        game::TriggerRec* rec = Record(Get(a));       // FindOrCreate first, then Session::FindDef
        game::Def* d = g_level->defs ? g_level->defs->Find(n.method == 306 ? "fireball" : "qwerty_laser") : nullptr;
        if (!d || !rec) return true;                  // (0x002A2B00 / 0x002A2AD8)
        ni::NiPoint3 p = g_level->triggers->Position(*rec);   // 0x000DA7E0
        if (n.method == 306)                          // FLD arg; FADD p; FSTP (x, y, z)
            p = {F(double(GetF(a + 4)) + p.x), F(double(GetF(a + 8)) + p.y), F(double(GetF(a + 12)) + p.z)};
        const ni::NiMatrix3 r = g_level->triggers->Rotation(*rec);   // 0x000DB090
        (void)d->Spawn(*g_level, g_level, p, r, nullptr);   // vf14(S, &p, &r, 0)
        return true;
    }
    case 160:                                         // FsReplayLevel 0x000C2320: App+0xAC = S+0xC, DAT_002EAA39 = 1
        if (g_level->setAppLevelName) g_level->setAppLevelName(g_level->name.c_str());   // (DAT_002E7C24) 0x000A23D0
        else NotPortedText("FsReplayLevel: no App (the App's level name 0x000A23D0)");
        g_level->carry->replay = true;
        return true;
    case 218: {                                       // FsCleanRoom(name) 0x000C5240 (thiscall S)
        const char* name = Str(Get(a));
        if (g_level->roomGroup && name)               // (a NULL name faults in the original's inline strcmp)
            for (const auto& room : g_level->roomGroup->roomList) {   // S+0x20 +0xE4
                if (!room || room->name.empty() || std::strcmp(room->name.c_str(), name) != 0) continue;
                while (!room->fixtures.empty())       // +0x140 count: the head (0x000C3600) detached (0x0011EEA0)
                    room->DetachFixture(room->fixtures.front().Get());
            }
        return true;
    }
    case 330: {                                       // (a, b) 0x000B0810 (cdecl, low byte): the line of sight a -> b
        const ni::NiPoint3 A{GetF(a), GetF(a + 4), GetF(a + 8)}, B{GetF(a + 12), GetF(a + 16), GetF(a + 20)};
        LineOfSightCache& c = g_losCache;             // FCOMP each word; TEST AH,0x44; JP (NaN: a new test)
        if (!(c.a.x == A.x && c.a.y == A.y && c.a.z == A.z && c.b.x == B.x && c.b.y == B.y && c.b.z == B.z)) {
            ni::NiPoint3 d{F(double(B.x) - A.x), F(double(B.y) - A.y), F(double(B.z) - A.z)};
            f32 t = 1.0f;
            const bool hit = static_cast<bool>(ni::RayCast(A, d, t, true, 3));   // 0x000FB780(&a, &d, &t, 1, 3)
            c.a = A;
            c.b = B;
            c.clear = hit ? 0 : 1;                    // NEG; SBB; INC
        }
        SetByte(a, c.clear);
        return true;
    }
    case 63: {                                        // FsLookAt(name, x, y, z) 0x000DB760 (TM; result dropped)
        game::TriggerRec* rec = Record(Get(a));       // FindOrCreate
        if (!rec) return true;
        const ni::NiPoint3 p = g_level->triggers->Position(*rec);   // 0x000DA7E0
        // FLD x; FSUB; FSTP / FLD y; FSUB (kept); FLD z; FSUB; FSTP z; FADD [0x00293C24] (5.0); FSTP y
        ni::NiPoint3 d{F(double(GetF(a + 4)) - p.x), F((double(GetF(a + 8)) - p.y) + 5.0f),
                       F(double(GetF(a + 12)) - p.z)};
        (void)ni::Unitize(d);                         // 0x000116A0
        const ni::NiPoint3 up = d.y > 0.9f ? ni::NiPoint3{0.0f, 0.0f, 1.0f} : ni::NiPoint3{0.0f, 1.0f, 0.0f};
        ni::NiPoint3 x = ni::Cross(up, d);            // [0x00293C08]; 0x002D77F8 / 0x002D77EC (TEST AH,0x41; JNE)
        (void)ni::Unitize(x);
        const ni::NiPoint3 y = ni::Cross(x, d);
        ni::NiMatrix3 R;                              // the chunks d, y, -x (FCHS) = the columns
        R.m[0][0] = d.x, R.m[1][0] = d.y, R.m[2][0] = d.z;
        R.m[0][1] = y.x, R.m[1][1] = y.y, R.m[2][1] = y.z;
        R.m[0][2] = -x.x, R.m[1][2] = -x.y, R.m[2][2] = -x.z;
        if (rec->kind == 2) {                         // a waypoint: its cached matrix +0x14 (new(0x30) first) replaced
            rec->waypointRotation = R;
            rec->rotationCached = true;
        } else {
            g_level->triggers->SetRotation(*rec, R);  // 0x000DB1E0
        }
        return true;
    }
    case 304: {                                       // (static, node) 0x000C3150 (thiscall S): a static follows a node
        const char* obj = Str(Get(a));
        const char* name = Str(Get(a + 4));
        if (!name || !obj || !g_level->scene) return true;
        // (S+0x18)->vf54(node) twice, the second through DynamicCast (NiNode RTTI 0x002F9230)
        auto* nd = dynamic_cast<ni::NiNode*>(game::GetObjectByName(g_level->scene.Get(), name));
        if (!nd) return true;
        game::TriggerRec* rec = Record(Get(a));       // FindOrCreate(static)
        if (!rec || !rec->target || rec->kind != 1) return true;
        auto* o = static_cast<game::GameObject*>(rec->target);
        if (!o->def || !o->def->IsType(0x1000)) return true;   // (+0x10)->vf0C(0x1000): a Static
        static_cast<game::Static*>(o)->SetFollowTarget(nd);   // 0x000956C0
        return true;
    }
    case 162: return true;                            // FsDemoEndLevelNow: the dispatcher's empty case (0x000B67EF)
    case 378: {                                       // AIIsPlayerFrozen 0x000319D0 (thiscall S+0x114; low byte)
        game::AiAgent* keep = g_ai.agent;             // +0x10 kept (EDI); AISelect("PLAYER") 0x000318F0 clears +0x10 /
        g_ai.agent = nullptr;                         // +0x14 (the sequence stays cleared)
        g_ai.seq = nullptr;
        for (game::AiAgent* ag : g_level->aiAgents)   // the first agent whose character label is "PLAYER" (0x00293BD4)
            if (ag->character && ag->character->label && crt::StrICmp(ag->character->label->c_str(), "PLAYER") == 0) {
                g_ai.agent = ag;
                break;
            }
        game::AiAgent* found = g_ai.agent;
        g_ai.agent = keep;
        SetByte(a, found && found->event.kind == 9 ? 1 : 0);   // A+0x1D8 == 9
        return true;
    }
    case 123:                                         // FsPullLever(name, anim, clip) 0x0003AB40
    case 124:                                         // FsPushButton 0x0003AB60
    case 125: {                                       // FsTurnWheel 0x0003AB80: all 0x0003AAC0(name, <seq>, anim, clip)
        static const char* const kSeq[] = {"pulllever", "press_button", "wheel_turn"};   // 0x00295D30 / 3C / 4C
        const VmAddr name = Get(a), anim = Get(a + 4), clip = Get(a + 8);
        if (!name || !clip) return true;
        game::TriggerRec* rec = Record(name);         // FindOrCreate (S+0xC0)
        if (!rec || rec->kind != 5 || !rec->target) return true;
        auto* c = static_cast<game::Character*>(rec->target);
        ni::NiSequence* s = c->FindSequence(kSeq[n.method - 123]);   // 0x00039BA0
        if (!s) return true;
        c->PlayAnimation(s, true, false, true, 0.0f); // 0x0003A130(seq, 1, 0, 1, 0)
        if (g_level->animations && anim)              // FindByName 0x000BA5B0 (_stricmp with a NULL name faults)
            if (level::LevelAnimation* la = g_level->animations->FindByName(Str(anim)))
                la->Play(Str(clip), false, false);    // 0x000BB220(clip, 0, 0)
        if (auto* p = dynamic_cast<game::Player*>(c)) p->locked = 1;   // +0xB8E = 1 (the interaction state)
        else NotPortedText("FsPullLever / FsPushButton / FsTurnWheel on a non-player (+0xB8E is past the object)");
        return true;
    }
    case 313: {                                       // FsObjAngle(a, b) 0x000DB630 (TM; FSTP result)
        game::TriggerManager& tm = *g_level->triggers;
        game::TriggerRec* ra = Record(Get(a));        // FindOrCreate(a) (EDI), then (b) (ESI)
        game::TriggerRec* rb = Record(Get(a + 4));
        f32 r = 0.0f;                                 // FLD [0x00293AB8]
        if (ra && rb) {
            const ni::NiMatrix3 R = tm.Rotation(*ra); // 0x000DB090 (esp+0x20)
            const ni::NiPoint3 pa = tm.Position(*ra); // esp+0x14
            const ni::NiPoint3 pb = tm.Position(*rb); // esp+0x08
            ni::NiPoint3 d{F(double(pb.x) - pa.x), 0.0f, F(double(pb.z) - pa.z)};
            (void)ni::Unitize(d);                     // 0x000116A0 (length dropped)
            r = F((double(d.z) * R.m[2][0] + double(d.x) * R.m[0][0]) + double(d.y) * 0.0f);   // chunk 0 = column 0
        }
        SetF(a, r);
        return true;
    }
    case 314: {                                       // FsObjVector(a, b) 0x000DB6E0 (TM; FSTP result)
        game::TriggerManager& tm = *g_level->triggers;
        game::TriggerRec* ra = Record(Get(a));        // FindOrCreate(a) (EDI), then (b) (ESI)
        game::TriggerRec* rb = Record(Get(a + 4));
        f32 r = 0.0f;                                 // FLD [0x00293AB8]
        if (ra && rb) {
            const ni::NiMatrix3 A = tm.Rotation(*ra); // 0x000DB090 (esp+0x40)
            const ni::NiMatrix3 B = tm.Rotation(*rb); // (esp+0x10)
            // the two chunk-0 vectors (column 0, the local X axes): z first, then y, then x
            r = F((double(B.m[2][0]) * A.m[2][0] + double(B.m[1][0]) * A.m[1][0]) + double(B.m[0][0]) * A.m[0][0]);
        }
        SetF(a, r);
        return true;
    }
    case 118: {                                       // FsCharacterAnimLength(name, anim) 0x0003AA60 (FSTP result)
        double r = 0.0;
        if (Get(a) && Get(a + 4))
            if (game::TriggerRec* rec = Record(Get(a)); rec && rec->kind == 5 && rec->target)
                if (ni::NiSequence* s = static_cast<game::Character*>(rec->target)->FindSequence(Str(Get(a + 4))))
                    r = game::SequenceDuration(s);    // 0x00039BA0, 0x00036210 ((end - begin) / frequency)
        SetF(a, F(r));
        return true;
    }
    // ---- the sound natives (dispatcher 0x000B3F73..0x000B4200; game/sound/GameSound.h): a bool argument is the
    // slot's low byte (MOVZX), FsIsPlayingSample / FsIsPlayingSound write only the result's low byte (0x000B19CB) ----
    case 85:                                          // FsLoadSound(path, streaming) -> Sound_Load 0x000C9860
        Set(a, g_level->loadSound ? g_level->loadSound(Str(Get(a)) ? Str(Get(a)) : "", Get(a + 4) & 0xFF) : 0);
        return true;
    case 86: SetByte(a, g_level->Sound().HasVoices(Get(a)) ? 1 : 0); return true;   // FsIsPlayingSample 0x000C98C0
    case 87:                                          // FsIsPlayingSound -> Voice_Exists 0x000C9900
        SetByte(a, g_level->Sound().VoiceExists(static_cast<s32>(Get(a))) ? 1 : 0);
        return true;
    case 88:                                          // FsPlaySound -> Sound_PlaySFX 0x000C99A0
    case 90:                                          // FsPlaySFX (the same call; object_gaps.md 18.2)
        Set(a, static_cast<u32>(g_level->Sound().PlaySfx(Get(a), (Get(a + 4) & 0xFF) != 0)));
        return true;
    case 89:                                          // FsPlayResume -> Voice_Play2D 0x000C9970
        g_level->Sound().VoicePlay2D(static_cast<s32>(Get(a)), (Get(a + 4) & 0xFF) != 0);
        return true;
    case 91: {                                        // FsPlay3DSFX(sample, name, loop) 0x000CAAA0
        const VmAddr name = Get(a + 4);
        Set(a, static_cast<u32>(g_level->Sound().PlayAtTarget(Get(a), Str(name), (Get(a + 8) & 0xFF) != 0,
                                                             [name](sound::SoundTarget& t) {
            game::TriggerRec* rec = Record(name);     // TriggerManager::FindOrCreate 0x000DB390
            if (!rec) return false;
            t.record = rec;
            t.node = g_level->triggers->Node(*rec);   // 0x000DA5F0
            t.position = g_level->triggers->Position(*rec);   // 0x000DA7E0
            return true;
        })));
        return true;
    }
    case 92: Set(a, static_cast<u32>(g_level->Sound().PlayMusic(Get(a), true))); return true;   // 0x000C9A00(s, 1)
    case 93:                                          // FsPlaySpeech -> Sound_PlaySpeech 0x000C9A60
        Set(a, static_cast<u32>(g_level->Sound().PlaySpeech(Get(a), (Get(a + 4) & 0xFF) != 0)));
        return true;
    case 94: g_level->Sound().StopAll(); return true;                        // FsStopAllSounds 0x000C9B40
    case 95: g_level->Sound().StopVoice(static_cast<s32>(Get(a))); return true;   // FsStopSound -> Voice_Stop 0x000C9B10
    case 96: g_level->Sound().StopSample(Get(a)); return true;               // FsStopSampleAll 0x000C9B70
    case 97: g_level->Sound().SetVolume(static_cast<s32>(Get(a)), GetF(a + 4)); return true;   // 0x000C9C70
    case 98: g_level->Sound().SetPitch(static_cast<s32>(Get(a)), GetF(a + 4)); return true;    // 0x000C9BD0
    case 99: g_level->Sound().SetPan(static_cast<s32>(Get(a)), GetF(a + 4)); return true;      // 0x000C9C00
    case 100: g_level->Sound().SetMaxDistance(static_cast<s32>(Get(a)), GetF(a + 4)); return true;   // 0x000C9CA0
    case 101: g_level->Sound().SetMinDistance(static_cast<s32>(Get(a)), GetF(a + 4)); return true;   // 0x000C9CF0
    case 102: g_level->Sound().SetReverb(Str(Get(a))); return true;          // FsSetReverb 0x000C9770
    case 103: {                                       // FsStopAllOnTargetable(name) 0x000CAC80
        const VmAddr name = Get(a);                   // (FindOrCreate: a record is made for an unknown name)
        g_level->Sound().StopAllOnTarget(Str(name), [name]() -> const void* { return Record(name); });
        return true;
    }
    case 168: g_level->pausingEnabled = 1; return true;   // FsEnablePausing (inline 0x000B3675): Ingame +0x19 = 1
    case 386:                                         // HUDShow 0x000B0A10 -> 0x000E5A60(1): every HUD element's vf14(1)
        if (g_level->hud) g_level->hud->SetActiveAll(true);
        return true;
    case 140: {                                       // FsFirePrimary(name, target, anim) 0x00036C50
        if (!Get(a)) return true;
        game::TriggerRec* rec = Record(Get(a));
        if (!rec || rec->kind != 5 || !rec->target || !Get(a + 4)) return true;
        game::TriggerRec* t = Record(Get(a + 4));
        auto* c = static_cast<game::Character*>(rec->target);
        if (t && c->type == 0xE)                      // +0x774 == 0xE: the Destructor
            static_cast<game::Destructor*>(c)->FirePrimary(Str(Get(a + 8)), *t);   // 0x000824F0(anim, rec)
        return true;
    }
    case 141: {                                       // FsFireCannon(name, x, y, z) 0x00036CC0
        if (!Get(a)) return true;
        game::TriggerRec* rec = Record(Get(a));
        if (!rec || rec->kind != 5 || !rec->target) return true;   // +0x18 == 5, +0xC
        auto* c = static_cast<game::Character*>(rec->target);
        if (c->type != 5) return true;                // +0x774 == 5
        ni::NiPoint3 dir{GetF(a + 4), GetF(a + 8), GetF(a + 12)};   // (the argument copy on the C stack)
        (void)ni::Unitize(dir);                       // 0x000116A0 (FSTP ST0)
        // vf114: the igner boss's slot (no other class has one; the port reports another type-5 character)
        if (auto* boss = dynamic_cast<game::IgnerBoss*>(c)) boss->AimAndFire(dir);
        else NotPorted(n);
        return true;
    }
    case 248: if (cam) cam->PushTransform(); return true;    // FsCamPushTransform 0x000132F0 (S+0x8C, no test)
    case 247: if (cam) cam->PopTransform(); return true;     // FsCamPopTransform 0x00011310
    case 249: if (cam) cam->stackTop = -1; return true;      // FsCamFlushTransform 0x00011370
    case 261:                                         // FsFadeOut(r, g, b, time) 0x00013060 (S+0x8C)
        if (cam) cam->FadeOut(GetF(a), GetF(a + 4), GetF(a + 8), GetF(a + 12));
        return true;
    case 82: {                                        // FsPlayerSnapToNode(name, dist) 0x000B0AF0
        const f32 dist = GetF(a + 4);                 // (0x000B0420 debug prints: no-ops)
        game::TriggerManager& tm = *g_level->triggers;
        game::TriggerRec* rec = tm.Find(Str(Get(a)));   // 0x000DAFA0 (no record is created)
        game::Player* p = g_level->player;            // S+0x94
        if (!rec || !p) return true;
        const ni::NiPoint3 wp = p->GetWorldPosition();   // vf34 (only y is kept)
        ni::NiPoint3 pos = tm.Position(*rec);         // 0x000DA7E0
        const ni::NiPoint3 f = ni::MulVector(tm.Rotation(*rec), {1.0f, 0.0f, 0.0f});   // 0x000DB090, 0x00011770
        pos.y = wp.y;
        pos.x = F(double(pos.x) + double(f.x) * dist);
        pos.z = F(double(pos.z) + double(f.z) * dist);
        p->SetPosition(pos);                          // vf1C
        const ni::NiPoint3 nf{-f.x, -f.y, -f.z};      // FCHS; FST
        const ni::NiPoint3 Y{0.0f, 1.0f, 0.0f};       // UNIT_Y 0x002D77EC
        ni::NiPoint3 c{F(double(Y.y) * nf.z - double(Y.z) * nf.y), F(double(Y.z) * nf.x - double(nf.z) * Y.x),
                       F(double(nf.y) * Y.x - double(Y.y) * nf.x)};
        (void)ni::Unitize(c);                         // 0x000116A0
        ni::NiMatrix3 m;
        for (int j = 0; j < 3; ++j) {                 // columns: -f, UNIT_Y, -c
            const f32 col0[3] = {nf.x, nf.y, nf.z}, col1[3] = {Y.x, Y.y, Y.z}, col2[3] = {-c.x, -c.y, -c.z};
            m.m[j][0] = col0[j], m.m[j][1] = col1[j], m.m[j][2] = col2[j];
        }
        p->SetRotation(m);                            // vf24
        return true;
    }
    case 175:                                         // FsPrintAt / FsDebugPrint overloads: the debug printf 0x000B0420
    case 176: case 177: case 178: case 179: case 180: case 181: case 182: case 183: case 184:
        return true;                                  //  (a bare RET in the retail build)
    // ---- particle definitions and emitters (natives_level1-1_port.md 2-5) ----
    case 278: {                                       // FsPartCopyDef(source, name) 0x000C7540
        u32 r = 0;
        const char* srcName = Get(a) ? Str(Get(a)) : nullptr;
        const char* newName = Get(a + 4) ? Str(Get(a + 4)) : nullptr;
        // IsType(0x4000) && vf24: sprite emitter definitions only (vf24 is false for MeshEmitterDef)
        auto* src = srcName && g_level->defs ? dynamic_cast<game::SpriteEmitterDef*>(g_level->defs->Find(srcName))
                                             : nullptr;
        if (src && newName && !g_level->defs->Find(newName)) {   // (a NULL name faults in the original)
            void* obj = mem::OperatorNew(0x114);       // orig: new(0x114), the word the script keeps
            Set(mem::ToVmAddr(obj), 0x0029C3E0);       // (the vtable word: the trace shows the pointer as hex)
            const u32 len = static_cast<u32>(std::strlen(newName)) + 1;
            std::memcpy(mem::OperatorNew(len), newName, len);   // orig: Def::Def 0x000962E0, the name copy
            auto d = std::make_unique<game::SpriteEmitterDef>();   // orig: copy ctor 0x0006E240
            d->name = newName;
            d->config = src->config;                   // +0x28..+0x60 (0x0006DA40)
            d->texturing = nullptr;                    // +0xA0 is not copied
            d->spriteTexturing = src->spriteTexturing; // +0xA4, shared (AddRef)
            d->spriteConfig = src->spriteConfig;       // +0xA8..+0xDC
            d->Reset();                                // orig: 0x0006CA00 then 0x0006D1C0: current = configured
            g_partDefs[mem::ToVmAddr(obj)] = static_cast<game::EmitterDef*>(g_level->defs->Add(std::move(d)));
            r = mem::ToVmAddr(obj);
        }
        Set(a, r);
        return true;
    }
    case 279: {                                       // FsPartChangeInstDef(label, def) 0x000C4900
        if (!Get(a + 4)) return true;
        game::GameObject* old = FindEmitterByLabel(Get(a) ? Str(Get(a)) : nullptr);
        if (!old || !dynamic_cast<game::SpriteEmitterDef*>(old->def)) return true;   // old->def->vf24()
        game::EmitterDef* def = PartDef(Get(a + 4));
        if (!def) {
            NotPortedText("FsPartChangeInstDef with a definition FsPartCopyDef did not create");
            return true;
        }
        const ni::NiPoint3 pos = old->GetWorldPosition();   // vf34 0x0004EFF0
        const ni::NiPoint3 dir = static_cast<game::EmitterInstance*>(old)->direction;   // +0x84
        game::GameObject* o = def->SpawnEffect(*g_level, pos, dir);   // vf20 (Init draws the particles' rand())
        if (o) o->SetName(Str(Get(a)));               // 0x000968C0 (no NULL check in the original)
        delete old;                                   // vf00(1)
        return true;
    }
    case 284: {                                       // FsSetParticlesEnable(label, on) 0x000C3070
        const char* label = Get(a) ? Str(Get(a)) : nullptr;
        if (!label) return true;                      // (_stricmp with NULL faults in the original)
        for (game::GameObject* o = g_level->NextObject(nullptr); o; o = g_level->NextObject(o))
            if (o->label && o->def->IsType(0x4000) && crt::StrICmp(o->label->c_str(), label) == 0) {
                static_cast<game::EmitterInstance*>(o)->emitting = static_cast<u8>(Get(a + 4));   // +0xB4
                break;
            }
        return true;
    }
    case 272:                                         // 0x000C0070: the global light source, type 4 (script_triggers.md 4.6)
    case 273:                                         // 0x000C0240: the same with type 3
        // AddLight twice (the first source dies), the second kept at the manager's +0x10; then the second vector
        // (args 3..5) into the global ambient DAT_002EA828 (0x000C0212..0x000C0227), read by the characters' step
        // 0x000C0410. (No null test of DAT_002EA824 in the original.)
        if (g_level->lightManager)
            g_level->lightManager->SetGlobalLight(*g_level, {GetF(a), GetF(a + 4), GetF(a + 8)},
                                                  {GetF(a + 12), GetF(a + 16), GetF(a + 20)}, n.method == 272 ? 4 : 3);
        else
            g_level->globalAmbient = {GetF(a + 12), GetF(a + 16), GetF(a + 20)};
        return true;
    case 305: {                                       // FsCreateFireball(name) 0x000C46A0 (fireball_port.md 1)
        game::TriggerRec* rec = Record(Get(a));       // orig: TriggerManager::FindOrCreate 0x000DB390
        game::Def* d = g_level->defs ? g_level->defs->Find("fireball") : nullptr;   // orig: Session::FindDef 0x000C3F10
        if (d && rec) {
            const ni::NiPoint3 pos = g_level->triggers->Position(*rec);   // orig: 0x000DA7E0
            const ni::NiMatrix3 rot = g_level->triggers->Rotation(*rec);  // orig: 0x000DB090
            (void)d->Spawn(*g_level, g_level, pos, rot, nullptr);   // orig: def vf14(S, &pos, &rot, 0) (result dropped)
        }
        return true;
    }
    case 308:                                         // FsLevel5SpawnWasp 0x000C4810 (simplenpc_moveable_port.md 1.2)
        game::SpawnWasp(*g_level);
        return true;
    case 309: {                                       // FsLevel5SpawnWaspBox(name) 0x000C48E0
        game::g_waspBox = Str(Get(a));                // DAT_002EAA44 = name; 308; DAT_002EAA44 = 0
        game::SpawnWasp(*g_level);
        game::g_waspBox = nullptr;
        return true;
    }
    case 310:                                         // FsLevel5DeleteWasps 0x000C3030
        game::DeleteWasps(*g_level);
        return true;
    case 293:                                         // FsCreateSmoke(size, name) 0x000C4A20 (object_gaps.md 9)
    case 294: {                                       // FsCreateWood(name) 0x000C4B40
        const bool smoke = n.method == 293;
        game::TriggerRec* rec = Record(Get(smoke ? a + 4 : a));   // orig: TriggerManager::FindOrCreate 0x000DB390
        if (!rec) return true;
        const ni::NiMatrix3 rot = g_level->triggers->Rotation(*rec);  // orig: 0x000DB090 (first)
        const ni::NiPoint3 pos = g_level->triggers->Position(*rec);   // orig: 0x000DA7E0
        const auto& m = rot.m;                        // dir = rot * (0, 1, 0) [0x002D77EC], chunk order 2, 1, 0
        const ni::NiPoint3 dir{F((0.0 * m[0][2] + 1.0 * m[0][1]) + 0.0 * m[0][0]),
                               F((0.0 * m[1][2] + 1.0 * m[1][1]) + 0.0 * m[1][0]),
                               F((0.0 * m[2][2] + 1.0 * m[2][1]) + 0.0 * m[2][0])};
        static const char* const kSmoke[6] = {"effect_Smoke_XS", "effect_Smoke_S", "effect_Smoke_M",   // 0x002A2BD8..
                                              "effect_Smoke_L", "effect_Smoke_XL", "effect_Smoke_XXL"};
        const u32 size = Get(a);
        if (smoke && size > 5) return true;           // (any other size: nothing)
        game::Def* d = g_level->defs ? g_level->defs->Find(smoke ? kSmoke[size] : "effect_wood") : nullptr;   // 0x000C3F10
        if (!d) return true;                          // (the original calls through a null definition: UB guard)
        if (!smoke && !d->IsType(0x4000)) return true;   // FsCreateWood only: vf0C(0x4000)
        (void)d->SpawnEffect(*g_level, pos, dir);     // vf20(&pos, &dir)
        return true;
    }
    case 303: {                                       // (unnamed) 0x000C30E0: (static definition, texture, unused)
        const char* defName = Get(a) ? Str(Get(a)) : nullptr;
        const char* texName = Get(a + 4) ? Str(Get(a + 4)) : nullptr;
        if (!defName || !g_level->defs) return true;  // (_stricmp with NULL faults in the original)
        std::vector<game::StaticDef*> hits;           // (the appended RippleDefs never match: collecting first is equal)
        for (const auto& d : g_level->defs->Defs())
            if (!d->name.empty() && d->IsType(0x1000) && crt::StrICmp(d->name.c_str(), defName) == 0)
                if (auto* s = dynamic_cast<game::StaticDef*>(d.get())) hits.push_back(s);
        for (game::StaticDef* s : hits) {             // orig: 0x000958E0
            auto r = std::make_unique<game::RippleDef>();   // orig: new(0x34) + ctor 0x00071710
            r->name = "Ripple" + s->name;
            r->texture = texName ? g_level->FindTexture(texName) : nullptr;
            r->value1C = 0.0f;
            s->ripple = static_cast<game::RippleDef*>(g_level->defs->Add(std::move(r)));   // +0x3C (the old one leaks)
        }
        return true;
    }
    case 72: {                                        // FsCreateInstance(label, definition, record) 0x000C44D0
        const char* defName = Get(a + 4) ? Str(Get(a + 4)) : nullptr;
        if (!defName || !Get(a + 8)) return true;
        game::Def* d = g_level->defs ? g_level->defs->Find(defName) : nullptr;   // Session::FindDef 0x000C3F10
        if (!d) return true;
        game::TriggerRec* r = Record(Get(a + 8));     // (S+0xC0)->FindOrCreate
        if (!r) return true;
        const ni::NiMatrix3 rot = g_level->triggers->Rotation(*r);   // 0x000DB090 (first)
        ni::NiPoint3 pos = g_level->triggers->Position(*r);          // 0x000DA7E0
        // the room (S+0x20 FindRoomContaining 0x0011F1D0, no null test); none: the point 0.1 higher, which is also
        // the spawn position then ([0x00293AB0], stored back into the position)
        ni::NiRoom* room = g_level->roomGroup ? g_level->roomGroup->FindRoomContaining(pos) : nullptr;
        if (!room) {
            pos.y = F(double(pos.y) + 0.1f);
            room = g_level->roomGroup ? g_level->roomGroup->FindRoomContaining(pos) : nullptr;
        }
        game::Owner* owner = g_level;                 // (no stage contains the room: S itself)
        for (const auto& st : g_level->stages)        // S+0x58 (next +0x10): the first stage with the room
            if (st->ContainsRoom(room)) {             // 0x000CE410
                owner = st.get();                     // (stage + 4: its Owner part)
                break;
            }
        (void)d->Spawn(*g_level, owner, pos, rot, Get(a) ? Str(Get(a)) : nullptr);   // vf14(owner, &pos, &rot, label)
        return true;
    }
    case 73: {                                        // FsDeleteInstance(label) 0x000C2CC0
        const char* label = Get(a) ? Str(Get(a)) : nullptr;
        if (!label) return true;
        // the first object with the label (+0x34, _stricmp) among S's own objects, else among each stage's (S+0x58
        // order); vf00(1)
        auto find = [&](const game::Owner& o) -> game::GameObject* {
            for (game::GameObject* x = o.NextObject(nullptr); x; x = o.NextObject(x))
                if (x->label && crt::StrICmp(x->label->c_str(), label) == 0) return x;
            return nullptr;
        };
        game::GameObject* o = find(*g_level);
        for (size_t i = 0; !o && i < g_level->stages.size(); ++i) o = find(*g_level->stages[i]);
        delete o;
        return true;
    }
    case 76: {                                        // (unnamed) 0x000C4630: (target, definition, label)
        game::TriggerRec* r = Get(a) ? Record(Get(a)) : nullptr;
        if (!r || !r->target) return true;
        game::Def* d = Get(a + 4) && g_level->defs ? g_level->defs->Find(Str(Get(a + 4))) : nullptr;
        if (!d) return true;
        const char* label = Get(a + 8) ? Str(Get(a + 8)) : nullptr;
        if (r->kind == 6) {                           // vf58 Destructable::AddDrop 0x000634C0
            static_cast<game::Destructable*>(static_cast<game::GameObject*>(r->target))->AddDrop(d, label);
        } else if (r->kind == 5) {                    // vfEC: GenericNpc::AddDrop 0x00047A40
            auto* c = static_cast<game::Character*>(r->target);
            if (auto* npc = dynamic_cast<game::GenericNpc*>(c)) npc->AddDrop(d, label);
            else NotPortedText("native 76 on a character that is not a GenericNpc (vfEC: a player's pre-hook)");
        }
        return true;
    }
    case 74: {                                        // FsSetPickupPickupable(name, b) 0x000CE4F0
        if (!Get(a)) return true;
        game::TriggerRec* r = Record(Get(a));
        if (r && r->kind == 1 && r->target) {
            auto* o = static_cast<game::GameObject*>(r->target);
            if (auto* p = dynamic_cast<game::Pickup*>(o)) p->SetPickupable(Get(a + 4) != 0 ? 1 : 0);   // vf54 0x00074890 (setne)
            else NotPortedText("FsSetPickupPickupable on a kind-1 object that is not a pickup (vf54)");
        }
        return true;
    }
    case 70:                                          // FsHideTarget(name) 0x000DB950 (the mirror of 71; AL dropped)
    case 71: {                                        // FsShowTarget(name) 0x000DBAD0
        const char* name = Get(a) ? Str(Get(a)) : nullptr;
        if (!name) return true;
        game::TriggerRec* r = Record(Get(a));
        ni::NiAVObject* nd = nullptr;
        if (r && r->target) {
            if (r->kind == 1 || r->kind == 5) nd = static_cast<game::GameObject*>(r->target)->GetNode();   // vf30
        } else if (g_level->scene) {
            nd = game::GetObjectByName(g_level->scene.Get(), name);   // S+0x18 vf54
        }
        if (nd && n.method == 70) nd->flags = static_cast<u16>(nd->flags | ni::kNiAVHidden);   // +0xB0 |= 1
        else if (nd) nd->flags = static_cast<u16>(nd->flags & ~ni::kNiAVHidden);   // +0xB0 &= ~1
        return true;
    }
    case 151:                                         // FsCancelAllDialogues 0x000D7D70 (DAT_002EBDAC)
        g_level->dialogue->CancelAll();
        return true;
    case 161:                                         // FsChangeLevel(name) 0x000B05B0 (dispatcher 0x000B371C: pops 4)
        if (g_level->changeLevel) g_level->changeLevel(Get(a) ? Str(Get(a)) : nullptr);   // Session::ChangeLevel
        return true;
    case 165:                                         // FsPrepareCutscene(name) 0x000D5810
        if (g_level->intermission) g_level->intermission->Prepare(Get(a) ? Str(Get(a)) : nullptr);
        return true;
    case 166:                                         // FsStartCutscene 0x000D4950
        if (g_level->intermission) g_level->intermission->Start();
        return true;
    case 315:                                         // FsPortalDisable(name) 0x000C4050 (enable 0)
    case 316: {                                       // FsPortalEnable(name) 0x000C4050 (enable 1)
        const char* name = Get(a) ? Str(Get(a)) : nullptr;
        if (!name) return true;
        void* buf = mem::OperatorNew(static_cast<u32>(std::strlen(name)) + 9);   // orig: the "%s-Reverse" buffer
        const std::string rev = std::string(name) + "-Reverse";
        if (g_level->roomGroup)
            for (const auto& room : g_level->roomGroup->roomList) {
                if (!room) continue;
                for (const auto* list : {&room->list1, &room->list2})   // +0xF8, +0x128
                    for (const auto& p : *list)
                        if (p && !p->name.empty() && (crt::StrICmp(p->name.c_str(), name) == 0 ||
                                                      crt::StrICmp(p->name.c_str(), rev.c_str()) == 0)) {
                            if (n.method == 316) p->flags |= 0x0100;   // +0xB1 |= 1 (0x000C418F)
                            else p->flags = static_cast<u16>(p->flags & ~0x0100);   // +0xB1 &= ~1
                        }
            }
        mem::OperatorDelete(buf);
        return true;
    }
    case 380: {                                       // HUD(name, cmd, value) 0x000E5B90: e = Find(name); e->vf04(cmd, value)
        // (Find 0x000E5920: strncmp(name, element name, 15); Hud.h: the elements ported)
        const char* name = Get(a) ? Str(Get(a)) : nullptr;
        const char* cmd = Get(a + 4) ? Str(Get(a + 4)) : nullptr;
        if (g_level->hud) g_level->hud->Command(name, cmd, GetF(a + 8));
        return true;
    }
    case 388:                                         // HUDTimeOut 0x000B0A70: Find("timer")+0x2F, low byte only
        if (!g_level->hudTimer || !g_level->hud || !g_level->hud->Find("timer"))
            return false;                             // (the original dereferences null without the element)
        SetByte(a, g_level->hudTimer->timedOut);
        return true;
    case 79: Set(a, static_cast<u32>(g_level->nibblersPickedUp)); return true;   // FsGetNibblersPickedUp: S+0x18C
    case 126:                                         // FsSetRodentSpeed(v) 0x00037120 (a raw dword copy)
    case 127: {                                       // FsSetRodentCam(n) 0x00037150
        game::Player* p = g_level->player;            // S+0x94
        if (p && p->type == 4 && p->ride) {
            if (n.method == 126) p->ride->speed = GetF(a);   // R+0x2C
            else p->ride->SetCameraPreset(static_cast<s32>(Get(a)));   // 0x00061680
        }
        return true;
    }
    case 274:                                         // FsGetLevel 0x000B07C0: the digit before the level name's '-'
    case 275: {                                       // FsGetStage 0x000B07F0: the digit after it
        const char* name = g_level->name.c_str();
        const char* dash = std::strchr(name, '-');
        u32 v = 0;
        if (n.method == 274 && dash && dash != name) v = static_cast<u32>(static_cast<s32>(dash[-1]) - '0');
        if (n.method == 275 && dash) v = static_cast<u32>(static_cast<s32>(dash[1]) - '0');
        Set(a, v);
        return true;
    }
    case 202: Set(a, 0x18); return true;              // inline constants of the dispatcher (the pad trigger ids the
    case 203: Set(a, 0x19); return true;              //  scripts pass to FsTriggerButton; values as the oracle shows)
    case 204: Set(a, 0x1F); return true;
    case 205: Set(a, 0x20); return true;
    case 206: Set(a, 0x1E); return true;
    case 234:                                         // FsResetChaseCam: 0x000111A0
        cam->recentre = 1;
        return true;
    case 235:                                         // FsInstantResetChaseCam: 0x00015350
        cam->ScriptPlaceBehindTarget();
        return true;
    case 236: {                                       // FsSetChaseCam(name) 0x00019110
        if (!Get(a)) return true;
        cam->ResetFov();
        game::TriggerRec* rec = Record(Get(a));
        if (rec && rec->target) cam->SetFollowTarget(static_cast<game::Character*>(rec->target));   // (characters)
        return true;
    }
    case 237: {                                       // FsSetStaticCam(name) 0x00018860
        if (!Get(a)) return true;
        cam->ResetFov();
        game::TriggerRec* rec = Record(Get(a));
        if (rec && rec->target) {
            const ni::NiPoint3 pos = g_level->triggers->Position(*rec);   // orig: 0x000DA7E0
            const ni::NiMatrix3 rot = g_level->triggers->Rotation(*rec);  // orig: 0x000DB090
            cam->SetStatic(&pos, &rot);
        }
        return true;
    }
    // ---- script triggers (script_triggers.md 2) ----
    case 189: Set(a, Trig().Test(Get(a)) ? 1 : 0); return true;   // 0x000B6FD0
    case 190:                                         // FsTriggerNULLify(&slot) 0x000B8900
        if (Get(a)) Trig().Nullify(Get(a));
        Set(a, 0);
        return true;
    case 191:                                         // FsAndTriggerGroup / FsOrTriggerGroup: new(0x24) 0x000B9060
    case 192: {
        ScriptTrigger& t = Trig().Create(ScriptTrigger::Group, 0x24, n.method == 191 ? 0x002A24C0 : 0x002A24D0);
        t.groupData = mem::ToVmAddr(mem::OperatorNew(4));
        Set(a, t.vm);
        return true;
    }
    case 193:                                         // FsAddTriggerToGroup(g, t) 0x000B9180 (low byte)
    case 194: {                                       // FsRemoveTriggerFromGroup(g, t) 0x000B8820 (low byte)
        ScriptTrigger* g = Trig().Get(Get(a));
        const VmAddr c = Get(a + 4);
        u8 ok = 0;
        if (g && c && g->kind == ScriptTrigger::Group && Get(a) != c) {
            auto it = std::find(g->children.begin(), g->children.end(), c);
            if (n.method == 193 && it == g->children.end()) {
                if (g->children.size() >= g->capacity) {   // SetSize(grow 1 + size) 0x000BA340
                    g->capacity = static_cast<u32>(g->children.size()) + 1;
                    void* d = mem::OperatorNew(g->capacity * 4);
                    mem::OperatorDelete(mem::FromVmAddr(g->groupData));
                    g->groupData = mem::ToVmAddr(d);
                }
                g->children.push_back(c);
                ++g->count;
                ok = 1;
            } else if (n.method == 194 && it != g->children.end()) {
                *it = g->children.back();             // (swap with the last: the order changes)
                g->children.pop_back();
                --g->count;
                ok = 1;
            }
        }
        SetByte(a, ok);
        return true;
    }
    case 195: case 196: case 198: case 207: case 208: case 209: case 210: case 211:
    case 212: case 213: case 214: case 215:
        Set(a, CreateTrigger(n, a));
        return true;
    case 197: {                                       // FsGetTriggerDistance
        ScriptTrigger* t = Trig().Get(Get(a));
        SetF(a, t && t->kind == ScriptTrigger::Sphere ? t->distance : 3.4028234663852886e38f);
        return true;
    }
    case 216: {                                       // FsGetTriggerHealth
        ScriptTrigger* t = Trig().Get(Get(a));
        SetF(a, t && t->kind == ScriptTrigger::Health ? t->health : 0.0f);
        return true;
    }
    case 217: {                                       // FsSetTriggerHealth 0x000B7A20
        ScriptTrigger* t = Trig().Get(Get(a));
        if (t && t->kind == ScriptTrigger::Health) t->health = GetF(a + 4);
        return true;
    }
    // ---- constants, characters, distances ----
    case 153: Set(a, 0); return true;
    case 154: Set(a, 1); return true;
    case 155:                                         // FsGetCannonCtrl (inline 0x000B37F8): the player's +0x7A8 object
        Set(a, PlayerCannon() ? kCannonHandle : 0);   // (the stand-in handle above; no player: 0, L)
        return true;
    case 128: {                                       // FsCharacterDoneDying(name) 0x000372B0 (low byte, 0x000B19CB)
        u8 r = 0;
        if (Get(a))                                   // (a NULL name: AL = 0)
            if (game::TriggerRec* rec = Record(Get(a)); rec && rec->kind == 5 && rec->target)   // FindOrCreate
                r = static_cast<game::Character*>(rec->target)->IsDyingAnim() ? 1 : 0;   // vfAC; SETNE
        SetByte(a, r);
        return true;
    }
    case 148: {                                       // FsIsCharacterSpeaking(name) 0x000373A0 (low byte, 0x000B19CB)
        u8 r = 0;
        if (Get(a))                                   // (a NULL name: AL = 0)
            if (game::TriggerRec* rec = Record(Get(a)); rec && rec->kind == 5 && rec->target)   // FindOrCreate
                r = static_cast<game::Character*>(rec->target)->lip.state == 0 ? 1 : 0;   // C+0x50; SETE
        SetByte(a, r);
        return true;
    }
    case 157: {                                       // FsCharIsDead(name) 0x000DBA50 (low byte)
        u8 dead = 0;
        if (game::TriggerRec* r = Record(Get(a)); r && r->kind == 5) {
            if (r->target) dead = static_cast<game::Character*>(r->target)->IsDead() ? 1 : 0;
            else if (r->id != -1) dead = (g_level->GetObjFlags(static_cast<u32>(r->id)) >> 4) & 1;
        }
        SetByte(a, dead);
        return true;
    }
    case 156: {                                       // FsCharIsOnGround(name) 0x000DBAA0 (low byte, 0x000B19CB)
        u8 on = 0;                                    // (no GetObjFlags fallback for an unbound record, unlike 157)
        if (game::TriggerRec* r = Record(Get(a)); r && r->kind == 5 && r->target) {
            auto* c = static_cast<game::Character*>(r->target);
            // vf70: the players 0x0004FFE0, every other character class 0x00033D10 = body ? body vf48 0x000AF1F0
            // (byte +0x45) : 0; then TEST AL,AL; SETNE AL
            on = c->OnGround() ? 1 : 0;
        }
        SetByte(a, on);
        return true;
    }
    case 169:                                         // FsDisablePausing (inline 0x000B364F): Ingame +0x19 = 0
        g_level->pausingEnabled = 0;                  // (the Ingame object exists while a level runs)
        return true;
    case 120:                                         // FsCharacterControllable(name, b) 0x0003A930 -> 0x00037010(.., 1)
    case 121: {                                       // FsCharacterControllable(name, b, keep) -> 0x00037010
        if (!Get(a)) return true;
        game::TriggerRec* r = Record(Get(a));
        if (!r || r->kind != 5 || !r->target) return true;
        auto* c = static_cast<game::Character*>(r->target);
        const u8 b = static_cast<u8>(Get(a + 4));     // (the case zero-extends the byte)
        c->hasControl = b;                            // +0x630
        if (c->def->IsType(2)) {                      // (+0x10)->vf0C(2)
            g_level->playerNoDamage694 = b == 0 ? 1 : 0;   // byte 0x002DD694
            if (n.method == 121 && (Get(a + 8) & 0xFF) == 0) g_level->playerNoDamage694 = 0;   // keep 0 (120: 1)
            if (input::InputSystem* in = g_level->input) {
                input::Pad* pad = in->activePad ? in->activePad : in->pads[0];   // Input_GetActivePad 0x0009B720
                if (pad) pad->Latch(10);              // Pad::Latch 0x0009B9A0
                pad = in->activePad ? in->activePad : in->pads[0];
                if (pad) pad->Latch(7);
            }
        }
        return true;
    }
    case 262:                                         // FsFadeIn(r, g, b, time) 0x00012FB0(.., 0.0f)
        cam->SetFade(GetF(a), GetF(a + 4), GetF(a + 8), GetF(a + 12), 0.0f);   // (the fade update: NOT PORTED)
        return true;
    case 285: game::ExplodeAtRecord(*g_level, Str(Get(a)), 0); return true;   // FsCreateGroundExplosion 0x000C2DE0
    case 286: game::ExplodeAtRecord(*g_level, Str(Get(a)), 6); return true;   // FsCreateBigGroundExplosion 0x000C2E30
    case 287: game::ExplodeAtRecord(*g_level, Str(Get(a)), 8); return true;   // FsCreateHugeGroundExplosion 0x000C2ED0
    case 288: game::ExplodeAtRecord(*g_level, Str(Get(a)), 7); return true;   // FsCreateGreenGroundExplosion 0x000C2E80
    case 384:                                         // HUDHide 0x000B09F0 -> 0x000E5A60(0)
        if (g_level->hud) g_level->hud->SetActiveAll(false);
        return true;
    case 387:                                         // HUDShow(name) 0x000B0A50 -> 0x000E5B60(name, 1)
        if (g_level->hud && Get(a)) g_level->hud->SetActive(Str(Get(a)), true);
        return true;
    case 245: {                                       // FsSetPathToStaticTgtCam(from, to, target, time) 0x00018910
        cam->ResetFov();
        game::TriggerRec* r0 = Record(Get(a));        // FindOrCreate order: from, to, target (no null checks)
        game::TriggerRec* r1 = Record(Get(a + 4));
        game::TriggerRec* r2 = Record(Get(a + 8));
        if (!r0 || !r1) return true;                  // (a NULL name faults in the original)
        const ni::NiPoint3 p1 = g_level->triggers->Position(*r1);   // 0x000DA7E0: `to` first
        const ni::NiPoint3 p0 = g_level->triggers->Position(*r0);
        cam->SetPathToStaticTarget(p0, p1, r2, GetF(a + 12));   // 0x00014240
        return true;
    }
    case 246: {                                       // FsSetPathToStaticTgtCam(a, b, target, time) 0x000189D0 (vectors)
        cam->ResetFov();                              // (inline: FPTAN of [0x00293BC0], +0xAC = 90, +0xB0, +0xA0 = 1)
        game::TriggerRec* r = Record(Get(a + 24));    // FindOrCreate(target)
        const ni::NiPoint3 p0{GetF(a), GetF(a + 4), GetF(a + 8)};   // (the arguments' addresses on the VM stack)
        const ni::NiPoint3 p1{GetF(a + 12), GetF(a + 16), GetF(a + 20)};
        cam->SetPathToStaticTarget(p0, p1, r, GetF(a + 28));   // 0x00014240(&a, &b, rec, time)
        return true;
    }
    case 242: {                                       // FsSetFixedVectorCam(target, v) 0x00014990 (direct)
        const ni::NiPoint3 v{GetF(a + 4), GetF(a + 8), GetF(a + 12)};
        cam->SetFixedVector(Get(a) ? Str(Get(a)) : nullptr, v);   // (the FOV reset inline)
        return true;
    }
    case 244: {                                       // FsSetPathToChaseCam(from, to, target, time) 0x00018A40 (S+0x8C)
        cam->ResetFov();                              // (inline: FPTAN of [0x00293BC0], +0xAC = 90, +0xB0, +0xA0 = 1)
        game::TriggerRec* r0 = Record(Get(a));        // FindOrCreate order: from, to, target (no null checks)
        game::TriggerRec* r1 = Record(Get(a + 4));
        game::TriggerRec* r2 = Record(Get(a + 8));
        if (!r0 || !r1 || !r2) return true;           // (a NULL name faults in the original)
        // (Position(from) and Position(to) are each read three times first: no effect)
        if (r2->kind != 5) return true;               // rec+0x18 == 5: a character
        const ni::NiPoint3 p1 = g_level->triggers->Position(*r1);   // 0x000DA7E0: `to` first
        const ni::NiPoint3 p0 = g_level->triggers->Position(*r0);
        cam->SetPathToChase(p0, p1, static_cast<game::Character*>(r2->target), GetF(a + 12), 0);   // 0x000143A0
        return true;
    }
    case 240: {                                       // FsSetSideCam(name, a, b) 0x00018DE0 (thiscall S+0x8C, no test)
        cam->ResetFov();                              // (inline: FPTAN of [0x00293BC0], +0xAC = 90, +0xB0, +0xA0 = 1)
        game::TriggerRec* r0 = Record(Get(a));        // FindOrCreate order: name, a, b (no null checks)
        game::TriggerRec* r1 = Record(Get(a + 4));
        game::TriggerRec* r2 = Record(Get(a + 8));
        if (!r0) return true;                         // (rec+0xC is read without a test: a NULL name faults)
        cam->SetSide(static_cast<game::Character*>(r0->target), r1, r2);   // 0x00014690(rec+0xC, r1, r2)
        return true;
    }
    case 243:                                         // FsSetRailCam(name, net) 0x00014F40 (object_gaps.md 17; S+0x8C, no test)
        cam->SetRail(Str(Get(a)), Str(Get(a + 4)));
        return true;
    case 241: {                                       // FsSetFixedVectorCam(target, from, to) 0x00018E70 (object_gaps.md 12)
        cam->ResetFov();                              // (inline, again inside 0x00014990)
        game::TriggerRec* r1 = Record(Get(a + 4));    // FindOrCreate order: from, to (no null checks)
        game::TriggerRec* r2 = Record(Get(a + 8));
        if (!r1 || !r2) return true;                  // (a NULL name faults in the original)
        const game::TriggerManager& tm = *g_level->triggers;
        const ni::NiPoint3 p1 = tm.Position(*r1);     // 0x000DA7E0: `from` first
        const ni::NiPoint3 p2 = tm.Position(*r2);
        ni::NiPoint3 v{F(double(p2.x) - p1.x), F(double(p2.y) - p1.y), F(double(p2.z) - p1.z)};   // to - from
        if (tm.Position(*r2).y > tm.Position(*r1).y) {   // FCOMP; TEST AH,0x41; JNE (`to` read first)
            const ni::NiPoint3 q2 = tm.Position(*r2), q1 = tm.Position(*r1);
            v = {F(double(q1.x) - q2.x), F(double(q1.y) - q2.y), F(double(q1.z) - q2.z)};   // from - to: downwards
        }
        cam->SetFixedVector(Get(a) ? Str(Get(a)) : nullptr, v);   // 0x00014990
        return true;
    }
    case 199: {                                       // FsSwitchEdgesInsideBoxTrigger(t, on) 0x0003A5E0
        ScriptTrigger* t = Trig().Get(Get(a));
        if (!t || t->kind != ScriptTrigger::Box) return true;   // [t+8] == 2
        const u8 on = static_cast<u8>(Get(a + 4));
        const auto& m = t->rotation.m;                // chunks +0x30 / +0x40 / +0x50 = columns 0 / 1 / 2
        const ni::NiPoint3 c = t->centre;             // +0x1C
        auto inside = [&](const ni::NiPoint3& p) {
            const ni::NiPoint3 d{F(double(p.x) - c.x), F(double(p.y) - c.y), F(double(p.z) - c.z)};
            auto proj = [&](int k) {                  // FLD x3; FMUL d.z; FXCH; FMUL d.y; FADDP; FXCH; FMUL d.x; FADDP; FABS
                return std::fabs((double(m[2][k]) * d.z + double(m[1][k]) * d.y) + double(m[0][k]) * d.x);
            };
            // chunk +0x30 vs +0x64, +0x50 vs +0x68, +0x40 vs +0x60 (TEST AH,0x41; JE: only '>' leaves; NaN passes)
            return !(proj(0) > t->half.x) && !(proj(2) > t->half.z) && !(proj(1) > t->half.y);
        };
        for (const auto& s : g_level->edgeSets)       // DAT_002EAF0C, newest first; edges in index order
            for (game::Edge& e : s->edges)
                if (inside(e.p0) && inside(e.p1)) e.active = on;   // byte +0x20
        return true;
    }
    case 200: {                                       // FsSwitchLadderInsideBoxTrigger(t, on) 0x0003A7F0 (object_gaps.md 18.5)
        ScriptTrigger* t = Trig().Get(Get(a));
        if (!t || t->kind != ScriptTrigger::Box) return true;   // [t+8] == 2
        const u8 on = static_cast<u8>(Get(a + 4));
        const auto& m = t->rotation.m;                // as 199: chunks +0x30 / +0x40 / +0x50 = columns 0 / 1 / 2
        const ni::NiPoint3 c = t->centre;             // +0x1C
        for (const auto& st : g_level->stages) {      // Session::NextStage 0x000C26C0 from null
            if (!st) continue;
            for (game::StageLadder& l : st->ladders) {   // +0x74 (+0x70 records of 0x1C bytes)
                const ni::NiPoint3 d{F(double(l.position.x) - c.x), F(double(l.position.y) - c.y),
                                     F(double(l.position.z) - c.z)};
                auto proj = [&](int k) {              // FLD x3; FMUL d.z; FXCH; FMUL d.y; FADDP; FXCH; FMUL d.x; FADDP
                    return std::fabs((double(m[2][k]) * d.z + double(m[1][k]) * d.y) + double(m[0][k]) * d.x);
                };
                // +0x30 vs +0x64, +0x50 vs +0x68, +0x40 vs +0x60 (TEST AH,0x41; JE: only '>' leaves; NaN passes)
                if (!(proj(0) > t->half.x) && !(proj(2) > t->half.z) && !(proj(1) > t->half.y)) l.active = on;   // +0x18
            }
        }
        return true;
    }
    case 158: g_level->materialDamage = GetF(a); return true;   // FsSetMaterialDamage: DAT_002D6218
    case 311: {                                       // FsObjDistance(a, b) 0x000DBBD0 (the second name first)
        f32 d = 0.0f;
        if (Get(a) && Get(a + 4)) {
            const ni::NiPoint3 pb = g_level->triggers->Position(*Record(Get(a + 4)));
            const ni::NiPoint3 pa = g_level->triggers->Position(*Record(Get(a)));
            const double dx = double(pa.x) - pb.x, dy = double(pa.y) - pb.y, dz = double(pa.z) - pb.z;
            d = F(std::sqrt((dz * dz + dy * dy) + dx * dx));
        }
        SetF(a, d);
        return true;
    }
    case 143: {                                       // FsGetTank(name) 0x000922F0
        u32 r = 0;
        if (game::TriggerRec* rec = Record(Get(a)); rec && rec->kind == 5 && rec->target) {
            auto* ch = static_cast<game::Character*>(rec->target);
            if (Lower(ch->cdef.name.c_str()) == "tank") {
                r = 0x07F10000;                       // (a stand-in for the Tank pointer; L)
                g_tank = static_cast<game::Tank*>(ch);
            }
        }
        Set(a, r);
        return true;
    }
    // ---- level animations (0x000C1F20 / 0x000C1EF0 / 0x000C1EC0 / 0x000B0260 / 0x000B0220) ----
    case 219:
    case 221: {
        level::LevelAnimation* la = g_level->animations ? g_level->animations->FindByName(Str(Get(a))) : nullptr;
        if (la) la->Play(Str(Get(a + 4)), n.method == 221, false);
        SetF(a, 0.0f);
        return true;
    }
    case 226: {
        level::LevelAnimation* la = g_level->animations ? g_level->animations->FindByName(Str(Get(a))) : nullptr;
        SetF(a, la ? la->CurrentClipDuration() : 0.0f);
        return true;
    }
    case 230:
    case 232:
        if (Get(a) && g_level->animations)
            if (level::LevelAnimation* la = g_level->animations->FindByName(Str(Get(a)))) {
                la->Play(Str(Get(a + 4)), false, false);
                if (n.method == 230) la->SkipToEnd();
                else la->Rewind();
            }
        return true;
    case 227: {                                       // FsAnimationLength(anim, clip) 0x000C2DB0 (fstp: a float)
        f32 r = 0.0f;                                 // (fld [0x00293AB8])
        if (Get(a) && g_level->animations)            // (FindByName 0x000BA5B0: _stricmp with a NULL name faults)
            if (level::LevelAnimation* la = g_level->animations->FindByName(Str(Get(a))))
                if (Get(a + 4))                       // (FindClip 0x000BAEB0: likewise, with any clip)
                    if (const level::LevelAnimClip* c = la->FindClip(Str(Get(a + 4)))) r = c->duration;   // +0xC
        SetF(a, r);
        return true;
    }
    case 220: {                                       // FsPlayAnimation(anim, clip, reverse) 0x000C1F20 (fstp 0.0)
        level::LevelAnimation* la = g_level->animations ? g_level->animations->FindByName(Str(Get(a))) : nullptr;
        if (la) la->Play(Str(Get(a + 4)), false, (Get(a + 8) & 0xFF) != 0);   // 0x000BB220(clip, 0, reverse)
        SetF(a, 0.0f);                                // FLD [0x00293AB8]
        return true;
    }
    case 224:                                         // FsPauseAnimation(anim) 0x000C2010 (fstp 0.0)
    case 225:                                         // (anim, reverse) 0x000C1F50 (fstp 0.0): Resume
    case 229: {                                       // FsFFEnd(anim) 0x000C1F80 (fstp 0.0)
        level::LevelAnimation* la = g_level->animations ? g_level->animations->FindByName(Str(Get(a))) : nullptr;
        if (la) {                                     // LevelAnimation::FindByName 0x000BA5B0
            if (n.method == 224) la->Pause();         // 0x000BA2C0
            else if (n.method == 225) la->Resume((Get(a + 4) & 0xFF) != 0);   // 0x000BA2D0 (MOV CL, [+4])
            else la->SkipToEnd();                     // 0x000BA850
        }
        SetF(a, 0.0f);
        return true;
    }
    case 228: {                                       // FsAnimationPos(anim) 0x000C2D70 (fstp: a float)
        f32 r = 0.0f;
        if (Get(a) && g_level->animations)
            if (level::LevelAnimation* la = g_level->animations->FindByName(Str(Get(a)))) r = la->time;   // +0x20
        SetF(a, r);
        return true;
    }
    // ---- stages, respawn (script_triggers.md 4.9 / 4.10) ----
    case 321: {                                       // 0x000B02A0 (setne + low byte)
        game::Stage* st = Get(a) ? g_level->FindStage(Str(Get(a))) : nullptr;
        if (st) st->Reset();                          // 0x000D0800
        SetByte(a, st ? 1 : 0);
        return true;
    }
    case 323: {                                       // FsIsStageLoaded(name) 0x000B02D0 (low byte: NEG / SBB / NEG)
        game::Stage* st = Get(a) ? g_level->FindStage(Str(Get(a))) : nullptr;   // 0x000C3F70
        SetByte(a, st && st->IsLoaded() ? 1 : 0);     // Stage::IsLoaded 0x000CE400 (no stage: EAX = 0)
        return true;
    }
    case 322:                                         // 0x000B0300
        g_selStage = Get(a) ? g_level->FindStage(Str(Get(a))) : nullptr;
        SetByte(a, g_selStage ? 1 : 0);
        return true;
    case 324: {                                       // 0x000B0320
        bool listed = false;                          // orig: 0x000C3F50 (still in the stage list)
        for (const auto& st : g_level->stages) listed = listed || st.get() == g_selStage;
        if (!listed) g_selStage = nullptr;
        if (g_selStage) g_selStage->SpawnByLabel(Str(Get(a)), true);   // 0x000D0790
        SetByte(a, g_selStage ? 1 : 0);
        return true;
    }
    case 325: {                                       // (unnamed) 0x000B0350(stage, label) (setne + low byte)
        game::Stage* st = Get(a) ? g_level->FindStage(Str(Get(a))) : nullptr;   // 0x000C3F70
        if (st) st->SpawnByLabel(Str(Get(a + 4)), true);   // 0x000D0790(label, 1)
        SetByte(a, st ? 1 : 0);
        return true;
    }
    case 327: {                                       // FuAddRespawnPoint(name) 0x000C7070
        game::TriggerRec* r = Record(Get(a));
        auto& v = g_level->respawnPoints;
        if (!r || std::find(v.begin(), v.end(), r) != v.end()) return true;
        auto hole = std::find(v.begin(), v.end(), nullptr);
        if (hole != v.end()) *hole = r;
        else v.push_back(r);
        return true;
    }
    case 326: {                                       // FuSetRespawnStageFromTargetable(name) 0x000C9390
        if (!g_level->roomGroup) return true;
        game::TriggerRec* r = Record(Get(a));
        if (!r) return true;                          // (the original faults on a NULL record)
        ni::NiPoint3 p = g_level->triggers->Position(*r);
        ni::NiRoom* room = g_level->roomGroup->FindRoomContaining(p);
        if (!room) {
            p.y = F(double(p.y) + 2.0f);
            room = g_level->roomGroup->FindRoomContaining(p);
        }
        if (!room) return true;
        // (a stale respawn stage of Level4-1 is compared by identity only: stage_streaming_port.md 2.4)
        if (!g_level->respawnStage || !g_level->LiveStage(g_level->respawnStage) ||
            !g_level->respawnStage->ContainsRoom(room)) {
            g_level->prevRespawnStage = g_level->respawnStage;
            g_level->respawnStage = nullptr;
            for (const auto& st : g_level->stages)
                if (st->ContainsRoom(room)) {
                    g_level->respawnStage = st.get();
                    break;
                }
            g_level->respawnRec = r;
            if (g_level->respawnStage != g_level->prevRespawnStage && g_level->updateStageStreaming)
                g_level->updateStageStreaming();      // orig: 0x000C70C0
        }
        return true;
    }
    // ---- the checkpoint and the respawn (quicksave_port.md) ----
    case 319: g_level->QuickSave(); return true;      // FsQuickSave 0x000C7910 (QuickSave.cpp)
    case 318: g_level->QuickLoad(); return true;      // FsQuickLoad 0x000C7D40 (QuickSave.cpp)
    case 329:                                         // FsDeductLives 0x000C12D0: --DAT_002D687C
        if (--g_level->lives < 0) {                   // JNS: below 0 the App (DAT_002DAC60) 0x0002A220("Death")
            if (g_level->leaveIngame) g_level->leaveIngame("Death");
            else NotPortedText("FsDeductLives: no lives left (the App's \"Death\" request 0x0002A220: no App)");
        }
        return true;
    case 328: {                                       // FsAtRespawn 0x000C3310
        auto* d = g_level->defs ? dynamic_cast<game::RespawnEffectDef*>(g_level->defs->respawnEffect) : nullptr;
        if (d && g_level->player) {                   // S+0x48 (the player S+0x94 is not tested)
            ni::NiPoint3 p = g_level->player->GetWorldPosition();   // vf34
            p.y = F(double(p.y) + 4.0f);              // FADD [0x00293C1C]
            (void)d->SpawnAt(*g_level, g_level, p, "Respawn_Eff");   // 0x000715D0(S, &p, 0x002A346C)
        }
        return true;
    }
    case 383:                                         // HUDRemoveAllText 0x000B09D0 -> 0x000E5BC0("info", "remove_all",
        if (g_level->hud) g_level->hud->Command("info", "remove_all", "");   //  "") (HUDInfo's text: NOT PORTED)
        return true;
    case 393: UnlinkCurrentThread(); return true;     // __detach__ 0x000B9550
    // ---- AI (stand-ins, script_triggers.md 5) ----
    case 336: {                                       // AISelect(name) 0x000318F0 (low byte)
        g_ai.agent = nullptr;
        g_ai.seq = nullptr;
        const char* name = Get(a) ? Str(Get(a)) : nullptr;
        if (name)
            for (game::AiAgent* ag : g_level->aiAgents)   // the first agent whose character label matches
                if (ag->character && ag->character->label && crt::StrICmp(ag->character->label->c_str(), name) == 0) {
                    g_ai.agent = ag;
                    break;
                }
        SetByte(a, g_ai.agent ? 1 : 0);               // SETNE AL of +0x10
        return true;
    }
    case 370: std::memcpy(mem::FromVmAddr(a), game::kAIDefaultProfile.data(), 0x68); return true;
    case 371: {                                       // AIGetProfile 0x00031510 (0x000326F0: copy + squares)
        std::array<u32, 26> p = game::kAIDefaultProfile;   // (no selection: the defaults 0x000325A0)
        if (g_ai.agent) p = game::CopyAIProfile(g_ai.agent->profile);
        std::memcpy(mem::FromVmAddr(a), p.data(), 0x68);
        return true;
    }
    case 372: {                                       // AISetProfile(profile) 0x00031540 -> 0x0002CF00
        std::array<u32, 26> p;
        std::memcpy(p.data(), mem::FromVmAddr(a), 0x68);
        if (g_ai.agent) g_ai.agent->SetProfile(p);
        return true;
    }

    // ---- the fog (natives_fog_say_rw_port.md 1) ----
    case 252: cam->SetFogIntensity(GetF(a)); return true;   // FsFogSetIntensity 0x00012C70
    case 253: cam->SetFogColor(GetF(a), GetF(a + 4), GetF(a + 8)); return true;   // FsFogSetColor 0x00012CC0
    case 254: cam->SetFogDistance(GetF(a)); return true;    // FsFogSetDistance 0x00011230
    case 255:                                         // FsFogSetRelativeNode(name, child, offset) 0x00018520
        cam->SetFogRelativeNode(Get(a) ? Str(Get(a)) : nullptr, Get(a + 4) ? Str(Get(a + 4)) : nullptr, GetF(a + 8));
        return true;
    case 256: cam->EnableFog(); return true;          // FsFogEnable 0x00012D20
    case 257: cam->DisableFog(); return true;         // FsFogDisable 0x00012DB0
    case 258: SetF(a, cam->fogDistance); return true; // FsFogGetDistance: inline fld C+0x1D8
    case 223:                                         // FsStopAnimation(name) 0x000C1FE0
    case 231:                                         // FsRWStart(name) 0x000C1FB0
        if (Get(a) && g_level->animations)            // (FindByName 0x000BA5B0: _stricmp with a NULL name faults)
            if (level::LevelAnimation* la = g_level->animations->FindByName(Str(Get(a)))) {
                if (n.method == 223) la->Stop();      // 0x000BA830
                else la->Rewind();                    // 0x000BA910
            }
        SetF(a, 0.0f);                                // fld 0.0; fstp [sp]
        return true;
    case 146:                                         // FsCharacterSay(name, textId, priority) 0x000372F0
    case 147: {                                       // FsCharacterSay(name, textId, db, priority) 0x00037340
        f32 r = 0.0f;
        const VmAddr name = Get(a), id = Get(a + 4);
        const VmAddr db = n.method == 147 ? Get(a + 8) : 0;
        const s32 priority = static_cast<s32>(Get(a + (n.method == 147 ? 12 : 8)));
        if (name && id && (n.method == 146 || db)) {
            game::TriggerRec* rec = Record(name);     // FindOrCreate 0x000DB390
            if (rec && rec->kind == 5 && rec->target)
                r = static_cast<game::Character*>(rec->target)->Say(Str(id), db ? Str(db) : nullptr, priority);
        }
        SetF(a, r);                                   // fstp [sp]
        return true;
    }
    case 331: case 332: case 333: case 334:          // constants 0x000B1891 / 0x000B186E / 0x000B184B / 0x000B1828
        Set(a, n.method - 331);                       // (dword): the sequence numbers of 337
        return true;
    case 335: {                                       // AIQuery(name) 0x00031970 (low byte)
        const char* name = Get(a) ? Str(Get(a)) : nullptr;
        bool found = false;
        if (name)
            for (game::AiAgent* ag : g_level->aiAgents)
                if (ag->character && ag->character->label &&
                    crt::StrICmp(ag->character->label->c_str(), name) == 0) { found = true; break; }
        SetByte(a, found ? 1 : 0);                    // (M+4, newest first: the character label +0x34, _stricmp)
        return true;
    }
    case 337:                                         // AISelectSequence(n) 0x00031460
        if (g_ai.agent) {
            switch (static_cast<s32>(Get(a))) {
            case 1: g_ai.seq = &g_ai.agent->seq248; break;
            case 2: g_ai.seq = &g_ai.agent->seq458; break;
            case 3: g_ai.seq = &g_ai.agent->seq668; break;
            default: break;
            }
        }
        return true;
    case 338: if (g_ai.agent && g_ai.seq) g_ai.seq->loop = static_cast<u8>(Get(a)); return true;   // 0x000315E0
    case 341:                                         // AIInstantEngage 0x00031580
        if (g_ai.agent)
            if (game::Player* p = FirstPlayer()) g_ai.agent->AlertEngage(*p);
        return true;
    case 342: if (g_ai.agent) g_ai.agent->Calm(); return true;                                    // 0x000315B0
    case 343: Set(a, g_ai.agent ? static_cast<u32>(g_ai.agent->state) : 0); return true;          // dword
    case 344: if (g_ai.agent) g_ai.agent->f20C = GetF(a); return true;                            // raw dword
    case 345: if (g_ai.agent && g_ai.seq) g_ai.seq->Reset(); return true;                         // 0x00032730
    case 346: if (g_ai.agent && g_ai.seq) g_ai.seq->AddWait(GetF(a)); return true;
    case 347: if (g_ai.agent && g_ai.seq) g_ai.seq->AddPrecision(GetF(a)); return true;
    case 348: if (g_ai.agent && g_ai.seq) g_ai.seq->AddGuard(static_cast<u8>(Get(a))); return true;
    case 349: case 351: {                             // AIAddCommandGoto(name [, prec]) 0x00031A70 (349: 5.0f)
        const char* name = Get(a) ? Str(Get(a)) : nullptr;
        const f32 prec = n.method == 349 ? 5.0f : GetF(a + 4);
        if (g_ai.agent && g_ai.seq && name)
            if (const game::StageWaypoint* wp = FindWaypointAnyStage(name)) g_ai.seq->AddGoto(wp->position, prec);
        return true;
    }
    case 350: case 352: {                             // AIAddCommandGoto(x, y, z [, prec]) 0x00031660
        const ni::NiPoint3 p{GetF(a), GetF(a + 4), GetF(a + 8)};
        if (g_ai.agent && g_ai.seq) g_ai.seq->AddGoto(p, n.method == 350 ? 5.0f : GetF(a + 12));
        return true;
    }
    case 353: {                                       // AIAddCommandLook(name) 0x00031B10
        const char* name = Get(a) ? Str(Get(a)) : nullptr;
        if (g_ai.agent && g_ai.seq && name) {
            game::TriggerRec* rec = g_level->triggers->FindOrCreate(name);   // 0x000DB5B0
            g_ai.seq->AddLook(rec ? g_level->triggers->Position(*rec) : ni::NiPoint3{});
        }
        return true;
    }
    case 354: if (g_ai.agent && g_ai.seq) g_ai.seq->AddLook({GetF(a), GetF(a + 4), GetF(a + 8)}); return true;
    case 355: if (g_ai.agent && g_ai.seq) g_ai.seq->AddLookTarget(GetF(a)); return true;
    case 356: if (g_ai.agent && g_ai.seq) g_ai.seq->AddLookTarget(0.0f); return true;              // (push 0)
    case 357: if (g_ai.agent && g_ai.seq) g_ai.seq->AddAnimate(Get(a)); return true;
    case 358: {                                       // AIAddCommandWarp(name) 0x00031AC0
        const char* name = Get(a) ? Str(Get(a)) : nullptr;
        if (g_ai.agent && g_ai.seq && name)
            if (const game::StageWaypoint* wp = FindWaypointAnyStage(name)) g_ai.seq->AddWarp(wp->position);
        return true;
    }
    case 359: if (g_ai.agent && g_ai.seq) g_ai.seq->AddWarp({GetF(a), GetF(a + 4), GetF(a + 8)}); return true;
    case 360: if (g_ai.agent && g_ai.seq) g_ai.seq->AddPlayerRun(static_cast<u8>(Get(a))); return true;
    case 361: if (g_ai.agent && g_ai.seq) g_ai.seq->AddPlayerFreeze(); return true;
    case 362: if (g_ai.agent && g_ai.seq) g_ai.seq->AddReset(); return true;
    case 373:                                         // AISetMap(name) 0x00031550 -> 0x00031E10
        if (g_ai.agent) g_ai.agent->finder.net = g_level->FindAINet(Get(a) ? Str(Get(a)) : nullptr);
        return true;
    case 374: if (g_ai.agent) g_ai.agent->b74 = static_cast<u8>(Get(a)); return true;
    case 375: SetByte(a, g_ai.agent ? g_ai.agent->enabled : 0); return true;                      // low byte
    case 376: {                                       // AISetActive(b) 0x000314E0
        if (!g_ai.agent) return true;
        const u8 on = static_cast<u8>(Get(a));
        g_ai.agent->enabled = on;
        if (g_ai.agent->character) g_ai.agent->character->SetFlag28(on == 0 ? 1 : 0);   // vf28 0x000968B0
        return true;
    }
    case 377: g_level->aiEnabled = static_cast<u8>(Get(a)) != 0; return true;                    // byte M+0
    case 381:                                         // 0x000B0970 -> 0x000E5BC0("info", "add", text)
    case 382:                                         // 0x000B09A0 -> 0x000E5BC0("info", "remove", text)
        if (g_level->hud && Get(a))
            g_level->hud->Command("info", n.method == 381 ? "add" : "remove", Str(Get(a)));   // (HUDInfo: text)
        return true;
    case 379: {                                       // HUD(name, cmd, text) 0x000E5BC0: e = Find(name); e->vf03(cmd, text)
        if (g_level->hud && Get(a) && Get(a + 4) && Get(a + 8))
            g_level->hud->Command(Str(Get(a)), Str(Get(a + 4)), Str(Get(a + 8)));
        return true;
    }
    case 385:                                         // HUDHide(name) 0x000B0A30 -> 0x000E5B60(name, 0)
        if (g_level->hud && Get(a)) g_level->hud->SetActive(Str(Get(a)), false);
        return true;
    case 250:                                         // FsSetCamPreset(kind) 0x00011380
        cam->ApplyPresets(static_cast<s32>(Get(a)));
        return true;
    case 238:                                         // FsSetStaticCam(position, target) 0x00018B50, easing 0
    case 239: {                                       // FsSetStaticCam(position, target, easing)
        const VmAddr p = Get(a), t = Get(a + 4);
        const f32 easing = n.method == 239 ? GetF(a + 8) : 0.0f;
        if (!p || !t) return true;
        cam->ResetFov();
        game::TriggerRec* rp = Record(p);
        game::TriggerRec* rt = Record(t);
        if (rp && rt) {
            const ni::NiPoint3 pos = g_level->triggers->Position(*rp);
            cam->SetStaticTarget(&pos, rt, easing);  // orig: 0x00014550
        }
        return true;
    }

    // ---- natives batch B (natives_batch_b_port.md 2) ----
    case 67: {                                        // FsGetTranslate(name) 0x000DB5B0 (pop 4, push 12)
        ni::NiPoint3 p{};                             // (zero for a NULL name / no record)
        if (game::TriggerRec* r = Record(Get(a))) p = g_level->triggers->Position(*r);   // orig: 0x000DA7E0
        SetF(a, p.x);                                 // (three raw dword copies)
        SetF(a + 4, p.y);
        SetF(a + 8, p.z);
        return true;
    }
    case 292: {                                       // FsSetLaserSpeed(def, speed) 0x000C49E0
        const char* name = Get(a) ? Str(Get(a)) : nullptr;
        if (!name || !g_level->defs) return true;
        game::Def* d = g_level->defs->Find(name);     // orig: Session::FindDef 0x000C3F10
        if (d && d->IsType(0x10))                     // orig: vf0C(0x10) (0x00079A30: the projectile definitions)
            static_cast<game::ProjectileDef*>(d)->value1C = GetF(a + 4);   // +0x1C (raw dword)
        return true;
    }
    case 114:                                         // FsCharacterAnimate(name, anim): 0x0003A950(.., 0, 1.0f)
    case 115:                                         // FsCharacterAnimateLoop(name, anim, loop): (.., (u8)loop, 1.0f)
    case 116: {                                       // FsCharacterAnimateSpeed(name, anim, loop, speed)
        const VmAddr name = Get(a), anim = Get(a + 4);
        const u8 loop = n.method == 114 ? 0 : static_cast<u8>(Get(a + 8));   // (xor ecx,ecx; mov cl,[eax+8])
        const f32 speed = n.method == 116 ? GetF(a + 12) : 1.0f;            // (push 0x3F800000)
        if (!name || !anim) return true;
        game::TriggerRec* r = Record(name);           // orig: FindOrCreate 0x000DB390
        if (!r || r->kind != 5 || !r->target) return true;
        auto* c = static_cast<game::Character*>(r->target);
        ni::NiSequence* seq = c->FindSequence(Str(anim));   // orig: 0x00039BA0
        if (!seq) return true;
        c->PlayAnimation(seq, loop == 0, false, true, -game::g_frameStep);   // orig: 0x0003A130 (SETE; FCHS: exact)
        if (!(speed == 1.0f))                         // FCOMP [0x00293A98]; TEST AH,0x44; JNP (NaN goes on)
            if (const game::AnimEntry* e = c->EntryOf(seq))   // orig: CharacterDef::FindAnimationBySequence 0x0003FD20
                game::Character::SetFrequency(seq, F(double(speed) * e->speed));   // orig: 0x00036240 (e+0x34)
        return true;
    }
    case 317: {                                       // FsIsBoxEmpty(box) 0x000DBC60 (low byte, 0x000B19CB)
        u8 empty = 1;
        game::TriggerRec* r = Record(Get(a));         // orig: FindOrCreate 0x000DB390
        if (r && r->kind == 4) {                      // (not a box trigger node: "empty")
            game::TriggerManager& tm = *g_level->triggers;
            const ni::NiPoint3 s = tm.BoxSize(*r);    // orig: 0x000DA770 (zero without a target)
            const f32 hx = F(double(s.x) * 0.5f), hy = F(double(s.y) * 0.5f), hz = F(double(s.z) * 0.5f);
            const ni::NiPoint3 c = tm.Position(*r);   // orig: 0x000DA7E0
            const ni::NiMatrix3 inv = ni::InverseOrZero(tm.Rotation(*r));   // orig: 0x000DB090, 0x00103BB0
            // Both loops: the point in the box's frame (inv * d; chunk k of the original's local = column k), x and
            // y rounded, z kept on the x87 stack; compared x, z, y (each TEST AH,0x41; JP: a NaN is outside).
            auto inside = [&](const ni::NiPoint3& p) {
                const auto& m = inv.m;
                const double dx = double(p.x) - c.x, dy = double(p.y) - c.y, dz = double(p.z) - c.z;
                const f32 lx = F((double(m[0][2]) * dz + double(m[0][1]) * dy) + dx * m[0][0]);
                const f32 ly = F((double(m[1][2]) * dz + double(m[1][1]) * dy) + dx * m[1][0]);
                const double lz = (double(m[2][2]) * dz + double(m[2][1]) * dy) + dx * m[2][0];   // (unrounded)
                return std::fabs(double(lx)) <= hx && std::fabs(lz) <= hz && std::fabs(double(ly)) <= hy;
            };
            // orig: Session::NextStage 0x000C26C0 (S+0x58) with each stage's objects: the characters (vf0C(1))
            for (const auto& st : g_level->stages) {
                for (game::GameObject* o = st->NextObject(nullptr); o && empty; o = st->NextObject(o))
                    if (o->def->IsType(1) && inside(o->CharacterPoint())) empty = 0;   // vf78
                if (!empty) break;
            }
            // then the level object's own list: the players (vf0C(2))
            for (game::GameObject* o = g_level->NextObject(nullptr); o && empty; o = g_level->NextObject(o))
                if (o->def->IsType(2) && inside(o->CharacterPoint())) empty = 0;
        }
        SetByte(a, empty);
        return true;
    }

    // ---- natives batch A (natives_batch_a_port.md 2-6; checked against the cases and their targets) ----
    case 389: Set(a, 0); return true;                 // FsIsDemo (inline 0x000B0DB1): byte 0x002E7C20 (never written)
    case 295: {                                       // FsCreateFire(name) 0x000C4C30 (thiscall S, RET 4)
        game::TriggerRec* r = Record(Get(a));         // FindOrCreate 0x000DB390 (a NULL name returns 0)
        if (!r) return true;
        ni::NiPoint3 pos = g_level->triggers->Position(*r);   // 0x000DA7E0
        const ni::NiMatrix3 rot{};                    // 12 dwords of 0x002D7820 (not read by the spawn)
        game::Def* d = g_level->defs ? g_level->defs->Find("effect_BonFire") : nullptr;   // 0x000C3F10, 0x002A2C5C
        if (!d || !d->IsType(0x40000)) return true;   // vf0C: FireDef only
        ni::NiRoom* room = g_level->roomGroup ? g_level->roomGroup->FindRoomContaining(pos) : nullptr;   // 0x0011F1D0
        if (!room) {
            pos.y = F(double(pos.y) + 2.0f);          // [0x00293B20] (pos keeps it)
            room = g_level->roomGroup ? g_level->roomGroup->FindRoomContaining(pos) : nullptr;
        }
        game::Owner* owner = g_level;                 // S without a stage
        for (const auto& st : g_level->stages)        // S+0x58 order; ContainsRoom 0x000CE410 (also with NULL)
            if (st->ContainsRoom(room)) {
                owner = st.get();                     // stage+4 (its Owner part)
                break;
            }
        game::GameObject* o = d->Spawn(*g_level, owner, pos, rot, nullptr);   // vf14: FireDef::Spawn 0x00069600
        if (!o) return true;
        if (g_level->lightManager) {                  // orig: DAT_002EA824 (no NULL test)
            ni::NiPointer<game::LMSource> l = g_level->lightManager->AddLight(
                *g_level, {0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 0.0f}, 50.0f, 2, false);   // vf04 0x000C08F0
            o->GetNode()->AttachChild(l.Get(), true); // vf30 -> vfA0
        }
        return true;
    }
    // ---- the fire circle and the sungod's head fire (level3-4; natives_batch_a_port.md 8, Fire.cpp) ----
    case 296:                                         // FsFireCircleGetDef(name) 0x000C4E20
        Set(a, CircleDefWord(game::FireCircleGetDef(*g_level, Str(Get(a))), false));
        return true;
    case 297: {                                       // FsFireCircleCreateDef(name) 0x000C4E60
        bool created = false;
        game::FireCircleDef* d = game::FireCircleCreateDef(*g_level, Str(Get(a)), &created);
        Set(a, CircleDefWord(d, created));
        return true;
    }
    case 298: {                                       // FsFireCircleCopyDef(dst, src) 0x000C2080
        game::FireCircleDef* dst = CircleDef(Get(a));
        game::FireCircleDef* src = CircleDef(Get(a + 4));
        if ((Get(a) && !dst) || (Get(a + 4) && !src))
            NotPortedText("FsFireCircleCopyDef with a definition natives 296 / 297 did not return");
        game::FireCircleCopyDef(dst, src);
        return true;
    }
    case 299: {                                       // FsFireCircleCreate(label, node, def) 0x000C20D0 (thiscall S)
        game::FireCircleDef* d = CircleDef(Get(a + 8));
        if (Get(a + 8) && !d) NotPortedText("FsFireCircleCreate with a definition natives 296 / 297 did not return");
        game::FireCircleCreate(*g_level, Str(Get(a)), Str(Get(a + 4)), d);
        return true;
    }
    case 300: game::FireCircleDestroy(*g_level, Str(Get(a))); return true;     // FsFireCircleDestroy 0x000C3270
    case 301: game::SetFireToSungodsHead(*g_level, Str(Get(a))); return true;  // FsSetFireToSungodsHead 0x000C4F00
    case 302: game::PutOutFireInSungodsHead(*g_level); return true;           // FsPutOutFireInSungodsHead 0x000C50F0
    case 251:                                         // FsSetMagnetEnabled(name, b) 0x000DBF10 ((u8)slot1)
        if (Get(a))                                   // (FindOrCreate returns a record for every non-NULL name)
            if (game::TriggerRec* r = Record(Get(a))) r->magnet = static_cast<u8>(Get(a + 4));   // rec+0
        return true;
    case 109:                                         // FsSetHealthCharacter(name, health) 0x00036E40
        if (Get(a))
            if (game::TriggerRec* r = Record(Get(a)); r && r->kind == 5 && r->target)
                std::memcpy(&static_cast<game::Character*>(r->target)->health, mem::FromVmAddr(a + 4), 4);   // +0x4F4
        return true;
    case 110: {                                       // FsGetHealthCharacter(name) 0x00036E80 (float through the x87)
        f32 h = 0.0f;                                 // fld [0x00293AB8]
        if (Get(a))
            if (game::TriggerRec* r = Record(Get(a)); r && r->kind == 5 && r->target)
                h = static_cast<game::Character*>(r->target)->health;   // fld [+0x4F4]
        SetF(a, h);                                   // fstp [sp]
        return true;
    }
    case 112:                                         // FsDamageCharacter(name, amount) 0x00036F90
        if (Get(a))
            if (game::TriggerRec* r = Record(Get(a)); r && r->kind == 5 && r->target)
                static_cast<game::Character*>(r->target)->Damage(GetF(a + 4), nullptr);   // vf7C(amount, 0)
        return true;
    case 117: {                                       // FsCharacterIsAnimating(name, anim) 0x0003AA00 (low byte)
        u8 r = 0;
        if (Get(a) && Get(a + 4))
            if (game::TriggerRec* rec = Record(Get(a)); rec && rec->kind == 5 && rec->target) {
                auto* c = static_cast<game::Character*>(rec->target);
                r = c->IsPlayingName(Str(Get(a + 4))) && !c->finished ? 1 : 0;   // 0x00039D60, +0x9B
            }
        SetByte(a, r);
        return true;
    }
    case 105: game::g_decomposeCorpses = static_cast<u8>(Get(a)); return true;   // FsSetDecomposeCorpses (inline)
    case 152:                                         // FsTriggerPlayerKilledEvent() 0x000F0E00
        if (game::Player* p = g_level->player) {      // (S+0x94: no null test in the original)
            const game::IngameEventArg ev{p->killer, nullptr};   // 0x000984D0, then 0x000D8150(P+0x68)
            p->killer = nullptr;                      // +0x68 = 0 (before the trigger)
            const game::IngameEventContext ctx = p->IngameContext();
            p->cdef.ingameEvents.Trigger(1, ev, ctx); // (D+0x28)->0x000F28F0(1, &arg); 0x000F0C00 clears the arg
        }
        return true;
    case 135:                                         // FsGetAmmoPlayer() 0x00036EC0: P->vfC4(P+0x528) 0x0004F7F0
    case 136: {                                       // FsGetMaxAmmoPlayer() 0x00036EF0: 0x0004F850(P+0x528)
        u32 r = 0;                                    // (no player: 0)
        if (game::Player* p = g_level->player)        // (a null weapon: 0 in both)
            r = static_cast<u32>(n.method == 135 ? p->WeaponAmmo(p->weapon) : game::Player::WeaponMaxAmmo(p->weapon));
        Set(a, r);                                    // (EAX, a full word: 0x000B3AA9 / 0x000B3A85)
        return true;
    }
    case 130:                                         // FsControlChicken(b) 0x000371F0 ((u8)slot0) -> 0x0004F8A0
        if (game::Player* p = g_level->player; p && p->chicken) {   // (0x0004F8A0 tests P+0x7B0)
            if (static_cast<u8>(Get(a))) p->chicken->Activate("ChickenNet");   // 0x00295AC4 -> 0x00044500
            else p->chicken->Dismount();              // 0x00043470 (without Activate's active test)
        }
        return true;
    case 131:                                         // FsSetChickenSpeed(v) 0x00037220 -> 0x000426A0
        if (game::Player* p = g_level->player; p && p->chicken) p->chicken->speed = Get(a);   // +0x60 (raw)
        return true;
    case 132:                                         // FsSetChickenSleep(b) 0x00037250 -> 0x000426B0 ((u8)slot0)
        if (game::Player* p = g_level->player; p && p->chicken) p->chicken->sleep = static_cast<u8>(Get(a));   // +0xB8
        return true;
    case 133:                                         // FsSetChickenShake(v) 0x00037280 -> 0x000426C0 (raw dword)
        if (game::Player* p = g_level->player; p && p->chicken) {
            p->chicken->shakeAmp = Get(a);            // +0xB0 (never read)
            p->chicken->shakeTime = 2.0f;             // +0xB4 = 0x40000000
        }
        return true;
    case 134:                                         // FsShieldTurret(name, b) 0x00037180
        // FindOrCreate(name); a character whose definition is a GenericNPCDef (vf0C(0x200000)) named "gunturret"
        // (_stricmp, 0x00295AB8) gets 0x00086E00(b): +0x8C0 = (u8)b, +0x8C1 = +0x8C2 = 0. (No stage of the shipped
        // levels spawns a gun turret: only the tests run.)
        if (game::TriggerRec* rec = Record(Get(a)); rec && rec->kind == 5 && rec->target) {
            auto* ch = static_cast<game::Character*>(rec->target);
            if (ch->cdef.IsType(0x200000) && Lower(ch->cdef.name.c_str()) == "gunturret")
                if (auto* t = dynamic_cast<game::GunTurret*>(ch)) t->SetShield(static_cast<u8>(Get(a + 4)));
        }
        return true;
    case 137:                                         // FsAddChickenRockets(n) inline 0x000B3A40 -> 0x00042700
        // (the original tests neither S+0x94 nor +0x7B0)
        if (game::Player* p = g_level->player; p && p->chicken) p->chicken->AddRockets(Get(a));
        return true;
    case 390:                                         // ResetBarrelsAndBeams (inline 0x000B0D88)
        if (g_level->animations) g_level->animations->counters = {};   // (per level in the port: KNOWN_ISSUES #39)
        return true;
    case 142:                                         // (unnamed) Script_CallNative's case 0x000B186E: the dword 1
        Set(a, 1);
        return true;
    case 391:                                         // PopBeamCount 0x000B0380 (dword)
    case 392: {                                       // PopBarrelCount 0x000B03A0
        u32 r = 0;
        if (g_level->animations) {
            u32& c = n.method == 391 ? g_level->animations->counters.beams : g_level->animations->counters.barrels;
            r = c;
            if (c) --c;
        }
        Set(a, r);
        return true;
    }
    case 271: {                                       // (orient an AI net) 0x000B06D0 (netName, waypointName)
        if (!g_level->currentStage) return true;      // S+0x5C
        game::AiNet* net = g_level->FindAINet(Get(a) ? Str(Get(a)) : nullptr);   // 0x000313E0
        if (!net) return true;
        const game::StageWaypoint* wp = Get(a + 4) ? FindWaypointAnyStage(Str(Get(a + 4))) : nullptr;   // 0x000C2A40
        if (!wp) return true;
        game::AiNetNode* cur = net->Nearest(wp->position);   // 0x00031D80
        auto nodeAt = [&](s32 i) -> game::AiNetNode* {   // 0x00031C20
            return i >= 0 && i < static_cast<s32>(net->nodes.size()) ? &net->nodes[static_cast<size_t>(i)] : nullptr;
        };
        auto link = [](const game::AiNetNode& nd, s32 k) {   // 0x00031C00 (the dummy {-1, -1.0f} outside the count)
            return k >= 0 && k < nd.count && k < 8 ? nd.links[static_cast<size_t>(k)].node : -1;
        };
        game::AiNetNode* prev = nullptr;
        s32 back2 = -1, back1 = -1;                   // orig: [esp+0x10] / [esp+0x14]
        while (cur) {                                 // (a NULL node: the original faults on its next pass, below)
            if (prev && cur->count == 1) {            // the far end node: no links left
                cur->count = 0;
                return true;
            }
            s32 i = link(*cur, 0);
            game::AiNetNode* nx = nodeAt(i);
            if (nx == prev) {
                i = link(*cur, 1);
                nx = nodeAt(i);
            }
            prev = cur;
            cur = nx;                                 // (no NULL test: the loop's next pass reads it - UB-avoiding stop)
            prev->links[0].node = i;                  // +4
            if (back2 != -1) {
                prev->links[1].node = back2;          // +0xC
                prev->count = 2;
            } else {
                prev->count = 1;
            }
            back2 = back1;
            back1 = i;
        }
        return true;
    }
    case 265: cam->SetShakeForever(GetF(a)); return true;   // FsFShakeCam(amp) 0x00012E00
    case 266: cam->SetShake(GetF(a), GetF(a + 4)); return true;   // FsFShakeCam(amp, time) 0x00011290
    case 267: SetByte(a, cam->shaking); return true;  // FsIsCamShaking 0x000B23A2: byte (S+0x8C)+0x14 (low byte only)
    case 259: return true;                            // FsBlurSetAlpha: 0x000AEDC0 is a bare RET 4
    case 149:                                         // FsPlayDialogue(textId, priority) 0x000D7FD0 = 0x000D7E10(id, 0, p)
    case 150: {                                       // FsPlayDialogue(textId, db, priority) 0x000D7E10 (float)
        f32 r = 0.0f;
        const char* id = Get(a) ? Str(Get(a)) : nullptr;
        const VmAddr dbName = n.method == 150 ? Get(a + 4) : 0;
        const VmAddr priorityAt = n.method == 150 ? a + 8 : a + 4;
        if (id && g_level->text) {                    // (a NULL id reaches TextManager::Find(NULL) in the original)
            const text::LanguageDb* db = dbName ? game::FindLanguageDb(*g_level, Str(dbName))   // 0x000C33F0
                                                : game::FindLanguageDbWithId(*g_level, *g_level->text, id);
            if (db) {
                // orig: the builder without AddLip: SetSoundDir(""), NewDialogue, NewSegment(0), AddSound,
                // CloseSegment(-1), Build; the priority (+0x18), DialogueDisplay::Play 0x000D7BE0, the duration
                // (+0x14; read after Play, which deletes a dialogue it refuses: the port keeps it until here)
                game::DialogueBuilder b(*g_level, g_level->text, db);
                b.NewDialogue();
                b.NewSegment(0.0f);
                (void)b.AddSound(id);
                b.CloseSegment(-1.0f);
                if (ni::NiPointer<game::Dialogue> d = b.Build()) {
                    d->priority = static_cast<s32>(Get(priorityAt));
                    (void)g_level->dialogue->Play(d.Get());
                    r = d->duration;
                }
            }
        }
        SetF(a, r);                                   // fstp [sp]
        return true;
    }
    // ---- the AI groups (natives_batch_a_port.md 4.2): M+0x18 = g_ai.group ----
    case 363: {                                       // (AI group select / create)(name) 0x00031B60 (low byte)
        const char* name = Get(a) ? Str(Get(a)) : nullptr;
        g_ai.group = nullptr;
        if (name)                                     // 0x00031870: NULL -> 0; newest first, case-sensitive strcmp
            for (const auto& g : g_level->aiGroups)
                if (g->name && *g->name == name) {
                    g_ai.group = g.get();
                    break;
                }
        if (!g_ai.group) {
            void* blk = mem::OperatorNew(0x34);       // new(0x34), then the ctor's name copy new(strlen + 1)
            auto g = std::make_shared<game::AiGroup>(name);   // 0x000301B0 (head insert into M+0xC)
            g->block = blk;
            g_level->aiGroups.push_front(g);
            g_ai.group = g.get();
        }
        SetByte(a, g_ai.group ? 1 : 0);               // setne
        return true;
    }
    case 364: {                                       // (AI group add member)(a, b, c, d) 0x00031780 (low byte)
        u8 ok = 0;
        if (g_ai.group && g_ai.agent) {               // M+0x18 && M+0x10
            g_ai.group->AddMember(g_ai.agent, Get(a), Get(a + 4), Get(a + 8), Get(a + 12));   // 0x00030270
            ok = 1;
        }
        SetByte(a, ok);
        return true;
    }
    case 365: {                                       // AIGroupDetach 0x000317C0 (low byte over the stale word)
        u8 ok = 0;
        if (g_ai.group && g_ai.agent) {
            g_ai.group->Detach(*g_ai.agent);          // 0x000304F0
            ok = 1;
        }
        SetByte(a, ok);
        return true;
    }
    case 366:                                         // 0x000317E0 -> 0x00030030 (raw dwords)
        if (g_ai.group) {
            std::memcpy(&g_ai.group->cap2, mem::FromVmAddr(a), 4);       // +0x14 = slot 0
            std::memcpy(&g_ai.group->cap3, mem::FromVmAddr(a + 4), 4);   // +0x10 = slot 1
        }
        return true;
    case 367:                                         // 0x000317F0 -> 0x00030050 (movzx of the slot's byte 0)
        if (g_ai.group) g_ai.group->active = static_cast<u8>(Get(a));
        return true;
    default:
        return false;
    }
}

}  // namespace

void ResetNativeStatics() { g_losCache = {}; }

const NativeInfo* FindNative(u32 cid, u32 method) {
    for (const NativeInfo& n : kNatives)
        if (n.cid == cid && n.method == method) return &n;
    return nullptr;
}

game::LevelData* NativeLevel() { return g_level; }

void SetNativeLevel(game::LevelData* level) {
    g_level = level;
    g_selStage = nullptr;
    g_tank = nullptr;
    g_ai = {};
    g_partDefs.clear();                               // (their VM blocks are not freed: KNOWN_ISSUES #39)
    g_circleDefs.clear();                             // (likewise)
}

void CallGameNative(u32 cid, u32 method, VmAddr& sp) {   // orig: 0x000B0CD0
    const NativeInfo* n = FindNative(cid, method);
    if (!n) {
        std::fprintf(stderr, "illegal native %u:%u\n", cid, method);   // orig: "Illegal CID / method" (debug printf)
        return;
    }
    const VmAddr args = sp - n->pop;
    if (cid == 0 && CallCoreNative(method, sp)) {     // the operators, conversions and math (ScriptNatives.cpp)
        return;
    }
    sp = args;
    if (!Call(*n, args)) {
        NotPorted(*n);
        if (n->push) std::memset(mem::FromVmAddr(sp), 0, n->push);
    }
    sp += n->push;
}

}  // namespace fut::script
