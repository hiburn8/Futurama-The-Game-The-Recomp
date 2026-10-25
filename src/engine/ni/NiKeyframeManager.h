// NiKeyframeManager: plays, blends, morphs and sums NiSequences on a scene graph (characters, level animations,
// cut-scenes, the frontend). Decompiled from default.xbe 0x0012A4F0-0x00131700; structure and x87 sequences are in
// docs/analysis/anim_runtime.md, verified against the original under emulation (tests/engine/NiKeyframeManagerTests).
//
// Sequence states (NiSequence::state, orig +0x98): 0 inactive, 1 playing, 2 / 3 blend-in destination (handled by
// UpdateBlend but never set by this game), 4 frozen blend / mix source, 5 blend destination, 6 morph source,
// 7 morph destination, 8 sum source, 9 sum destination.
//
// NOT PORTED: root motion accumulation ("cumulative animations", manager +0x4C / sequence +0xAC with the loop
// transform 0x0012CC00, AccumulateTransform 0x00130150, SetAccumTransform 0x0012FE20 and the temp transforms
// +0x100 / +0x130): no code of the game sets the manager's flag (only writes of 0 exist), so it never activates.
// The name-string pool (+0x9C) is not needed: the port keeps each sequence's own controller names.
#pragma once

#include "engine/ni/NiObjects.h"

#include <array>
#include <string>
#include <vector>

namespace fut::ni {

class NiKeyframeManager : public NiTimeController {
public:
    static constexpr std::string_view kClassName = "NiKeyframeManager";   // (FUT_NI_CLASS is local to NiObjects.h)
    std::string_view ClassName() const override { return kClassName; }
    NiKeyframeManager();                             // orig: 0x00130BD0
    explicit NiKeyframeManager(NiAVObject* target);  // orig: 0x00130D20 (prepends itself to the target's chain)
    ~NiKeyframeManager() override;

    // orig: vtbl+0x38 (0x00130030). Runs every sequence's state handler in map order; no time or active checks.
    void Update(f32 time) override;
    bool IsTransformController() const override { return true; }

    bool AddSequence(NiSequence* seq, bool link);    // orig: 0x0012FFB0
    bool RemoveSequence(NiSequence* seq);            // orig: 0x00130FE0 (does not deactivate)
    void RemoveAllSequences();                       // orig: 0x00131010
    NiSequence* GetSequence(const std::string& name) const;   // orig: 0x0001BFC0
    u32 SequenceCount() const { return count_; }     // orig: this+0x3C

    bool ActivateSequence(NiSequence* seq, s8 priority);   // orig: 0x0012F3A0
    bool DeactivateSequence(NiSequence* seq);        // orig: 0x0012FAB0
    // orig: 0x0012F3D0 (0x0012FD30 = priority 0). `src` may be null: blend from the current pose.
    bool BlendFromSequence(NiSequence* src, NiSequence* dst, const char* destTextKey, f32 blendStart, f32 blendEnd,
                           s8 priority);
    bool BlendDeactivate(NiSequence* src, NiSequence* dst);   // orig: 0x001310D0
    bool MorphActivate(NiSequence* src, NiSequence* dst, f32 morphStart, f32 morphEnd);   // orig: 0x0012F5D0
    bool MorphDeactivate(NiSequence* src, NiSequence* dst);   // orig: 0x0012FAE0
    bool SumActivate(NiSequence* s1, NiSequence* s2, f32 weight);   // orig: 0x0012F8F0
    // orig: 0x0012F790: sum with a transition from the current pose ending at `transitionEnd`.
    bool SumActivate(NiSequence* s1, NiSequence* s2, f32 weight, f32 transitionEnd);
    bool SumDeactivate(NiSequence* s1, NiSequence* s2);   // orig: 0x0012FB40
    void SetSumWeight(f32 weight);                   // orig: 0x0012FD60 (ignored outside [0, 1])
    f32 SumWeight() const { return sumWeight_; }     // orig: 0x0012F320
    bool DeactivateAll();                            // orig: 0x00131170

    bool cumulativeAnimations = false;               // orig: this+0x4C (never set; see the file comment)

private:
    struct Item {                                    // orig: NiTMap item {key, value, next} (0xC bytes)
        std::string key;
        NiPointer<NiSequence> value;
        Item* next = nullptr;
    };
    static constexpr u32 kHashSize = 37;             // orig: this+0x40
    static u32 Hash(const std::string& key);         // orig: 0x000C3700
    void SetAt(const std::string& key, NiSequence* seq);   // orig: 0x00037B40 (new keys at the bucket head)
    bool RemoveAt(const std::string& key);           // orig: 0x0003B4B0
    Item* NextItem(const Item* item) const;          // orig: 0x0012FF20 iterator step

    std::array<Item*, kHashSize> buckets_{};         // orig: this+0x44
    u32 count_ = 0;                                  // orig: this+0x3C
    // Items removed while Update iterates stay allocated until it returns: the original frees them immediately and
    // its iterator may still step through a freed item's (unchanged) next pointer.
    u32 updating_ = 0;
    std::vector<Item*> retired_;
    f32 sumWeight_ = 0;                              // orig: this+0x8C
    bool sumActive_ = false;                         // orig: this+0x90
    NiSequence* sum1_ = nullptr;                     // orig: this+0x94
    NiSequence* sum2_ = nullptr;                     // orig: this+0x98
};

// orig: NiTimeController::StartAnimations 0x0013EF40 (object): every controller of the object Start(-FLT_MAX)
// (vf2C), then for an NiAVObject (RTTI 0x002F9190) its properties' that have controllers and for an NiNode (0x002F9230)
// every child, recursively.
void StartAnimations(NiObjectNET* object);
// orig: NiTimeController::StopAnimations 0x0013F090: the same walk with Stop (vf30).
void StopAnimations(NiObjectNET* object);

}  // namespace fut::ni
