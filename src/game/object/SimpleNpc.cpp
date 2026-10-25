// SimpleNpc (the level5 wasp); see SimpleNpc.h and docs/analysis/simplenpc_moveable_port.md 2 (section numbers below).
// Every x87 sequence follows the disassembly: double intermediates stay on the stack, F() = an FSTP to float.
#include "game/object/SimpleNpc.h"

#include "core/CrtRand.h"
#include "core/CrtString.h"
#include "engine/ni/NiCollision.h"
#include "engine/ni/NiMath.h"
#include "game/GameRandom.h"
#include "game/GameTime.h"
#include "game/defs/Defs.h"
#include "game/defs/ExplosionDef.h"
#include "game/level/Camera.h"
#include "game/level/LevelData.h"
#include "game/level/TriggerManager.h"
#include "game/object/Destructable.h"
#include "game/physics/Physics.h"
#include "game/sound/GameSound.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

const char* g_waspBox = nullptr;                      // orig: DAT_002EAA44

namespace {

void AddVelocity(phys::CharBody& b, const NiPoint3& a) {   // orig: body vf50 0x000AF7D0
    NiPoint3& v = b.velocity;
    v = {F(double(v.x) + a.x), F(double(a.y) + v.y), F(double(a.z) + v.z)};
}

}  // namespace

GameObject* SimpleNPCDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                                const char* label) {  // orig: vf14 0x00060320
    RequestStream();                                  // vf04 0x0003FE00 (always true: the false branch returns 0)
    auto* o = new SimpleNpc(level, *this, pos, rot);  // new(0x800) 0x0005F860 (no null test of the result)
    o->SetOwner(owner);                               // vf10 0x000967E0
    if (model) o->Init();                             // D+0x20: vf04
    if (label) o->SetName(label);                     // 0x000968C0
    return o;
}

// ---- construction (2.3) -------------------------------------------------------------------------------------------
SimpleNpc::SimpleNpc(LevelData& lvl, SimpleNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : Character(lvl, d) {                             // orig: 0x0005F860
    lvl.wasps.push_back(this);                        // S+0xA0, tail append (+0x7F4 next, +0x7F8 prev)
    // new(0x48) CharBody(D+0x90, D+0x94, D+0x98, 10.0) 0x000AF0D0. (H) its +0x45 and its shape's +0x9C are never
    // written (heap garbage, KNOWN_ISSUES #41; the play oracle does not record the wasp's: 0 here).
    body = std::make_unique<phys::CharBody>(d.boundWidth, d.boundHeight, d.boundDepth,
                                            lvl.physic ? &lvl.physic->world : nullptr);
    body->shape.b1 = flag60;                          // shape+0xB1 = byte +0x60 (body +0x1A untouched)
    body->owner = this;                               // vf28
    Character::SetPosition(pos);                      // 0x000353B0
    Character::SetRotation(rot);                      // 0x00033D20
    if (d.type144 != 0) return;                       // (no shipped definition: the rest stays unset)
    body->SetForce({0.0f, 0.0f, 0.0f});               // vf10: no gravity
    t = 0;
    const double r0 = RandUnit() + 0.5f;              // rand #1: FADD [0x00293AA0]; FADD ST0,ST0; FSTP
    state = 0;
    shown = 0;
    speed = F(r0 + r0);
    TriggerRec* rec = level.triggers ? level.triggers->FindOrCreate(g_waspBox ? g_waspBox : "Kami_Bound")
                                     : nullptr;       // 0x000DB390 (0x0029B864)
    // orig: 0x0005FA20 `JMP $`: a record that is not a kind-4 box hangs the game (only level5-3 has Kami_Bound).
    if (!rec || rec->kind != 4) FatalError("SimpleNpc: the wasp box record is not a box (the original hangs)");
    const NiPoint3 A = level.triggers->Position(*rec);        // 0x000DA7E0 [esp+0x98]
    const ni::NiMatrix3 M = level.triggers->Rotation(*rec);   // 0x000DB090 [esp+0x50]
    NiPoint3 E = level.triggers->BoxSize(*rec);               // 0x000DA770 [esp+0x30]
    // ONE (0x002D7804) * 20.0 (0x00295AB4) per component: E - 20 (exact)
    const double k0 = double(1.0f) * 20.0f, k1 = double(1.0f) * 20.0f;
    const f32 k2 = F(double(1.0f) * 20.0f);
    E = {F(double(E.x) - k0), F(double(E.y) - k1), F(double(E.z) - k2)};
    // the box axes M * UNIT_X / UNIT_Y / UNIT_Z (0x002D77E0 / 0x002D77EC / 0x002D77F8), each component
    // (m[r][2]*u.z + m[r][1]*u.y) + m[r][0]*u.x; X.x and X.y stay on the stack, everything else is stored
    const auto& m = M.m;
    const auto axis = [&](int r, const NiPoint3& u) {
        return (double(m[r][2]) * u.z + double(m[r][1]) * u.y) + double(m[r][0]) * u.x;
    };
    const NiPoint3 ux{1, 0, 0}, uy{0, 1, 0}, uz{0, 0, 1};
    const double xx = axis(0, ux), xy = axis(1, ux);
    const f32 xz = F(axis(2, ux));
    const NiPoint3 X{F(xx * E.x), F(xy * E.x), F(double(xz) * E.x)};   // [esp+0x24..0x2C]
    const NiPoint3 Y{F(double(F(axis(0, uy))) * E.y), F(double(F(axis(1, uy))) * E.y),
                     F(double(F(axis(2, uy))) * E.y)};   // [esp+0x18..0x20]
    const NiPoint3 Z{F(double(F(axis(0, uz))) * E.z), F(double(F(axis(1, uz))) * E.z),
                     F(double(F(axis(2, uz))) * E.z)};   // [esp+0x40..0x48]
    const f32 r1 = F(RandUnit() - 0.5f);              // rand #2 [esp+0x3C] (X)
    const f32 r2 = F(RandUnit() - 0.5f);              // rand #3 [esp+0x4C] (Z)
    const double r3 = RandUnit() - 0.5f;              // rand #4 (Y; kept on the stack)
    const double zx = double(Z.x) * r2;               // (on the stack)
    const f32 zy = F(double(Z.y) * r2), zz = F(double(Z.z) * r2);   // [esp+0x84] / [esp+0x88]
    const double yx = double(Y.x) * r3, yy = double(Y.y) * r3;   // (on the stack)
    const f32 yz = F(double(Y.z) * r3);               // [esp+0x48]
    const double wx = double(X.x) * r1, wy = double(X.y) * r1;   // (on the stack)
    const f32 wz = F(double(X.z) * r1);               // [esp+0x2C]
    const f32 ax = F(wx + A.x);                       // [esp+0x18]
    const double ay = double(A.y) + wy;               // (never stored)
    const f32 az = F(double(A.z) + wz);               // [esp+0x20]
    const f32 bx = F(double(ax) + yx);                // [esp+0x24]
    const f32 by = F(ay + yy);                        // [esp+0x28]
    const f32 bz = F(double(az) + yz);                // [esp+0x2C]
    home = {F(double(bx) + zx), F(double(by) + zy), F(double(bz) + zz)};   // +0x7B0
    p0 = GetWorldPosition();                          // +0x7BC (Character 0x00035440)
    p1 = RandomPoint();                               // +0x7C8: rand #5-#7
    p2 = RandomPoint();                               // +0x7D4: rand #8-#10
    segmentsLeft = crt::Rand() % 4 + 3;               // rand #11 (AND 0x80000003 ...: the C remainder)
    voiceHover = 0;
    voiceDive = 0;
    voiceWarning = 0;
}

SimpleNpc::~SimpleNpc() {                             // orig: 0x000603E0
    SimpleNpc::UnloadModel();                         // 0x0005FE40 (directly)
    std::erase(level.wasps, this);                    // the S+0xA0 unlink
}

s32 SimpleNpc::CompareLeftFootKey(const char* key) const {   // orig: vfF0 0x0005F750
    return crt::StrICmp(key, "morph:lfdown");         // 0x002974DC
}

NiPoint3 SimpleNpc::RandomPoint() const {             // orig: 0x0005F7A0
    const f32 a = F(RandUnit());                      // [esp+8]  -> z
    const f32 b = F(RandUnit());                      // [esp+4]  -> y
    const double c = RandUnit();                      // (stack)  -> x
    const f32 cx = F((c + c) - 1.0f);                 // FSUB ONE.x [0x002D7804]
    const f32 cy = F((double(b) + b) - 1.0f);         // ONE.y
    const f32 cz = F(double(F(double(a) + a)) - 1.0f);   // 2a stored first; ONE.z
    const f32 vx = F(double(cx) * 10.0f);             // [0x00293C2C]
    const f32 vy = F(double(cy) * 10.0f);
    return {F(double(vx) + home.x), F(double(vy) + home.y), F(double(cz) * 10.0f + home.z)};   // (z: unrounded product)
}

bool SimpleNpc::Init() {                              // orig: 0x0005FEE0
    Character::Init();                                // 0x00038070
    updateDistance = simpleDef().value160;            // +0x64 = D+0x160
    return true;
}

void SimpleNpc::Think(f32 dt) {                       // orig: vf54 0x00060200
    if (simpleDef().type144 == 0) Fly(dt);            // jmp vfEC
}

// ---- the per-step update (2.7) -------------------------------------------------------------------------------------
bool SimpleNpc::Update(f32 dt, int pass) {            // orig: vf58 0x00060480
    if (pass == 0) {
        animTime = F(double(dt) + animTime);          // FLD dt; FADD [+0x78] (no time scale)
        // S+0x8C camera +0x48 (no null test in the original: a UB guard)
        const NiPoint3 cam = level.camera ? level.camera->position : NiPoint3{};
        const NiPoint3 p = GetWorldPosition();
        NiPoint3 d{F(double(cam.x) - p.x), F(double(cam.y) - p.y), F(double(cam.z) - p.z)};
        cameraDistance = F(ni::Unitize(d));           // 0x000116A0 (the length in ST0)
        lodStep = F(double(lodStep) + 1.0f);          // [0x00293A98]
        if (!IsDead() && !(cameraDistance > updateDistance) && !flag28) {   // vfA8; TEST AH,0x41; JE
            deathFade = 0;
            if (body) {                               // (+0x5C and its shape +0x40)
                body->SetEnabled(true);               // vf34(1) 0x000AED40
                body->shape.ext.x = F(double(node->local.scale) * cdef.boundWidth);    // node +0x6C * D+0x90
                body->shape.ext.y = F(double(node->local.scale) * cdef.boundHeight);   // D+0x94
                body->shape.ext.z = F(double(node->local.scale) * cdef.boundDepth);    // D+0x98
                body->SetStepHeight(cdef.stepHeight); // vf38(D+0xAC)
                if (type == 7) body->shape.pinned = 1;
            }
        } else if (body) {                            // (no null test in the original: a UB guard)
            body->SetEnabled(false);                  // vf34(0)
        }
        Think(dt);                                    // vf54
        return true;
    }
    if (pass != 1) return true;
    const NiPoint3 p = GetWorldPosition();
    const NiPoint3& n = node->local.translate;        // 0x0002DE70 (per component, NaN = different)
    if (!(p.x == n.x) || !(p.y == n.y) || !(p.z == n.z)) UpdateRoom();   // 0x00096F20
    node->local.translate = GetWorldPosition();       // (a second vf34 call)
    node->local.rotate = rotation;                    // +0x570 (12 dwords)
    UpdateAnimation(dt);                              // vf5C (AnimTick 0x0003B5E0)
    for (GameObject* o = inventory.NextObject(nullptr); o;) {   // (the next one fetched first)
        GameObject* next = inventory.NextObject(o);
        if (!o->Tick()) delete o;                     // vf08; vf00(1)
        o = next;
    }
    if (node->framesNotDrawn == 0) UpdateAmbientLight();   // +0xB2 == 0: vf48 (alive or not)
    if (IsDyingAnim()) deleteMe = 1;                  // vfAC
    return !deleteMe;
}

bool SimpleNpc::PostUpdate() {                        // orig: vf0C 0x00060220
    if (deleteMe) return false;
    if (!level.camera) {                              // S+0x8C
        body->SetEnabled(true);                       // vf34(1) (no null test of the body)
        PrepareRender();                              // tail jump 0x00039890 (returns 1)
        return true;
    }
    if (cameraDistance < updateDistance) {            // FCOMP [+0x64]; TEST AH,5; JP
        if (!IsDead()) {
            body->SetEnabled(true);
            PrepareRender();
            return true;
        }
        if (finished && deathFade > 20.0f && type == 0) return false;   // [0x00295AB4]; TEST AH,0x41; JNE
        deathFade = F((double(g_frameStep) + g_frameStep) + deathFade);   // FLD step; FADD ST0,ST0; FADD [+0x660]
        PrepareRender();
        return true;
    }
    if (deathFade > 20.0f && IsDead() && (type == 0 || type == 0xC)) return false;
    body->SetEnabled(false);                          // vf34(0)
    PrepareRender();
    return true;
}

// ---- damage (2.8, 2.9) --------------------------------------------------------------------------------------------
void SimpleNpc::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x00060110
    // FLD amount; FCOMP [+0x4F4]; TEST AH,1; JNE (< and NaN skip), then FLD [+0x4FC]; FMUL 0.5; FCOMP amount;
    // TEST AH,5; JP (only <)
    if (!(amount < health) && !std::isnan(amount) && !std::isnan(health) && double(maxHealth) * 0.5f < amount &&
        simpleDef().type144 == 0)
        ExplodeAtCharacterRecord(level, label ? label->c_str() : nullptr);   // [0x002DAC64]->0x000C2F70(+0x34)
    Character::Damage(amount, attacker);              // 0x000354B0
    if (!(amount > 0.0f)) return;                     // [0x00293AB8]; TEST AH,0x41; JNE
    if (health < 0.0f || health == 0.0f) {           // TEST AH,0x41; JP (> and NaN skip)
        Die();                                        // 0x0005FF00
        return;
    }
    // D+0x150: never written by the definition's constructor (heap garbage in the original; 0 here)
    if (!level.Sound().HasVoices(simpleDef().sample150)) {   // 0x000C98C0
        const NiPoint3 p = GetWorldPosition();        // vf34
        VoiceMaxDistance(SoundAt(2, &p, 3, 0, nullptr), simpleDef().value160);   // 0x0003FEA0; 0x000C9CA0
    }
}

void SimpleNpc::ApplyHit(GameObject* tgt, NiPoint3& dir, u32 n) {   // orig: vfD4 0x00060030
    if (!curSeq) return;                              // +0xA4
    const AnimEntry* e = EntryOf(curSeq);             // 0x0003FD20
    const f32 dmg = F(e && e->hasDamage ? e->damage : 0.0f);   // 0x0003EE90 (no null test of e: a UB guard)
    if (!tgt) return;                                 // (no null test of t in the original: a UB guard)
    if (tgt->def->IsType(1)) {
        auto* c = static_cast<Character*>(tgt);
        const f32 k = (n & 2) ? 20.0f : 5.0f;         // [0x00295AB4] / [0x00293C24]
        const NiPoint3 imp{F(double(dir.x) * k), F(double(dir.y) * k), F(double(dir.z) * k)};
        if (c->body) AddVelocity(*c->body, imp);      // body vf50 0x000AF7D0 (no null test in the original)
        c->Damage(dmg, this);                         // vf7C
    } else if (tgt->def->IsType(0x40)) {
        (void)static_cast<Destructable*>(tgt)->Damage(dmg);   // vf64
    }
}

// ---- the flight (2.10) --------------------------------------------------------------------------------------------
void SimpleNpc::Fly(f32 dt) {                         // orig: vfEC 0x00060740
    if (IsDead()) return;                             // vfA8
    sound::GameSound& snd = level.Sound();
    const SimpleNPCDef& dd = simpleDef();
    if (!shown && health > 0.0f) {                    // [0x00293AB8]; TEST AH,0x41; JNE
        if (ni::NiAVObject* k = GetObjectByName(GetNode(), "kamikaze")) {   // vf30 ->vf54 (0x00297D08)
            k->flags &= ~1u;                          // AND byte [+0xB0], 0xFE
            if (ni::NiAVObject* q = GetObjectByName(GetNode(), "kamikaze_parts")) {   // 0x0029B97C
                q->flags |= 1u;                       // OR word [+0xB0], 1
                shown = 1;
            }
        }
    }
    if (state == 0) {
        const s32 v = snd.PlayAttached(dd.sounds148[3], GetNode(), false);   // D+0x15C launch (the id is dropped)
        snd.SetMaxDistance(v, dd.value160);           // 0x000C9CA0
        state = 1;
        voiceHover = static_cast<u32>(snd.PlayAttached(dd.sounds148[0], GetNode(), true));   // D+0x148 hover
        snd.SetMaxDistance(static_cast<s32>(voiceHover), dd.value160);
    }
    t = F(double(dt) * speed + t);                    // FLD dt; FMUL [+0x7E4]; FADD [+0x7E0]
    if (state == 2 && body && !body->shape.partnerIsBox && body->shape.partner) {   // shape +0xB2 / +0xB4
        for (Character* c : level.characters) {       // 0x000C26E0 (S+0x90)
            const phys::CharShape& sh = body->shape;  // (the partner re-read each iteration)
            const void* partner = sh.partnerIsBox ? nullptr : sh.partner;
            if (c->body && &c->body->shape == partner) {   // c->body->shape (+0x5C +0x40; no null test of the body)
                c->Damage(20.0f, this);               // vf7C(20.0, this)
                break;
            }
        }
        t = std::bit_cast<f32>(0x3F8020C5u);          // 1.001 (whether or not one matched)
    }
    if (snd.VoiceExists(static_cast<s32>(voiceDive))) {   // 0x000C9900 (no null test)
        const double vol = snd.GetVolume(static_cast<s32>(voiceDive));   // 0x000C9C30 (ST0)
        if (vol < 1.0f) snd.SetVolume(static_cast<s32>(voiceDive), F(double(g_frameStep) + vol));   // TEST AH,5; JP
    }
    if (t > 1.0f) {                                   // FCOMP [0x00293A98]; TEST AH,0x41; JNE place
        if (state == 2) {                             // the dive's end (or the touch): explode
            Die();
            return;
        }
        if (!IsAnimPlaying(0x69) && !IsAnimPlaying(0x2F)) PlayAnimById(0x69, false, true, false, 0.0f);   // hover
        t = F(double(t) - 1.0f);
        p0 = GetWorldPosition();                      // vf34 -> +0x7BC
        // the control point extrapolated past the old end: p1 + (p2 - p1) * 1.4 (only x's difference is rounded
        // first; [0x0029B9B4])
        const double dz = double(p2.z) - p1.z, dy = double(p2.y) - p1.y;
        const f32 dx = F(double(p2.x) - p1.x);
        const f32 sz = F(dz * 1.4f), sy = F(dy * 1.4f), sx = F(double(dx) * 1.4f);
        p1 = {F(double(sx) + p1.x), F(double(sy) + p1.y), F(double(sz) + p1.z)};
        p2 = RandomPoint();                           // 3 rand
        const double r = RandUnit() + 0.5f;           // 1 rand: FADD [0x00293AA0]; FADD ST0,ST0
        speed = F(r + r);
        --segmentsLeft;
        if (state == 1 && segmentsLeft == 1) {        // the warning, the dive loop fading in
            voiceWarning = static_cast<u32>(snd.PlayAttached(dd.sounds148[2], GetNode(), false));   // D+0x158
            snd.SetMaxDistance(static_cast<s32>(voiceWarning), dd.value160);
            if (VoiceLive(voiceDive)) snd.StopVoice(static_cast<s32>(voiceDive));   // (not cleared)
            voiceDive = static_cast<u32>(snd.PlayAttached(dd.sounds148[1], GetNode(), true));   // D+0x14C
            snd.SetMaxDistance(static_cast<s32>(voiceDive), dd.value160);
            snd.SetVolume(static_cast<s32>(voiceDive), 0.0f);
        }
        if (state == 1 && segmentsLeft == 0) {        // the dive
            segmentsLeft = crt::Rand() % 4 + 3;       // 1 rand
            Player* target = nullptr;
            for (Player* pl : level.players)          // 0x000C2730 (S+0x98): the first Fry with a pad
                if (pl->type == 1 && pl->pad) {
                    target = pl;
                    break;
                }
            if (target) {
                NiPoint3 q = target->GetWorldPosition();   // vf34
                q.y = F(double(q.y) + 2.0f);          // [0x00293B20]
                const NiPoint3 me = GetWorldPosition();
                NiPoint3 dir{F(double(q.x) - me.x), F(double(q.y) - me.y), F(double(q.z) - me.z)};
                (void)ni::Unitize(dir);               // 0x000116A0 (the length dropped)
                f32 len = std::bit_cast<f32>(0x453B8000u);   // 3000.0
                (void)ni::RayCast(me, dir, len, false, 3);   // 0x000FB780 (only the shortened length is used)
                const NiPoint3 hit{F(double(len) * dir.x), F(double(len) * dir.y), F(double(len) * dir.z)};   // 0x000115C0
                p2 = {F(double(me.x) + hit.x), F(double(me.y) + hit.y), F(double(me.z) + hit.z)};
                t = 0;
                speed = 1.0f;
                state = 2;
                PlayAnimById(0x2F, false, true, true, 0.0f);   // attack1
            }
        }
    }
    // place: the quadratic Bezier through p0, p1, p2 at u = t (de Casteljau; 0x00060CDB..0x00060E48)
    const double u = t, w = double(1.0f) - u;
    const double l0x = double(p0.x) * w + double(p1.x) * u;   // (on the stack)
    const double l0y = double(F(double(p1.y) * u)) + F(double(p0.y) * w);
    const f32 l0z = F(double(F(double(p1.z) * u)) + F(double(p0.z) * w));
    const f32 fw = F(w);
    const f32 ax = F(l0x * fw), ay = F(l0y * fw), az = F(double(l0z) * fw);
    const double l1x = double(p2.x) * u + double(p1.x) * w;
    const double l1y = double(F(double(p2.y) * u)) + F(double(p1.y) * w);
    const f32 l1z = F(double(F(double(p2.z) * u)) + F(double(p1.z) * w));
    const NiPoint3 P{F(double(F(l1x * u)) + ax), F(l1y * u + ay), F(double(F(double(l1z) * u)) + az)};
    // (three debug draws 0x0002C9F0 of p0, p1, p2: no-ops)
    const NiPoint3 me = GetWorldPosition();           // vf34
    NiPoint3 d{F(double(P.x) - me.x), 0.0f, F(double(P.z) - me.z)};
    SetPosition(P);                                   // vf1C (Character::SetPosition: the body)
    if (ni::Unitize(d) > double(1e-6f)) {             // FCOMP [0x00293ACC]; TEST AH,0x41; JNE
        const f32 yaw = F(double(std::bit_cast<f32>(0x40490E56u)) - std::atan2(double(d.x), double(d.z)));   // FPATAN
        SetRotation(ni::MakeYRotation(yaw));          // 0x001030B0; vf24
    }
}

void SimpleNpc::Die() {                               // orig: 0x0005FF00
    if (simpleDef().type144 != 0) return;
    health = 0;
    sound::GameSound& snd = level.Sound();
    if (VoiceLive(voiceHover)) snd.StopVoice(static_cast<s32>(voiceHover));
    voiceHover = 0;
    if (VoiceLive(voiceDive)) snd.StopVoice(static_cast<s32>(voiceDive));
    voiceDive = 0;
    if (VoiceLive(voiceWarning)) snd.StopVoice(static_cast<s32>(voiceWarning));
    voiceWarning = 0;
    PlayAnimById(0x4D, true, true, true, 0.0f);       // dying1 0x0003CE70
    health = -1.0f;                                   // 0xBF800000
    const NiPoint3 p = GetWorldPosition();            // vf34
    if (level.defs && level.defs->explosion)          // S+0x44 (no null test in the original: a UB guard)
        (void)level.defs->explosion->Spawn(level, &level, p, 1, "WaspExplosion", true);   // vf20(S, &p, 1, .., 1)
    if (ni::NiAVObject* k = GetObjectByName(GetNode(), "kamikaze")) k->flags |= 1u;          // hide the wasp
    if (ni::NiAVObject* q = GetObjectByName(GetNode(), "kamikaze_parts")) q->flags &= ~1u;   // show the parts
}

void SimpleNpc::UnloadModel() {                       // orig: vf68 0x0005FE40
    Character::UnloadModel();                         // 0x0003B7D0
    StopVoiceAndClear(voiceHover);                    // (each: v && Voice_Exists(v): Voice_Stop(v); v = 0)
    StopVoiceAndClear(voiceDive);
    StopVoiceAndClear(voiceWarning);
}

// ---- natives (1.2) ------------------------------------------------------------------------------------------------
void SpawnWasp(LevelData& level) {                    // orig: 0x000C4810
    Character* c = nullptr;
    for (Character* x : level.characters)             // 0x000C26E0 walk (S+0x90, next +0x76C)
        if (crt::StrICmp(x->def->name.c_str(), "Destructor") == 0) {   // _stricmp(D+0x10, 0x002A355C)
            c = x;
            break;
        }
    if (!c || !c->model) return;                      // +0x564
    ni::NiAVObject* n = GetObjectByName(c->model.Get(), "booom");   // vf54 (0x002A3554)
    if (!n) return;
    const NiPoint3 pos = n->world.translate;          // +0xA0 (raw copy)
    const ni::NiMatrix3 rot{};                        // 0x00102FF0: identity (pads zeroed)
    if (Def* d = level.defs ? level.defs->Find("kamikaze") : nullptr)   // Session::FindDef 0x000C3F10
        (void)d->Spawn(level, c->owner, pos, rot, nullptr);   // vf14(owner, &pos, &rot, 0) (result unused)
}

void DeleteWasps(LevelData& level) {                  // orig: 0x000C3030
    for (size_t i = 0; i < level.wasps.size();) {     // (the next one taken first; the dtor unlinks)
        SimpleNpc* w = level.wasps[i];
        if (w->simpleDef().type144 == 0) delete w;    // vf00(1)
        else ++i;
    }
}

void ExplodeAtCharacterRecord(LevelData& level, const char* name) {   // orig: 0x000C2F70
    TriggerRec* rec = level.triggers ? level.triggers->FindOrCreate(name) : nullptr;   // S+0xC0, 0x000DB390
    if (!level.defs || !level.defs->explosion || !rec) return;   // S+0x44
    const NiPoint3 p = rec->kind == 5 ? static_cast<Character*>(rec->target)->Character::CharacterPoint()   // 0x0003D7C0
                                      : level.triggers->Position(*rec);   // 0x000DA7E0
    (void)level.defs->explosion->Spawn(level, &level, p, 1, nullptr, true);   // vf20(S, &p, 1, 0, 1)
}

}  // namespace fut::game
