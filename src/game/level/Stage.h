// Stages: the streamed parts of a level (Levels\<level>\<stage>.nif). Decompiled from default.xbe (ctor 0x000D01A0,
// stream callback Stage::vf00 0x000D1410; structure: docs/analysis/stage.md). A stage file's rooms hand their
// contents ("fixtures") to the level's room shells of the same name, and it carries the BSP, the link markers to
// the previous / next stage, waypoints, triggers, ladders, AI nets, instances and lights; its sequences are attached
// to the level animations.
//
// Ported: the stage object, room merging, BSP, link markers, waypoints, triggers, ladders, the sequence binding,
// the instances (records and spawns), Lights, PreCachedScenes (the NiData_* effect tags; the renderer pre-cache is
// not needed), the trigger flags (0x000C2BF0), the player start placement (with the "fry" fallback), the AI nets
// (0x000CE760) and the edge sets (0x000CE950). NOT PORTED yet: the camera, Update / Render and the streaming between
// stages (0x000C70C0).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"
#include "engine/ni/NiRenderer.h"
#include "engine/stream/StreamManager.h"
#include "game/object/GameObject.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fut::level {
class LevelAnimations;
}

namespace fut::game {

class Def;
class LevelData;

struct StageWaypoint {                               // orig: 0x20 bytes
    ni::NiPoint3 position;                           // +0x00
    f32 yaw = 0;                                     // +0x0C (ToEulerAnglesXYZ first angle)
    std::string name;                                // +0x10, at most 15 characters
};

struct StageLadder {                                 // orig: 0x1C bytes
    ni::NiPoint3 position;                           // +0x00
    f32 yaw = 0;                                     // +0x0C
    f32 v0 = 0, v1 = 0;                              // +0x10 / +0x14, from the "#V#" extra data
    u8 active = 1;                                   // +0x18 (0x000CFB96 writes 1 for every record; read by vf188)
};

// orig: 0x3C bytes (ctor 0x000CDBB0, dtor 0x000CDA20), one per "<DefName>|<hex id>[: <label>]" node of "Instances".
struct StageInstance {
    ni::NiPoint3 position;                           // +0x00, the node's local translation
    f32 yaw = 0;                                     // +0x0C (ToEulerAnglesXYZ first angle)
    std::string label;                               // +0x10, at most 31 characters ("(noname)" without a label)
    u32 id = 0;                                      // +0x30, the level instance slot
    Def* def = nullptr;                              // +0x34
    std::optional<std::string> text;                 // +0x38, the node's first extra data if a NiStringExtraData
};

// orig: 0x24 bytes, ctor 0x000CD680: a geometry tagged "NiData_CamFade" that fades out while it lies between the
// camera and the player (Stage::Update's timer 0x000CD300, Stage::Render's 0x000CD3D0).
struct CamFadeRec {
    // orig: 0x000CD3D0 (each rendered stage frame, after the objects' PostUpdate): the alpha `current` moves by twice
    // the time since the last render towards `minAlpha` (the geometry's bound centre in front of the camera within
    // the player's distance - 3 and the bound's sphere test) or `maxAlpha`, then the material's alpha, the alpha
    // property's blend bit and an NiTriShape's vertex colours' alpha follow it; `state` = 0.
    void Render(LevelData& level);
    ni::NiPointer<ni::NiGeometry> geometry;           // +0x00
    ni::NiPointer<ni::NiMaterialProperty> material;   // +0x04 (new, at the head of the geometry's properties)
    ni::NiPointer<ni::NiAlphaProperty> alpha;         // +0x08 (new, flags 0xED)
    f32 state = 0;                                    // +0x0C (the time since the last render: Update adds the step)
    f32 current = 1.0f, maxAlpha = 1.0f;              // +0x10 the alpha, +0x14 its fade-in limit
    f32 minAlpha = 0.25f;                             // +0x18 its fade-out limit
    ni::NiPointer<ni::NiMaterialProperty> originalMaterial;   // +0x1C (removed from the geometry)
    ni::NiPointer<ni::NiAlphaProperty> originalAlpha;         // +0x20 (always null from PreCache)
};

class Stage : public Owner, public stream::Client {   // orig: 0xB0 bytes (Owner at +4, its list head at +8)
public:
    Stage(LevelData& level, const std::string& name);   // orig: 0x000D01A0
    ~Stage() override;
    // orig: 0x000D0510: the level animations unbound, the owned objects deleted, the records / BSP / fixtures (out of
    // the level rooms) / CamFade records released; the stage stays a valid, unloaded object.
    void Unload();
    // orig: 0x000D0800 (native 321): ResetObjects, then the instance records of def types 4, 0x40 and 0x80 respawn
    // (0x000D0740 each), then vf50 on the stage's statics.
    void Reset();
    // orig: 0x000CE570: the level's type-0x10 objects (projectiles) and the stage's objects other than players,
    // statics and liquids are deleted; the object flags of the records not live are cleared.
    void ResetObjects();
    // orig: 0x000CF750 (record, check the flags): the record's object spawned (nullptr when skipped or unsupported).
    GameObject* SpawnInstance(StageInstance& r, bool check);
    // orig: 0x000D0790 (native 324): the first record whose label matches (_stricmp) is spawned.
    void SpawnByLabel(const char* label, bool check);
    // orig: 0x000CDBE0 (Session step 3): the CamFade timers, the objects' Tick (false = delete) and the pickup
    // collection by the player, then the stage clock.
    bool Update();
    // orig: 0x000CE340 (Session::Render): the objects' vf0C (characters: false = delete).
    void Render();
    bool IsStreamPending() const { return request && request->state == stream::State::Reading; }   // 0x000CE3E0
    std::unique_ptr<ni::NiStream> file;               // (port: the stage's loaded file, kept until Unload)

    // Owner (vtbl 0x002A3E70): objects' nodes go under the stage node; DetachObject (0x000CEE00) only detaches
    // from the stage node (a node already moved into a room stays there).
    void DetachObject(GameObject& object) override;
    bool IsOwnerType(u32 t) const override { return (t & 2) == t; }   // orig: 0x000D04F0
    ni::NiNode* GetNode() const override { return node.Get(); }      // orig: 0x000C3EA0 (+0x20)
    bool ContainsRoom(const ni::NiRoom* room) const;  // orig: 0x000CE410 (rooms +0x28, linear)
    bool IsLoaded() const { return !fixtures.empty(); }   // orig: 0x000CE400 (+0x4C fixture count)

    // orig: Stage::vf00 0x000D1410 when the stream has loaded (part 1, see the file comment).
    void Load(ni::NiStream& stream, level::LevelAnimations& animations);
    // orig: 0x000CEDAE: a stream request on the current manager (Levels\<level>\<stage>.nif).
    void RequestStream();
    bool OnStream(stream::Request& r) override;       // orig: path, then Stage::OnStream 0x000D1410
    std::shared_ptr<stream::Request> request;         // orig: +0x18
    const StageWaypoint* FindWaypoint(const std::string& name) const;   // orig: 0x000CE450 (_stricmp)

    LevelData& level;
    std::string name;                                // orig: +0x0C
    f32 time = 0;                                    // orig: +0x1C, the stage clock passed to every update
    ni::NiPointer<ni::NiNode> node;                  // orig: +0x20 "Stage::m_spNode(<name>)" under the scene
    ni::NiPointer<ni::NiBsp> bsp;                    // orig: +0x24
    std::vector<ni::NiPointer<ni::NiRoom>> rooms;    // orig: +0x28, the level rooms filled (duplicates kept)
    std::vector<ni::NiPointer<ni::NiAVObject>> fixtures;   // orig: +0x40 (non-empty = loaded)
    std::string linkEnd, linkStart;                  // orig: +0x58 next stage / +0x5C previous ("" = none)
    std::vector<StageWaypoint> waypoints;            // orig: +0x6C (+0x68 count)
    std::vector<StageLadder> ladders;                // orig: +0x74 (+0x70 count)
    std::vector<StageInstance> instances;            // orig: +0x64 (+0x60 count)
    std::vector<std::unique_ptr<CamFadeRec>> camFades;   // orig: +0x78 array (+0x84 size)
    // orig: stage +0xAC && DAT_0030EC70: the renderer's geometry groups exist (always in the game). Without them
    // PreCache skips the subtrees of animated objects (docs/analysis/stage_precache.md 3).
    bool precacheRenderer = true;
    // orig: +0xAC = DAT_0030EC70 ? DAT_0030EC70->vf04(0, 0) : 0: the stage's geometry group (shared vertex buffers),
    // null without a renderer (LevelData::renderer).
    ni::NiGeometryGroup* geometryGroup = nullptr;

private:
    void LoadWaypoints(ni::NiNode& nd);              // orig: 0x000CF880
    void LoadLadders(ni::NiNode& nd);                // orig: 0x000CFA20
    // orig: 0x000D11B0 (post-order: children first, then the object's tags); renderer parts not ported.
    void PreCache(ni::NiAVObject* obj);
    void PreCacheAnimated(ni::NiAVObject* obj);       // orig: 0x000D0E60 (the subtree of an animated object)
    bool TagEffects(ni::NiAVObject* obj);             // orig: 0x000D0870
    bool FindAnimating(const ni::NiAVObject& obj) const;   // orig: LevelAnimation::FindAnimating 0x000BAF50
    level::LevelAnimations* animations_ = nullptr;    // (the level animations, while Load runs)
    void LoadLights(ni::NiNode& nd);                 // orig: 0x000CFBD0
    void LoadAINets(ni::NiObjectNET* o);             // orig: 0x000CE760
    void LoadEdges(ni::NiObjectNET* o);              // orig: 0x000CE950 (an EdgeSet into LevelData::edgeSets)
    void LoadInstances(ni::NiNode& nd);              // orig: 0x000CEE50
    // orig: the "emitter<up|side>_<effect>" / "SteamEmitter<Up|Side>" branches of 0x000CEE50: an effect owned by the
    // level object instead of a static.
    void SpawnEmitterPlaceholder(const StageInstance& r);
    void BindSequence(ni::NiSequence& seq, level::LevelAnimations& animations);   // vf00's second pass
};

// orig: 0x000CDB30 GetVectorExtraData: the first 16-byte binary extra data starting with the "#V#" tag -> its three
// floats (zero otherwise).
ni::NiPoint3 VectorExtra(const ni::NiObjectNET& obj);

}  // namespace fut::game
