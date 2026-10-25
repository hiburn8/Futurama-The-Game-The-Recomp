// Level animations; see LevelAnimation.h. Floating point follows the original instruction by instruction
// (NiMath.h): x87 values are `double`, F() marks every store to a float.
#include "game/level/LevelAnimation.h"

#include <cstdlib>
#include <cstring>

namespace fut::level {
namespace {

using ni::detail::F;

int StrICmp(const char* a, const char* b) {          // orig: _stricmp (0x001684D1), ASCII folding
    for (;; ++a, ++b) {
        int ca = static_cast<unsigned char>(*a), cb = static_cast<unsigned char>(*b);
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca) return ca - cb;
    }
}

f32 Bits(u32 b) {
    f32 f;
    std::memcpy(&f, &b, 4);
    return f;
}
const f32 kPoseOffset = Bits(0x38D1B717);             // orig: 0x00293B1C, 1e-4f

// The pose time: just below t, so a clip's end key is not wrapped to its start (orig: inline in 5 places).
f32 PoseTime(f32 t) { return t > kPoseOffset ? F(double(t) - kPoseOffset) : t; }

// orig: NiAVObject::Update (0x001018D0) on the target, then the world bounds of its ancestors up to the room.
void PoseTarget(ni::NiAVObject* target, f32 t, bool boundsUp) {
    target->UpdateDownwardPass(t, true);
    if (!boundsUp) return;
    for (ni::NiNode* p = target->parent; p; p = p->parent) {
        if (dynamic_cast<ni::NiRoom*>(p)) break;
        p->UpdateWorldBound();
    }
}

}  // namespace

// ---- clips / text keys --------------------------------------------------------------------------------------------
void SetSequenceCycleType(ni::NiSequence* seq, bool loop) {   // orig: 0x000BA3C0
    if (!seq) return;
    const u32 want = loop ? 0 : 2;                    // LOOP / CLAMP
    for (u32 i = 0; i < seq->ControllerCount(); ++i) {
        ni::NiTimeController* c = seq->Controller(i);
        if (!c) continue;
        if (c->CycleType() == want) return;           // (the whole function, as the original)
        c->flags = static_cast<u16>((c->flags & 0xFFF9) | (want << 1));
    }
}

void FireTextKeys(ni::NiSequence* seq, f32 a, f32 b, AnimTextKeyCounters& counters) {   // orig: 0x000BAC00
    auto* tk = dynamic_cast<ni::NiTextKeyExtraData*>(seq->textKeysObj.Get());
    if (!tk || tk->keys.empty()) return;
    ni::NiTimeController* c = seq->ReferenceController();
    if (!c) return;
    if (a > b) std::swap(a, b);
    const f32 lo = F(c->ComputeScaledTime(a));
    const f32 hi = F(c->ComputeScaledTime(b));
    const bool wrap = !(lo <= hi);
    for (const auto& k : tk->keys) {
        const bool fire = wrap ? (k.time > lo || k.time <= hi) : (k.time > lo && k.time <= hi);
        if (!fire) continue;                          // orig: OnTextKey 0x000B0A90
        if (StrICmp("morph:barrel", k.text.c_str()) == 0) {
            ++counters.barrels;
        } else if (StrICmp("morph:beams", k.text.c_str()) == 0) {
            ++counters.beams;
        }
    }
}

// ---- LevelAnimation -----------------------------------------------------------------------------------------------
LevelAnimation::LevelAnimation(const std::string& animName) : name(animName) {}

LevelAnimation::~LevelAnimation() {                   // orig: 0x000BB0B0
    // (the list unlink is LevelAnimations::DeleteAll's) the clips in index order: the sequence out of the manager
    // (when there is one) and released, the clip deleted; then the name, +0 = 0, the target +0x10, the manager +0x14
    for (auto& c : clips) {
        if (c->sequence) {
            if (manager) manager->RemoveSequence(c->sequence.Get());
            c->sequence = nullptr;
        }
        c.reset();
    }
    clips.clear();
    owner = nullptr;
    target = nullptr;
    manager = nullptr;
}

LevelAnimClip* LevelAnimation::AddClip(const std::string& clipName, f32 duration) {   // orig: 0x000BB400
    auto clip = std::make_unique<LevelAnimClip>();
    clip->name = clipName;
    clip->duration = duration;
    clips.push_back(std::move(clip));
    return clips.back().get();
}

LevelAnimClip* LevelAnimation::FindClip(const std::string& clipName) {   // orig: 0x000BAEB0
    for (auto& c : clips)
        if (StrICmp(c->name.c_str(), clipName.c_str()) == 0) return c.get();
    return nullptr;
}

void LevelAnimation::SetClipSequence(LevelAnimClip& clip, ni::NiSequence* seq) {   // orig: 0x000BA570
    clip.sequence = seq;
    if (manager) manager->AddSequence(seq, true);
}

void LevelAnimation::Bind(const void* stage, ni::NiAVObject* node) {   // orig: 0x000BA710
    owner = stage;
    target = node;
}

void LevelAnimation::Unbind(const void* stage) {      // orig: 0x000BA750
    if (owner != stage) return;
    for (auto& c : clips) {
        if (!c->sequence) continue;
        if (manager) manager->RemoveSequence(c->sequence.Get());
        c->sequence = nullptr;
    }
    owner = nullptr;
    target = nullptr;
    manager = nullptr;
}

const char* LevelAnimation::CurrentClipName() const {   // orig: 0x000BA7F0
    return current == -1 ? nullptr : clips[current]->name.c_str();
}

f32 LevelAnimation::CurrentClipDuration() const {     // orig: 0x000BA810
    return current == -1 ? -1.0f : clips[current]->duration;
}

void LevelAnimation::EnsureManager() {
    manager = new ni::NiKeyframeManager(target.Get());
    for (auto& c : clips)
        if (c->sequence) manager->AddSequence(c->sequence.Get(), true);
}

void LevelAnimation::Play(const char* clipName, bool loopIt, bool reverseIt) {   // orig: 0x000BB220
    if (playing) {
        if (manager) manager->DeactivateAll();
        playing = false;
    }
    s32 idx = 0;
    LevelAnimClip* c = nullptr;
    if (clipName) {
        const s32 n = static_cast<s32>(clips.size());
        if (n <= 0) return;
        for (;; ++idx) {
            if (idx >= n) return;
            if (StrICmp(clips[idx]->name.c_str(), clipName) == 0) break;
        }
        c = clips[idx].get();
        if (c->duration == 0.0f && loopIt) return;    // a zero-length clip cannot loop (NaN plays)
    } else {
        if (clips.empty()) return;
        c = clips[0].get();
    }
    if (target && !manager) EnsureManager();
    if (manager && c->sequence) {
        manager->DeactivateAll();
        c->sequence->offset = 0;
        manager->ActivateSequence(c->sequence.Get(), 0);
        c->sequence->offset = 0;
    }
    current = idx;
    playing = true;
    loop = loopIt;
    if (c->sequence) SetSequenceCycleType(c->sequence.Get(), loopIt);
    reverse = reverseIt;
    time = reverseIt ? CurrentClipDuration() : 0.0f;
}

void LevelAnimation::Stop() {                         // orig: 0x000BA830
    if (!playing) return;
    if (manager) manager->DeactivateAll();
    playing = false;
}

void LevelAnimation::Pause() {                        // orig: 0x000BA2C0
    if (playing) playing = false;
}

void LevelAnimation::Resume(bool reverseIt) {         // orig: 0x000BA2D0
    if (playing || current == -1) return;
    reverse = reverseIt;
    playing = true;
}

void LevelAnimation::SkipToEnd() {                    // orig: 0x000BA850
    if (!playing) return;
    const f32 d = CurrentClipDuration();
    time = d;
    if (target) PoseTarget(target.Get(), PoseTime(d), true);
    reverse = playing = false;
    dirty = true;
    culled = false;
    current = -1;
    if (manager) manager->DeactivateAll();
}

void LevelAnimation::Rewind() {                       // orig: 0x000BA910 (unconditional)
    time = 0;
    if (target) PoseTarget(target.Get(), 0.0f, true);
    reverse = playing = culled = false;
    dirty = true;
    current = -1;
    if (manager) manager->DeactivateAll();
}

void LevelAnimation::Advance(f32 dt, AnimTextKeyCounters& counters) {   // orig: 0x000BACF0
    if (current == -1 || culled) return;
    if (target && !manager) {
        EnsureManager();
        ni::NiSequence* s = clips[current]->sequence.Get();
        if (!s) {                                     // a clip without a sequence cancels the animation
            current = -1;
            return;
        }
        manager->ActivateSequence(s, 0);
        s->offset = 0;
        if (!playing) {
            PoseTarget(target.Get(), PoseTime(time), false);
            return;
        }
    }
    if (!playing) return;
    const f32 prev = time;
    dirty = true;
    time = reverse ? F(double(time) - dt) : F(double(dt) + time);
    if (ni::NiSequence* s = clips[current]->sequence.Get()) FireTextKeys(s, prev, time, counters);
}

void LevelAnimation::ApplyPose(const AnimCullCamera* camera) {   // orig: 0x000BA990
    if (!dirty || current == -1) return;
    const bool wasCulled = culled;
    culled = false;
    if (autoPlay && target && camera) {               // (g_cullAutoAnims 0x002D683C is always 1)
        const ni::NiBound& wb = target->worldBound;
        const f32 dx = F(double(wb.center.x) - camera->position.x);
        const f32 dy = F(double(wb.center.y) - camera->position.y);
        const f32 dz = F(double(wb.center.z) - camera->position.z);
        const ni::NiPoint3& d = camera->direction;
        const double dot = (double(d.z) * dz + double(d.y) * dy) + double(dx) * d.x;
        const f32 r = wb.radius;
        bool cull;
        if (-double(r) > dot) {                       // behind the camera
            cull = true;
        } else {                                      // or farther than radius + 80
            const f32 rr = F(double(r) + 80.0);
            const double d2 = (double(dx) * dx + double(dy) * dy) + double(dz) * dz;   // orig: 0x00011680
            cull = double(rr) * rr < d2;
        }
        if (cull) {
            dirty = true;
            culled = true;
            if (wasCulled) return;                    // one last pose on the frame it becomes culled
        }
    }
    dirty = culled;
    const f32 dur = clips[current]->duration;
    if (dur < 0.0f) {
        playing = false;
        return;
    }
    if (loop && dur != 0.0f) {
        if (!(time < dur)) {
            double x = time;
            do x -= dur;
            while (!(x < dur));
            time = F(x);
        }
        if (time < 0.0f) {
            double x = time;
            do x += dur;
            while (x < 0.0);
            time = F(x);
        }
    } else if (reverse) {
        if (time < 0.0f) {
            time = 0.0f;
            playing = false;
        }
    } else if (!(time < dur)) {
        time = dur;
        playing = false;
    }
    if (target) PoseTarget(target.Get(), PoseTime(time), true);
}

// ---- the list -----------------------------------------------------------------------------------------------------
LevelAnimation* LevelAnimations::CreateFromNode(ni::NiAVObject& node) {   // orig: 0x000BB490
    list.push_back(std::make_unique<LevelAnimation>(node.name));
    LevelAnimation* la = list.back().get();
    auto* first = dynamic_cast<ni::NiStringExtraData*>(node.extraData.Get());
    if (!first) return la;                            // (the original dereferences it unchecked)
    la->autoPlay = StrICmp(first->string.c_str(), "Auto") == 0;
    for (ni::NiExtraData* e = first->nextData.Get(); e; e = e->nextData.Get()) {
        auto* str = dynamic_cast<ni::NiStringExtraData*>(e);
        if (!str) continue;
        const size_t comma = str->string.find(',');
        if (comma == std::string::npos) continue;     // (the original writes through a null strchr result)
        la->AddClip(str->string.substr(0, comma),
                    static_cast<f32>(std::strtod(str->string.c_str() + comma + 1, nullptr)));   // (float)atof
    }
    if (la->autoPlay) la->Play(nullptr, true, false);
    return la;
}

LevelAnimation* LevelAnimations::FindByName(const std::string& animName) const {   // orig: 0x000BA5B0
    for (const auto& la : list)
        if (StrICmp(la->name.c_str(), animName.c_str()) == 0) return la.get();
    return nullptr;
}

void LevelAnimations::DeleteAll() {                   // orig: the Session dtor's 0x000C84F0 loop
    while (!list.empty()) {                           // the head deleted (0x000BB0B0 unlinks it) until none is left
        std::unique_ptr<LevelAnimation> la = std::move(list.front());
        list.erase(list.begin());
        la.reset();
    }
}

void LevelAnimations::UnbindAll(const void* stage) {  // orig: 0x000BB080
    for (auto& la : list) la->Unbind(stage);
}

void LevelAnimations::Advance(f32 dt) {
    for (auto& la : list) la->Advance(dt, counters);
}

void LevelAnimations::ApplyPose(const AnimCullCamera* camera) {
    for (auto& la : list) la->ApplyPose(camera);
}

}  // namespace fut::level
