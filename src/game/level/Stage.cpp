// Stages; see Stage.h.
#include "game/level/Stage.h"

#include "core/CrtQsort.h"
#include "engine/ni/NiCollision.h"
#include "game/GameGlobals.h"
#include "game/GameTime.h"
#include "game/defs/CharacterDef.h"
#include "game/defs/Defs.h"
#include "game/defs/EffectDefs.h"
#include "game/object/Character.h"
#include "game/object/Emitter.h"
#include "game/object/GenericNpc.h"
#include "game/object/Pickup.h"
#include "game/object/Static.h"
#include "game/level/Camera.h"
#include "game/level/Hud.h"
#include "game/level/LevelAnimation.h"
#include "game/level/LevelData.h"
#include "game/level/LightManager.h"
#include "game/level/ScriptTriggers.h"
#include "game/level/TriggerManager.h"
#include "game/sound/GameSound.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fut::game {
namespace {

using ni::NiPoint3;
using ni::detail::F;

int StrICmp(const char* a, const char* b) {          // orig: _stricmp (0x001684D1), ASCII folding
    for (;; ++a, ++b) {
        int ca = static_cast<unsigned char>(*a), cb = static_cast<unsigned char>(*b);
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca) return ca - cb;
    }
}

int StrNICmp(const char* a, const char* b, size_t n) {   // orig: _strnicmp (0x00170B4A; C locale: ASCII folding)
    for (; n; --n, ++a, ++b) {
        int ca = static_cast<unsigned char>(*a), cb = static_cast<unsigned char>(*b);
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}

}  // namespace

ni::NiPoint3 VectorExtra(const ni::NiObjectNET& obj) {   // orig: 0x000CDB30 GetVectorExtraData (Stage.h)
    for (ni::NiExtraData* e = obj.extraData.Get(); e; e = e->nextData.Get())
        if (e->size == 0x10 && e->data.size() >= 0x10 && std::memcmp(e->data.data(), "#V#\0", 4) == 0) {
            ni::NiPoint3 v;
            std::memcpy(&v, e->data.data() + 4, 12);
            return v;
        }
    return {};
}

Stage::Stage(LevelData& lvl, const std::string& stageName) : level(lvl), name(stageName) {   // orig: 0x000D01A0
    node = new ni::NiNode;
    level.scene->AttachChild(node.Get(), true);
    node->name = "Stage::m_spNode(" + stageName + ")";
    node->UpdateDownwardPass(time, true);             // orig: NiAVObject::Update(+0x1C, true)
    if (level.renderer) geometryGroup = level.renderer->CreateGeometryGroup(0);   // +0xAC (L: its place in the ctor)
    // The level's three stage-name slots (+0x4C..+0x54): a new name goes into the first empty one.
    for (const auto& slot : level.stageNames)
        if (!slot.empty() && StrICmp(slot.c_str(), stageName.c_str()) == 0) return;
    for (auto& slot : level.stageNames)
        if (slot.empty()) {
            slot = stageName;
            return;
        }
}

namespace {
void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}
}  // namespace

void Stage::ResetObjects() {                          // orig: 0x000CE570
    g_decomposeCorpses = 1;                           // orig: 0x000CE57E (byte 0x002D6214)
    for (GameObject* o = level.NextObject(nullptr); o;) {   // the level object's list
        GameObject* next = level.NextObject(o);
        if (o->def->IsType(0x10)) delete o;           // vf00(1)
        o = next;
    }
    for (GameObject* o = NextObject(nullptr); o;) {
        GameObject* next = NextObject(o);
        if (!o->def->IsType(2)) {                     // 0x000CE5E8: the sounds on its trigger record stop
            if (TriggerRec* r = level.triggers->FindByTarget(o)) level.Sound().StopAllOnTargetable(r);   // 0x000CE60B
            if (!o->def->IsType(0x1000) && !o->def->IsType(0x20000)) delete o;
        }
        o = next;
    }
    for (const StageWaypoint& w : waypoints)          // 0x000CE650: the sounds on the waypoints' records (+0x6C)
        if (TriggerRec* r = level.triggers->FindByTarget(&w)) level.Sound().StopAllOnTargetable(r);   // 0x000CE677
    // 0x000CE682: the sounds on the records of the "Triggers" node's children ("<kind>::<name>": Find(name))
    if (auto* t = node ? dynamic_cast<ni::NiNode*>(GetObjectByName(node.Get(), "Triggers")) : nullptr)   // vf54
        for (size_t i = 0; i < t->Children().size(); ++i) {
            const ni::NiAVObject* c = t->Children()[i].Get();
            if (!c || c->name.empty()) continue;
            const size_t colon = c->name.find(':');
            if (colon == std::string::npos || colon + 2 > c->name.size()) continue;   // (port guard past the end)
            if (TriggerRec* r = level.triggers->Find(c->name.c_str() + colon + 2))   // 0x000DAFA0
                level.Sound().StopAllOnTargetable(r); // 0x000CE701
        }
    for (const StageInstance& r : instances)          // +0x60 / +0x64
        if (r.id && !(level.GetObjFlags(r.id) & 1)) level.SetObjFlags(r.id, 0);
}

GameObject* Stage::SpawnInstance(StageInstance& r, bool check) {   // orig: 0x000CF750
    if (check) {
        if (level.GetObjFlags(r.id) & 0x1B) return nullptr;
        if (StrNICmp(r.def->name.c_str(), "nibbler", 7) == 0 && level.NibblerState(r.id, 0))   // 0x002A3E20
            return nullptr;
    }
    Owner* owner = this;                              // stage+4
    if (r.def->IsType(2)) owner = &level;
    const ni::NiMatrix3 rot = ni::MakeYRotation(r.yaw);   // 0x001030B0
    GameObject* o = r.def->Spawn(level, owner, r.position, rot, r.label.c_str());   // vf14
    if (!o) return nullptr;
    if (r.def->IsType(4)) static_cast<Pickup*>(o)->SetKey(static_cast<s32>(r.id), 0);   // 0x00076590 (even unchecked)
    if (check) {
        o->SetId(r.id);                               // vf14
        level.SetObjFlags(o->id, 0x21);
    }
    o->ApplyInstanceString(r.text ? r.text->c_str() : nullptr);   // vf2C(rec+0x38)
    return o;
}

void Stage::SpawnByLabel(const char* label, bool check) {   // orig: 0x000D0790
    if (!label) return;
    for (StageInstance& r : instances)
        if (StrICmp(label, r.label.c_str()) == 0) {
            SpawnInstance(r, check);
            return;
        }
}

bool Stage::Update() {                                // orig: 0x000CDBE0 (pickup_port.md 3.1 / 3.2)
    if (!IsLoaded()) return true;                     // the stage clock does not advance either
    for (auto& r : camFades)                          // CamFadeRec::Update 0x000CD300
        if (r) {
            level.camFadeDirty = true;                // byte DAT_002D6D98
            r->state = F(double(g_frameStep) + r->state);
        }
    Player* p = level.player;
    for (GameObject* o = NextObject(nullptr); o;) {
        if (!p) break;                                // (checked per object): no player -> nothing ticks
        GameObject* next = NextObject(o);
        if (!o->Tick()) {                             // vf08 (Static: 0x000945F0)
            delete o;
            o = next;
            continue;
        }
        if (p->IsDead() || !o->def->IsType(4)) {
            o = next;
            continue;
        }
        auto* pk = static_cast<Pickup*>(o);
        if (!pk->CanCollect()) {
            o = next;
            continue;
        }
        NiPoint3 d = o->GetWorldPosition();
        const NiPoint3 pw = p->GetWorldPosition();
        d = {F(double(d.x) - pw.x), F(double(d.y) - pw.y), F(double(d.z) - pw.z)};
        if (!(-1.0f < d.y && d.y < 10.0f) && !pk->IgnoresRange()) {   // TEST AH,5; JP / JNP
            o = next;
            continue;
        }
        d.y = 0.0f;
        pk->playerDist2 = F(double(d.x) * d.x + double(d.z) * d.z);
        if (!pk->IgnoresRange() && !(pk->playerDist2 < 25.0f)) {   // [0x00297240]; TEST AH,5; JP
            o = next;
            continue;
        }
        f32 t = 1.0f;
        const NiPoint3 pp = p->GetWorldPosition();   // (a second vf34 call)
        const NiPoint3 nd{F(-double(d.x)), F(-double(d.y)), F(-double(d.z))};   // FCHS: nd.y = -0.0
        // (sic) the ray starts at the player and points away from the pickup
        if (!pk->IgnoresRange() && ni::RayCast(pp, nd, t, true, 3)) {
            o = next;
            continue;
        }
        bool allowed = true;                          // 0x000CDDA3..0x000CE2F4: the dispatch by type
        auto snd = [&](u32 sound) { p->SoundAt(sound, &pp, 3, 0, nullptr); };
        Def* rechargeDef = nullptr;                   // EDI = Session::FindDef("RechargeEff") 0x000C3F10 (0x002A2828)
        auto recharge = [&](int n) {                  // def->0x000708D0(S+0x5C ? S+0x5C + 4 : 0, "Rechargeive", n)
            if (auto* rd = dynamic_cast<RechargeEffectDef*>(rechargeDef))
                (void)rd->SpawnAt(level, level.currentStage, "Rechargeive", n);   // 0x002A3D9C
        };
        if (o->def->IsType(0x800)) {                  // Ammo
            if (level.defs && (rechargeDef = level.defs->Find("RechargeEff"))) {
                const s32 c = static_cast<Ammo*>(o)->count;
                if (c == 25) snd(0x72), recharge(3);
                else if (c == 50) snd(0x73), recharge(4);
                else if (c == 100) snd(0x73), recharge(5);
                else snd(0x72);
            }
        } else if (o->def->IsType(0x100000)) {        // Charge
            if (!p->CanTakeCharge()) allowed = false;  // vf16C 0x000504B0
            else snd(0x7A);
        } else if (o->def->IsType(0x400)) {           // Powerup
            if (!(p->health != p->maxHealth) && !p->HasCockpit()) {   // 0x000CDBC0
                allowed = false;
            } else if (level.defs && (rechargeDef = level.defs->Find("RechargeEff"))) {
                const f32 a = static_cast<Powerup*>(o)->amount;
                u32 bits;
                std::memcpy(&bits, &a, 4);
                if (a < 30.0f) snd(0x77), recharge(0);
                else if (a < 100.0f) snd(0x78), recharge(1);
                else if (bits == 0x42C80000u) snd(0x78), recharge(2);
                else snd(0x78);
            }
        } else if (o->def->IsType(8)) {               // Weapon
            bool held = false;
            for (GameObject* i = p->inventory.NextObject(nullptr); i && !held; i = p->inventory.NextObject(i))
                held = i->def == o->def;
            if (!held) {
                snd(0x74);
                // 0x000CE0AD: (S+0x118)->0x000E5BC0("weapons", "run_cool_effect", "") (HUDWeapons vf03)
                if (level.hud) level.hud->Command("weapons", "run_cool_effect", "");
            }
        } else {
            const char* n = o->def->name.c_str();
            auto eq = [&](const char* s) { return StrICmp(n, s) == 0; };
            if (eq("BenderRuby") || eq("LeelaGoldbar") || eq("FryCash") || eq("Fish_skeleton_pickup"))
                snd(0x76);
            else if (eq("Drill") || eq("Cup") || eq("Bucket") || eq("Book") || eq("Cog") || eq("Calculator") ||
                     eq("HairDryer") || eq("FireExtinguisher") || eq("Pogostick") || eq("PairOfPliers") ||
                     eq("Jack") || eq("Thing1") || eq("propeller1") || std::strstr(n, "keycard") ||
                     std::strstr(n, "KeyCard"))
                snd(0x75);
        }
        if (allowed) {
            o->SetOwner(&p->inventory);               // vf10(P+0x38)
            if (o->def->IsType(8)) static_cast<Weapon*>(o)->TakeOut();   // vf64 0x0007DD00
        }
        o = next;
    }
    time = F(double(g_frameStep) + time);             // +0x1C
    return true;
}

void Stage::Render() {                                // orig: 0x000CE340
    for (GameObject* o = NextObject(nullptr); o;) {
        GameObject* next = NextObject(o);
        if (o->def->IsType(1)) {                      // characters: a false return deletes
            if (!o->PostUpdate()) delete o;
        } else {
            o->PostUpdate();                          // vf0C (Static: 0x00094F50)
        }
        o = next;
    }
    for (auto& r : camFades)                          // +0x7C (count +0x84), index order
        if (r) r->Render(level);                      // 0x000CD3D0
}

void CamFadeRec::Render(LevelData& level) {           // orig: 0x000CD3D0
    if (!geometry) return;
    Camera* cam = level.camera.get();                 // S+0x8C
    Player* player = level.player;                    // S+0x94
    bool fadeOut = false;
    if (player && cam) {
        const ni::NiPoint3& c = cam->position;        // +0x48
        const ni::NiPoint3& b = geometry->worldBound.center;   // +0x20
        const ni::NiPoint3 d{F(double(b.x) - c.x), F(double(b.y) - c.y), F(double(b.z) - c.z)};
        if (level.camFadeDirty) {                     // DAT_002D6D98
            // (vf34 is called twice, the first result unused: GetWorldPosition has no side effect)
            const ni::NiPoint3 p = player->GetWorldPosition();
            ni::NiPoint3& dir = level.camFadeDir;     // DAT_002EAF14
            dir = {F(double(p.x) - c.x), F(double(p.y) - c.y), F(double(p.z) - c.z)};
            const double len = ni::Unitize(dir);      // 0x000116A0
            level.camFadeDirty = false;
            level.camFadeDist = F(len - double(std::bit_cast<f32>(0x40400000u)));   // - 3.0 ([0x0029563C])
            // 0x00037090("PLAYER"): the record's character (kind 5) +0x630, else 0
            TriggerRec* rec = level.triggers ? level.triggers->FindOrCreate("PLAYER") : nullptr;
            level.camFadePlayerControl =
                rec && rec->kind == 5 && rec->target && static_cast<Character*>(rec->target)->hasControl != 0;
        }
        if (level.camFadePlayerControl) {             // DAT_002EAF10
            const ni::NiPoint3& dir = level.camFadeDir;
            const double dot = (double(d.z) * dir.z + double(d.y) * dir.y) + double(dir.x) * d.x;
            const double dist = level.camFadeDist;
            if (dot > -double(geometry->worldBound.radius) && dot < dist) {   // FCOMPP (TEST AH,0x41), FCOMP (AH,5; JP)
                const double len2 = (double(d.x) * d.x + double(d.y) * d.y) + double(d.z) * d.z;   // 0x00011680
                fadeOut = dist * dist > len2;         // FCOMPP; TEST AH,0x41
            }
        }
    }
    if (fadeOut) {
        const double v = double(current) - (double(state) + state);
        current = F(v);
        if (v < double(minAlpha)) current = minAlpha; // FCOMP +0x18 on the unrounded value; TEST AH,5; JP
    } else {
        const double v = (double(state) + state) + current;
        current = F(v);
        if (v > double(maxAlpha)) current = maxAlpha; // FCOMP +0x14; TEST AH,0x41
    }
    if (material && material->alpha != current) {     // FCOMP; TEST AH,0x44; JNP (a NaN differs)
        material->alpha = current;                    // +0x50
        ++material->revision;                         // +0x54
        if (alpha) {                                  // the blend bit (byte +0x14 bit 0)
            if (std::bit_cast<u32>(current) == 0x3F800000u) alpha->flags = u16(alpha->flags & ~1u);
            else alpha->flags = u16(alpha->flags | 1u);
        }
        if (auto* shape = dynamic_cast<ni::NiTriShape*>(geometry.Get())) {   // DynamicCast 0x0030EC38
            ni::NiGeometryData* data = shape->modelData.Get();   // +0xC8 (the original has no test)
            if (data && !data->colors.empty()) {      // +0x24
                for (u32 i = 0; i < data->vertexCount && i < data->colors.size(); ++i)   // +8
                    data->colors[i].a = current;      // +0xC of each NiColorA
                data->revisionFlags = u8(data->revisionFlags | 4);   // +0x2E |= 4
            }
        }
    }
    state = 0;                                        // +0x0C
}

void Stage::Reset() {                                 // orig: 0x000D0800
    ResetObjects();
    for (u32 mask : {4u, 0x40u, 0x80u})               // 0x000D0740(mask): records in order
        for (size_t i = 0; i < instances.size(); ++i)
            if (instances[i].def->IsType(mask)) SpawnInstance(instances[i], true);
    for (GameObject* o = NextObject(nullptr); o; o = NextObject(o))
        if (o->def->IsType(0x1000)) static_cast<Static*>(o)->ResetPhysics();   // vf50 0x00094590
}

void Stage::LoadAINets(ni::NiObjectNET* o) {          // orig: 0x000CE760
    auto* nd = dynamic_cast<ni::NiNode*>(o);
    if (!nd) return;
    const size_t n = nd->Children().size();
    for (size_t i = 0; i < n; ++i) {
        ni::NiAVObject* ch = i < nd->Children().size() ? nd->Children()[i].Get() : nullptr;
        if (!ch) continue;
        const ni::NiExtraData* ed = ch->extraData.Get();   // 0x0013EB60 (no null test in the original)
        if (!ed) continue;
        const u32 count = ed->size / 0x1C;
        auto net = std::make_unique<AiNet>();              // new(0x20), then new(count * 0x50)
        std::strncpy(net->name.data(), ch->name.c_str(), 15);
        net->nodes.resize(count);
        for (u32 j = 0; j < count; ++j) {
            const u8* rec = ed->data.data() + j * 0x1C;
            AiNetNode& nn = net->nodes[j];
            std::memcpy(&nn.pos, rec, 12);
            nn.count = 0;
            for (int k = 0; k < 8; ++k) {
                s16 s;
                std::memcpy(&s, rec + 12 + 2 * k, 2);
                if (s != -1) {
                    nn.links[static_cast<size_t>(k)].node = s;
                    ++nn.count;
                }
            }
        }
        level.aiNets.push_front(std::move(net));
    }
}

namespace {
int CompareEdgeMin(const void* pa, const void* pb) {  // orig: 0x000CD130 (the qsort comparator)
    const auto* a = static_cast<const Edge*>(pa);
    const auto* b = static_cast<const Edge*>(pb);
    if (a->minX < b->minX) return -1;                 // TEST AH,5; JP
    if (a->minX > b->minX) return 1;                  // TEST AH,0x41; JNE
    return 0;                                         // (equal or NaN)
}
}  // namespace

void Stage::LoadEdges(ni::NiObjectNET* o) {           // orig: 0x000CE950 (ECX unused)
    const ni::NiExtraData* ed = o->extraData.Get();   // 0x0013EB60
    if (!ed) return;
    const u32 n = ed->size / 24;                      // MUL 0xAAAAAAAB; SHR 4
    auto set = std::make_shared<EdgeSet>();           // new(0xC), ctor 0x000CD1E0
    set->edges.resize(n);                             // new(n * 0x24) (also for n == 0)
    for (u32 i = 0; i < n; ++i) {
        Edge& e = set->edges[i];
        std::memcpy(&e.p0, ed->data.data() + i * 24, 12);   // (dword copies)
        std::memcpy(&e.p1, ed->data.data() + i * 24 + 12, 12);
        e.active = 1;                                 // byte +0x20
        const bool swap = e.p0.x > e.p1.x;            // FLD p0.x; FCOM p1.x; TEST AH,0x41; JNE keep (NaN: keep)
        e.minX = swap ? e.p1.x : e.p0.x;              // +0x18
        e.maxX = swap ? e.p0.x : e.p1.x;              // +0x1C
    }
    crt::Qsort(set->edges.data(), n, sizeof(Edge), CompareEdgeMin);   // 0x001679F0 (also with n == 0)
    for (u32 i = 1; i < n; ++i)                       // the running maximum (TEST AH,5; JP: only '<'; dword copy)
        if (set->edges[i].maxX < set->edges[i - 1].maxX) set->edges[i].maxX = set->edges[i - 1].maxX;
    level.edgeSets.push_front(std::move(set));        // +0 = DAT_002EAF0C; DAT_002EAF0C = set
}

void Stage::Unload() {                                // orig: 0x000D0510
    request.reset();                                  // +0x18 (a queued request is collected by the next step)
    if (level.animations) level.animations->UnbindAll(this);   // 0x000BB080
    DeleteObjects();                                  // while (+0x08) head->vf00(1)
    waypoints.clear();                                // +0x6C / +0x68
    instances.clear();                                // +0x64 / +0x60
    bsp = nullptr;                                    // +0x24
    file.reset();                                     // (the last NiBsp reference: ~NiBsp leaves g_bspList)
    const size_t n = fixtures.size();                 // +0x4C, captured
    for (size_t i = 0; i < n; ++i) {
        ni::NiAVObject* f = fixtures[i].Get();
        if (!f) continue;
        if (auto* room = dynamic_cast<ni::NiRoom*>(f->parent)) room->DetachFixture(f);   // 0x0011EEA0
        fixtures[i] = nullptr;                        // RemoveAt 0x000CFE40
    }
    fixtures.clear();                                 // Compact (IsLoaded() is false from here on)
    rooms.clear();                                    // RemoveAll + Compact
    for (auto& c : camFades) c.reset();               // 0x000CD320 + delete, index order
    camFades.clear();
}

Stage::~Stage() {                                     // orig: 0x000D0BF0
    Unload();                                         // (a no-op when LevelData::DeleteStage already ran it)
    if (node) {
        level.scene->DetachChild(node.Get());         // vfA4: the scene slot becomes empty
        node = nullptr;
    }
    if (geometryGroup && level.renderer) level.renderer->DestroyGeometryGroup(geometryGroup);   // +0xAC vf00, vf08
    geometryGroup = nullptr;
    if (level.triggers) level.triggers->ResolveAll(); // 0x000D0D55
}

void Stage::DetachObject(GameObject& object) {       // orig: 0x000CEE00
    node->DetachChild(object.GetNode());              // (no-op when the node is not a child of the stage node)
}

bool Stage::ContainsRoom(const ni::NiRoom* room) const {   // orig: 0x000CE410
    if (!room) return false;
    for (const auto& r : rooms)
        if (r.Get() == room) return true;
    return false;
}

const StageWaypoint* Stage::FindWaypoint(const std::string& wpName) const {   // orig: 0x000CE450
    for (const auto& w : waypoints)
        if (StrICmp(w.name.c_str(), wpName.c_str()) == 0) return &w;
    return nullptr;
}

void Stage::LoadWaypoints(ni::NiNode& nd) {           // orig: 0x000CF880
    waypoints.clear();
    const auto& kids = nd.Children();
    // The original re-tests the same index while shrinking the count, so the first child that is not a NiUDSNode
    // ends the list there.
    for (size_t i = 0; i < kids.size(); ++i) {
        auto* ch = dynamic_cast<ni::NiUDSNode*>(kids[i].Get());
        if (!ch) break;
        StageWaypoint w;
        f32 b, c;
        ni::ToEulerAnglesXYZ(ch->local.rotate, w.yaw, b, c);
        w.name = ch->name.substr(0, 15);
        w.position = ch->local.translate;
        waypoints.push_back(std::move(w));
    }
}

void Stage::LoadLadders(ni::NiNode& nd) {             // orig: 0x000CFA20 (same loop, any NiAVObject)
    ladders.clear();
    const auto& kids = nd.Children();
    for (size_t i = 0; i < kids.size(); ++i) {
        ni::NiAVObject* ch = kids[i].Get();
        if (!ch) break;
        StageLadder l;
        f32 b, c;
        ni::ToEulerAnglesXYZ(ch->local.rotate, l.yaw, b, c);
        l.position = ch->local.translate;
        const ni::NiPoint3 v = VectorExtra(*ch);
        l.v0 = v.x;
        l.v1 = v.y;
        ladders.push_back(l);
    }
}

void Stage::LoadLights(ni::NiNode& nd) {               // orig: 0x000CFBD0
    const size_t count = nd.Children().size();       // (captured; the live size is re-checked)
    for (size_t i = 0; i < count; ++i) {
        auto* ch = i < nd.Children().size() ? dynamic_cast<ni::NiUDSNode*>(nd.Children()[i].Get()) : nullptr;
        if (!ch) continue;                            // (the original faults on a non-NiUDSNode child)
        // "<TYPE>:<rest>": the name is cut at the ':' in place (the node keeps only the type).
        const size_t colon = ch->name.find(':');
        if (colon == std::string::npos) continue;     // (the original writes through a null pointer)
        ch->name.resize(colon);
        const std::string& type = ch->name;
        const ni::NiPoint3 pos = ch->local.translate;
        const f32 radius = VectorExtra(*ch).x;        // the file velocity, as "#V#" extra data
        ni::NiColor color{0, 0, 0};
        for (ni::NiExtraData* e = ch->extraData.Get(); e; e = e->nextData.Get())
            if (auto* s = dynamic_cast<ni::NiStringExtraData*>(e)) {
                std::sscanf(s->string.c_str(), "%f,%f,%f", &color.r, &color.g, &color.b);
                break;
            }
        u32 t = 0;
        if (StrICmp(type.c_str(), "SLIME") == 0) {
            t = 6;
        } else if (StrICmp(type.c_str(), "LAVA") == 0) {
            t = 5;
        } else if (StrICmp(type.c_str(), "ALIEN_GOO") == 0) {
            t = 7;
        } else if (StrICmp(type.c_str(), "LAMP") == 0) {
            t = 1;
        }
        if (level.lightManager) level.lightManager->AddLight(level, pos, color, radius, t, true);   // (released)
    }
}

void Stage::LoadInstances(ni::NiNode& nd) {           // orig: 0x000CEE50
    // Phase A, the records (only while the stage has none: a reload keeps them).
    // (The original allocates one record per NiUDSNode child; a child whose name does not parse leaves the last
    // records uninitialised - never the case in the shipped data. The port keeps the parsed ones.)
    if (instances.empty()) {
        // One name buffer for the whole loop, as the original's stack buffer: a label read past a name's end ("x|1:")
        // sees what earlier, longer names left there (its first contents are unknown: zero here).
        char buf[260] = {};
        for (const auto& kid : nd.Children()) {
            auto* ch = dynamic_cast<ni::NiUDSNode*>(kid.Get());
            if (!ch) continue;
            const size_t n = std::min(ch->name.size(), sizeof buf - 1);
            std::memcpy(buf, ch->name.data(), n);
            buf[n] = 0;
            char* bar = std::strchr(buf, '|');
            if (!bar) continue;
            *bar = 0;
            const char* label = nullptr;              // "<def>|<id>: <label>": the label starts two past the ':'
            if (char* colon = std::strchr(bar + 1, ':')) {
                *colon = 0;
                label = colon + 2;
            }
            unsigned id = 0;
            if (std::sscanf(bar + 1, "%04x", &id) != 1) continue;
            Def* def = level.defs ? level.defs->Find(buf) : nullptr;
            if (!def) continue;
            StageInstance r;
            r.position = ch->local.translate;
            f32 b, c;
            ni::ToEulerAnglesXYZ(ch->local.rotate, r.yaw, b, c);
            r.def = def;
            r.label = std::string(label ? label : "(noname)").substr(0, 0x1F);   // strncpy(+0x10, .., 0x1F)
            r.id = id;
            if (auto* s = dynamic_cast<ni::NiStringExtraData*>(ch->extraData.Get())) r.text = s->string;   // head only
            instances.push_back(std::move(r));
        }
    }
    // Phase B: spawn every record whose level slot is free (flags & 0x1B: live, collected, destroyed, dead).
    for (StageInstance& r : instances) {
        const u32 flags = level.GetObjFlags(r.id);
        if (flags & 0x1B) continue;
        bool spawnAnyway = (flags >> 5) & 1;          // spawned on an earlier load
        Owner* owner = this;
        if (r.def->IsType(2)) {                       // players belong to the level object
            owner = &level;
            spawnAnyway = true;
        }
        if (r.def->IsType(0x20000)) spawnAnyway = true;   // liquids
        if (r.def->IsType(0x1000)) {                  // statics
            const std::string& dn = r.def->name;
            if (StrNICmp(dn.c_str(), "emitter", 7) == 0 || std::strncmp(dn.c_str(), "SteamEmitter", 12) == 0) {
                SpawnEmitterPlaceholder(r);
                continue;
            }
        } else if (!spawnAnyway) {
            continue;
        }
        const ni::NiMatrix3 rot = ni::MakeYRotation(r.yaw);   // orig: 0x001030B0
        GameObject* o = r.def->Spawn(level, owner, r.position, rot, r.label.c_str());
        if (!o) continue;
        o->SetId(r.id);
        level.SetObjFlags(o->id, 0x21);
    }
}

namespace {
// orig: the side direction of both placeholder branches: row 0 of RotY(yaw), evaluated per component as
// ((R[2][c] * 0) + (R[1][c] * 0)) + R[0][c] * 1 with the constants (1, 0, 0) at 0x002D77E0 (signs of zero kept).
ni::NiPoint3 SideDirection(f32 yaw) {
    const ni::NiMatrix3 r = ni::MakeYRotation(yaw);   // (the original's row r = this matrix's column r)
    ni::NiPoint3 d;
    f32* out[3] = {&d.x, &d.y, &d.z};
    for (int c = 0; c < 3; ++c)
        *out[c] = F((double(r.m[c][2]) * 0.0f + double(r.m[c][1]) * 0.0f) + double(r.m[c][0]) * 1.0f);
    return d;
}
}  // namespace

void Stage::SpawnEmitterPlaceholder(const StageInstance& r) {   // orig: 0x000CEE50, 0x000CF219-0x000CF698
    const std::string& dn = r.def->name;
    Def* effect = nullptr;
    bool side = false;
    bool sparkFountain = false;
    if (StrNICmp(dn.c_str(), "emitter", 7) == 0) {    // "emitter<up|side>_<effect>" (_strnicmp 0x00170B4A)
        const size_t us = dn.find('_', 7);
        if (us == std::string::npos) return;
        const size_t n = us - 7;
        if (n > 4) return;
        const std::string dir5 = dn.substr(7, n);
        if (StrICmp(dir5.c_str(), "up") == 0) {
            side = false;
        } else if (StrICmp(dir5.c_str(), "side") == 0) {
            side = true;
        } else {
            return;
        }
        const char* effectName = dn.c_str() + us + 1;
        effect = level.defs ? level.defs->Find(effectName) : nullptr;
        if (!effect || !effect->IsType(0x4000)) return;
        sparkFountain = std::strcmp(effectName, "effect_SparkFountain") == 0;   // memcmp of 0x15 bytes
    } else {                                          // "SteamEmitter<suffix>" (strncmp 0x001675D0)
        effect = level.defs ? level.defs->Find("effect_SteamEmitter") : nullptr;
        if (!effect) return;                          // (no type check here)
        const char* suffix = dn.c_str() + 12;
        side = std::strcmp(suffix, "Up") != 0 && std::strcmp(suffix, "Side") == 0;
    }
    const ni::NiPoint3 dir = side ? SideDirection(r.yaw) : ni::NiPoint3{0, 1, 0};   // orig: (0, 1, 0) at 0x002D77EC
    GameObject* o = effect->SpawnEffect(level, r.position, dir);
    if (!o) return;
    if (sparkFountain) {                              // 0x000CF45F: effect_SparkFountain's loop sound
        const u32 sample = level.loadSound ? level.loadSound("Sounds/Level1/Generic/ElecBox-Loop.wav", 0) : 0;
        if (auto* e = dynamic_cast<EmitterInstance*>(o))   // (the spark fountain's object: an EmitterInstance)
            e->PlayLoopSound(sample, 40.0f, 30.0f);   // 0x000CF47C (0x42200000, 0x41F00000)
        else
            NotPorted("Stage: the spark fountain's loop sound on an object that is not an emitter");
    }
    o->SetName(r.label.c_str());
    o->SetId(r.id);
    level.SetObjFlags(o->id, 0x21);
}

void Stage::BindSequence(ni::NiSequence& seq, level::LevelAnimations& animations) {
    if (seq.entries.empty()) return;
    const std::string& objectName = seq.entries[0].name;   // orig: GetObjectName(0)
    level::LevelAnimation* la = animations.FindByName(objectName);
    if (!la) return;
    if (!la->target) {
        for (const auto& room : rooms) {
            if (!room) continue;
            if (ni::NiAVObject* hit = GetObjectByName(room.Get(), objectName.c_str())) {
                la->Bind(this, hit);
                break;
            }
        }
        if (!la->target)
            if (ni::NiAVObject* hit = GetObjectByName(level.scene.Get(), objectName.c_str())) {
                bool inRoom = false;                  // objects inside a room are bound through the rooms only
                for (ni::NiNode* p = hit->parent; p && !inRoom; p = p->parent) inRoom = dynamic_cast<ni::NiRoom*>(p);
                if (!inRoom) la->Bind(this, hit);
            }
    }
    level::LevelAnimClip* clip = la->FindClip(seq.name);
    if (!clip) {
        ni::NiTimeController* k = seq.ReferenceController();
        clip = la->AddClip(seq.name, F((double(k->hiKeyTime) - k->loKeyTime) / k->frequency));
    }
    la->SetClipSequence(*clip, &seq);
}

void Stage::RequestStream() {                         // orig: 0x000CEDAE
    if (request) return;
    if (stream::Manager* m = stream::Manager::Current()) request = m->Create(*this, "Stage", name);
}

bool Stage::OnStream(stream::Request& r) {
    if (r.state == stream::State::New) {
        r.SetPath("Levels\\" + level.name + "\\" + name + ".nif");
        return true;
    }
    if (ni::NiStream* s = r.Stream())
        if (level.animations) Load(*s, *level.animations);
    request.reset();
    return false;
}

void Stage::Load(ni::NiStream& stream, level::LevelAnimations& animations) {   // orig: 0x000D1410
    // orig: 0x000C2730(0) == 0: no player object existed before this stage's file was processed.
    const bool noPlayerPos = level.players.empty();
    const auto& objs = stream.Objects();
    std::vector<ni::NiObject*> tops;
    for (u32 link : stream.TopLevelLinks()) tops.push_back(link < objs.size() ? objs[link].Get() : nullptr);

    // Pass 1, in top-level order.
    for (ni::NiObject* o : tops) {
        if (!o) continue;
        std::string objName;
        if (auto* net = dynamic_cast<ni::NiObjectNET*>(o)) {
            objName = net->name;
            if (StrICmp(objName.c_str(), "AINets") == 0) {   // orig: 0x000CE760
                LoadAINets(net);
                continue;
            }
            if (StrICmp(objName.c_str(), "Edges") == 0) {    // orig: 0x000D162A
                LoadEdges(net);
                continue;
            }
        }
        if (auto* room = dynamic_cast<ni::NiRoom*>(o)) {
            ni::NiRoom* levelRoom =
                level.roomGroup ? dynamic_cast<ni::NiRoom*>(GetObjectByName(level.roomGroup.Get(), room->name.c_str()))
                                : nullptr;
            if (!levelRoom) continue;
            const size_t count = room->Children().size();
            for (size_t c = 0; c < count; ++c) {
                ni::NiAVObject* ch = c < room->Children().size() ? room->Children()[c].Get() : nullptr;
                if (!ch) continue;
                levelRoom->AttachFixture(ch);         // (moves it out of the stage file's room)
                fixtures.push_back(ch);
            }
            rooms.push_back(levelRoom);
            levelRoom->UpdateEffects();               // 0x000D17DC
            levelRoom->UpdateProperties();
            levelRoom->UpdateDownwardPass(time, true);
            continue;
        }
        if (auto* b = dynamic_cast<ni::NiBsp*>(o)) {
            bsp = b;
            continue;
        }
        if (dynamic_cast<ni::NiAVObject*>(o)) {
            if (StrNICmp(objName.c_str(), "LinkStart:", 10) == 0) {   // _strnicmp 0x00170B4A
                linkStart = objName.substr(10) == "NULL" ? "" : objName.substr(10);
                continue;
            }
            if (StrNICmp(objName.c_str(), "LinkEnd:", 8) == 0) {
                linkEnd = objName.substr(8) == "NULL" ? "" : objName.substr(8);
                continue;
            }
        }
        if (auto* nd = dynamic_cast<ni::NiNode*>(o)) {
            if (StrICmp(objName.c_str(), "Waypoints") == 0) {
                LoadWaypoints(*nd);
            } else if (StrICmp(objName.c_str(), "Triggers") == 0) {
                node->AttachChild(nd, true);
                nd->UpdateDownwardPass(time, true);
            } else if (StrICmp(objName.c_str(), "Ladders") == 0) {
                LoadLadders(*nd);
            }
        }
    }
    if (level.roomGroup) level.roomGroup->UpdateWorldBound();

    // Pass 2: sequences -> level animations.
    for (ni::NiObject* o : tops)
        if (auto* seq = dynamic_cast<ni::NiSequence*>(o)) BindSequence(*seq, animations);
    // PreCachedScenes: every child of every level room this stage filled (counts captured).
    animations_ = &animations;
    const size_t roomCount = rooms.size();
    for (size_t i = 0; i < roomCount; ++i) {
        ni::NiRoom* room = rooms[i].Get();
        if (!room) continue;
        const size_t n = room->Children().size();
        for (size_t c = 0; c < n; ++c)
            if (c < room->Children().size())
                if (ni::NiAVObject* ch = room->Children()[c].Get()) PreCache(ch);
    }
    animations_ = nullptr;

    // Pass 3: instances and lights.
    for (ni::NiObject* o : tops) {
        auto* nd = dynamic_cast<ni::NiNode*>(o);
        if (!nd || nd->name.empty()) continue;
        if (StrICmp(nd->name.c_str(), "Instances") == 0) LoadInstances(*nd);
        if (StrICmp(nd->name.c_str(), "Lights") == 0) LoadLights(*nd);
    }
    if (level.triggers) level.triggers->ApplyTriggerFlags(*this);   // orig: Session::ApplyTriggerFlags 0x000C2BF0
    if (const StageWaypoint* wp = FindWaypoint("Start")) {   // orig: 0x000D1D80-0x000D1EB7
        const ni::NiMatrix3 mat = ni::MakeYRotation(wp->yaw);   // orig: 0x001030B0
        ni::NiPoint3 pos = wp->position;
        pos.y = F(double(pos.y) + 1.0f);              // orig: FADD [0x00293A98]
        if (!level.player) {
            CharacterDef* d = level.defs ? level.defs->FindCharacterDef("fry") : nullptr;   // orig: 0x000C3FB0
            if (!d && level.defs) {
                d = static_cast<CharacterDef*>(level.defs->Add(std::make_unique<PlayerDef>("fry")));   // 0x0005E610
                d->streamContext = level.defContext;
                d->RequestStream();                   // orig: vf04 (Chars\fry.ucf, loaded after this stage)
            }
            GameObject* o = d ? d->Spawn(level, &level, pos, mat, "PLAYER") : nullptr;
            if (level.camera) level.camera->SetFollowTarget(dynamic_cast<Character*>(o));   // orig: 0x00018FB0
        } else if (noPlayerPos) {
            // (before the move: the camera uses the instance position)
            if (level.camera) level.camera->SetFollowTarget(level.player);   // orig: 0x00018FB0
            level.player->SetPosition(pos);           // orig: vf1C (body only)
            level.player->SetRotation(mat);           // orig: vf24 (+0x570 only)
        }
    }
    if (level.triggers) level.triggers->ResolveAll();   // orig: RefreshTriggers 0x000B72D0
    if (level.scriptTriggers) level.scriptTriggers->RefreshAll();   // (its loop over the scripts' triggers)
    if (level.player) level.player->OnStageLoaded(*this);   // orig: 0x000D1E87 (S+0x94; the rat ride's AddStage)
}

}  // namespace fut::game
