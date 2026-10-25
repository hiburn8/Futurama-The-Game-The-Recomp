// Level animations: named keyframe animations of level geometry (doors, lifts, switches, machinery ...), declared by
// the children of the "Animations" node of defs.nif and played by scripts, triggers and "Auto" start. Decompiled from
// default.xbe 0x000BA2A0-0x000BB590 (structure: docs/analysis/level_animation.md). Each animation owns a
// NiKeyframeManager on its target node and a list of clips (a sequence + a duration); the stage stream binds the
// target and attaches the sequences (Stage::vf00 0x000D1410).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiKeyframeManager.h"

#include <memory>
#include <string>
#include <vector>

namespace fut::ni {
class NiNode;
}

namespace fut::level {

class LevelAnimations;

// orig: 0x10-byte clip record (ctor 0x000BA440, dtor 0x000BA4E0).
struct LevelAnimClip {
    std::string name;                                // orig: +0x00, the NiSequence name it binds to
    ni::NiPointer<ni::NiSequence> sequence;          // orig: +0x08, attached by the stage stream
    f32 duration = 0;                                // orig: +0x0C seconds; < 0 disables, 0 cannot loop
};

// The camera ApplyPose culls auto animations against (orig: level +0x8C: position +0x48, NiCamera +0x30 world
// direction +0x110).
struct AnimCullCamera {
    ni::NiPoint3 position;
    ni::NiPoint3 direction;
};

class LevelAnimation {                               // orig: 0x44 bytes, no vtable
public:
    explicit LevelAnimation(const std::string& name);   // orig: 0x000BA5F0 (the list append is LevelAnimations::Add)
    ~LevelAnimation();                               // orig: 0x000BB0B0 (minus the list unlink)

    LevelAnimClip* AddClip(const std::string& name, f32 duration);   // orig: 0x000BB400
    LevelAnimClip* FindClip(const std::string& name);                // orig: 0x000BAEB0 (_stricmp)
    void SetClipSequence(LevelAnimClip& clip, ni::NiSequence* seq);  // orig: 0x000BA570
    void Bind(const void* stage, ni::NiAVObject* target);            // orig: 0x000BA710
    void Unbind(const void* stage);                                  // orig: 0x000BA750

    void Play(const char* clipName, bool loop, bool reverse);   // orig: 0x000BB220 (null name = first clip)
    void Stop();                                     // orig: 0x000BA830
    void Pause();                                    // orig: 0x000BA2C0
    void Resume(bool reverse);                       // orig: 0x000BA2D0
    void SkipToEnd();                                // orig: 0x000BA850 ("FFEnd")
    void Rewind();                                   // orig: 0x000BA910 ("RWStart")
    const char* CurrentClipName() const;             // orig: 0x000BA7F0
    f32 CurrentClipDuration() const;                 // orig: 0x000BA810 (-1 when none)

    // orig: 0x000BACF0, every fixed step (Session::Update), `dt` = g_frameStep.
    void Advance(f32 dt, struct AnimTextKeyCounters& counters);
    // orig: 0x000BA990, every rendered frame (Session::Render); `camera` null = no culling (no level camera).
    void ApplyPose(const AnimCullCamera* camera);

    std::string name;                                // orig: +0x0C
    const void* owner = nullptr;                     // orig: +0x00, the binding stage (not set by the ctor)
    ni::NiPointer<ni::NiAVObject> target;            // orig: +0x10
    ni::NiPointer<ni::NiKeyframeManager> manager;    // orig: +0x14
    bool autoPlay = false;                           // orig: +0x18
    bool culled = false;                             // orig: +0x19
    bool reverse = false;                            // orig: +0x1A
    bool playing = false;                            // orig: +0x1B
    bool loop = false;                               // orig: +0x1C
    f32 time = 0;                                    // orig: +0x20
    bool dirty = false;                              // orig: +0x24
    s32 current = -1;                                // orig: +0x28
    std::vector<std::unique_ptr<LevelAnimClip>> clips;   // orig: array at +0x2C (data +0x30, size +0x38)

private:
    void EnsureManager();                            // the lazy manager creation shared by Play and Advance
};

// The "morph:barrel" / "morph:beams" text-key counters (orig: 0x002E9E08 / 0x002E9E0C, read by script natives).
struct AnimTextKeyCounters {
    u32 barrels = 0, beams = 0;
};

// orig: 0x000BAC00 FireTextKeys + 0x000B0A90 OnTextKey: the reference controller's keys in (a, b] (wrapping).
void FireTextKeys(ni::NiSequence* seq, f32 a, f32 b, AnimTextKeyCounters& counters);
// orig: 0x000BA3C0. Note the original's early return at the first controller that already has the cycle type.
void SetSequenceCycleType(ni::NiSequence* seq, bool loop);

// The level's animation list (orig: intrusive list at level +0x170, appended at the tail).
class LevelAnimations {
public:
    // orig: 0x000BB490 LevelAnimation_Create, for a child of the defs.nif "Animations" node.
    LevelAnimation* CreateFromNode(ni::NiAVObject& node);
    LevelAnimation* FindByName(const std::string& name) const;   // orig: 0x000BA5B0 (_stricmp)
    void UnbindAll(const void* stage);               // orig: 0x000BB080
    // orig: the level object's destructor 0x000C84D5..0x000C8510: `while (S+0x170) delete S+0x170` (head first).
    void DeleteAll();
    void Advance(f32 dt);                            // Session::Update's loop (0x000C8C7E)
    void ApplyPose(const AnimCullCamera* camera);    // Session::Render's loop (0x000C9229)

    std::vector<std::unique_ptr<LevelAnimation>> list;
    AnimTextKeyCounters counters;
};

}  // namespace fut::level
