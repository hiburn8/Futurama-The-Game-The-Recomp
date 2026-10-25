// The level1-6 chicken walker; see ChickenWalker.h and docs/analysis/walker_port.md (checked against the assembly).
// x87 conventions as in CharacterUpdate.cpp: F() = FSTP to a float, intermediates are double. The original's matrices
// are column chunks (chunk k = column k of the port's NiMatrix3).
#include "game/object/ChickenWalker.h"

#include "core/CrtRand.h"
#include "core/CrtString.h"
#include "game/GameTime.h"
#include "game/defs/Defs.h"
#include "game/defs/CharacterDef.h"
#include "game/defs/EffectDefs.h"
#include "game/level/Camera.h"
#include "game/level/Hud.h"
#include "game/level/LevelData.h"
#include "game/level/Stage.h"
#include "game/object/Character.h"
#include "game/object/GenericNpc.h"
#include "game/object/NodeUpdate.h"
#include "game/object/Pickup.h"
#include "game/object/Laser.h"
#include "game/input/Pad.h"
#include "game/GameGlobals.h"
#include "game/sound/GameSound.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

f32 Bits(u32 b) {
    f32 f;
    std::memcpy(&f, &b, 4);
    return f;
}

const NiPoint3 kUnitX{1, 0, 0};                       // orig: 0x002D77E0
const NiPoint3 kUnitY{0, 1, 0};                       // orig: 0x002D77EC
const NiPoint3 kUnitZ{0, 0, 1};                       // orig: 0x002D77F8

const char* const kNets[3] = {"ChickenNet", "ChickenNet2", "ChickenNet3"};          // 0x00295AC4 / 0x00296F8C / 0x00296FA8
const char* const kStarts[3] = {"ChickenStart", "ChickenStart2", "ChickenStart3"};  // 0x00296F6C / 0x00296F7C / 0x00296F98
// The cockpit nodes the rider's environment-map effect lights (Mount / Dismount).
const char* const kAffected[6] = {"Chickenwalker_fry", "stick1", "stick2", "Joy_stickk", "Red_button", "Greenbutton"};

// The rotation argument of vfFC: column k at +0x10 * k of the 0x30-byte matrix.
ni::NiMatrix3 FromColumns(const NiPoint3& c0, const NiPoint3& c1, const NiPoint3& c2) {
    ni::NiMatrix3 m;
    const NiPoint3* c[3] = {&c0, &c1, &c2};
    for (int k = 0; k < 3; ++k) {
        m.m[0][k] = c[k]->x;
        m.m[1][k] = c[k]->y;
        m.m[2][k] = c[k]->z;
    }
    return m;
}

}  // namespace

// ---- 1.3 the constructor ------------------------------------------------------------------------------------------
ChickenWalker::ChickenWalker(LevelData& lvl, Player& rider) : level(lvl), player(rider) {   // orig: 0x00043D80
    // (the field defaults: the three sound lists with new(4) each, +0xE4 = 500, +0xCC = +0xD0 = -1, +0x4C = -1,
    //  +0x60 = 10.0, +0x64 = 0.02, +0xC4 = 5)
    if (lvl.defs) {
        // orig: new(0xA8) LaserDef::LaserDef("Laser") 0x00079BF0 (its spark emitter "Something" follows it in the list)
        laser = NewLaser(lvl, "Laser");
        laser->value68[0] = Bits(0x3DCCCCCD);         // +0x68 = 0.1
        laser->value68[1] = 0.25f;                    // +0x6C
        laser->value68[2] = 1.0f;                     // +0x70
        laser->value80 = 75.0f;                       // +0x80
        laser->value68[4] = 2.0f;                     // +0x78
        laser->value68[5] = 40.0f;                    // +0x7C
        laser->value1C = 400.0f;                      // +0x1C
        // orig: new(0x68); the model 0x000C2AF0("ChickenRocket:Animation (Translational)") tested, looked up again and
        // DynamicCast to NiNode (0x002F9230); then ProjectileDef("ChickRocket", model, 5.0, 1, 0, 5.0, 0, 30.0, 700.0,
        // 500.0, 300.0) 0x0007BCB0.
        ni::NiAVObject* found = lvl.FindEffectObject("ChickenRocket:Animation (Translational)");
        ni::NiNode* model = found ? dynamic_cast<ni::NiNode*>(lvl.FindEffectObject(
                                        "ChickenRocket:Animation (Translational)"))
                                  : nullptr;
        auto owned = std::make_unique<ProjectileDef>();
        rocketDef = owned.get();
        rocketDef->name = "ChickRocket";
        lvl.defs->Add(std::move(owned));              // (Def::Def 0x000962E0)
        ProjectileDef::Args a;
        a.value1C = 5.0f;
        a.flag2C = 1;
        a.value20 = 0.0f;
        a.value28 = 5.0f;
        a.value24 = 0.0f;
        a.value4C = 30.0f;
        a.value38 = 700.0f;
        a.value40 = 500.0f;
        a.value48 = 300.0f;
        rocketDef->Construct(model, a);
        // orig: new(0x114) SpriteEmitterDef("effect_ChickRocket") 0x0006E090 and its virtual setters in this order.
        auto em = std::make_unique<SpriteEmitterDef>();
        SpriteEmitterDef* e = em.get();
        e->name = "effect_ChickRocket";
        lvl.defs->Add(std::move(em));
        e->SetSpriteTexture("smoke.tga", lvl);        // vfA4
        e->SetColor(1.0f, 1.0f, Bits(0x3F333333));    // vfC4
        e->SetGravity(0.0f);                          // vf50
        e->SetSize(1.0f, 3.0f);                       // vfB0
        e->SetRangeB4(0.5f, 0.0f);                    // vfB8
        e->SetRange28(0.0f, 3.0f);                    // vf28
        e->SetRange38(0.0f, 1.0f);                    // vf3C (the max only)
        e->SetRange40(0.0f, 0.5f);                    // vf40
        e->SetRange30(Bits(0x3E4CCCCD), Bits(0x3F333333));   // vf30
        e->SetFlagA8(0);                              // vfAC
        e->SetColor(0.0f, 0.0f, 0.0f);                // vfC4 (the configured colour ends black)
        e->SetCount(20);                              // vf48
        e->SetFlag54(1);                              // vf54
        rocketDef->emitter34 = e;                     // +0x34
    }
    // (+0xB8 = 0, +0x68 = +0x6C = 0, +0xD4 = 0, +0x34 = +0x38 = 0, +0x10 = 0)
    // The eleven sound loads (Sound_Load 0x000C9860(path, 0)), appended to their lists.
    auto load = [&](const char* s) { return lvl.loadSound ? lvl.loadSound(s, 0) : 0u; };
    stepSounds.push_back(load("Sounds/Level1/Walker/Step.wav"));
    turnSounds.push_back(load("Sounds/Level1/Walker/Turn-1.wav"));
    turnSounds.push_back(load("Sounds/Level1/Walker/Turn-2.wav"));
    turnSounds.push_back(load("Sounds/Level1/Walker/Turn-3.wav"));
    turnSounds.push_back(load("Sounds/Level1/Walker/Turn-4.wav"));
    impactSounds.push_back(load("Sounds/Level1/Walker/impact-1.wav"));
    impactSounds.push_back(load("Sounds/Level1/Walker/impact-2.wav"));
    impactSounds.push_back(load("Sounds/Level1/Walker/impact-3.wav"));
    rocketSound = load("Sounds/Level1/Walker/rocket.wav");   // +0x134
    gunSound = load("sounds/level1/walker/gun.wav");          // +0x138
    pulseSound = load("sounds/level1/walker/pulse.wav");      // +0x13C
    // (+0x140 = +0x144 = 0, +0x178 = 0)
    // orig: 0x000431A0 (Player::Player right after the ctor): if !+4, +4 = new(0x28) Request 0x000D2160(the App's
    // manager DAT_002E7C24, this).
    if (stream::Manager* m = stream::Manager::Current()) request = m->Create(*this, "L16obj");
}

// ---- 1.4 OnStream ------------------------------------------------------------------------------------------------
bool ChickenWalker::OnStream(stream::Request& r) {    // orig: vf00 0x000458C0
    if (r.state == stream::State::New) {
        r.SetPath("Cockpit\\ChickenCockpit.nif");     // sprintf 0x002973CC; 0x000D1F30
        return true;
    }
    if (ni::NiStream* st = r.Stream()) {
        static const std::pair<const char*, s32 ChickenWalker::*> kNames[5] = {
            {"walker_startup_sequence", &ChickenWalker::iStartup}, {"big_aims_looping", &ChickenWalker::iBigAims},
            {"Greenline_looping", &ChickenWalker::iGreenline}, {"Small_aim_loop", &ChickenWalker::iSmallAim},
            {"Walker_walk", &ChickenWalker::iWalk}};
        static const char* const kJoy[9] = {
            "joystick_pointing_up_left",  "joystick_pointing_up",   "joystick_pointing_up_right",
            "joystick_pointing_left",     "joystick_centered",      "joystick_pointing_right",
            "joystick_pointing_down_left", "joystick_pointing_down", "joystick_pointing_down_right"};
        for (u32 link : st->TopLevelLinks()) {       // st+0x54 / +0x5C
            ni::NiObject* o = st->ResolveLink(link);
            if (!o) continue;
            if (auto* s = dynamic_cast<ni::NiSequence*>(o)) {   // RTTI 0x002FA124
                const s32 idx = static_cast<s32>(seqs.size());
                seqs.emplace_back(s);                 // SetAt(count, o) 0x0012B7F0 (grows through 0x00037E70)
                const char* n = s->name.c_str();      // the first match of the _stricmp chain
                bool done = false;
                for (const auto& [name, field] : kNames)
                    if (!done && crt::StrICmp(n, name) == 0) this->*field = idx, done = true;
                for (int k = 0; k < 9 && !done; ++k)  // ("joystick_pointing_down_right" is tested twice)
                    if (crt::StrICmp(n, kJoy[k]) == 0) iJoy[static_cast<size_t>(k)] = idx, done = true;
                if (!done && crt::StrICmp(n, "push_button") == 0) iPush = idx;
            } else if (auto* nd = dynamic_cast<ni::NiNode*>(o)) {   // RTTI 0x002F9230 (the last NiNode wins)
                cockpit = nd;
            }
        }
    }
    // orig: root vf54 (GetObjectByName, no reference; no null test of +0xC)
    camNull = cockpit ? GetObjectByName(cockpit.Get(), "Camera_null") : nullptr;
    walkNode = cockpit ? GetObjectByName(cockpit.Get(), "Walker:Animation (Translational)") : nullptr;
    if (walkNode) {
        walkKfm = new ni::NiKeyframeManager(walkNode);   // new(0xC0) 0x00130D20
        for (s32 i : {iStartup, iBigAims, iGreenline, iSmallAim, iWalk, iPush})
            walkKfm->AddSequence(Seq(i), true);       // 0x0012FFB0
    }
    if (ni::NiAVObject* j = cockpit ? GetObjectByName(cockpit.Get(), "Joy_stick_handle") : nullptr) {
        joyKfm = new ni::NiKeyframeManager(j);
        for (s32 i : iJoy) joyKfm->AddSequence(Seq(i), true);
    }
    request.reset();                                  // st->0x001084A0 (the stream's cleanup); release +4
    return false;
}

// ---- 3 Activate, Mount, Dismount, the net walk --------------------------------------------------------------------
void ChickenWalker::Activate(const char* name) {     // orig: 0x00044500 (thiscall, RET 4)
    if (active) return;
    if (!name) {
        Dismount();                                   // 0x00043470
        return;
    }
    active = 1;
    Player& p = player;                               // S+0x94
    u32 maxBits;
    std::memcpy(&maxBits, &p.maxHealth, 4);
    if (maxBits == 0x42C80000u) savedHealth = p.health;   // dword compare with 100.0f
    // S+0x5C->FindWaypoint("ChickenStart") 0x000CE450: no null test in the original
    const StageWaypoint* wp = level.currentStage ? level.currentStage->FindWaypoint("ChickenStart") : nullptr;
    if (wp) p.SetPosition(wp->position);              // vf1C (a copy of the waypoint's first three dwords)
    else NotPorted("ChickenWalker::Activate: no ChickenStart waypoint (a null dereference in the original)");
    Mount();                                          // 0x00043240
    if (p.body) p.body->b1A = 1;                      // body +0x1A
}

void ChickenWalker::Mount() {                         // orig: 0x00043240
    Player& p = player;                               // S+0x94
    stageIndex = 0;                                   // +0xD8
    const NiPoint3 pos = p.GetWorldPosition();        // vf34
    // S+0x20 NiRoomGroup::FindRoomContaining 0x0011F1D0 (a smart pointer, released at once)
    const ni::NiRoom* room = level.roomGroup ? level.roomGroup->FindRoomContaining(pos) : nullptr;
    for (const auto& st : level.stages) {             // Session::NextStage 0x000C26C0 walk
        if (st->ContainsRoom(room)) break;            // 0x000CE410
        ++stageIndex;
    }
    if (stageIndex >= 0 && stageIndex < 3) net = level.FindAINet(kNets[stageIndex]);   // S+0x114 0x000313E0
    if (!net) return;                                 // (other indices keep +0x44)
    prev = nullptr;                                   // +0x40
    node = net->Nearest(pos);                         // +0x3C = 0x00031D80
    prev = PrevOnPath(*net, node, StartNode());       // 0x00042950, 0x00042740(net, node, start)
    sleep = 0;                                        // +0xB8
    state = 0;                                        // +0x48
    pitch = yaw = startTime = 0.0f;                   // +0x50 / +0x54 / +0x58
    // (P->vf74(0): Fry's slot is 0x000AEDC0, a nop)
    ni::NiNode* pn = p.GetNode();                     // vf30
    pn->AttachChild(cockpit.Get(), true);             // vfA0(child, 1)
    if (p.model) p.model->flags |= ni::kNiAVHidden;   // P+0x564 +0xB0 |= 1 (no null test in the original)
    for (const char* n : kAffected) {                 // (P+0x5A4)->AttachAffectedNode(root vf54(n)) 0x001419B0
        auto* an = dynamic_cast<ni::NiNode*>(GetObjectByName(cockpit.Get(), n));
        if (p.envEffect && an) p.envEffect->AttachAffectedNode(an);   // (the original tests neither)
    }
    pn->UpdateProperties();                           // 0x0004342C (vf30: the rider's node)
    pn->UpdateEffects();
    UpdateNodeAt(pn, 0.0f);                           // NiAVObject::Update(0, 1) 0x001018D0
    p.health = p.maxHealth = 400.0f;                  // +0x4F4 = +0x4FC = 0x43C80000
}

void ChickenWalker::Dismount() {                      // orig: 0x00043470
    active = 0;                                       // +8
    Player& p = player;                               // S+0x94
    if (level.camera) level.camera->SetFollowTarget(&p);   // S+0x8C 0x00018FB0
    // (P+0x7B8)->SetRumble(0, 0, 0) 0x0009B740 (the pad is not null-checked in the original)
    if (p.pad) const_cast<input::Pad*>(p.pad)->SetRumble(0.0f, 0.0f, false);
    // (P->vf74(1): Fry's slot is a nop)
    if (p.model) p.model->flags &= ~ni::kNiAVHidden; // P+0x564 +0xB0 &= 0xFE
    for (const char* n : kAffected) {                 // (P+0x5A4)->DetachAffectedNode(root vf54(n)) 0x001419F0
        auto* an = cockpit ? dynamic_cast<ni::NiNode*>(GetObjectByName(cockpit.Get(), n)) : nullptr;
        if (p.envEffect && an) p.envEffect->DetachAffectedNode(an);
    }
    ni::NiNode* pn = p.GetNode();
    (void)pn->DetachChild(cockpit.Get());             // vfA4 (the returned pointer released)
    pn->UpdateProperties();                           // 0x00043589
    pn->UpdateEffects();
    UpdateNodeAt(pn, 0.0f);                           // Update(0, 1)
    p.maxHealth = 100.0f;                             // +0x4FC = 0x42C80000
    p.health = savedHealth;                           // +0x4F4 = +0xE0
    if (p.body) p.body->b1A = 0;                      // body +0x1A
}

AiNetNode* ChickenWalker::StartNode() const {         // orig: 0x00042950
    s32 i = 0;
    Stage* st = nullptr;
    for (const auto& s : level.stages) {              // Session::NextStage walk up to +0xD8 (null past the last)
        st = s.get();
        if (i == stageIndex) break;
        ++i;
        st = nullptr;
    }
    if (i < 0 || i > 2) return nullptr;
    AiNet* n = level.FindAINet(kNets[i]);             // 0x000313E0
    // orig: Stage::FindWaypoint 0x000CE450 without a test of the stage (a null `this` past the last stage)
    const StageWaypoint* wp = st ? st->FindWaypoint(kStarts[i]) : nullptr;
    if (!n || !wp) return nullptr;
    return n->Nearest(wp->position);                  // 0x00031D80
}

AiNetNode* PrevOnPath(AiNet& net, const AiNetNode* goal, AiNetNode* start) {   // orig: 0x00042740
    if (start == goal) return nullptr;
    AiNetNode* before = nullptr;                      // ebx
    AiNetNode* cur = start;                           // esi
    while (cur) {
        AiNetNode* nx = NodeAt(net, LinkNode(*cur, 0));   // 0x00031C00 / 0x00031C20
        if (cur->count > 1 && before == nx) nx = NodeAt(net, LinkNode(*cur, 1));   // (signed compare)
        before = cur;
        cur = nx;
        if (cur == goal) break;
    }
    return before;
}

AiNetNode* ChickenWalker::NextNode(AiNet& n, AiNetNode& nd) {   // orig: 0x000439A0 (thiscall, (net, node), RET 8)
    netEnd = 0;                                       // +0xE8
    if (nd.count == 1) {
        AiNetNode* n0 = NodeAt(n, LinkNode(nd, 0));
        if (n0 != prev) return n0;
        netEnd = 1;                                   // the end of the net: the next stage's start node
        s32 i = 0;
        Stage* st = nullptr;
        for (const auto& s : level.stages) {          // Session::NextStage walk up to +0xD8 + 1
            if (i == stageIndex + 1) {
                st = s.get();
                break;
            }
            ++i;
        }
        if (!st || i < 0 || i > 2) return nullptr;
        AiNet* n2 = level.FindAINet(kNets[i]);
        const StageWaypoint* wp = st->FindWaypoint(kStarts[i]);
        if (!n2 || !wp) return nullptr;
        return n2->Nearest(wp->position);
    }
    AiNetNode* p = PrevOnPath(n, &nd, StartNode());   // 0x00042950, 0x00042740
    if (p != prev) prev = p;
    AiNetNode* nx = NodeAt(n, LinkNode(nd, 0));
    if (nx == prev) nx = NodeAt(n, LinkNode(nd, 1));
    return nx;
}

// ---- 4 Control ----------------------------------------------------------------------------------------------------
void ChickenWalker::Control(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x000445A0 (thiscall, (P, vel, rot), RET 0xC)
    Player& p = player;                               // (the argument P)
    if (!active || !cockpit || !walkNode) return;     // +8, +0xC, +0x34 (and P != 0)
    if (node) {                                       // 0x000445DD: ride on the node, 15 units up
        NiPoint3 pos = p.GetWorldPosition();          // vf34
        pos.y = F(double(node->pos.y) + 15.0f);       // node +0x48; FADD [0x00297250]
        p.SetPosition(pos);                           // vf1C
        p.body->shape.ext.x = F(double(p.cdef.boundWidth) * 1.5f);   // shape +0x90 = D+0x90 * [0x00293BEC]
        p.body->shape.ext.y = 15.0f;                  // +0x94 = 0x41700000
        p.body->shape.ext.z = F(double(p.cdef.boundDepth) * 1.5f);   // +0x98 = D+0x98 * 1.5
    }
    if (!p.IsDead() && state == 4) Mount();           // vfA8; 0x00043240 (a respawned rider)
    bool moved = false;                               // esp+0x37
    const NiPoint3 pos = p.GetWorldPosition();        // esp+0xB0 (vf34)
    rot = p.rotation;                                 // vf38 (rep movsd, 12 dwords)
    const f32 fwdX = rot.m[0][0], fwdZ = rot.m[2][0]; // rot +0x00 / +0x08 (column 0)
    vel = {};                                         // ZERO 0x002F915C
    NiPoint3 d;                                       // esp+0x14
    f32 step = 0;                                     // esp+0x24 (written before every read)
    if (state == 0) {                                 // ---- the start-up (0x000446D7)
        // (the original passes +0x44 / +0x3C without a test: NextNode dereferences the node)
        if (AiNetNode* n = net && node ? NextNode(*net, *node) : nullptr) {
            d = {F(double(n->pos.x) - pos.x), 0.0f, F(double(n->pos.z) - pos.z)};   // (the y difference is stored
            (void)ni::Unitize(d);                                                     //  and replaced by 0)
        } else {
            d = kUnitX;                               // 0x002D77E0
        }
        const NiPoint3 c = ni::Cross(d, kUnitY);      // 0x00011700
        rot = FromColumns(d, kUnitY, c);              // rot +0x00 = d, +0x10 = UNIT_Y, +0x20 = c
        if (startTime == 0.0f)                        // TEST AH,0x44; JP: only on ==
            if (level.camera) level.camera->FadeTo(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.25f);   // 0x00013130
        const double t = double(g_frameStep) + startTime;   // FLD dt; FADD +0x58; FST +0x58
        startTime = F(t);
        if (t >= double(2.5f)) state = 1;             // FCOMP [0x00293B10]; TEST AH,1; JNE: < and NaN keep 0
    } else if (p.IsDead() || state == 4) {
        state = 4;                                    // 0x00044B4F
    } else if (state != 3) {                          // ---- walk / halt (0x00044881)
        AiNetNode* n = net && node ? NextNode(*net, *node) : nullptr;
        if (!n || sleep) {
            state = 2;                                // 0x00044B46
        } else {
            state = 1;
            d = {F(double(n->pos.x) - pos.x), 0.0f, F(double(n->pos.z) - pos.z)};
            const f32 dist = F(ni::Unitize(d));       // esp+0x20
            const NiPoint3 c = ni::Cross(d, kUnitY);  // esp+0x44
            const f32 spd = Bits(speed);              // +0x60
            step = dist;                              // esp+0x24 (0x0004494C)
            if (dist > spd) {                         // FCOMP +0x60; TEST AH,0x41; JNE: only on >
                f32 cosA = F(double(d.x) * fwdX + double(d.z) * fwdZ);   // FLD dx; FMUL fx; FLD dz; FMUL fz; FADDP
                const double side = double(c.z) * fwdZ + double(c.x) * fwdX;   // kept in ST0 (unrounded)
                if (cosA > 1.0f) cosA = 1.0f;         // TEST AH,0x41; JNE
                else if (cosA < -1.0f) cosA = -1.0f;  // TEST AH,5; JP
                const double a = side > 0.0 ? AcosClamped(cosA) : -AcosClamped(cosA);   // 0x00034100 (FCHS)
                const double a01 = a * double(Bits(0x3DCCCCCD));   // FMUL [0x00293AB0]
                f32 af = F(a01);                      // FST esp+0x20
                if (a01 > double(Bits(0x3D4CCCCD))) af = Bits(0x3D4CCCCD);   // FCOMP [0x00293BDC] on the unrounded
                else if (af < Bits(0xBD4CCCCD)) af = Bits(0xBD4CCCCD);       // FCOMP [0x0029724C] on the stored
                step = std::fabs(af) > Bits(0x3BA3D70A) ? 0.0f : spd;       // FABS; FCOMP [0x00293A9C]; TEST AH,0x41
                const ni::NiMatrix3 Y = ni::MakeYRotation(-af);              // 0x001030B0(F(-af))
                yaw = F(double(af) + yaw);            // +0x54
                rot = ni::Multiply(Y, rot);           // 0x00013B30 (ECX = YRot)
                moved = true;
            } else {                                  // arrived: take the next node (0x00044AA1)
                if (netEnd) ++stageIndex;             // +0xE8; +0xD8
                if (stageIndex >= 0 && stageIndex < 3) net = level.FindAINet(kNets[stageIndex]);
                prev = node;                          // +0x40 = +0x3C
                node = n;
            }
            vel = {F(double(d.x) * step), F(double(step) * d.y), F(double(d.z) * step)};   // 0x00044B09
        }
    }
    if (!p.IsDead()) {                                // ---- the stick: cockpit pitch / yaw (0x00044B69)
        const double inv = 1.0;                       // 0x0005E0B0 on a copy of the action map (DAT_002E79AA, off)
        // ST0 = inv; FMUL moveY; FLD moveX; FXCH; FMUL +0x64; FSUBR +0x50; FST t; FSTP +0x50; FMUL +0x64; FADD +0x54
        const f32 t = F(double(pitch) - (inv * p.moveY) * double(gain));
        pitch = t;
        const double ysum = double(p.moveX) * gain + double(yaw);   // FST +0x54 (ST0 keeps the unrounded sum)
        yaw = F(ysum);
        if (t > Bits(0x3F333333)) pitch = Bits(0x3F333333);          // [0x00295608]; TEST AH,0x41; JNE
        else if (t < Bits(0xBF333333)) pitch = Bits(0xBF333333);     // [0x00293B48]; TEST AH,5; JP
        if (ysum > double(Bits(0x3FC90FD8))) yaw = Bits(0x3FC90FD8);         // FCOM [0x00297248] on ST0
        else if (ysum < double(Bits(0xBFC90FD8))) yaw = Bits(0xBFC90FD8);    // FCOMP [0x00297244]
    }
    // ---- the cockpit transforms (0x00044C19)
    cockpit->local.rotate = ni::MakeYRotation(Bits(0xBFC90FD8));   // +0x30
    cockpit->local.translate = {0.0f, -15.0f, 0.0f};  // +0x60 (0xC1700000)
    {
        const ni::NiMatrix3 X = ni::MakeXRotation(pitch);   // 0x00103070
        const ni::NiMatrix3 Y = ni::MakeYRotation(yaw);     // 0x001030B0
        walkNode->local.rotate = ni::Multiply(Y, X);        // 0x00013B30 (ECX = YRot)
    }
    UpdateNodeAt(p.GetNode(), p.animTime);            // P->vf30()->Update(P+0x78, 1) 0x001018D0
    // esp+0x68 / esp+0xC0: the camera point / rotation the fire block and the turn sound read (set below; without
    // a Camera_null the original reads stale stack: the walk node's product at esp+0xC0, garbage at esp+0x68)
    NiPoint3 camT{};
    ni::NiMatrix3 camR = walkNode->local.rotate;
    if (camNull && level.camera) {                    // ---- the camera (0x00044D85; S+0x8C is not tested)
        const NiPoint3 T = camNull->world.translate;  // +0xA0
        const ni::NiMatrix3 Rw = camNull->world.rotate;   // +0x70
        camT = T;                                     // esp+0x68
        camR = Rw;                                    // esp+0xC0
        const NiPoint3 f = ni::MulVector(Rw, kUnitX); // 0x00011770
        // the x / y sums add the unrounded products, z the stored F(f.z * 0)
        const f32 fz0 = F(double(f.z) * 0.0f);
        const NiPoint3 cp{F(double(T.x) + double(f.x) * 0.0f), F(double(T.y) + double(f.y) * 0.0f),
                          F(double(T.z) + fz0)};
        Camera& cam = *level.camera;
        cam.position = cp;                            // 0x00011120 (by value)
        cam.dirtyTranslate = 1;
        cam.rotation = Rw;                            // 0x00011180
        cam.dirtyRotate = 1;
        cam.SetStatic(&cp, &Rw);                      // 0x00015040(&cp, &Rw, 0) (the third argument is not read)
        cam.SetFov(50.0f);                            // 0x000110E0(0x42480000)
        if (!frustumDone) {
            ComputeFrustumSides();                    // 0x00042DA0
            frustumDone = 1;
        }
    }
    p.body->SetForce({});                             // body vf10(ZERO)
    p.body->velocity.y = 0.0f;                        // body +0x2C
    if (!p.IsDead() && (state == 1 || state == 2)) {  // ---- fire (0x00044F0A)
        laserCool = F(double(laserCool) - g_frameStep);   // +0x5C
        rocketCool = F(double(rocketCool) - g_frameStep); // +0xC8
        if (laserCool < 0.0f) laserCool = 0.0f;       // TEST AH,5; JP
        if (rocketCool < 0.0f) rocketCool = 0.0f;
        if (p.aValue > Bits(0x3DCCCCCD) && laserCool == 0.0f && laser) {   // TEST AH,0x41 / TEST AH,0x44
            const double e = double(energy) - 25.0f;  // FSUB [0x00297240]; FCOM 0; TEST AH,1; JNE
            if (e >= 0.0) {
                energy = F(e);                        // 0x00044FAD
                FireLaser(camR, camT);                // 0x00044FB3..0x000453CF
            }
        }
        if (p.xValue > Bits(0x3DCCCCCD) && rocketCool <= 0.0f && rocketDef && rockets)   // 0x000453D3
            FireRocket(camR, camT);                   // 0x0004541D..0x000456DF
    }
    if (moved) {
        if (!turnVoice) {                             // 0x000456EE
            // the voice position: camT + M * (2.5, -2, 0) (0x00011770 on esp+0xC0, then three FADDs)
            const NiPoint3 o = ni::MulVector(camR, {2.5f, -2.0f, 0.0f});
            const NiPoint3 at{F(double(camT.x) + o.x), F(double(camT.y) + o.y), F(double(camT.z) + o.z)};
            const s32 i = crt::Rand() % static_cast<s32>(turnSounds.size());   // 0x00045750; cdq; idiv +0x110
            turnVoice = static_cast<u32>(level.Sound().Play3D(turnSounds[i], at, false));   // 0x00045769 -> +0x140
        }
    } else {
        turnVoice = 0;                                // 0x00045779
    }
    Animate();                                        // 0x00043AF0
    {                                                 // the laser energy (0x0004578A)
        const double e = double(g_frameStep) * 100.0f + energy;   // FLD dt; FMUL [0x00295610]; FADD +0xE4; FST
        energy = F(e);
        if (e > 500.0) energy = 500.0f;               // FCOM [0x00297220]; TEST AH,0x41; JNE (unrounded ST0)
        else if (e < 0.0) energy = 0.0f;              // FCOMP 0; TEST AH,5; JP
        // 0x000E5B90(S+0x118, "cw_hud", "ammo", F(energy [0x0029721C] 0.002))
        if (level.hud) level.hud->Command("cw_hud", "ammo", F(double(energy) * std::bit_cast<f32>(0x3B03126Fu)));
    }
    u32 hb;
    std::memcpy(&hb, &p.health, 4);
    if (hb != 0x47590300u) {                          // 55555.0: the invulnerable value (dword compare)
        if (p.health > 0.0f) {                        // TEST AH,0x41; JNE
            const double h = double(g_frameStep) * 4.5f + p.health;   // FMUL [0x002947A0]; FADD; FST
            p.health = F(h);
            if (h > 400.0) p.health = 400.0f;         // FCOMP [0x00297210] (the unrounded sum)
        }
        // 0x000E5B90(S+0x118, "cw_hud", "health", F(health [0x00293ABC] 0.0025))
        if (level.hud) level.hud->Command("cw_hud", "health", F(double(p.health) * std::bit_cast<f32>(0x3B23D70Au)));
    } else if (level.hud) {
        level.hud->Command("cw_hud", "health", 1.0f);   // (the invulnerable value)
    }
}

// orig: 0x00044FB3..0x000453CF: the laser shot (the energy is already paid). M / T = the camera's rotation / point
// (esp+0xC0 / esp+0x68).
void ChickenWalker::FireLaser(const ni::NiMatrix3& M, const NiPoint3& T) {
    const NiPoint3 f = ni::MulVector(M, kUnitX);      // 0x00011770 (on the copy at esp+0x80)
    const f32 fz50 = F(double(f.z) * 50.0f);          // [0x0029564C]; FSTP esp+0x40
    const NiPoint3 aim{F(double(T.x) + double(f.x) * 50.0f), F(double(T.y) + double(f.y) * 50.0f),
                       F(double(T.z) + fz50)};        // esp+0x5C
    const NiPoint3 o = ni::MulVector(M, {2.5f, -2.0f, 0.0f});   // 0x40200000 / 0xC0000000 / 0 at esp+0x50
    const NiPoint3 O{F(double(T.x) + o.x), F(double(T.y) + o.y), F(double(T.z) + o.z)};   // esp+0x14 (copy +0x50)
    NiPoint3 d{F(double(aim.x) - O.x), F(double(aim.y) - O.y), F(double(aim.z) - O.z)};   // esp+0x5C
    (void)ni::Unitize(d);                             // 0x000116A0
    const NiPoint3 s = ni::Cross(kUnitY, d);          // 0x00011700 (ECX = the UNIT_Y copy at esp+0x28) -> esp+0x14
    // (the first frame (d, Y, -s) is stored at esp+0x80 and overwritten below)
    // the spread: Pad::GetValue(binding 0x1F of S+0x94's action map, on a copy) of the active pad
    const input::InputSystem* in = level.input;
    const input::Pad* pad = in ? (in->activePad ? in->activePad : in->pads[0]) : nullptr;   // 0x0009B720
    const f32 v = pad && level.player ? pad->Value(level.player->actions.Binding(0x1F)) : 0.0f;   // 0x0009BA60; FSTP
    const s32 k1 = crt::Rand() % 80;                  // 0x000451B8; cdq; idiv 0x50
    const f32 a1 = F((double(Bits(0x42200000)) - k1) * double(Bits(0x3A5A740E)) * v);   // FILD; FSUBR 40; FMUL c; FMUL v
    const s32 k2 = crt::Rand() % 80;                  // 0x000451E1
    const double a2 = (double(Bits(0x42200000)) - k2) * double(Bits(0x3A5A740E)) * v;   // (kept in ST0)
    const double sx = double(s.x) * a2;               // (kept in ST0)
    const f32 sy = F(double(s.y) * a2), sz = F(double(s.z) * a2);   // esp+0x3C / esp+0x40
    const f32 t74 = F(double(kUnitY.x) * a1 + d.x);   // FLD Y.x; FMUL a1; ...; FXCH; FADD d.x
    const f32 t78 = F(double(kUnitY.y) * a1 + d.y);
    const f32 t7C = F(double(F(double(kUnitY.z) * a1)) + d.z);   // FSTP esp+0x4C; FLD; FADD d.z
    NiPoint3 d2{F(double(t74) + sx), F(double(t78) + sy), F(double(t7C) + sz)};   // esp+0x5C
    (void)ni::Unitize(d2);                            // 0x000452A2
    const NiPoint3 s2 = ni::Cross(kUnitY, d2);        // 0x000452D4
    const ni::NiMatrix3 frame = FromColumns(d2, kUnitY, {-s2.x, -s2.y, -s2.z});   // esp+0x80 (FCHS)
    GameObject* g = laser->Spawn(level, &level, O, frame, "Chicken-Laser");   // vf14(S, &O, &frame, 0x00297228)
    if (auto* pr = dynamic_cast<Projectile*>(g)) pr->shooter = &player;      // result +0x40 = P
    player.rumble.Add(Bits(0x3EB33333), Bits(0x3EB33333), Bits(0x3E19999A), 0);   // (P+0xA04) 0x0005F000
    level.Sound().SetVolume(level.Sound().Play3D(gunSound, O, false), 0.75f);   // +0x138; 0x000453B2 / 0x000453BD
    laserCool = Bits(0x3E19999A);                     // +0x5C = 0.15
}

// orig: 0x0004541D..0x000456DF: the rocket shot (g_rocketSide 0x002DCDB8 is a process global).
void ChickenWalker::FireRocket(const ni::NiMatrix3& M, const NiPoint3& T) {
    --rockets;                                        // +0xC4
    rocketCool = F(double(rocketCool) + Bits(0x3F0CCCCD));   // FADD [0x00297224] (0.55)
    if (++g_rocketSide > 3) g_rocketSide = 0;         // 0x002DCDB8 (JLE: signed)
    const NiPoint3 f = ni::MulVector(M, kUnitX);      // 0x00011770 (copy at esp+0x80)
    const f32 fz = F(double(f.z) * 500.0f);           // [0x00297220]; FSTP esp+0x40
    const NiPoint3 aim{F(double(T.x) + double(f.x) * 500.0f), F(double(T.y) + double(f.y) * 500.0f),
                       F(double(T.z) + fz)};          // esp+0x44
    static const f32 kSide[4][2] = {{-8.0f, 6.0f}, {4.0f, 6.0f}, {-8.0f, -6.0f}, {4.0f, -6.0f}};   // (y, z)
    const NiPoint3 off{2.5f, kSide[g_rocketSide][0], kSide[g_rocketSide][1]};   // esp+0x14
    const NiPoint3 o = ni::MulVector(M, off);
    const NiPoint3 O{F(double(T.x) + o.x), F(double(T.y) + o.y), F(double(T.z) + o.z)};   // esp+0x14 (copy +0x50)
    NiPoint3 d{F(double(aim.x) - O.x), F(double(aim.y) - O.y), F(double(aim.z) - O.z)};   // esp+0x14
    (void)ni::Unitize(d);
    const NiPoint3 s = ni::Cross(kUnitY, d);          // 0x00011700 (ECX = UNIT_Y at esp+0x28) -> esp+0x44
    const ni::NiMatrix3 frame = FromColumns(d, kUnitY, {-s.x, -s.y, -s.z});
    GameObject* g = rocketDef->Spawn20(level, &level, O, frame, 1, nullptr);   // vf20(S, &O, &frame, 1, 0)
    if (auto* pr = dynamic_cast<Projectile*>(g)) pr->shooter = &player;      // +0x40 = P
    if (g_rocketSide == 0 || g_rocketSide == 2) player.rumble.Add(1.0f, 0.5f, Bits(0x3ECCCCCD), 0);
    else player.rumble.Add(0.5f, 1.0f, Bits(0x3ECCCCCD), 0);
    level.Sound().SetVolume(level.Sound().Play3D(rocketSound, O, false), 1.0f);   // +0x134; 0x000456CC / 0x000456D7
}

// orig: 0x000436C0 (thiscall K, (amount, attacker), RET 8), from Player::Damage before Character::Damage.
void ChickenWalker::Impact(f32 amount, Character* attacker) {
    (void)amount;
    if (level.IsCharacter(attacker)) {                // 0x00033EE0
        const NiPoint3 at = attacker->GetWorldPosition();   // vf34
        DamageIndicator(at);                          // 0x00042F90(&at)
    }
    if (!impactSounds.empty()) {                      // +0x128 (count)
        const NiPoint3 pos = ImpactSoundPosition();   // 0x000435E0 (before the rand)
        const s32 i = crt::Rand() % static_cast<s32>(impactSounds.size());   // 0x00043702; cdq; idiv
        (void)level.Sound().Play3D(impactSounds[i], pos, false);   // Sound_Play3D 0x0004371B
    }
}

// orig: 0x00042F90 (thiscall K, (const NiPoint3* at), RET 4): the attacker's direction in the scene camera's frame
// (S+0x8C +0x30: d = at - its world translate, v = d in the camera's axes (Transpose 0x00103C60 of the rotation's
// copy, then its columns: the dot products with the world direction / up / right), unstored) against the frustum's
// outward side normals: in front (v . UNIT_X > 0) the cw_hud's "show_indicator" 2 (outside the top plane) or 3
// (bottom), then 0 (left) or 1 (right); behind, 4. (S+0x118 the HUD, 0x000E5B90.)
void ChickenWalker::DamageIndicator(const NiPoint3& at) {
    Camera* cam = level.camera.get();
    ni::NiCamera* nc = cam ? cam->sceneCamera.Get() : nullptr;
    if (!nc) return;                                  // (the original tests neither)
    const ni::NiMatrix3& R = nc->world.rotate;        // +0x70 (12 dwords copied)
    const NiPoint3& T = nc->world.translate;          // +0xA0
    const NiPoint3 d{F(double(at.x) - T.x), F(double(at.y) - T.y), F(double(at.z) - T.z)};   // FSUB; FSTP
    const auto& m = R.m;
    // (the console's row k of R, words 4k..4k+2, is the port's column k: m[0..2][k])
    const double vx = (double(d.z) * m[2][0] + double(d.y) * m[1][0]) + double(d.x) * m[0][0];
    const double vy = (double(d.z) * m[2][1] + double(d.y) * m[1][1]) + double(d.x) * m[0][1];
    const double vz = (double(d.z) * m[2][2] + double(d.y) * m[1][2]) + double(d.x) * m[0][2];
    auto dot = [&](const NiPoint3& n) { return (vz * n.z + vy * n.y) + vx * n.x; };
    const f32 bottom = F(dot(frustumSides[1]));       // +0x154 -> [esp+0x38]
    const f32 left = F(dot(frustumSides[2]));         // +0x160 -> [esp+0x3C]
    const f32 right = F(dot(frustumSides[3]));        // +0x16C -> [esp+0x28]
    Hud* hud = level.hud.get();                       // S+0x118 (no test in the original)
    auto show = [&](f32 v) { if (hud) hud->Command("cw_hud", "show_indicator", v); };   // 0x00296FB4 / 0x00296FBC
    const double front = (double(kUnitX.z) * vz + vy * double(kUnitX.y)) + vx * double(kUnitX.x);   // [0x002D77E0]
    if (!(front > 0.0)) {                             // FCOMP 0.0; TEST AH,0x41; JNE (NaN too): behind
        show(4.0f);
        return;
    }
    const double top = (vz * frustumSides[0].z + vy * frustumSides[0].y) + vx * frustumSides[0].x;   // +0x148
    if (top > 0.0) show(2.0f);                        // TEST AH,0x41; JNE
    else if (double(bottom) > 0.0) show(3.0f);
    if (double(left) > 0.0) show(0.0f);
    else if (double(right) > 0.0) show(1.0f);
}

// orig: 0x000435E0 (thiscall K, (out*), RET 4): camNull's world translate (+0xA0) plus an inlined product with the
// world rotation's 12 words from +0x70 read with a stride of 4 (component k: w[k] * 2.5 - 2 * w[k + 4] + 0 * w[k + 8],
// w[9] / w[10] = the words after the matrix, +0x94 / +0x98, times 0.0: zero for finite words), i.e. not the
// rotation * (2.5, -2, 0) of the laser's muzzle. x and y are summed unrounded, z is rounded (FSTP) before its add.
NiPoint3 ChickenWalker::ImpactSoundPosition() const {
    if (!camNull) return {};                          // (the original reads +0x38 without a test)
    const ni::NiMatrix3& m = camNull->world.rotate;   // the copy at esp+0x30 (rep movsd, 12 dwords)
    const NiPoint3& t = camNull->world.translate;
    const double x = (0.0 * double(m.m[2][2]) - double(m.m[1][1]) * 2.0) + double(m.m[0][0]) * 2.5;   // [0x00293B10]
    const double y = (0.0 - double(m.m[1][2]) * 2.0) + double(m.m[0][1]) * 2.5;   // (+0x94 * 0.0)
    const f32 z = F((0.0 - double(m.m[2][0]) * 2.0) + double(m.m[0][2]) * 2.5);  // (+0x98 * 0.0); FSTP [esp+0x24]
    return {F(x + t.x), F(double(t.y) + y), F(double(t.z) + z)};
}

// orig: 0x00042DA0 (thiscall K): four unit side normals of the camera frustum (NiCamera S+0x8C +0x30: l, r, t, b, n =
// +0x134..+0x144), for the damage indicator 0x00042F90. Each component a double difference of two products, stored.
void ChickenWalker::ComputeFrustumSides() {
    if (!level.camera || !level.camera->sceneCamera) return;   // (the original tests neither)
    const f32* fr = level.camera->sceneCamera->frustum;
    const double l = fr[0], r = fr[1], t = fr[2], b = fr[3], n = fr[4];
    const NiPoint3& Z = kUnitZ;                       // 0x002D77F8
    const NiPoint3& Y = kUnitY;                       // 0x002D77EC
    NiPoint3& top = frustumSides[0];                  // +0x148
    top = {F(double(Z.y) * 0.0f - t * Z.z), F(double(Z.z) * n - double(Z.x) * 0.0f), F(t * Z.x - double(Z.y) * n)};
    (void)ni::Unitize(top);
    NiPoint3& bottom = frustumSides[1];               // +0x154
    bottom = {F(double(Z.z) * b - double(Z.y) * 0.0f), F(double(Z.x) * 0.0f - double(Z.z) * n),
              F(double(Z.y) * n - b * Z.x)};
    (void)ni::Unitize(bottom);
    NiPoint3& left = frustumSides[2];                 // +0x160
    left = {F(double(Y.y) * l - double(Y.z) * 0.0f), F(n * Y.z - l * Y.x), F(double(Y.x) * 0.0f - n * Y.y)};
    (void)ni::Unitize(left);
    NiPoint3& right = frustumSides[3];                // +0x16C
    right = {F(double(Y.z) * 0.0f - r * Y.y), F(r * Y.x - n * Y.z), F(n * Y.y - double(Y.x) * 0.0f)};
    (void)ni::Unitize(right);
}

void ChickenWalker::RenderUpdate() {                  // orig: 0x00042D80 (thiscall K, (P), RET 4)
    if (active && cockpit) UpdateNodeAt(cockpit.Get(), player.animTime);   // NiAVObject::Update(P+0x78, 1)
}

// ---- 5 the cockpit animation --------------------------------------------------------------------------------------
void ChickenWalker::Animate() {                       // orig: 0x00043AF0
    const f32 t = player.animTime;                    // S+0x94 -> +0x78
    if (prevState != state) StopPrevious();           // 0x00042C90
    switch (state) {                                  // jump table 0x00043D60 (ja: states > 4)
    case 0:
        if (walkKfm && prevState != 0)
            if (ni::NiSequence* s = Seq(iStartup)) {
                SetCycleType(s, false);               // 0x000362D0(seq, 0): clamp
                s->offset = -t;                       // FLD t; FCHS; FSTP seq +0xA8
                walkKfm->ActivateSequence(s, 0);      // 0x0012F3A0
                footSeq = s;
            }
        Joystick(false);                              // 0x00042A90(0)
        Keys(t, footSeq);                             // 0x00043730
        break;
    case 1:
        if (walkKfm && prevState != 1) {
            walkKfm->ActivateSequence(Seq(iBigAims), 1);
            walkKfm->ActivateSequence(Seq(iGreenline), 1);
            walkKfm->ActivateSequence(Seq(iSmallAim), 2);
            if (ni::NiSequence* s = Seq(iWalk)) {
                SetCycleType(s, true);                // loop (no offset)
                walkKfm->ActivateSequence(s, 0);
                footSeq = s;
            }
        }
        Joystick(false);
        Keys(t, footSeq);
        Keys(t, Seq(iSmallAim));
        break;
    case 2:
        if (walkKfm && prevState != 2) {
            walkKfm->ActivateSequence(Seq(iBigAims), 1);
            walkKfm->ActivateSequence(Seq(iGreenline), 1);
            walkKfm->ActivateSequence(Seq(iSmallAim), 2);
        }
        Joystick(false);
        Keys(t, Seq(iSmallAim));
        break;
    case 3:
    case 4: Joystick(true); break;
    default: break;
    }
    if (joyKfm) {
        if (player.aValue > Bits(0x3DCCCCCD) && (state == 1 || state == 2)) {   // P+0xB30; TEST AH,0x41; JNE
            if (!pushActive)
                if (ni::NiSequence* s = Seq(iPush)) {
                    SetCycleType(s, true);            // 0x000362D0(seq, 1)
                    s->offset = -t;
                    joyKfm->ActivateSequence(s, 3);
                }
            pushActive = 1;
        } else if (ni::NiSequence* s = Seq(iPush)) {
            s->offset = -t;
        }
    }
    prevState = state;                                // +0x4C
}

void ChickenWalker::StopPrevious() {                  // orig: 0x00042C90
    footSeq = nullptr;                                // +0x10
    if (walkKfm) {
        switch (prevState) {
        case 0: walkKfm->DeactivateSequence(Seq(iStartup)); break;   // 0x0012FAB0
        case 1:
            for (s32 i : {iBigAims, iGreenline, iSmallAim, iWalk}) walkKfm->DeactivateSequence(Seq(i));
            break;
        case 2:
            for (s32 i : {iBigAims, iGreenline, iSmallAim}) walkKfm->DeactivateSequence(Seq(i));
            break;
        default: break;
        }
    }
    if (joyKfm && pushActive) joyKfm->DeactivateSequence(Seq(iPush));   // (+0xD4 is not cleared)
}

void ChickenWalker::Joystick(bool stop) {             // orig: 0x00042A90 (thiscall, (u8), RET 4)
    if (stop) {
        if (joyKfm && joyA >= 0) joyKfm->SumDeactivate(Seq(joyA), Seq(joyB));   // 0x0012FB40
        return;
    }
    const Player& p = player;                         // S+0x94
    f32 y = p.moveY, x = p.moveX;                     // +0x4DC / +0x4D8
    if (y > 1.0f) y = 1.0f;                           // TEST AH,0x41 (on the stored copy)
    else if (y < -1.0f) y = -1.0f;                    // TEST AH,5
    if (x > 1.0f) x = 1.0f;                           // (on ST0: the same float)
    else if (x < -1.0f) x = -1.0f;
    const f32 ys = F((double(y) + lastMoveY) * 0.5f); // FADD +0x68; FMUL [0x00293AA0]
    lastMoveY = y;
    const f32 xs = F((double(x) + lastMoveX) * 0.5f); // FADD +0x6C
    lastMoveX = x;
    auto w1 = [&] { return F(double(xs) + 1.0f); };   // FADD [0x00293A98] (the "<= 0" sides)
    if (ys > Bits(0x3DCCCCCD)) {                      // [0x00293AB0]; TEST AH,0x41; JNE
        if (xs > 0.0f) Blend(iJoy[1], iJoy[2], xs);   // up, up_right
        else Blend(iJoy[0], iJoy[1], w1());           // up_left, up
    } else if (ys < Bits(0xBDCCCCCD)) {               // [0x00293BF4]; TEST AH,5; JP
        if (xs > 0.0f) Blend(iJoy[7], iJoy[8], xs);   // down, down_right
        else Blend(iJoy[6], iJoy[7], w1());           // down_left, down
    } else {
        if (xs > 0.0f) Blend(iJoy[4], iJoy[5], xs);   // centered, right
        else Blend(iJoy[3], iJoy[4], w1());           // left, centered
    }
}

void ChickenWalker::Blend(s32 a, s32 b, f32 w) {      // orig: 0x00042A10 (thiscall, (a, b, w), RET 0xC)
    if (!joyKfm) return;
    if (a == joyA && b == joyB) {
        joyKfm->SetSumWeight(w);                      // 0x0012FD60
        return;
    }
    if (joyA >= 0) joyKfm->SumDeactivate(Seq(joyA), Seq(joyB));
    joyA = a;
    joyB = b;
    joyKfm->SumActivate(Seq(a), Seq(b), w);           // 0x0012F8F0
}

void ChickenWalker::Keys(f32 t, ni::NiSequence* s) {  // orig: 0x00043730 (thiscall, (t, seq), RET 8)
    if (!s) return;                                   // (the original dereferences the sequence without a test)
    ni::NiTimeController* rc = s->ReferenceController();   // seq+0x48[seq+0x90]
    if (!rc) return;                                  // (its frequency is read first, without a test)
    if (crt::StrICmp(s->name.c_str(), "Walker_walk") == 0) {
        for (s32 i = 0; i < 2; ++i) {                 // two steps per cycle, at 1.25 s and 2.5 s
            const f32 q = F(double(t) / SequenceDuration(s));   // FDIVR; FSTP
            const double L = SequenceDuration(s);
            const s32 n = static_cast<s32>(q);        // _ftol 0x00167110 (truncation)
            double key = L * n + (double(i) + 1.0) * 1.25f;   // FIMUL n; FILD i; FADD 1; FMUL [0x00297024]; FADDP
            const f32 keyF = F(key);                  // FST (ST0 keeps the unrounded key)
            if (double(t) > key) key = SequenceDuration(s) + keyF;   // FLD t; FCOMP ST1; TEST AH,0x41; FADD [stored]
            if (key < double(g_frameStep) + t && key >= double(t))   // TEST AH,5; JP / TEST AH,1; JNE
                OnKey(t, i == 0 ? "lfdown" : "rfdown");   // 0x00042800
        }
        return;
    }
    if (crt::StrICmp(s->name.c_str(), "Small_aim_loop") == 0) {
        const f32 q = F(double(t) / SequenceDuration(s));
        const double L = SequenceDuration(s);
        const s32 n = static_cast<s32>(q);
        double key = L * n;                           // FIMUL n; FST
        const f32 keyF = F(key);
        if (double(t) > key) key = SequenceDuration(s) + keyF;
        if (key < double(g_frameStep) + t && key >= double(t)) OnKey(t, "pulse");
        return;
    }
    // any other sequence: its text keys (O: the start-up sequence has none)
    auto* tk = dynamic_cast<ni::NiTextKeyExtraData*>(s->textKeysObj.Get());   // seq +0x8C
    if (!tk || tk->keys.empty()) return;
    const f32 inv = F(1.0 / double(rc->frequency));   // FLD 1.0; FDIV [rc+0x14]
    for (const auto& k : tk->keys) {
        const f32 ofs = -s->offset;                   // FLD +0xA8; FCHS; FSTP
        const f32 kt = k.time;                        // (stored, then multiplied)
        const bool loop = rc->CycleType() == 0;       // 0x0013EBC0
        if (KeyCrossed(F(double(inv) * kt), t, ofs, F(SequenceDuration(s)), loop))   // 0x00033E60
            OnKey(F(double(inv) * kt + ofs), k.text.c_str());
    }
}

void ChickenWalker::OnKey(f32 t, const char* name) { // orig: 0x00042800 (thiscall, (t, name), RET 8)
    (void)t;
    Player& p = player;                               // S+0x94
    if (crt::StrICmp(name, "lfdown") == 0 || crt::StrICmp(name, "rfdown") == 0) {
        const NiPoint3 pos = p.GetWorldPosition();    // vf34
        const s32 i = crt::Rand() % static_cast<s32>(stepSounds.size());   // rand() cdq idiv (count 1: draws anyway)
        (void)level.Sound().Play3D(stepSounds[i], pos, false);   // Sound_Play3D 0x0004285E / 0x000428CF
        p.rumble.Add(0.0f, 0.5f, Bits(0x3EB33333), 3);   // (P+0xA04)->0x0005F000(0, 0.5, 0.35, 3)
    } else if (crt::StrICmp(name, "pulse") == 0) {
        (void)p.GetWorldPosition();                   // (vf34; unused)
        level.Sound().SetVolume(level.Sound().PlaySfx(pulseSound, false), 0.1f);   // 0x00042929 / 0x00042934
    }
}

}  // namespace fut::game
