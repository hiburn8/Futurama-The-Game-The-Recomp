// Character per-step update and animation control; see Character.h and docs/analysis/character_update.md (section
// numbers below). Every x87 sequence follows the disassembly: D(..) intermediates are doubles, F() = an FSTP.
#include "game/object/Character.h"

#include "core/CrtRand.h"
#include "core/CrtString.h"
#include "engine/ni/NiKeyframeManager.h"
#include "engine/ni/NiMath.h"
#include "game/GameGlobals.h"
#include "game/GameTime.h"
#include "engine/ni/NiCollision.h"
#include "game/defs/CharacterDef.h"
#include "game/defs/Defs.h"
#include "game/defs/EffectDefs.h"
#include "game/GameRandom.h"
#include "game/object/Impact.h"
#include "game/level/Camera.h"
#include "game/level/LevelData.h"
#include "game/level/ScriptTriggers.h"
#include "game/level/Stage.h"
#include "game/object/ChickenWalker.h"
#include "game/object/Destructable.h"
#include "game/object/NodeUpdate.h"
#include "game/object/Pickup.h"
#include "game/object/RatRide.h"
#include "game/sound/GameSound.h"
#include "game/sound/SoundEvents.h"

#include <bit>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

const NiPoint3 kUnitX{1, 0, 0};                       // orig: 0x002D77E0
const NiPoint3 kUnitY{0, 1, 0};                       // orig: 0x002D77EC

// A path the port does not cover yet was reached: reported once per site (stderr), the step continues.
void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

}  // namespace

// orig: 0x000C0410 (cdecl(NiPoint3* out, NiPoint3 c, f32 step)): each component moves towards the global ambient `g`
// by F(step + step) without overshooting (npc_combat2_port.md 7; render state only; also the cannon's step, Cannon.cpp).
ni::NiColor AmbientStep(const ni::NiColor& c, f32 step, const NiPoint3& g) {
    const f32 s2 = F(double(step) + step);            // FADD ST0,ST0; FSTP
    auto one = [&](f32 v, f32 t) -> f32 {
        if (v < t) {                                  // TEST AH,5; JP
            const double w = double(v) + s2;          // FST (compared unrounded)
            return w > t ? t : F(w);                  // TEST AH,0x41; JNE
        }
        if (v > t) {                                  // TEST AH,0x41; JNE
            const double w = double(v) - s2;
            return w < t ? t : F(w);                  // TEST AH,5; JP
        }
        return v;                                     // (== or NaN: unchanged)
    };
    return {one(c.r, g.x), one(c.g, g.y), one(c.b, g.z)};
}

namespace {

// orig: 0x000365F0 (thiscall, (from, to), RET 8; ECX not read): do the two sequences' text keys (+0x8C, records of 8
// bytes {time, name}) share a name (strcmp)? The inner loop over `to`'s keys runs to `from`'s key count (the original
// reads past a shorter array: the port stops at `to`'s end, a UB guard; object_gaps.md 18.4).
bool SharesTextKey(const ni::NiSequence* from, const ni::NiSequence* to) {
    if (!from || !to) return false;
    const auto* a = dynamic_cast<const ni::NiTextKeyExtraData*>(from->textKeysObj.Get());
    const auto* b = dynamic_cast<const ni::NiTextKeyExtraData*>(to->textKeysObj.Get());
    if (!a || !b || a->keys.empty() || b->keys.empty()) return false;   // (+0x14 counts, +0x18 arrays)
    const size_t n = a->keys.size();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n && j < b->keys.size(); ++j)
            if (std::strcmp(a->keys[i].text.c_str(), b->keys[j].text.c_str()) == 0) return true;
    return false;
}

// orig: CharacterDef::FindAnimationBySequence 0x0003FD20: the first animation whose sequence has the same name
// (strcmp).
AnimEntry* EntryOfDef(const CharacterDef& d, const ni::NiSequence* seq) {
    for (const auto& a : d.animations)
        if (a && a->sequence && std::strcmp(a->sequence->name.c_str(), seq->name.c_str()) == 0) return a.get();
    return nullptr;
}

// orig: AnimEntry 0x0003F150 (time) / 0x0003F0C0 (type): the transition to the animation of that sequence name.
const AnimTransition* TransitionByName(const AnimEntry* from, const char* name) {
    if (!from) return nullptr;
    for (const AnimTransition& t : from->transitions)
        if (t.target && t.target->sequence && std::strcmp(t.target->sequence->name.c_str(), name) == 0) return &t;
    return nullptr;
}

}  // namespace

// orig: 0x000362D0: every controller of the sequence (all NiTimeControllers) loops or clamps.
void SetCycleType(ni::NiSequence* seq, bool loop) {
    if (!seq) return;
    const u32 type = loop ? 0 : 2;
    for (u32 i = 0; i < seq->ControllerCount(); ++i)
        if (ni::NiTimeController* c = seq->Controller(i))
            c->flags = static_cast<u16>((c->flags & 0xFFF9) | (type << 1));   // orig: 0x0013EBA0
}

namespace {

// orig: 0x00036240: the reference controller and every keyframe controller of the sequence get the frequency.
void SetSequenceFrequency(ni::NiSequence* seq, f32 f) {
    if (!seq) return;
    if (ni::NiTimeController* rc = seq->ReferenceController()) rc->frequency = f;
    for (u32 i = 0; i < seq->ControllerCount(); ++i)
        if (auto* kc = dynamic_cast<ni::NiKeyframeController*>(seq->Controller(i))) kc->frequency = f;
}

}  // namespace

// orig: 0x00033E60 (cdecl): does the key time cross [t, t + dt)? A looping key repeats every `period` (the offset is
// then not used).
bool KeyCrossed(f32 key, f32 t, f32 ofs, f32 period, bool loop) {
    double x = key;
    if (key < 0.0f) x = double(key) * -1.0f;          // TEST AH,5; JP skips (>= / NaN)
    if (loop) {
        const s32 n = static_cast<s32>(static_cast<s64>(double(t) / period));   // orig: _ftol 0x00167110
        x = double(n) * period + x;
        if (!(double(t) <= x)) x = x + period;        // TEST AH,0x41; JNE skips (t <= x / NaN)
    } else {
        x = x + ofs;
    }
    if (!(x < double(g_frameStep) + t)) return false; // TEST AH,5; JP: x >= t + dt / NaN
    return !(x < double(t));                          // TEST AH,1; JNE: x < t
}

namespace {

// orig: 0x000402D0 PlaySoundEvents (thiscall def, kind, pos, sequence, local time, flag): the sound events of the
// sequence's animation whose key time is crossed this step.
void PlaySoundEvents(const CharacterDef& d, u32 kind, const NiPoint3& pos, ni::NiSequence* seq, f32 tLocal, bool flag,
                     LevelData& level);

}  // namespace

Character* CharacterOwningMesh(LevelData& level, ni::NiAVObject* geom) {   // orig: 0x00036A20
    for (ni::NiAVObject* a = geom; a; a = a->parent)  // (the geometry first, then its parents +0x14)
        for (Character* c : level.characters)         // 0x000C26E0 (S+0x90)
            if (c && c->GetNode() == a) return c;     // vf30
    return nullptr;
}

// orig: Footstep_Play 0x00033F20 (cdecl kind, pos): one of four samples of the surface kind (rand() % 4 + 1, also for
// the kinds outside 1..10), loaded by name and played in 3D at pos.
void FootstepPlay(LevelData& level, u32 kind, const NiPoint3& pos) {
    static const char* const kFormats[10] = {
        "sounds/footsteps/metal%d.wav",      "sounds/footsteps/hard%d.wav",  "sounds/footsteps/wood%d.wav",
        "sounds/footsteps/metal%d.wav",      "sounds/footsteps/wet_puddle/wet%d.wav",
        "sounds/footsteps/wet2_%d.wav",      "sounds/footsteps/wet1_%d.wav", "sounds/footsteps/gravel%d.wav",
        "sounds/footsteps/wet3_%d.wav",      "sounds/footsteps/wood%d.wav"};   // orig: jump table 0x0003407C
    sound::GameSound& snd = level.Sound();
    const char* fmt = kind - 1 < 10 ? kFormats[kind - 1] : "sounds/footsteps/metal%d.wav";
    char path[256];
    std::snprintf(path, sizeof path, fmt, crt::Rand() % 4 + 1);
    const u32 sample = level.loadSound ? level.loadSound(path, 0) : 0u;   // orig: Sound_Load 0x000C9860
    (void)snd.Play3D(sample, pos, false);             // orig: Sound_Play3D 0x000C9AC0 (0x0003406C)
}

namespace {

// orig: SoundEvents::Play 0x000CA8C0 (cdecl event, kind, pos, flag): a footstep event plays Footstep_Play (with the
// flag), any other `rand() % count` (DIV, unsigned) of its sounds, 3D at pos or 2D, without loop; the event's volume /
// +0x28 / +0x2C parameters are not passed (only `push 0; push sample`). Returns the voice (the caller's AnimEvent +8).
// L: the footstep path (and a footstep event without the flag) returns a stale stack word; the port returns 0.
s32 PlaySoundEvent(s32 ev, u32 kind, const NiPoint3& pos, bool flag, LevelData& level) {
    sound::GameSound& snd = level.Sound();
    const SoundEventTable* table = level.soundEvents.get();
    if (!table || !table->Config()) return 0;         // orig: DAT_002EAC18
    if (ev <= -1 || u32(ev) >= table->Events().size()) return 0;
    const SoundEvent* e = table->Events()[ev].get();
    if (!e) return 0;
    if (e->is3D && e->footstep == 1) {
        if (flag) FootstepPlay(level, kind, pos);    // orig: Footstep_Play 0x00033F20
        return 0;
    }
    if (e->sounds.empty()) {
        NotPorted("SoundEvents::Play with an empty event (division by zero in the original)");
        return 0;
    }
    const u32 i = static_cast<u32>(crt::Rand()) % static_cast<u32>(e->sounds.size());   // 0x000CA929 / 0x000CA957
    if (e->is3D) return snd.Play3D(e->sounds[i], pos, false);   // orig: Sound_Play3D 0x000CA940
    return snd.PlaySfx(e->sounds[i], false);          // orig: Sound_PlaySFX 0x000CA969
}

void PlaySoundEvents(const CharacterDef& d, u32 kind, const NiPoint3& pos, ni::NiSequence* seq, f32 tLocal, bool flag,
                     LevelData& level) {
    if (!seq) return;
    AnimEntry* a = EntryOfDef(d, seq);
    if (!a || a->events.empty()) return;
    ni::NiTimeController* rc = seq->ReferenceController();
    if (!rc) return;
    const bool loop = rc->CycleType() == 0;
    const f32 dur = F((double(rc->hiKeyTime) - rc->loKeyTime) / rc->frequency);
    const f32 ofs = loop ? F(-double(seq->offset)) : 0.0f;
    for (AnimEvent& ev : a->events) {
        const f32 kt = F(double(ev.time) / rc->frequency);
        if (KeyCrossed(kt, tLocal, ofs, dur, loop) && ev.soundEvent >= 0)
            ev.handle = static_cast<u32>(PlaySoundEvent(ev.soundEvent, kind, pos, flag, level));   // (AnimEvent +8)
    }
}

// orig: DynamicCast(RTTI 0x002F9FF4 NiKeyframeController) 0x00011470. NiVisController / NiUVController derive from it in
// this build (initializers 0x0022B2C0 / 0x0022B140), as NiKeyframeManager.cpp's IsKeyframeKind.
bool IsKeyframeKind(const ni::NiTimeController* c) {
    return dynamic_cast<const ni::NiKeyframeController*>(c) || dynamic_cast<const ni::NiVisController*>(c) ||
           dynamic_cast<const ni::NiUVController*>(c);
}

// orig: NiSequence::GetControllerByName 0x0012B4E0 (thiscall seq, (const char*), RET 4): the controller (+0x48[i]) of
// the first entry whose object name equals `name` (inlined strcmp: exact, case-sensitive); the manager's string pool
// (+0x30, linked) or the name array (+0x18, unlinked) both hold the entry names.
ni::NiObject* ControllerByName(const ni::NiSequence* seq, const std::string& name) {
    for (const auto& e : seq->entries)
        if (std::strcmp(e.name.c_str(), name.c_str()) == 0) return e.controllerObj.Get();
    return nullptr;
}

}  // namespace

// ---- 2.1 / 2.2 the update --------------------------------------------------------------------------------------
void GameObject::UpdateAmbientLight() {               // orig: 0x00096AA0
    if (!ambientLight) return;                        // +0x30
    ambientLight->ambient = AmbientStep(ambientLight->ambient, g_frameStep, level.globalAmbient);   // 0x000C0410
    ++ambientLight->revision;
    const NiPoint3& w = node->world.translate;        // +0x2C +0xA0
    const NiPoint3 start{w.x, F(double(w.y) + 2.0f), w.z};   // [0x00293B20]
    f32 dist = 30.0f;                                 // 0x41F00000
    const ni::RayHit hit = ni::RayCast(start, {0, -1, 0}, dist, true, 3);   // 0x000FB780
    if (!hit) return;
    (void)hit.Surface();                              // HitSurface 0x000FA330 (result unused)
    ++ambientLight->revision;
    ambientLight->dimmer = F(double(ambientLight->dimmer) * std::bit_cast<f32>(0x3F733333u) +   // [0x00294FDC]
                             double(std::bit_cast<f32>(0x3D4CCCD0u)));                         // [0x00293AC8]
    ambientLight->UpdateDownwardPass(0.0f, true);     // Update(0, 1)
}

bool Character::Update(f32 dt, int pass) {            // orig: 0x0003EA90
    if (pass == 0) {
        if (body) velBefore = body->velocity;         // raw copy
        animTime = F(double(1.0f) * dt + animTime);   // orig: DAT_002D6210 (time scale 1.0)
        UpdateTimers(dt);
        UpdateCameraDistance();
        UpdateBodyShape();
        pcActive = physic && physic->IsActive();      // orig: 0x0013EBF0
        Think(dt);                                    // orig: vf54
        ApplyRootMotion();
        return true;
    }
    // orig: 0x0003EB2D: a held player (D->vf0C(2) && 0x000503B0(0), +0x7A0) skips the ground contacts, the room
    // update and the node transform (its node hangs at the holder's tip: object_gaps.md 16.3).
    if (!(cdef.IsType(2) && HeldBy())) {
        // orig: 0x0003EB48: a rat with its ride (type 4, +0x7AC) skips the ground contacts while the ride's intro
        // timer is below 3.0 (+0x44 < [0x0029563C]; TEST AH,5; JNP), i.e. during the intro.
        const RatRide* ride = type == 4 ? static_cast<const Player*>(this)->ride.get() : nullptr;
        if (!ride || !(ride->introTime < 3.0f)) UpdateGroundContacts();
        const NiPoint3 p = GetWorldPosition();
        const NiPoint3& n = node->local.translate;    // orig: 0x0002DE70 (per component, NaN = different)
        if (!(p.x == n.x) || !(p.y == n.y) || !(p.z == n.z)) UpdateRoom();
        SyncNodeTransform();
    }
    if ((IsDead() && IsDyingAnim()) || !(cameraDistance <= updateDistance)) {
        if (IsDead() && ambientLight) {               // orig: 0x0003EBCE (vfA8; no null test of +0x30)
            ambientLight->ambient = AmbientStep(ambientLight->ambient, g_frameStep, level.globalAmbient);   // +0xE4
            ++ambientLight->revision;                 // +0xCC
        }
    } else {
        PushOffCharacters();
        animUpdated = 1;
        UpdateAnimation(dt);                          // orig: vf5C
        AfterAnimation();                             // orig: vf60
        for (GameObject* o = inventory.NextObject(nullptr); o;) {   // orig: 0x0003ECA0 (next fetched first)
            GameObject* next = inventory.NextObject(o);
            if (!o->Tick()) delete o;                 // vf08; vf00(1)
            o = next;
        }
        // orig: 0x0003ECE5: only while the node was drawn (+0xB2 == 0): a living character's UpdateAmbientLight
        // (vf48), a dead one's colour step
        if (node->framesNotDrawn == 0) {
            if (!IsDead()) {
                UpdateAmbientLight();
            } else if (ambientLight) {
                ambientLight->ambient = AmbientStep(ambientLight->ambient, g_frameStep, level.globalAmbient);
                ++ambientLight->revision;             // 0x00034490 (stores +0xE4..+0xEC, ++revision)
            }
        }
    }
    return !deleteMe;
}

void Character::UpdateTimers(f32 dt) {                // orig: 0x00034AA0 (2.3)
    if (animTime > 6000.0f) NotPorted("Character::UpdateTimers: the 6000 s time base fold");
    timer634 = hasControl ? F(double(dt) + timer634) : 0.0f;
    if (IsDead()) {
        if (cdef.IsType(2)) return;
        deathFade = F(double(dt) + deathFade);        // FST
        if (deathFade > 1.0f) deathFade = 1.0f;       // FCOMP [0x00293A98]; TEST AH,0x41; JNE (NaN kept)
        if (g_decomposeCorpses) return;               // orig: byte 0x002D6214 (0x00034DD2)
    }
    deathFade = 0;
}

void Character::UpdateCameraDistance() {              // orig: 0x00034DF0 (2.4)
    const NiPoint3 cam = level.camera ? level.camera->position : NiPoint3{};
    const NiPoint3 p = GetWorldPosition();
    const double dx = double(cam.x) - p.x, dy = double(cam.y) - p.y, dz = double(cam.z) - p.z;
    const double d2 = (dz * dz + dy * dy) + dx * dx;
    const double r = updateDistance;
    cameraDistance = F(d2 < r * r ? std::sqrt(d2) : r - 1.0f);
    f32 x = F(double(cameraDistance) * 0.04f);       // orig: 0x00293C38
    if (x < 1.0f || cdef.IsType(2) || type == 0xE || type == 0xB || type == 0xC) x = 1.0f;
    lodStep = F(1.0 / double(x) + lodStep);
}

void Character::UpdateBodyShape() {                   // orig: 0x00034EE0 (2.5)
    if (body) {
        const double s = node->local.scale;
        body->shape.ext = {F(s * cdef.boundWidth), F(s * boundHeight), F(s * cdef.boundDepth)};
        if (IsDead()) {
            body->noRay = 1;
            if (!cdef.IsType(2)) body->b1A = 1;
            DeleteInventoryEffects();                 // 0x00034A50 (every step while dead)
        } else if (!flag60) {
            body->noRay = 0;
            body->b1A = 0;
        }
    }
    if ((IsDead() && IsDyingAnim()) || cameraDistance > updateDistance || flag28) {
        if (IsDead() && !(body && body->supported)) return;   // a dead body keeps falling
        if (body) body->SetEnabled(false);
        return;
    }
    deathFade = 0;
    if (body) {
        body->SetEnabled(true);
        if (type == 7) body->shape.pinned = 1;
    }
}

void Character::ApplyRootMotion() {                   // orig: 0x0003E240 (2.6; docs/analysis/root_motion_port.md)
    // 0x0003E268: a first GetActive whose result is discarded. This exit keeps rootStarted.
    if (!physic || !physic->IsActive()) return;       // 0x0003E26D..0x0003E282
    const ni::NiPointer<ni::NiAVObject> bone = physic->bone;   // 0x0003E290 pc+0x68 (AddRef for the call)
    if (bone) {                                       // 0x0003E2B6 (null: straight to the tail, rootStarted = 0)
        // The original reads +0xCC and calls vfA8 / vfA0 without a type check (the root bone is a NiNode).
        auto* bn = dynamic_cast<ni::NiNode*>(bone.Get());
        std::vector<ni::NiPointer<ni::NiAVObject>> tmp;   // 0x000378F0(1, 1)
        if (bn)
            for (u32 i = 0; i < bn->Children().size(); ++i) {   // size +0xCC re-read every pass (0x0003E372)
                ni::NiAVObject* child = bn->Children()[i].Get();
                if (!child) continue;                 // 0x0003E2E0: null slots are neither kept nor detached
                tmp.emplace_back(child);              // 0x0003E2F7..0x0003E322 (inlined Add)
                bn->DetachChild(child);               // orig: vfA8 DetachChildAt(i) 0x00105D70 (same slot; see 4)
            }
        ni::NiSequence* cur = curSeq;                 // +0xA4
        if (cur && !finished) {                       // 0x0003E381..0x0003E397
            ni::NiTimeController* r0 = cur->ReferenceController();   // +0x48[+0x90] (no bounds check in the original)
            ni::NiTimeController* rc = r0 && IsKeyframeKind(r0) ? r0 : nullptr;   // 0x0003E3B6
            ni::NiTimeController* c = nullptr;        // 0x0003E3D0..0x0003E404: by the bone's name, cast 0x0030E628
            if (ni::NiObject* o = ControllerByName(cur, bone->name)) c = dynamic_cast<ni::NiTimeController*>(o);
            if (!rc) {                                // 0x0003E40E: tmp destroyed, bone released, return
                NotPorted("Character::ApplyRootMotion: no keyframe reference controller (original bug: the bone's "
                          "children stay detached)");
                return;                               // (rootStarted is NOT cleared on this path)
            }
            if (c) {                                  // 0x0003E44C
                ni::NiObjectNET* oldTarget = c->targetPtr;   // 0x0003E456 (0x0013EB60 GetTarget)
                c->SetTarget(bone.Get());             // 0x0003E464 vf3C 0x0013EC20 (no-op when it is the bone)
                ni::NiTimeController* oldNext = c->nextControllerPtr.Get();   // 0x0003E46E (raw, no reference)
                if (!bone->controller) {              // 0x0003E47B
                    c->nextControllerPtr = nullptr;   // 0x0013EDD0(c, 0)
                    bone->controller = c;             // 0x0013EDD0 on the bone's +0x10
                } else {                              // 0x0003E491: append c at the tail of the bone's chain
                    ni::NiTimeController* last = bone->controller.Get();
                    while (last->nextControllerPtr) last = last->nextControllerPtr.Get();
                    last->nextControllerPtr = c;      // 0x0003E4B7 (c itself when c is the tail)
                    c->nextControllerPtr = nullptr;   // 0x0003E4C0
                }
                c->SetActive(true);                   // 0x0003E4C9
                if (IsKeyframeKind(c)) c->forceUpdate = true;   // 0x0003E4DE byte +0x3F = 1
                // else: DynamicCast(0x0030EA74 NiUDSKFController) -> 0x00141EC0 (+0x24 = 1); no such class in the port
                const f32 dur = F((double(rc->hiKeyTime) - rc->loKeyTime) / rc->frequency);   // 0x0003E4FF..0x0003E51C
                const double tt = double(cur->offset) + animTime;   // 0x0003E524 FLD +0xA8; FADD +0x78 (kept in ST0)
                f32 ts = F(tt);                       // 0x0003E52D FST
                if (IsAnimPlaying(46)) {              // ladderclimboff (+0x178)
                    const f32 lim = F(double(dur) - 0.001f);   // 0x00293AC4
                    if (tt > lim) ts = lim;           // FCOMP; TEST AH,0x41; JNE: <, == and NaN keep
                } else if (IsAnimPlaying(41)) {       // ledgeclimboff (+0x164)
                    const f32 lim = F(double(dur) - 0.1f);     // 0x00293AB0
                    if (tt > lim) ts = lim;
                } else if (tt >= dur) {               // 0x0003E59B FCOMP; TEST AH,1; JNE: < and NaN keep
                    ts = dur;
                }
                c->Update(ts);                        // 0x0003E5B7 vf38 (NiKeyframeController 0x001270B0)

                // The root bone's offset in its parent's space: world scale * (world rotate * local translate).
                const ni::NiNode* parent = bone->parent;   // +0x14
                const f32 s = parent->world.scale;    // +0xAC
                const NiPoint3 r = ni::MulVector(parent->world.rotate, bone->local.translate);   // 0x00011770
                const NiPoint3 p{F(double(s) * r.x), F(double(s) * r.y), F(double(s) * r.z)};   // 0x000115C0
                const bool started = rootStarted != 0;   // BL, read at 0x0003E5EC
                if (started) {                        // 0x0003E605: first step after BeginRootMotion
                    rootPrev = p;
                    rootY = 0;
                }
                NiPoint3 d{F(double(p.x) - rootPrev.x), F(double(p.y) - rootPrev.y), F(double(p.z) - rootPrev.z)};
                rootPrev = p;                         // 0x0003E63C..0x0003E648

                if (IsAnimPlaying(182)) {             // runkick (+0x398), 0x0003E694
                    rootY = p.y;
                    rootFlags = 1;
                    const double h = double(node->local.scale) * cdef.boundHeight;   // FLD node+0x6C; FMUL D+0x94
                    if (h <= double(p.y)) rootY = F(h);   // FCOMP p.y; TEST AH,0x41; JP: (h > p.y or NaN) -> d.y = 0
                    else d.y = 0;
                    if (started) {                    // 0x0003E6D8
                        d.y = 0;
                        NiPoint3 q = GetWorldPosition();   // vf34 0x00035440
                        q.y = F((double(rootY) - body->shape.ext.y) + q.y);
                        SetPosition(q);               // vf1C 0x000353B0
                    }
                    body->shape.ext.y = rootY;        // 0x0003E722 (body is not null-checked)
                } else if (IsAnimPlaying(96) || IsAnimPlaying(97) || IsAnimPlaying(94) || IsAnimPlaying(95) ||
                           IsAnimPlaying(180) || IsAnimPlaying(183) || type == 4) {   // 0x0003E72D..0x0003E800
                    rootFlags = 1;                    // 0x0003E8F1
                    d.y = 0;                          // 0x0003E8FE
                    physic->keepY = 1;                // 0x0003E906
                } else if (IsAnimPlaying(77) || IsAnimPlaying(78) || IsAnimPlaying(79) || IsAnimPlaying(80)) {
                    rootFlags = 1;                    // 0x0003E8B0 (before the model test)
                    if (model) {                      // C+0x564
                        const f32 rad = model->worldBound.radius;   // model+0x2C
                        body->shape.ext.x = rad;      // 0x0003E8D9
                        body->shape.ext.z = rad;      // 0x0003E8E9
                    }
                    d.y = 0;
                    physic->keepY = 1;
                } else {                              // 0x0003E876 (jump, jumprun, skid, ledge / ladder, ...)
                    rootY = F(double(node->local.scale) * cdef.boundHeight);
                    if (!(body->force.y == 0.0f)) d.y = 0;   // FCOMP 0.0; TEST AH,0x44; JNP: equal keeps (NaN zeroes)
                }

                if (type == 7 || directRoot) {        // 0x0003E90A / 0x0003E913 -> 0x0003E954
                    const NiPoint3 w = GetWorldPosition();   // vf34
                    SetPosition({F(double(d.x) + w.x), F(double(d.y) + w.y), F(double(d.z) + w.z)});   // vf1C
                } else {                              // 0x0003E91D: d / g_frameStep (0x00011590), body vf50
                    const double invD = 1.0 / double(g_frameStep);   // FLD 1.0; FDIV [0x002D67E8]
                    const f32 inv = F(invD);          // FST (x and y reload it; z multiplies the unrounded quotient)
                    const NiPoint3 a{F(double(inv) * d.x), F(double(inv) * d.y), F(invD * d.z)};
                    NiPoint3& v = body->velocity;     // vf50 AddVelocity 0x000AF7D0
                    v.x = F(double(v.x) + a.x);
                    v.y = F(double(a.y) + v.y);
                    v.z = F(double(a.z) + v.z);
                }

                c->SetActive(false);                  // 0x0003E9A8
                c->SetTarget(oldTarget);              // 0x0003E9B7 vf3C
                bone->RemoveController(c);            // 0x0003E9BE 0x000FE4C0
                c->nextControllerPtr = oldNext;       // 0x0003E9CA 0x0013EDD0
            }
        }
        if (bn)
            for (const auto& ch : tmp) bn->AttachChild(ch.Get(), true);   // 0x0003E9E0 vfA0(child, 1)
        tmp.clear();                                  // 0x00034190
    }
    rootStarted = 0;                                  // 0x0003EA0A
}

void Character::UpdateGroundContacts() {              // orig: 0x00038880 (2.7; inventory_batch1_port.md 4.4)
    if (!body) return;
    const phys::CharShape& sh = body->shape;
    bool meshRay = false;                             // [esp+0xF]: the 0x80 ray is cast once per call
    bool inLiquid = false;                            // [esp+0xE]: a liquid contact this call
    for (u32 i = 0; i < 8; ++i) {
        (void)cdef.IsType(2);                         // 0x000388CA (both branches read the same shape +0x40)
        const u8 m = sh.contactFlags[i];              // [esp+0xD]
        if (m == 0xFF) break;
        const u32 kind = level.footstepMaterials[m & 3];   // S+0x160[m & 3] ([esp+0x20])
        if (cdef.IsType(2) && m == 0x80 && !meshRay) {   // orig: 0x0003892C: standing on a moving collision mesh?
            meshRay = true;
            const NiPoint3 wp = GetWorldPosition();   // vf34
            f32 len = 20.0f;                          // 0x41A00000
            const ni::RayHit h = ni::RayCast({wp.x, F(double(wp.y) + 3.0f), wp.z}, {0, -1, 0}, len, true, 2);   // [0x0029563C]
            if (h && h.Flags() == 0x80)              // 0x000FA320
                if (Character* meshOwner = CharacterOwningMesh(level, ni::g_lastMeshHit)) {   // 0x00036A20
                    meshOwner->OnMeshContact(this, 0.0f, false);   // 0x00039347: vfB8(this, 0, 0)
                    break;                            // 0x0003933E -> 0x0003934D (after the contact loop)
                }
        }
        // ---- the damage surfaces (0x000389DD..0x00038E49; inventory_batch1_port.md 4.4.2)
        const f32 dmg = level.materialDamage;         // DAT_002D6218 -> [esp+0x7C]
        bool deadly = false;
        if (cdef.IsType(2) && (m & 4) &&
            double(animTime) - static_cast<Player&>(*this).lastSurfaceDamage > 1.0f) {   // +0x78 - +0xB58; TEST AH,0x41; JNE
            if (health > 0.0f) {                      // TEST AH,0x41; JNE -> the liquid part
                Player& P = static_cast<Player&>(*this);
                const NiPoint3 pos = GetWorldPosition();   // vf34 -> esp+0xB0
                if (type == 3)                        // Bender: sparks
                    if (Def* sp = level.defs ? level.defs->Find("effect_sparks") : nullptr)   // 0x00295D0C
                        (void)sp->SpawnEffect(level, pos, {0.0f, 1.0f, 0.0f});   // vf20(&pos, &(0, 1, 0))
                if (static_cast<s32>(m) != level.levelTable[7] ||   // 0x000C1A90(7)
                    double(animTime) - P.lastSurfaceDamage > 2.0f) {   // [0x00293B20]; TEST AH,0x41; JNE
                    Damage(dmg, this);                // vf7C(dmg, this)
                    P.lastSurfaceDamage = animTime;   // +0xB58 = +0x78 (dword)
                    // the fade colour: lava (0.8, 0.7), sewage (0, 0.8), else (0.8, 0) (pushes 0x00038ADB..0x00038B34)
                    const f32 r = kind == 5 ? 0.0f : 0.8f;
                    const f32 g = kind == 4 ? 0.7f : kind == 5 ? 0.8f : 0.0f;
                    level.camera->FadeTo(r, g, 0.0f, 0.5f, 0.1f, 0.2f, 0.2f);   // 0x00013130 on S+0x8C
                    P.rumble.Add(0.5f, 0.5f, 0.3f, 0);   // 0x0005F000(P+0xA04)
                    if (health <= 0.0f) {             // TEST AH,0x41; JP
                        deathKind = 9;                // +0x778
                        SurfaceDeathKind(m);          // 0x000373E0
                    }
                    if (static_cast<s32>(m) == level.levelTable[7]) P.shock = 1;   // +0xB9E (0x000C1A90(7) again)
                    if (static_cast<s32>(m) == level.levelTable[8]) (void)SoundAt(0x7F, &pos, 3, 0, nullptr);   // def 0x0003FEA0
                }
            }
        } else if (m & 8) {                           // 0x00038BD0
            deadly = true;
        } else if (cdef.IsType(2) || !(m & 4) || !(m & 0x40)) {   // vf0C(2) again at 0x00038BDE
            // the liquid part
        } else if (m & 0x80) {
            continue;                                 // (an NPC on a 0xC4 contact: JS 0x0003932A, the next contact)
        } else {
            deadly = true;                            // an NPC on an m & 0x44 contact
        }
        if (deadly) {                                 // 0x00038C05: the deadly surface
            const NiPoint3 pos = GetWorldPosition();  // vf34 -> esp+0x5C (before the health test)
            if (health > 0.0f) {                      // TEST AH,0x41; JNE -> the liquid part
                if (cdef.IsType(2)) {
                    Damage(health, this);             // vf7C(+0x4F4, this): the whole health (0x00038C46)
                    switch (kind) {                   // jump table 0x000394DC (kind - 4, 0..6; else the default)
                    case 4:                           // lava (0x00038CED)
                        level.camera->FadeTo(0.0f, 0.8f, 0.8f, 0.5f, 0.1f, 0.2f, 0.5f);
                        (void)SoundAt(0x49, &pos, 3, 0, nullptr);
                        break;
                    case 5: case 7: case 9:           // sewage / water / mud (0x00038CA8)
                        level.camera->FadeTo(0.0f, 0.8f, 0.0f, 0.5f, 0.1f, 0.2f, 0.5f);
                        (void)SoundAt(0x4A, &pos, 3, 0, nullptr);
                        break;
                    case 10:                          // sand (0x00038C60)
                        level.camera->FadeTo(0.8f, 0.0f, 0.0f, 0.5f, 0.1f, 0.2f, 0.2f);
                        (void)SoundAt(0x4B, &pos, 3, 0, nullptr);
                        break;
                    default:                          // 0x00038D34
                        level.camera->FadeTo(0.8f, 0.0f, 0.0f, 0.5f, 0.1f, 0.2f, 0.5f);
                        break;
                    }
                    static_cast<Player&>(*this).rumble.Add(0.5f, 0.5f, 0.5f, 0);   // 0x0005F000
                } else {
                    Damage(health, this);             // vf7C(+0x4F4, this)
                    if (health > 0.0f && type != 0xB) health = 0;   // TEST AH,0x41; JNE
                    if (def->IsType(0x200000)) {      // jump table 0x000394F8 (kind - 4)
                        u32 snd = 0;
                        if (kind == 4) snd = 0x49;
                        else if (kind == 5 || kind == 7 || kind == 9) snd = 0x4A;
                        else if (kind == 10) snd = 0x4B;
                        if (snd) {                    // 0x00038E08; a non-zero voice: Voice_SetMaxDistance(v, D+0x198)
                            const s32 v = SoundAt(snd, &pos, 3, 0, nullptr);
                            if (v) {                  // 0x00038E1C (D+0x198: a GenericNPCDef field)
                                if (const auto* nd = dynamic_cast<const GenericNPCDef*>(&cdef))
                                    VoiceMaxDistance(v, nd->value198);
                                else
                                    NotPorted("Character 0x00038E1C: D+0x198 of a definition without the field");
                            }
                        }
                    }
                }
                if (health <= 0.0f) {                 // 0x00038E24: TEST AH,0x41; JP
                    deathKind = 9;
                    SurfaceDeathKind(m);
                }
            }
        }
        // 0x00038E49: the liquid part (docs/analysis/rodent_port.md 5). The ray casts are pure queries (the ray state
        // they leave is refreshed by every cast along the axes it reads), so only the liquid kinds compute them.
        if (m & 0x80) continue;                       // JS
        const phys::TriRef& c = sh.contacts[i];       // shape +0x5C + 4i (EDI): a stage or mesh scratch triangle
        if (c.Empty()) continue;                      // (port guard: a listed contact always has its triangle)
        if (!(kind >= 4 && kind <= 7) && kind != 9) continue;   // jump table 0x00039514: kind 8 (and > 9) nothing
        NiPoint3 p = GetWorldPosition();              // L10 (vf34)
        NiPoint3 nrm = c.Normal();                    // L50 (0x000FA8C0), copied to L34
        const NiPoint3& v0 = c.V(0);                  // *(NiPoint3*)[EDI]: the triangle's first vertex
        const double dx = double(p.x) - v0.x, dy = double(p.y) - v0.y, dz = double(p.z) - v0.z;
        const double D = (dz * nrm.z + double(nrm.y) * dy) + dx * nrm.x;   // FMULP st3; FMUL st2; FADDP; FMUL; FADDP
        // project onto the plane along (0, -1, 0) (L44): each offset is stored as a float first
        p = {F(double(F(0.0 * D)) + p.x), F(double(F(-1.0 * D)) + p.y), F(double(p.z) + F(0.0 * D))};
        const f32 top = F(double(F(double(boundHeight) + heightOffset)) + GetWorldPosition().y);   // +0x4D4 + +0x4D0
        const double hD = double(top) - GetWorldPosition().y;   // a second vf34; FST
        const f32 hF = F(hD);
        const NiPoint3 o{p.x, F(double(hF) + p.y), p.z};   // L80
        f32 len = F(hD + 0.1f);                       // L40: the unrounded height + 0.1 (0x00293AB0)
        // the same triangle only (a mesh contact is a scratch triangle: never the ray's result)
        const ni::RayHit same = ni::RayCast(o, {0, -1, 0}, len, true, 3);
        if (c.mesh || !same.triangle || same.triangle != &c.bsp->triangles[c.index]) continue;
        // 0x00039016: the splash (once per liquid entry) and the ripples (inventory_batch1_port.md 4.4.3)
        const char* name = kind == 4 ? "Lava_Splash"  // jump table 0x00039514 (kind - 4): 0x00295CE4
                         : kind == 5 ? "Slime_Splash" // 0x00295CFC
                         : kind == 6 ? "Oil_Splash"   // 0x00295CF0
                         : kind == 7 ? "Water_Splash" // 0x00295CB8
                                     : "Mud_Splash";  // 0x00295CAC (kind 9)
        if (!splashed) {                              // +0x629
            Owner* stageOwner = level.currentStage;   // S+0x5C ? S+0x5C + 4 : 0 (the stage's Owner part)
            if (Def* sd = level.defs ? level.defs->Find(name) : nullptr)
                if (auto* splash = dynamic_cast<SplashDef*>(sd))   // (the original calls 0x00073880 directly)
                    SetNodeScale(*splash->SpawnDir(level, stageOwner, p, nrm, "Splashive"), 1.0f);   // 0x00295CD8;
                                                      //  0x00073720(1.0); nrm becomes UNIT_Z
            if (kind == 4)                            // lava: also the smoke (with the splash's UNIT_Z when it spawned)
                if (Def* sm = level.defs ? level.defs->Find("effect_Smoke_L") : nullptr)   // 0x00295CC8
                    (void)sm->SpawnEffect(level, p, nrm);   // vf20(&p, &nrm)
            splashed = 1;                             // also when the definition is missing
        }
        inLiquid = true;                              // esp+0xE
        const double v2 = (double(velBefore.x) * velBefore.x + double(velBefore.y) * velBefore.y) +
                          double(velBefore.z) * velBefore.z;   // +0x4E4..+0x4EC: x first, then y, then z
        if (v2 > 6.25f) rippleTimer = F(double(rippleTimer) - g_frameStep);   // [0x00295CA8]; TEST AH,0x41; JNE
        if (rippleTimer < 0.0f) {                     // TEST AH,5; JP
            p.y = F(double(p.y) + 0.5f);              // [0x00293AA0]
            Def* rd = level.defs ? level.defs->Find("Character_Ripple") : nullptr;   // (into DAT_002DC9F8)
            if (!rd && level.defs) {                  // a new RippleDef, registered by Def::Def
                const char* tex = crt::StrNICmp(level.name.c_str(), "Level1-2", 8) == 0 ? "Sewer_ripple_g_5.tga"
                                : crt::StrNICmp(level.name.c_str(), "Level3-", 7) == 0 ? "Ripple_lava_circle.tga"
                                                                                          : "Ripple_white_circle.tga";
                rd = NewRippleDef(level, "Character_Ripple", tex, 0.1f);   // new(0x34) + 0x00071710(.., 0x3DCCCCCD)
            }
            if (auto* ripple = dynamic_cast<RippleDef*>(rd))   // (null only when operator_new failed)
                (void)ripple->SpawnRipple(level, &level, p, ni::NiMatrix3{}, 1.0f, 3.0f, 3.0f, 10.0f, 10.0f,
                                          3.40282347e38f);   // 0x00071B10: owner S, identity 0x002D7820
            rippleTimer = F(RandUnit() * 0.4f + 0.8f);   // 0x0002CE30; FMUL [0x00293B28]; FADD [0x00293C18]
        }
    }
    if (!inLiquid) {                                  // 0x0003934D
        splashed = 0;                                 // +0x629
        rippleTimer = 0;                              // +0x62C
    }
    if (cdef.IsType(2) && body->supported) {
        if (sh.partnerIsBox && sh.partner) {          // the box's world up vector (column 1 of its rotation)
            const ni::NiMatrix3& r = static_cast<const phys::RigidBox*>(sh.partner)->rot;
            auto up = [&](int row) {
                return F((double(kUnitY.y) * r.m[row][1] + double(kUnitY.z) * r.m[row][2]) +
                         double(kUnitY.x) * r.m[row][0]);
            };
            groundUp = {up(0), up(1), up(2)};
        } else {
            groundUp = kUnitY;
        }
        groundPos = GetWorldPosition();
    }
}

void Character::SurfaceDeathKind(u32 m) {           // orig: 0x000373E0 (thiscall, RET 4; the caller set +0x778 = 9)
    const auto& t = level.levelTable;                 // S+0x12C[i] (0x000C1A90), compared in this order
    const s32 v = static_cast<s32>(m);
    if (v == t[0]) deathKind = 0xE;
    else if (v == t[1]) deathKind = 0xF;
    else if (v == t[2]) deathKind = 0xB;
    else if (v == t[4]) deathKind = 0xC;
    else if (v == t[3]) deathKind = 0xD;
    else if (v == t[12]) deathKind = 0x15;
    else if (v == t[5]) deathKind = 0x12;
    else if (v == t[6]) deathKind = 0x13;
    else if (v == t[10] || v == t[9]) deathKind = 0x11;
}

void Character::SyncNodeTransform() {                 // orig: 0x00039530 (2.8)
    if (!cdef.IsType(2) || !model) {
        node->local.translate = GetWorldPosition();
        node->local.rotate = rotation;
        return;
    }
    const NiPoint3 f = ni::MulVector(rotation, kUnitX);
    const NiPoint3 u = ni::MulVector(node->local.rotate, kUnitY);
    const bool onGround = body && body->supported;
    const NiPoint3 n = (onGround && groundUp.y > 0.75f) ? groundUp : kUnitY;   // TEST AH,0x41; JNE
    const double dx = double(n.x) - u.x, dy = double(n.y) - u.y;
    const f32 dz = F(double(n.z) - u.z);
    NiPoint3 v{F(double(F(dx * 0.2f)) + u.x), F(dy * 0.2f + u.y), F(double(dz) * 0.2f + u.z)};
    ni::Unitize(v);
    NiPoint3 s{F(double(f.z) * v.y - double(f.y) * v.z), F(double(v.z) * f.x - double(f.z) * v.x),
               F(double(f.y) * v.x - double(v.y) * f.x)};
    ni::Unitize(s);
    NiPoint3 w{F(double(s.z) * v.y - double(s.y) * v.z), F(double(v.z) * s.x - double(s.z) * v.x),
               F(double(s.y) * v.x - double(v.y) * s.x)};
    ni::Unitize(w);
    ni::NiMatrix3& R = node->local.rotate;            // columns -w | v | -s (FCHS per element)
    R.m[0][0] = -w.x, R.m[1][0] = -w.y, R.m[2][0] = -w.z;
    R.m[0][1] = v.x, R.m[1][1] = v.y, R.m[2][1] = v.z;
    R.m[0][2] = -s.x, R.m[1][2] = -s.y, R.m[2][2] = -s.z;
    NiPoint3 p = GetWorldPosition();
    if (onGround && groundUp.y > 0.75f && double(p.y) - 2.0f <= double(groundPos.y) && groundPos.y < p.y)
        p.y = groundPos.y;
    node->local.translate = p;
}

void Character::PushOffCharacters() {                 // orig: 0x00034600 (2.10)
    if (body && !body->shape.partnerIsBox && body->shape.partner) {   // shape +0xB2 / +0xB4
        const auto* o = static_cast<const phys::CharShape*>(body->shape.partner);
        const f32 top = F(double(o->ext.y) + o->pos.y);   // FLD o+0x94; FADD o+0x18; FSTP
        // standing on it: FCOMP; TEST AH,1; JNE (only > or ==; NaN does not push; object_gaps.md 18.1)
        if (double(GetWorldPosition().y) + 1.0f >= top) {
            for (Character* c : level.characters) {   // the S+0x90 walk 0x000C26E0
                const bool kind = c->def->IsType(2) ||
                                  (c->def->IsType(0x200000) && static_cast<const GenericNPCDef*>(c->def)->flag1A0);
                if (!kind || !c->body || &c->body->shape != o) continue;   // c.body+0x40 == o
                if (c->body->b1A && c->body->enabled) continue;   // (!+0x1A || !+0x24)
                const NiPoint3 p = GetWorldPosition();   // vf34
                const double ax = double(p.x) - o->pos.x;   // (kept on the x87 stack)
                const f32 az = F(double(p.z) - o->pos.z);
                double kx = !(ax < 0.0f) ? (double(o->ext.x) - ax) / o->ext.x + 0.1f   // TEST AH,5; JP; FSUBR; FDIV
                                         : -((ax + o->ext.x) / o->ext.x) - 0.1f;       // FADD; FDIV; FCHS; FSUB
                f32 kz = F(!(az < 0.0f) ? (double(o->ext.z) - az) / o->ext.z + 0.1f
                                        : -((double(az) + o->ext.z) / o->ext.z) - 0.1f);
                if (double(kz) * kz + kx * kx < 1.0f) {   // FCOMP [0x00293A98]; TEST AH,5; JP
                    kx = 1.0;
                    kz = 0;
                }
                body->velocity.x = F(kx * 75.0f + body->velocity.x);   // [0x00295A08]; FADD; FSTP (y rewritten as is)
                body->velocity.z = F(double(kz) * 75.0f + body->velocity.z);
                c->body->velocity = {};               // c.body+0x28 = 0
                break;
            }
        }
    }
    // 0x000347E5: Bender's charge punch (210) landing on what his shape stands on (+0xB2 / +0xB4; no body test)
    if (type != 3 || !IsAnimPlaying(210)) return;     // +0x408 is the current or the overlay sequence
    const phys::CharShape& sh = body->shape;
    const void* onChar = sh.partnerIsBox ? nullptr : sh.partner;
    const void* onBox = sh.partnerIsBox ? sh.partner : nullptr;
    if (onChar) {
        for (Character* c : level.characters) {       // S+0x90 (0x000C26E0)
            if (!c->body || &c->body->shape != onChar) continue;   // c.body+0x40 (no body test)
            if (c->body->b1A && c->body->enabled) continue;   // (!+0x1A || !+0x24)
            const NiPoint3 p = c->GetWorldPosition(); // vf34
            const NiPoint3 q = CharacterPoint();      // vf78
            if (p.y < q.y) {                          // FCOMP; TEST AH,5; JP (not below or NaN: no kill)
                c->Kill(this);                        // vf84
                body->velocity.y = velBefore.y;       // body+0x2C = +0x4E8
            }
            break;
        }
    }
    if (onBox && level.currentStage) {                // the current stage's objects (S+0x5C, +4)
        for (GameObject* o = level.currentStage->NextObject(nullptr); o; o = level.currentStage->NextObject(o)) {
            auto* d = o->def->IsType(0x40) ? dynamic_cast<Destructable*>(o) : nullptr;   // vf0C(0x40)
            if (!d || !d->box || &d->box->box != onBox) continue;   // +0x3C, its body +0x28
            const NiPoint3 p = o->GetWorldPosition(); // vf34
            const NiPoint3 q = CharacterPoint();      // vf78
            if (p.y < q.y) {
                (void)d->Damage(100.0f);              // vf64(100.0)
                body->velocity.y = velBefore.y;
            }
            return;
        }
    }
}

void Character::Kill(Character* attacker) {           // orig: vf84 0x000357B0
    Damage(100.0f, attacker);                         // vf7C(0x42C80000, attacker)
    deathKind = 3;                                    // +0x778
}

// ---- accessors ------------------------------------------------------------------------------------------------
bool Character::IsAnimPlaying(s32 anim) const {         // orig: 0x00033E20
    const ni::NiSequence* s = SeqById(anim);
    return s && (s == curSeq || s == overlaySeq);
}

bool Character::IsIdling() const {                    // orig: vfA0 0x00035BD0 (id 8 idle9 is not tested)
    static constexpr s32 kIds[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 98, 202, 203, 228};
    for (s32 k : kIds)
        if (IsAnimPlaying(k)) return true;
    return false;
}

bool Character::IsDyingAnim() const {                 // orig: vfAC 0x00035DD0
    if (def->IsType(2))                               // a player: the level1-6 chicken walker (+0x7B0) while active
        if (const auto* p = dynamic_cast<const Player*>(this); p && p->chicken && p->chicken->active)
            return true;                              // 0x000A32A0
    static constexpr s32 kIds[] = {77, 78, 79, 80, 81, 102, 103, 198, 216};
    for (s32 k : kIds)
        if (IsAnimPlaying(k) && finished) return true;
    if (IsAnimPlaying(82) && double(animTime) - animStart > 1.5f) return true;   // orig: 0x00293BEC
    return type == 0xB && IsDead();
}

ni::NiSequence* Character::FindSequence(const char* name) const {   // orig: 0x00039BA0
    if (!model || !name) return nullptr;
    for (ni::NiSequence* s : sequences)
        if (s && std::strcmp(s->name.c_str(), name) == 0) return s;
    return nullptr;
}

// ---- the overlay slot (docs/analysis/fry_leela_port.md 1) ---------------------------------------------------------
void Character::PlayOverlay(ni::NiSequence* seq, bool loop) {   // orig: 0x00036780
    if (!keyframes) return;
    if (!seq) {                                       // 0x000367A2
        if (overlaySeq) keyframes->DeactivateSequence(overlaySeq);
        overlaySeq = nullptr;
        overlayPrev = nullptr;
        return;
    }
    if (overlaySeq == seq) return;
    if (overlaySeq) {
        keyframes->DeactivateSequence(overlaySeq);
        overlaySeq = nullptr;
    }
    overlayBlending = 0;
    SetCycleType(seq, loop);
    seq->offset = F(-double(animTime));               // FLD; FCHS; FSTP
    // (0x000367F8..0x00036852: the blend from the old overlay is dead code, +0xB0 was cleared above)
    keyframes->ActivateSequence(seq, 1);
    overlayPrev = overlaySeq;                         // (always null here)
    overlaySeq = seq;
    overlayStart = animTime;
    overlayLooping = loop ? 1 : 0;
    overlayFinished = 0;
}

void Character::ClearOverlay() {                      // orig: 0x000363C0
    if (overlaySeq && keyframes) keyframes->DeactivateSequence(overlaySeq);
    overlaySeq = nullptr;
    overlayPrev = nullptr;
}

bool Character::IsPlayingName(const char* name) const {   // orig: 0x00039D60
    const ni::NiSequence* s = FindSequence(name);
    return s && (s == curSeq || s == overlaySeq);
}

double SequenceDuration(const ni::NiSequence* s) {    // orig: 0x00036210
    if (!s) return 0.0;
    const ni::NiTimeController* rc = s->ReferenceController();
    if (!rc) return 0.0;
    return (double(rc->hiKeyTime) - rc->loKeyTime) / rc->frequency;
}

// ---- 2.11 animation control ------------------------------------------------------------------------------------
void Character::PlayAnimation(ni::NiSequence* seq, bool once, bool intr, bool force, f32 start) {   // 0x0003A130
    if (!keyframes) return;
    if (!interruptible && !force) return;
    if (seq == curSeq) {
        if (!finished) return;
        StopAnimation();
    }
    if (animLock) return;
    hitActive = 0;                                    // +0x5D1
    if (!seq) {
        StopAnimation();
    } else {
        FinishTransitions();
        SetCycleType(seq, !once);
        const f32 t = animTime;
        if (!curSeq) {
            rootKeep = blending = morphing = 0;
            seq->offset = F(double(start) - t);
            keyframes->ActivateSequence(seq, 0);
        } else {
            const AnimEntry* from = EntryOfDef(cdef, curSeq);
            const AnimEntry* to = EntryOfDef(cdef, seq);
            const char* toName = to ? to->sequence->name.c_str() : seq->name.c_str();
            const AnimTransition* tr = TransitionByName(from, toName);
            transitionTime = tr ? tr->time : 0.0f;    // orig: 0x0003F150
            seq->offset = F(double(start) - t);
            keyframes->DeactivateSequence(curSeq);
            const char* key = tr && tr->hasType ? tr->type.c_str() : nullptr;   // orig: 0x0003F0C0
            const AnimTransition* byTarget = nullptr;  // orig: 0x0003F1E0 (by target)
            if (from)
                for (const AnimTransition& x : from->transitions)
                    if (x.target == to) {
                        byTarget = &x;
                        break;
                    }
            u32 mode = byTarget ? byTarget->mode : 0;
            bool upd = false;
            if ((physic && physic->IsActive()) || pcActive || rootStarted) {
                mode = 0;
                EndRootMotion();                      // orig: 0x000369E0
                upd = true;
            }
            if (cdef.IsType(2) && HeldBy()) mode = 0;   // 0x0003A2D4: a held player (+0x7A0)
            if (cdef.IsType(2) && IsFallAnim()) mode = 0;
            if (seq->ControllerCount() != curSeq->ControllerCount()) mode = 0;
            if (type < 1 || type > 4) mode = 0;
            else if (IsAnimPlaying(215)) mode = 0;    // shake
            else if (mode == 2) {
                NotPorted("Character::PlayAnimation: morph transitions (0x000365F0)");
                mode = 0;
            }
            if (mode == 0) {
                seq->offset = F(double(start) - t);
                keyframes->ActivateSequence(seq, 0);
                if (upd) UpdateNodeAt(node.Get(), t);
            } else if (mode == 1) {
                const f32 e = F(double(transitionTime) + t);
                if (key && std::strcmp(key, "none") != 0) {
                    const std::string buf = std::string("morph:") + key;
                    keyframes->BlendFromSequence(curSeq, seq, buf.c_str(), t, e, 0);
                } else {
                    keyframes->BlendFromSequence(curSeq, seq, nullptr, t, e, 0);
                }
                blending = 1;
            } else {
                keyframes->MorphActivate(curSeq, seq, t, F(double(transitionTime) + t));
                morphing = 1;
            }
        }
        interruptible = intr;
        animStart = F(double(t) - start);
    }
    prevSeq = curSeq;
    curSeq = seq;
    finished = 0;
    looping = !once;
}

void Character::PlaySequenceDirect(ni::NiSequence* seq, bool loop) {   // orig: 0x0003A4B0
    if (!keyframes || overlaySeq) return;             // +0x558 / +0xB0
    if (!seq) {
        rootKeep = 0;                                 // +0x4CE
        blending = morphing = 0;                      // +0x93 / +0x94
        keyframes->DeactivateAll();                   // 0x00131170
        overlaySeq = curSeq = prevSeq = nullptr;      // +0xB0 / +0xA4 / +0xA8
        return;
    }
    StopAnimation();                                  // 0x00039D90 (clears curSeq on every path)
    SetCycleType(seq, loop);                          // 0x000362D0
    // 0x0003A520: curSeq is null here (StopAnimation), so the transition time of 0x0003F150 is never computed
    transitionTime = std::bit_cast<f32>(0x3DCCCCCDu);   // +0x84 = 0.1
    seq->offset = F(-double(animTime));               // FLD +0x78; FCHS; FSTP seq+0xA8
    keyframes->ActivateSequence(seq, 0);              // 0x0012F3A0
    finished = 0;                                     // +0x9B
    interruptible = 0;                                // +0x91
    curSeq = seq;                                     // +0xA4
    animStart = animTime;                             // +0x80 = +0x78
    rootKeep = 0;
    blending = morphing = 0;
    looping = loop;                                   // +0x98
    prevSeq = nullptr;                                // +0xA8
}

void Character::StopAnimation() {                     // orig: 0x00039D90
    if (physic) {
        physic->SetActive(false);
        rootStarted = rootFlags = 0;
    }
    if (blending) {
        keyframes->BlendDeactivate(prevSeq, curSeq);
        blending = 0;
        rootKeep = 0;                                 // (and +0x4CF)
    } else if (morphing) {
        keyframes->MorphDeactivate(prevSeq, curSeq);
        morphing = 0;
    } else if (summing) {
        keyframes->SumDeactivate(curSeq, sumSeq);
        summing = 0;
    } else {
        if (curSeq) keyframes->DeactivateSequence(curSeq);
        sumSeq = nullptr;
        curSeq = nullptr;
        return;
    }
    sumSeq = nullptr;
    prevSeq = curSeq;
    curSeq = nullptr;
}

void Character::FinishTransitions() {                 // orig: 0x000363F0
    if (blending) {
        keyframes->BlendDeactivate(prevSeq, curSeq);
        blending = 0;
        prevSeq = curSeq;
        keyframes->ActivateSequence(curSeq, 0);
    }
    if (morphing) {
        keyframes->MorphDeactivate(prevSeq, curSeq);
        morphing = 0;
        prevSeq = curSeq;
        keyframes->ActivateSequence(curSeq, 0);
    }
    if (summing) {
        keyframes->SumDeactivate(curSeq, sumSeq);
        prevSeq = curSeq;
        summing = 0;
        curSeq = nullptr;
    }
    sumSeq = nullptr;
}

void Character::SumBlend(ni::NiSequence* a, ni::NiSequence* b, f32 w) {   // orig: 0x00039ED0 (4.2.2)
    if (!keyframes || !a || !b) return;
    const f32 t = animTime;
    if (cdef.IsType(2) && curSeq == a && sumSeq == b && !(std::fabs(double(keyframes->SumWeight()) - w) < 0.5f))
        FinishTransitions();
    auto frequency = [&] {
        const AnimEntry* ea = EntryOfDef(cdef, a);
        const AnimEntry* eb = EntryOfDef(cdef, b);
        return F((double(eb->speed) - ea->speed) * w + ea->speed);
    };
    if (curSeq == a && sumSeq == b) {
        const f32 f = frequency();
        SetSequenceFrequency(a, f);
        SetSequenceFrequency(b, f);
        keyframes->SetSumWeight(w);
        summing = 1;
        return;
    }
    SetCycleType(a, true);
    SetCycleType(b, true);
    a->offset = F(-double(t));
    b->offset = F(-double(t));
    const f32 f = frequency();
    SetSequenceFrequency(a, f);
    SetSequenceFrequency(b, f);
    if (summing) {                                    // +0x95: the new pair starts in the old pair's phase
        PhaseSync(curSeq, a);                         // 0x00036700(+0xA4, a)
        PhaseSync(curSeq, b);                         // 0x00036700(+0xA4, b)
    }
    const bool blocked = (physic && physic->IsActive()) || pcActive || rootStarted;
    StopAnimation();
    if (cdef.IsType(2) && !blocked) {
        keyframes->SumActivate(a, b, w, F(double(t) + 0.15f));   // orig: 0x00293AA8
    } else {
        keyframes->SumActivate(a, b, w);
        if (blocked) UpdateNodeAt(node.Get(), t);
    }
    curSeq = a;
    sumSeq = b;
    summing = 1;
}

// orig: 0x00036700 (thiscall(from, to), RET 8; object_gaps.md 18.4): `to` continues at `from`'s phase when their text
// keys share a name.
void Character::PhaseSync(ni::NiSequence* from, ni::NiSequence* to) {
    if (!to || !from || !SharesTextKey(from, to)) return;   // 0x000365F0
    double x = double(from->offset) + animTime;       // FLD +0xA8; FADD +0x78 (replaced below with a controller)
    if (ni::NiTimeController* rc = from->ReferenceController()) {   // (+0x48)[+0x90]
        rc->phase = F(double(g_frameStep) + rc->phase);   // +0x18 += step (FLD [0x002D67E8]; FADD; FSTP)
        x = double(rc->accumulatedTime) / rc->frequency;  // +0x2C / +0x14 (FDIV)
    }
    if (ni::NiTimeController* rt = to->ReferenceController())
        to->offset = F(double(rt->frequency) * x - animTime);   // FLD +0x14; FMUL ST1; FSUB +0x78; FSTP +0xA8
}

void Character::FireTextKeys(ni::NiSequence* seq) {   // orig: 0x00039C50
    if (!seq) return;
    auto* tk = dynamic_cast<ni::NiTextKeyExtraData*>(seq->textKeysObj.Get());
    if (!tk || tk->keys.empty()) return;
    ni::NiTimeController* rc = seq->ReferenceController();
    const f32 inv = F(1.0 / double(rc->frequency));
    for (const auto& k : tk->keys) {
        const f32 ofs = F(-double(seq->offset));
        const f32 kt = F(double(inv) * k.time);
        const f32 dur = F((double(rc->hiKeyTime) - rc->loKeyTime) / rc->frequency);
        if (KeyCrossed(kt, animTime, ofs, dur, rc->CycleType() == 0))
            OnTextKey(F(double(inv) * k.time + ofs), k.text.c_str());   // orig: vfE8
    }
}

void Character::UpdateAnimFinished() {                // orig: 0x00036180
    auto ratio = [&](ni::NiSequence* s, f32 start) {
        const ni::NiTimeController* rc = s->ReferenceController();
        return (double(animTime) - start) / ((double(rc->hiKeyTime) - rc->loKeyTime) / rc->frequency);
    };
    if (curSeq && ratio(curSeq, animStart) >= 1.0) {  // TEST DL(1),AH; JNE: less / NaN keep
        interruptible = 1;
        finished = 1;
    }
    if (overlaySeq && ratio(overlaySeq, overlayStart) >= 1.0) overlayFinished = 1;
}

void Character::AnimTick(f32 dt) {                    // orig: 0x0003B5E0
    (void)dt;
    if (!model || !keyframes) return;
    if (!holdPose) {
        const u8 mat = body ? body->shape.contactFlags[0] : 0xFF;
        const u32 kind = level.footstepMaterials[mat & 3] & 3;
        if (hitActive) HitSpheres(1);                 // 0x0003B643: vfD8(1), the hit window's sweep
        FireTextKeys(curSeq);
        FireTextKeys(overlaySeq);
        const NiPoint3 p = GetWorldPosition();
        const bool up = IsAnimPlaying(44), down = IsAnimPlaying(45);   // ladderup / ladderdown (+0x170 / +0x174)
        const bool flag = (body && body->supported) || down || up;
        PlaySoundEvents(cdef, kind, p, curSeq, F(double(animTime) - animStart), flag, level);
        PlaySoundEvents(cdef, kind, p, overlaySeq, F(double(animTime) - overlayStart), true, level);
        if ((blending || morphing) && double(transitionTime) + animStart < double(animTime)) {   // TEST AH,5; JP
            FinishTransitions();
            rootKeep = 0;
        }
        if (overlayBlending && double(overlayTransition) + overlayStart < double(animTime)) {
            keyframes->BlendDeactivate(overlayPrev, overlaySeq);
            overlayBlending = 0;
            overlayPrev = overlaySeq;
            keyframes->ActivateSequence(overlaySeq, 0);
        }
        UpdateAnimFinished();
    }
    animLock = 0;
    holdPose = 0;
}

// ---- the melee hit window (docs/analysis/combat_ai_port.md 9.2 / 9.3) ------------------------------------------------
void Character::HitEnd() {                            // orig: vfE0 0x000360B0
    hitList.clear();                                  // (entries zeroed, +0x5C4 / +0x5C8 = 0)
    hitActive = 0;
}

void Character::HitSweep(const char* name, f32 a, f32 b, u32 n, bool box) {   // orig: 0x0003C090 (the reached path)
    (void)b;                                          // (the box query's second extent)
    if (a < 0.001f || a == 0.001f) a = 1.0f;          // FCOMP [0x00293AC4]; TEST AH,0x41; JP (NaN keeps a)
    f32 centre = 0.0f, upper = 0.0f;                  // esp+0x18 / esp+0x38 (both 0 at entry)
    NiPoint3 T;
    // 0x0003C2C5: the facing vector from vf38's chunks (columns), read by the box path; it also stays in esp+0x28,
    // the slot the wall normal (0x0003C450) and every object hit's direction (0x0003CA36) overwrite later, which the
    // health-trigger hit below reads (0x0003CD1E)
    NiPoint3 slot28;
    // +0x528 (a weapon with IsType(8); only players carry one): 0x0003C0FF..0x0003C17F -> 0x0003C318: the weapon's tip
    // (vf80), the facing vf38 * (1, 0, 0) (0x00011770) and the range 3.0
    auto* pl = dynamic_cast<Player*>(this);
    if (pl && pl->weapon && pl->weapon->def->IsType(8)) {
        T = pl->weapon->TipPosition();                // vf80 0x0007D0D0
        slot28 = ni::MulVector(GetWorldRotation(), {1.0f, 0.0f, 0.0f});   // vf38; esp+0x48 = (1, 0, 0)
        a = 3.0f;                                     // [ebp+0xC] = 0x40400000
    } else {
        ni::NiAVObject* tip = nullptr;
        if (std::memcmp(name, "tip", 4) == 0) {           // REPE CMPSB with 0x00295D98 (4 bytes)
            tip = tips.empty() ? nullptr : tips[0];       // (+0x78C)[0]
            if (!tip) tip = GetObjectByName(node.Get(), name);   // node vf54
        } else if (std::memcmp(name, "center", 7) == 0) { // REPE CMPSB with 0x00295D90 (7 bytes)
            centre = 1.0f;
        } else if (crt::StrICmp("upper", name) == 0) {    // _stricmp(0x00295D88, name)
            upper = 1.5f;
        } else {
            tip = GetObjectByName(node.Get(), name);      // "bottom", "joint14", "R_ANKLE", ... (node vf54)
        }
        if (tip) {
            T = tip->world.translate;                     // +0xA0
        } else {                                          // 0x0003C203: a point above the feet (named node missing: p)
            const f32 h = heightOffset;                   // +0x4D0
            const f32 zU = F(double(upper) * 0.0), zC = F(double(centre) * 0.0);   // FMUL [0x00293AB8]
            const f32 hU = F(double(h) * upper), hC = F(double(h) * centre);
            const NiPoint3 p = GetWorldPosition();        // vf34
            const double zz = double(zC) + p.z;           // (one x87 chain with + zU)
            const f32 y1 = F(double(hC) + p.y);
            const f32 x1 = F(double(zC) + p.x);
            T = {F(double(x1) + zU), F(double(y1) + hU), F(zz + zU)};
        }
        {                                                 // 0x0003C2C5
            const ni::NiMatrix3 R = GetWorldRotation();   // vf38
            // FLD +0x28; FADD +0x18; FMUL 0; FADD +8
            const double fz = (double(R.m[2][2]) + R.m[2][1]) * 0.0 + R.m[2][0];
            const double fy = (double(R.m[1][2]) + R.m[1][1]) * 0.0 + R.m[1][0];
            const f32 fx = F((double(R.m[0][2]) + R.m[0][1]) * 0.0 + R.m[0][0]);   // FSTP x first
            slot28 = {fx, F(fy), F(fz)};
        }
    }                                                 // (the weapon path joins at 0x0003C318)
    hitTip = T;                                       // +0x5D8
    hitRadius = a;                                    // +0x5E4
    b5D0 = 1;
    f5D4 = 0.0f;
    if (!box && !(centre == 1.0f)) {                  // FCOMP [0x00293A98]; TEST AH,0x44; JNP: "center" has no wall test
        NiPoint3 q = GetWorldPosition();              // vf34
        q.y = T.y;
        NiPoint3 d{F(double(T.x) - q.x), 0.0f, F(double(T.z) - q.z)};   // FLD 0.0 for y (0.0 * nothing)
        const double l = ni::Unitize(d);              // 0x000116A0 (ST0)
        const f32 len = F(l);                         // FST [esp+0x18]
        f32 dist = F(l + l);                          // FADD ST0,ST0; FSTP
        const ni::RayHit hit = ni::RayCast(q, d, dist, true, 3);
        if (hit && (dist < len || dist == len))       // TEST AH,0x41; JP skips (> / NaN)
            slot28 = WallSparks(hit, q, d, dist, n);  // 0x0003C405
    }
    const NiPoint3 cp = CharacterPoint();             // vf78
    const double vz = double(T.z) - cp.z;             // (FST: the unrounded z is tested)
    NiPoint3 v{F(double(T.x) - cp.x), F(double(T.y) - cp.y), F(vz)};
    f32 len = 0.0f;
    if (std::fabs(double(v.x)) < 0.01f && std::fabs(vz) < 0.01f) {   // FABS; FCOMP [0x00293AAC]; TEST AH,5; JP (x2)
        v = {};                                       // 0x002F915C (the zero vector)
    } else {
        len = F(ni::Unitize(v));                      // FSTP [esp+0x1C]
    }
    f32 s = 0.0f;
    if (0.0f < len || 0.0f == len) {                  // FLD 0.0; FCOMP len; TEST AH,0x41; JP skips (> / NaN)
        for (;;) {
            const NiPoint3 o{F(double(v.x) * s), F(double(v.y) * s), F(double(v.z) * s)};
            const NiPoint3 c = CharacterPoint();      // vf78 (again per sphere)
            const NiPoint3 S{F(double(o.x) + c.x), F(double(o.y) + c.y), F(double(o.z) + c.z)};
            f32 r = a;                                // len == 0 (TEST AH,0x44; JP computes otherwise)
            if (!(len == 0.0f)) r = F(((double(a) * 0.5f) / len) * s);
            if (box) {
                NotPorted("Character::HitSweep: the box query Physic vf06 (0x000AEBA0)");
                break;
            }
            const std::vector<phys::PhysObject*>* list = nullptr;
            if (level.physic) list = &level.physic->QueryObjects(body.get(), S, r);   // Physic vf05 0x000AEB10
            if (list && !list->empty()) {
                // the local array new(4 count) (0x0003C821, deleted at 0x0003CC0E): the owners hit by this sphere
                std::vector<GameObject*> hit;
                for (phys::PhysObject* po : *list) {
                    if (!po || !po->owner) continue;  // vf24
                    if (std::find(hitList.begin(), hitList.end(), po) != hitList.end()) continue;
                    hit.push_back(po->owner);
                    hitList.push_back(po);            // (+0x5B8 SetAtGrow at the count)
                }
                for (GameObject* w : hit) {
                    if (!w) continue;
                    const bool isChar = w->def->IsType(1);
                    if (isChar && static_cast<Character*>(w)->IsDead()) continue;   // vfA8
                    if (!(n & 1) && w->def->IsType(0x40)) continue;
                    const NiPoint3 me = GetWorldPosition(), wp = w->GetWorldPosition();   // vf34 (this first)
                    const double dz = double(wp.z) - me.z;
                    const NiPoint3 d{F(double(wp.x) - me.x), F(double(wp.y) - me.y), F(dz)};
                    const double h = std::sqrt(double(d.x) * d.x + dz * dz);   // (the z term unrounded)
                    NiPoint3 dir{};
                    if (h > double(9.999999974752427e-07f)) {   // [0x00293ACC]; TEST AH,0x41; JNE
                        const double inv = 1.0 / h;
                        dir = {F(double(d.x) * inv), F(0.0 * inv), F(double(d.z) * inv)};
                    }
                    ApplyHit(w, dir, n);              // vfD4
                    slot28 = dir;                     // (dir is esp+0x28)
                    if (w->def->IsType(0x40)) {       // 0x0003CAD9
                        if (Def* sd = level.defs ? level.defs->Find("Smack_Impact") : nullptr) {   // 0x00295D78
                            const f32 fy = F(double(dir.y) * 0.1f), fx = F(double(dir.x) * 0.1f);   // [0x00293AB0]
                            const NiPoint3 P{F(double(fx) + S.x), F(double(fy) + S.y), F(double(dir.z) * 0.1f + S.z)};
                            // owner: S+0x5C ? S+0x5C + 4 : 0; FindDef again; 0x0004EE90, then 0x00073720 (no null check)
                            if (auto* idef = dynamic_cast<ImpactDef*>(sd))
                                SetNodeScale(*idef->SpawnUp(level, level.currentStage, P, dir, "SMACK!!!"), 1.0f);
                        }
                    } else if (w->def->IsType(1) && level.defs && level.defs->Find("Smack_Impact")) {   // 0x0003CB7A
                        // the billboard spark at the sphere centre (its rand() % 100 at 0x0004F6C2)
                        if (auto* idef = dynamic_cast<ImpactDef*>(level.defs->Find("Smack_Impact")))
                            SetNodeScale(*idef->SpawnBillboard(level, level.currentStage, S, "SMACK!!!"), 1.0f);
                    }
                }
            }
            if (level.scriptTriggers) HitHealthTriggers(S, r, T, slot28);   // 0x0003CC16
            if (s >= len) break;                      // TEST AH,1; JE (NaN goes on)
            const double s2 = double(a) * 0.5f + s;
            s = !(s2 >= len) ? F(s2) : len;           // TEST AH,1; JNE
            if (!(s < len || s == len)) break;        // TEST AH,0x41; JNP
        }
    }
    if (!hitActive) hitList.clear();                  // (the one-shot "morph:hit" callers)
}

// orig: 0x0003CC16..0x0003CDBE (docs/analysis/object_gaps.md 21.2).
void Character::HitHealthTriggers(const NiPoint3& S, f32 r, const NiPoint3& T, NiPoint3& dir) {
    for (auto& tp : level.scriptTriggers->list) {     // DAT_002EA000 (newest first; link +4)
        ScriptTrigger* t = tp.get();
        if (!t || t->kind != ScriptTrigger::Health) continue;   // +8 == 8
        if (!(def->IsType(2) ? t->flag65 : t->flag64)) continue;
        if (std::find(hitList.begin(), hitList.end(), static_cast<const void*>(t)) != hitList.end()) continue;
        if (!HealthContainsPoint(*t, S, r)) continue; // 0x000B86F0(&S, r)
        hitList.push_back(t);                         // SetAtGrow(+0x5C4, t)
        HitTrigger(*t, dir);                          // vfD0(t, &dir)
        if (!level.defs || !level.defs->Find("Smack_Impact")) continue;   // 0x00295D78
        const double px = double(dir.x) * 0.1f, py = double(dir.y) * 0.1f;   // [0x00293AB0]
        const f32 pz = F(double(dir.z) * 0.1f);       // FSTP first
        const NiPoint3 P{F(px + T.x), F(py + T.y), F(double(pz) + T.z)};
        // owner: S+0x5C ? S+0x5C + 4 : 0; FindDef again; 0x0004EE90 (no null check), then 0x00073720(1.0)
        if (auto* idef = dynamic_cast<ImpactDef*>(level.defs->Find("Smack_Impact")))
            SetNodeScale(*idef->SpawnUp(level, level.currentStage, P, dir, "SMACK!!!"), 1.0f);   // 0x00295D6C
    }
}

// orig: 0x0003C405..0x0003C5EF (docs/analysis/player_moves_port.md 6.3).
NiPoint3 Character::WallSparks(const ni::RayHit& hit, const NiPoint3& q, const NiPoint3& d, f32 dist, u32 n) {
    const NiPoint3 P{F(double(F(double(d.x) * dist)) + q.x), F(double(d.y) * dist + q.y),
                     F(double(q.z) + double(d.z) * dist)};   // esp+0x78 / +0x18 / +0x24
    NiPoint3 N = hit.Normal();                        // BspTriangle::Normal 0x000FA8C0
    if (level.defs && level.defs->Find("Smack_Impact")) {   // Session::FindDef 0x000C3F10 (0x00295D78)
        const f32 ny = F(double(N.y) * 0.5f), nx = F(double(N.x) * 0.5f);   // [0x00293AA0]
        const NiPoint3 S{F(double(nx) + P.x), F(double(ny) + P.y), F(double(N.z) * 0.5f + P.z)};
        // owner: S+0x5C ? S+0x5C + 4 : 0 (the current stage's Owner part); FindDef again, then 0x0004EE90
        if (auto* idef = dynamic_cast<ImpactDef*>(level.defs->Find("Smack_Impact"))) {
            Impact* o = idef->SpawnUp(level, level.currentStage, S, N, "SMACK!!!");   // 0x00295D6C
            SetNodeScale(*o, 1.0f);                   // 0x00073720 (no null check)
            ni::NiMatrix3 R = o->GetWorldRotation();  // vf38 = slot 14 0x0004F020 (node +0x70)
            const s32 r = crt::Rand() % 100;          // rand 0x0003C53D; CDQ; IDIV 100
            const f32 a = F(double(r) * 0.01f * 180.0f);   // FILD; FMUL [0x00293AAC]; FMUL [0x00295D68]
            R = ni::Multiply(R, ni::MakeZRotation(a));   // 0x001030F0, 0x00013B30
            o->SetRotation(R);                        // vf24
        }
    }
    ApplyHit(nullptr, N, n);                          // vfD4(0, &N, n), also without the definition
    if (def->IsType(2)) static_cast<Player*>(this)->rumble.Add(0.5f, 0.5f, 0.2f, 0);   // players: 0x0005F000
    return N;                                         // (N lives in HitSweep's esp+0x28)
}

void Character::TipToKeyTime(f32 t, ni::NiAVObject* tip) {   // orig: 0x00034A10
    if (tip) {
        bool selective = true, rigid = false;         // the bytes at esp+7 (1) and esp+0x10 (0)
        tip->SetSelectiveUpdateFlags(selective, true, rigid);   // vf58
    }
    node->UpdateSelected(t);                          // NiAVObject::UpdateSelected 0x00101900
}

AnimEntry* Character::EntryOf(const ni::NiSequence* seq) const { return seq ? EntryOfDef(cdef, seq) : nullptr; }
void Character::SetFrequency(ni::NiSequence* seq, f32 f) { SetSequenceFrequency(seq, f); }

// ---- root motion ---------------------------------------------------------------------------------------------------
void Character::BeginRootMotion() {                   // orig: 0x000368A0
    if (!physic) return;
    physic->SetActive(true);
    physic->keepY = 0;
    rootStarted = 1;
    ni::NiAVObject* bone = physic->bone;
    UpdateNodeAt(node.Get(), animTime);
    // the root bone's offset in its parent's space: world scale * (world rotate * local translate) (0x00036910)
    const ni::NiNode* parent = bone->parent;
    const ni::NiMatrix3& M = parent->world.rotate;
    const NiPoint3& b = bone->local.translate;
    const double zs = (double(M.m[2][0]) * b.x + double(M.m[2][2]) * b.z) + double(M.m[2][1]) * b.y;
    const f32 ys = F((double(M.m[1][0]) * b.x + double(M.m[1][2]) * b.z) + double(M.m[1][1]) * b.y);
    const f32 xs = F((double(M.m[0][2]) * b.z + double(M.m[0][1]) * b.y) + double(b.x) * M.m[0][0]);
    const double s = parent->world.scale;
    rootPrev = {F(double(xs) * s), F(double(ys) * s), F(zs * s)};
}

void Character::EndRootMotion() {                     // orig: 0x000369E0
    if (!physic) return;
    physic->SetActive(false);                         // (after def->IsType(2), result unused)
    rootStarted = rootFlags = 0;
}

bool Character::VoiceLive(u32 v) const { return v && level.Sound().VoiceExists(static_cast<s32>(v)); }

void Character::StopVoiceAndClear(u32& v) {
    if (!VoiceLive(v)) return;
    level.Sound().StopVoice(static_cast<s32>(v));     // 0x000C9B10
    v = 0;
}

void Character::StopVoiceIfExists(u32& v, bool clear) {
    if (!level.Sound().VoiceExists(static_cast<s32>(v))) return;
    level.Sound().StopVoice(static_cast<s32>(v));
    if (clear) v = 0;
}

void Character::VoiceMaxDistance(s32 v, f32 d) { level.Sound().SetMaxDistance(v, d); }

s32 Character::SoundAt(u32 sound, const NiPoint3* pos, u32 flags, u32 index, ni::NiAVObject* attach) {
    // orig: CharacterDef 0x0003FEA0 (docs/analysis/root_motion_port.md 3): the voice id of the started sound, 0 when
    // nothing played.
    u32 count = 0;
    for (const CharacterDef::SoundEntry& e : cdef.sounds)   // 0x0003FED0 (null records skipped)
        if (e.id == sound) ++count;
    if (count == 0) return 0;                         // 0x0003FEEE: no rand()
    u32 pick = index;                                 // 0x0003FEBD (arg 4)
    if (flags & 2) pick = static_cast<u32>(crt::Rand() % static_cast<s32>(count));   // CDQ; IDIV
    // (0x0003FEC1 / 0x0003FF0C: the loop flag goes out in the low byte of arg 4's slot; every caller passes index 0)
    const bool loop = (flags & 4) != 0;
    sound::GameSound& snd = level.Sound();
    u32 k = 0;
    for (const CharacterDef::SoundEntry& e : cdef.sounds) {   // 0x0003FF25
        if (e.id != sound) continue;
        if (k != pick) {
            ++k;
            continue;
        }
        if (attach) return snd.PlayAttached(e.sound, attach, loop);   // 0x0003FF6B Sound_PlayAttached 0x000CAB80
        if (!pos) return snd.PlaySfx(e.sound, loop);                  // 0x0003FF8D Sound_PlaySFX 0x000C99A0
        return snd.Play3D(e.sound, *pos, loop);                       // 0x0003FFA8 Sound_Play3D 0x000C9AC0
    }
    return 0;                                         // (index >= count)
}

// ---- 2.12 render-time pose ------------------------------------------------------------------------------------
bool Character::WalkRunPlaying() const {             // orig: 0x00035AE0
    for (s32 anim : {13, 14, 15, 16, 17, 21, 22, 23})
        if (IsAnimPlaying(anim)) return true;
    return false;
}

bool Player::WalkRunPlaying() const {                // orig: vf9C 0x0004FA80
    if (Character::WalkRunPlaying()) return true;     // 0x00035AE0
    for (s32 anim : {0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97})
        if (IsAnimPlaying(anim)) return true;
    return false;
}

bool Character::ShouldUnloadModel() const {          // orig: 0x00035020
    if (!cdef.IsType(2) && type != 0xE && type != 0xB && type != 0xC &&
        double(updateDistance) - 1.0f < cameraDistance)   // FLD +0x64; FSUB 1.0; FCOMP +0x500; TEST AH,5; JNP
        return true;
    return !(deathFade < 1.0f);                       // TEST AH,1; JNE
}

void Character::DeleteInventoryEffects() {           // orig: 0x00034A50
    for (GameObject* o = inventory.NextObject(nullptr); o;) {
        GameObject* next = inventory.NextObject(o);   // (read before the deletion)
        if (o->def->IsType(0x8000)) delete o;         // vf00(1)
        o = next;
    }
}

void Character::AddModelCollisionMeshes() {           // orig: 0x000350B0
    if (!model || !owner->IsOwnerType(2)) return;     // +0x564; owner vf0C(2) (+4: no null test)
    ni::NiBsp* b = static_cast<Stage*>(owner)->bsp.Get();   // (+4 ? +4 - 4 : 0)->+0x24
    if (!b) return;
    const char* n = def->name.c_str();                // D+0x10
    auto add = [&](const char* child, u32 fl) {       // model vf54 (GetObjectByName); 0x000FC110 (a null part: nothing)
        ni::AddCollisionMeshes(*b, GetObjectByName(model.Get(), child), fl);
    };
    if (crt::StrICmp(n, "patrol_guard") == 0) {       // 0x00295A78
        add("spikes", 6);                             // 0x00295A70
        add("blades", 0xC6);                          // 0x00295A68
        add("bodyShape", 0);                          // 0x00295A4C
        add("head", 0x80);                            // 0x00295A44
    } else if (crt::StrICmp(n, "spinbot") == 0) {     // 0x00295A60
        add("Spikes", 6);                             // 0x00295A58 (capital S)
        add("blades", 0xC6);
        add("bodyShape", 0x80);
        add("head", 0x80);
    } else if (crt::StrICmp(n, "chickenwalker") == 0) {   // 0x00295A34: nothing
    } else if (crt::StrICmp(n, "tentacle_dude") == 0) {   // 0x00295A24
        ni::AddCollisionMeshes(*b, model.Get(), 0x80);
    } else {
        ni::AddCollisionMeshes(*b, model.Get(), 8);
    }
}

void Character::OnAttach() {                          // orig: vf3C 0x00035270
    if (flag60) AddModelCollisionMeshes();            // byte +0x60; jmp 0x000350B0
}

void Character::OnDetach() {                          // orig: vf40 0x00035280
    // byte +0x60, +0x564, owner vf0C(2), (+4 ? +4 - 4 : 0)->+0x24, 0x000FC2D0(+0x564): RemoveModelCollisionMeshes' body
    if (flag60 && model) RemoveModelCollisionMeshes();
}

bool Character::OnGround() const {                    // orig: vf70 0x00033D10
    return body && body->supported;                   // body vf48 0x000AF1F0 (byte +0x45); no body: 0
}

void Character::RemoveModelCollisionMeshes() {        // orig: 0x00035230
    if (!model || !owner->IsOwnerType(2)) return;     // (+4: no null test)
    if (ni::NiBsp* b = static_cast<Stage*>(owner)->bsp.Get()) ni::RemoveCollisionMeshes(*b, model.Get());   // 0x000FC2D0
}

// docs/analysis/level2-2_port.md 5.6: the model goes back under the definition's model root (AcquireModel reuses it),
// the current animation's name and offset are kept for BuildModel's restore.
void Character::UnloadModel() {                       // orig: vf68 0x0003B7D0
    if (!model) return;                               // 0x0003B7DD -> 0x0003B94E
    DeleteInventoryEffects();                         // 0x00034A50
    tips.clear();                                     // 0x00034190(+0x788): released, size 0
    if (flag60) RemoveModelCollisionMeshes();         // 0x0003B7FC: 0x00035230
    if (cdef.model) cdef.model->AttachChild(model.Get(), true);   // 0x0003F6D0: (D+0x20)->vfA0(model, 1) (no null check)
    if (envEffect) {                                  // +0x5A4
        if (auto* m = dynamic_cast<ni::NiNode*>(model.Get())) envEffect->DetachAffectedNode(m);   // 0x001419F0
        envEffect = nullptr;
    }
    if (curSeq) {
        animName = curSeq->name;                      // strcpy(+0x66C, seq+0x08)
        animOffset = curSeq->offset;                  // +0x668 = seq+0xA8 (FLD; FSTP)
    } else {
        animName = "none";                            // 0x00295D28 (+0x668 unchanged)
    }
    StopAnimation();                                  // 0x00039D90
    if (aim55C) {                                     // +0x55C
        node->RemoveController(aim55C.Get());         // 0x000FE4C0
        aim55C = nullptr;
    }
    keyframes->RemoveAllSequences();                  // 0x00131010
    sequences.clear();                                // 0x00034190(+0x52C) and the name map 0x0003B570(+0x544)
    sequenceRefs.clear();
    byAnimation.fill(nullptr);                        // +0xC0, 0x100 dwords
    model->flags |= ni::kNiAVHidden;                  // model+0xB0 |= 1
    model = nullptr;
    node->UpdateProperties();                         // 0x0003B933
    node->UpdateEffects();
    UpdateNodeAt(node.Get(), animTime);               // NiAVObject::Update(+0x78, 1) 0x001018D0
}

void Character::SetScale(f32 s) {                     // orig: 0x00035080
    heightOffset = F(double(s) / node->local.scale * heightOffset);   // FLD s; FDIV [node+0x6C]; FMUL [+0x4D0]; FSTP
    node->local.scale = F(std::fabs(double(s)));      // FLD s; FABS; FSTP [node+0x6C]
}

NiPoint3 Character::CharacterPoint() const {          // orig: 0x0003D7C0
    const f32 h = heightOffset;                       // FLD +0x4D0; FSTP local
    const NiPoint3 p = GetWorldPosition();            // vf34
    return {p.x, F(double(h) + p.y), p.z};
}

// orig: 0x000360F0 (cdecl(seq, name), ST0): the time of the sequence's text key `name` (_stricmp) times its reference
// controller's frequency (read before the loop), unrounded; -1.0 ([0x00293BF0]) without the sequence, its text keys
// (+0x8C, the array +0x18) or the key.
static double TextKeyTime(const ni::NiSequence* seq, const char* name) {
    if (!seq) return -1.0f;
    const auto* tk = dynamic_cast<const ni::NiTextKeyExtraData*>(seq->textKeysObj.Get());
    if (!tk || tk->keys.empty()) return -1.0f;
    const ni::NiTimeController* rc = seq->ReferenceController();   // (+0x48)[+0x90]->+0x14 (no null test)
    const f32 freq = rc ? rc->frequency : 0.0f;       // (port guard)
    for (const auto& k : tk->keys)                    // (+0x14 count, {f32 time, char* text} pairs)
        if (crt::StrICmp(k.text.c_str(), name) == 0) return double(k.time) * freq;   // FLD time; FMUL freq
    return -1.0f;
}

void Character::HoldAnimation(const char* key, f32 b) {   // orig: 0x000364C0
    if (!keyframes) return;                           // +0x558
    if (key) b = F(TextKeyTime(curSeq, key) + b);     // 0x000360F0(+0xA4, key); FADD [b]; FSTP [b]
    animStart = F(double(g_frameStep) + animStart);   // +0x80
    transitionTime = F(double(g_frameStep) + transitionTime);   // +0x84
    ni::NiSequence* other = nullptr;
    if (blending || morphing) {                       // +0x93 / +0x94
        other = prevSeq;                              // +0xA8
    } else if (summing) {                             // +0x95
        if (key) {                                    // 0x0003653D (the current sequence is not tested for null)
            if (curSeq) curSeq->offset = F(double(b) - animTime);   // (port guard)
            if (sumSeq) sumSeq->offset = F(double(b) - animTime);   // +0xAC (port guard)
            animStart = F(double(animTime) - b);
            holdPose = 1;
            return;
        }
        other = sumSeq;                               // +0xAC
    } else if (!curSeq) {
        holdPose = 1;
        return;
    } else if (key) {                                 // 0x000365C6
        curSeq->offset = F(double(b) - animTime);     // +0xA8 of +0xA4
        animStart = F(double(animTime) - b);
        holdPose = 1;
        return;
    }
    if (other) other->offset = F(double(other->offset) - g_frameStep);
    if (curSeq) curSeq->offset = F(double(curSeq->offset) - g_frameStep);   // (no null test in the original)
    holdPose = 1;                                     // +0x9E
}

void Character::PrepareRender() {                     // orig: 0x00039890
    renderTime = animTime;
    if (ShouldUnloadModel()) UnloadModel();           // orig: vf6C / vf68
    else BuildModel();                                // orig: vf64 (returns at once once built)
    // orig: 0x000398CB: a non-player whose node was not drawn last frame (+0xB2 != 0) skips the subtree update below
    const bool culled = !def->IsType(2) && node->framesNotDrawn != 0;   // D vf0C(2); byte node+0xB2
    if (lodStep >= 1.0f) {                         // FCOMP [0x00293A98]; TEST AH,1; JNE (< / NaN: the else)
        // NOT PORTED: the NPC death fade (alpha 1 - deathFade).
        if (physic && rootKeep) physic->SetActive(true);
        const bool active = physic && physic->IsActive();
        if (active) physic->byte6C = 1;
        UpdateNodeAt(node.Get(), animTime);
        if (active) physic->byte6C = 0;
        lodStep = 0;                                  // +0x70 (0x00039AF5)
        for (GameObject* o = inventory.NextObject(nullptr); o;) {   // orig: 0x00039AF8 (the next fetched first)
            GameObject* next = inventory.NextObject(o);
            if (o->def->IsType(0x8000) && !o->PostUpdate()) delete o;   // vf0C; vf00(1)
            o = next;
        }
    } else if (culled || IsIdling()) {                // orig: 0x00039B39 (vfA0); only the node itself
        node->UpdateWorldData();                      // orig: vf9C
        node->UpdateWorldBound();                     // orig: vf98
    } else {
        UpdateNodeAt(node.Get(), animTime);
    }
    cameraDistance = F(double(updateDistance) - 1.0f);
}

}  // namespace fut::game
