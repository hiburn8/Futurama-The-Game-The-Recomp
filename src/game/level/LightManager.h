// The level light manager (DAT_002EA820, created by the level object's constructor through 0x000C11F0) and its
// targets: every built object gets an LMTarget child carrying lights the manager positions each frame. Decompiled
// from default.xbe (docs/analysis/game_objects.md 3.4).
//
// Ported: construction, AddTarget, LMTarget, AddLight / LMSource (the stage "Lights" nodes, FsCreateFire's light,
// natives 272 / 273), the per-frame Update 0x000C0B50 (the type-2 flicker, the player target, the sources scored and
// sorted, the targets' lights placed; renderer_lighting_port.md 2.6).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"

#include <vector>

namespace fut::game {

class LevelData;
class LightManager;

// orig: vtbl 0x002A30E0, ctor 0x000C0550, dtor 0x000BFD00, 0xE0 bytes: an NiAVObject (not a node).
class LMTarget : public ni::NiAVObject {
public:
    static constexpr std::string_view kClassName = "LMTarget";
    std::string_view ClassName() const override { return kClassName; }
    // orig: 0x000C0550: `count` point lights and a directional light, all switched off and affecting `node`; the
    // target is pushed at the head of the manager's list.
    LMTarget(ni::NiNode* node, u32 count, u32 kind, LightManager* manager);
    ~LMTarget() override;
    void UpdateWorldBound() override;                // orig: 0x000BFE00: centre = world translation, radius 0

    std::vector<ni::NiPointer<ni::NiPointLight>> pointLights;   // orig: +0xC0
    ni::NiPointer<ni::NiDirectionalLight> dirLight;  // orig: +0xC4
    f32 fade = 0;                                    // orig: +0xC8 (the directional light's time in range)
    u32 kind = 0;                                    // orig: +0xD0 (+0xCC: the light count)
    LightManager* manager = nullptr;
    LMTarget* next = nullptr;                        // orig: +0xD4
    LMTarget* prev = nullptr;                        // orig: +0xD8
};

// orig: LMSource (vtbl 0x002A3180, ctor 0x000C0790, dtor 0x000BFDC0, 0xF0 bytes; base LightSource 0x002A2EF8): a
// light source of the stage "Lights" nodes, an NiAVObject placed in the room containing it. Linked at the head of
// the manager's source list by the constructor (no reference), owned by its parent node.
class LMSource : public ni::NiAVObject {
public:
    static constexpr std::string_view kClassName = "LMSource";
    std::string_view ClassName() const override { return kClassName; }
    explicit LMSource(LightManager* manager);         // orig: 0x000C0790
    ~LMSource() override;                             // orig: 0x000BFDC0 (RemoveSource 0x000BFBC0)
    void UpdateWorldBound() override;                 // orig: 0x000BFE00 (LMTarget's): world translate, radius 0

    ni::NiColor color{1, 1, 1};                       // orig: +0xC0
    f32 radius = 1.0f;                                // orig: +0xCC
    f32 score = 0;                                    // orig: +0xE0 (0x000BFE30: the sort key of Update)
    u32 type = 0;                                     // orig: +0xD0: 1 lamp, 5 lava, 6 slime, 7 alien goo, 2 fire
                                                      //  (FsCreateFire; flickers), 0 other
    LightManager* manager = nullptr;
    LMSource* next = nullptr;                         // orig: +0xE4
    LMSource* prev = nullptr;                         // orig: +0xE8
};

class LightManager {                                  // orig: vtbl 0x002A3220, ctor 0x000BFF20, 0x18 bytes
public:
    LightManager();
    ~LightManager();                                  // (targets still alive forget the manager)
    LightManager(const LightManager&) = delete;
    LightManager& operator=(const LightManager&) = delete;
    // orig: vf00 0x000C0A50: nullptr without a node; otherwise the manager's directional light affects the node and
    // a new one-light target (kind 1) at `offset` becomes the node's child (first free slot), updated at time 0.
    ni::NiPointer<LMTarget> AddTarget(const ni::NiPoint3& offset, ni::NiNode* node);

    // orig: vf04 0x000C08F0 (thiscall, RET 0x28): a new source; with `attach` it goes under the level room
    // containing `pos` (first free slot), else under "Session:m_spDynamicNode", and is updated at time 0. The
    // returned reference is the caller's (LoadLights releases it: unattached sources die).
    ni::NiPointer<LMSource> AddLight(LevelData& level, const ni::NiPoint3& pos, const ni::NiColor& color, f32 radius,
                                     u32 type, bool attach);

    // orig: Update 0x000C0B50 (light manager vf0C, Session::Render; it returns at once without a player, S+0x94):
    // the player target (+0x0C, made on the first call: LMTarget(scene, 5, 2)); type-2 sources flicker with one rand()
    // each, every source is updated at time 0 (newest first); the sources with a score >= 0 (0x000BFE30) sorted by it
    // (qsort 0x001679F0, larger first); each target's lights placed from them (the player: the directional kinds
    // 2 / 3 / 4 / 8 / 9 / 11 into the manager's light +0x14 and its point lights; the others: the sources in range).
    // `frameTime` = DAT_002E7C28 (the real time of the frame).
    void Update(const LevelData& level, f32 frameTime);
    // orig: 0x000C0070 (native 272, type 4) / 0x000C0240 (native 273, type 3): AddLight(zero, colour, 1.0, type,
    // unattached) twice (a NiSmartPointerCast macro evaluating its argument twice: the first source dies), the second
    // kept at +0x10; then the global ambient.
    void SetGlobalLight(LevelData& level, const ni::NiColor& color, const ni::NiPoint3& ambient, u32 type);

    LMSource* firstSource = nullptr;                  // orig: +0x04 (newest first)
    LMTarget* firstTarget = nullptr;                  // orig: +0x08 (newest first)
    ni::NiPointer<LMTarget> playerTarget;             // orig: +0x0C
    ni::NiPointer<LMSource> globalSource;             // orig: +0x10 (natives 272 / 273)
    ni::NiPointer<ni::NiDirectionalLight> dirLight;  // orig: +0x14 (switched off)
};

}  // namespace fut::game
