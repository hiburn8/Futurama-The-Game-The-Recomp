// Level data filled while a level loads, decompiled from default.xbe: the parts of the level object (Session, g_level
// 0x002DAC64, 0x200 bytes, ctor 0x000C38B0) that the stream callback (0x000C5BD0, this = level + 8) fills from data
// files (scene graph only; render state, the HUD and the runtime are elsewhere):
//   * kind 9 (Levels\<level>\defs.nif), besides the definitions (see Defs.h / CharacterDef.h):
//       "Materials"            four footstep surfaces from its NiStringExtraData chain, their footstep sounds, then
//                              the per-level table (0x000C1AA0);
//       "Instances::<hex>"     the number of instance slots;
//     ("Stage::<name>" and "Animations" need the scene runtime and are not ported yet);
//   * kind 4 (Levels\<level>\<level>.nif): the stage (room group) under the scene root, textures, inst_count.txt;
//   * kind 10 (Levels\<level>\ingame_dlg_<language>.nif, three top-level nodes):
//       "LipsyncDir"           lip-sync key files by name (a list, newest first);
//       "SoundDir"             dialogue sounds through the sound cache, by file name;
//       "DatabaseDir"          language databases through the text manager, by upper-cased file name.
//
// Verified against the original level object and callback under emulation for every level and language of the disc
// (tests/game/LevelDataTests.cpp, oracle tools/difftest/level_oracle.py).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"
#include "game/object/GameObject.h"

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fut::text {
class LanguageDb;
class TextManager;
}  // namespace fut::text

namespace fut::level {
class LevelAnimations;
}

namespace fut::input {
class InputSystem;
}

namespace fut::phys {
class Physic;
class CollisionBox;
}

namespace fut::script {
struct ScriptProgram;
}

namespace fut::sound {
class GameSound;
}

namespace fut::game {

using text::LanguageDb;
using text::TextManager;

class Character;
struct DefContext;
class DefRegistry;
class LightManager;
class Player;
class Destructable;
class Intermission;
class GenericNpc;
class SimpleNpc;
struct AiAgent;
struct AiGroup;
struct AiNet;
class Stage;
class Camera;
class Hud;
class DialogueDisplay;
class ScriptTriggers;
struct TriggerRec;
class SoundEventTable;
class TriggerManager;
class LiquidResource;

// Footstep surfaces, indexed by the values stored in LevelData::footstepMaterials (orig: 0x002D6850).
extern const std::array<const char*, 11> kSurfaceNames;

// orig: the level part of the sound system's Update 0x000CBD40: the listener is g_level's camera (S+0x8C): its
// position +0x48 and its NiCamera's world rotation ((+0x30)+0x70); the origin / identity without a level or camera.
class LevelData;
void UpdateSound(sound::GameSound& snd, const LevelData* level);

struct LevelServices {
    // orig: App+0xF0 vtbl+0x10 (path, flags) -> handle (0x000C9860).
    std::function<u32(const std::string& path, u32 flags)> loadSound;
    // orig: App+0xF0 vtbl+0x14 (sound file object) -> sound cache handle, 0 without a sound system (0x000C9840).
    std::function<u32(ni::NiUDSFileObject& file)> loadCachedSound;
    TextManager* text = nullptr;                       // orig: TextManager::Get (0x000D9D80)
    std::function<bool(const std::string& path)> fileExists;   // (LevelData::fileExists)
    std::function<double(u32 handle)> soundDuration;           // (LevelData::soundDuration)
};

// orig: the player-state texts that outlive a level (docs/analysis/frontend_port.md 1.12): DAT_002EAA28 / 2C (the
// text the next load check applies: FsChangeLevel's 0x000C2770(0), a loaded save), DAT_002EAA30 / 34 (its level-start
// copy, for Replay), DAT_002EAA38 (keep: the pause's load 0x000C1280), DAT_002EAA39 (replay 0x000C2320). The App owns
// one (game/app App::carry); a level without one uses its own (LevelData::ownCarry). Empty = NULL.
struct CarryOver {
    std::optional<std::string> state, start;
    bool keep = false, replay = false;
};

// orig: HUDTimer (0x54 bytes, ctor 0x000ECC00 (bool swamp), vtbl 0x002A5698), the HUD element named "timer": only the
// state the script natives 380 / 388 observe (its text objects and drawing are NOT PORTED). docs/analysis/rodent_port.md 7.
struct HudTimer {
    explicit HudTimer(bool sw) : time(sw ? 120.0f : 0.0f), swamp(sw ? 1 : 0) {}   // orig: 0x000ECC00
    f32 time = 0;                                      // +0x28: 120.0 (0x42F00000) when built with swamp = 1
    u8 running = 0, paused = 0, swamp = 0, timedOut = 0;   // +0x2C / +0x2D / +0x2E / +0x2F
    void Command(const char* cmd, f32 v);              // orig: vf04 0x000EC7B0 (strncmp, 4 characters)
    void Update();                                     // orig: vf01 0x000EC760 (Session step 12)
};

// orig: an edge of a stage's "Edges" set (0x24 bytes; EdgeSet ctor 0x000CD1E0, docs/analysis/level2-2_port.md 3).
struct Edge {
    ni::NiPoint3 p0, p1;                               // +0x00 / +0x0C
    f32 minX = 0, maxX = 0;                            // +0x18 / +0x1C (after the sort +0x1C is the running maximum)
    u8 active = 1;                                     // +0x20 (native 199)
    std::array<u8, 3> pad{};                           // +0x21..+0x23 (never written by the original: heap bytes)
};
static_assert(sizeof(Edge) == 0x24);
struct EdgeSet {                                       // orig: new(0xC): +0 next, +4 new(n * 0x24), +8 n
    std::vector<Edge> edges;
};

class LevelData : public Owner {                       // (the level object's Owner part sits at +0, its list at +4)
public:
    std::string name;                                  // orig: +0x0C

    // ---- kind 9 ----
    // Returns true when `object` is one of the nodes handled here (the callback's "Materials" / "Instances::"
    // branches; matched with _stricmp / strncmp in the callback's prefix order).
    bool ApplyDefsNode(ni::NiObjectNET& object, const LevelServices& services);
    // orig: the "Materials" branch of 0x000C5BD0. Slot k (0-3) is the surface named by the k-th extra data (0 when it
    // is not a NiStringExtraData or names no surface); every named surface except Nothing and Lava loads its four
    // footstep sounds; then SetLevelTable().
    void ApplyMaterials(ni::NiObjectNET& node, const LevelServices& services);
    // orig: the "Instances::" branch: sscanf("%x"); on success count + 1 zeroed slots.
    void ApplyInstances(const char* nodeName);
    // orig: 0x000C1AA0 (on the level object): twelve of the thirteen entries are reset to -1 (+0x14C is not), then
    // the level's own values are set.
    void SetLevelTable();

    u32 instanceSlots = 0;                             // orig: +0x124 (the count + 1)
    std::vector<u32> instances;                        // orig: +0x128, the object flag table
    // orig: 0x000C1A60 / 0x000C1A40: valid for 0 < id < count only (other ids read 0, writes are ignored). Bits: 0x01
    // a live object exists, 0x02 collected, 0x08 destroyed, 0x10 dead, 0x20 spawned before.
    u32 GetObjFlags(u32 id) const { return id > 0 && id < instanceSlots ? instances[id] : 0; }
    void SetObjFlags(u32 id, u32 v) {
        if (id > 0 && id < instanceSlots) instances[id] = v;
    }

    // ---- Owner (vtbl 0x002A34F4): objects' nodes go under "Session:m_spDynamicNode" ----
    void DetachObject(GameObject& object) override;    // orig: 0x000C3FF0 (from any parent)
    bool IsOwnerType(u32 t) const override { return (t & 1) == t; }   // orig: 0x00040D50
    ni::NiNode* GetNode() const override { return dynamicNode.Get(); }   // orig: 0x000C3EA0 (+0x1C)
    std::array<s32, 13> levelTable{};                  // orig: +0x12C..+0x15C (meaning not identified yet)
    std::array<u32, 4> footstepMaterials{};            // orig: +0x160..+0x16C

    // ---- kind 1 ----
    // orig: the kind-1 branch of 0x000C5BD0 (SoundCache\<level>.nif): every top-level NiUDSFileObject goes to the
    // sound cache (the handles are not kept).
    static void LoadSoundCache(ni::NiStream& stream, const LevelServices& services);

    // ---- kind 3 ----
    // orig: 0x000BF980 (Effects\<level>.nif): top-level NiNodes go under the effects root (first empty slot),
    // NiSourceTextures are appended to the texture array, and the NiUDSFileObject named exactly "EffectList.txt"
    // (the last one wins) lists the effects to create. Returns those names in order (0x000BF910: split on ';' in
    // the file's buffer; a name without a closing ';' is ignored).
    std::vector<std::string> LoadEffectsStream(ni::NiStream& stream);
    // orig: Session::Start 0x000C2575 records whether "Effects\<level>.nif" exists (+0x198); the kind-3 request
    // (0x000C6D1A) then asks for "Effects\<level>.nif" or, when it does not exist, the name cut after 6 characters
    // (the world's file: "level4-1" -> "Effects\level4.nif").
    static std::string EffectsFileName(const std::string& level, bool levelFileExists) {
        std::string s = "Effects\\" + level;
        if (!levelFileExists && s.size() > 14) s.resize(14);
        return s + ".nif";
    }
    ni::NiAVObject* FindEffectObject(const char* objectName) const;   // orig: 0x000C2AF0 (root vtbl+0x54, strcmp)
    ni::NiObjectNET* FindTexture(const char* textureName) const;   // orig: 0x000C2A80 (_stricmp, first match)
    ni::NiPointer<ni::NiNode> effectsRoot{new ni::NiNode};     // orig: +0xE0, created by the Session ctor
    std::vector<ni::NiPointer<ni::NiObjectNET>> textures;      // orig: NiTArray at +0xC8 (data +0xCC, size +0xD4)

    // ---- scene (orig: the Session constructor 0x000C38B0) ----
    LevelData();
    ~LevelData();
    ni::NiPointer<ni::NiNode> scene;                   // orig: +0x18 "Session::m_spScene" (z-buffer property 3 / 3)
    ni::NiPointer<ni::NiNode> dynamicNode;             // orig: +0x1C "Session:m_spDynamicNode", a child of the scene
    ni::NiPointer<ni::NiCamera> ingameCamera;          // (the Ingame camera's NiCamera, a child of the scene)
    // The App's renderer (App->vf14(); its geometry-group manager is DAT_0030EC70): the stages' pre-cache goes there.
    // Null without one (the play oracle's fake renderer: no groups either).
    ni::NiRenderer* renderer = nullptr;
    // orig: S+0x88 / S+0x8C, the camera object (camera.md: one per level, created by Session::Start).
    std::shared_ptr<Camera> camera;
    // orig: the sound event globals (0x002D6884 / 0x002EAC18; SoundEvents.h); set in the game's flow only.
    std::shared_ptr<SoundEventTable> soundEvents;
    // orig: the App's sound system App+0xF0 (g_app 0x002EB3B0), the target of every game-facing sound call
    // (game/sound/GameSound.h; LevelLoadOptions::sound).
    sound::GameSound* sound = nullptr;
    sound::GameSound& Sound() const;
    // orig: S+0xA8, the level script (scripts\<level>.Ful), in the VM heap; destroyed with the level.
    script::ScriptProgram* script = nullptr;
    // orig: the global list 0x002E9FFC, the level scripts' trigger objects (ScriptTriggers.h).
    std::shared_ptr<ScriptTriggers> scriptTriggers;
    f32 materialDamage = 10.0f;                        // orig: DAT_002D6218 (FsSetMaterialDamage; 10 after a load)
    // orig: the respawn points S+0x70 (array +0x74, count +0x80): trigger records (FuAddRespawnPoint), and the respawn
    // stage S+0x60 / the previous one +0x64 / its record +0x68 (FuSetRespawnStageFromTargetable).
    std::vector<TriggerRec*> respawnPoints;
    Stage* respawnStage = nullptr;
    Stage* prevRespawnStage = nullptr;
    TriggerRec* respawnRec = nullptr;
    std::function<void()> updateStageStreaming;        // (the Session's 0x000C70C0, for the respawn native)
    // ---- the level change (native 161 FsChangeLevel 0x000B05B0; docs/analysis/object_gaps.md 21.3) ----
    std::function<void(const char*)> changeLevel;      // (the Session's part of 0x000B05B0)
    // orig: the Ingame context's leave 0x0002A220(name) (DAT_002DAC60: request + ctx+0x18): FsDeductLives' "Death",
    // FsActivateFrontend's "Frontend". Set by the App's Ingame context (game/app); unset = no App (reported).
    std::function<void(const char*)> leaveIngame;
    // orig: FsChangeLevel's App part (0x000B05C6..0x000B06A7): the level record (App+0xA4: the all-nibblers flag, the
    // best time `seconds`), App+0xAC = `next`, the end-of-level menu DAT_002E8EEC made / opened (its lives top-up).
    // Set by the App's Ingame context (game/app); unset = no App (a level loaded without one never runs its script).
    std::function<void(const char* next, s32 seconds)> endLevel;
    // orig: the App's level name App+0xAC (0x000A23D0; FsReplayLevel 0x000C2320 sets the level's own). Set by the App's
    // Ingame context (game/app); unset = no App (reported).
    std::function<void(const char* name)> setAppLevelName;
    // The carry-over texts (CarryOver above): the App's when it hosts the level, else ownCarry.
    CarryOver ownCarry;
    CarryOver* carry = &ownCarry;
    f32 respawnTimer = 0;                              // orig: S+0x6C (no writer but the constructor: dead)
    Def* pickupFlareDef = nullptr;                     // orig: DAT_002E06AC (set by every pickup constructor)
    bool camFadeDirty = false;                         // orig: byte DAT_002D6D98 (set by the CamFade timers)
    // orig: CamFadeRec::Render 0x000CD3D0's statics, recomputed by the first record's render after a timer step:
    // DAT_002EAF14 the unit direction camera -> player (ZERO at first use), DAT_002D6D9C its length - 3 (1.0 in the
    // image), DAT_002EAF10 the "PLAYER" record's character's +0x630 (0x00037090). Process globals in the original,
    // kept per level here (L: the stale values of a previous level are read only by a render before the level's
    // first timer step, whose state 0 moves no alpha).
    ni::NiPoint3 camFadeDir;
    f32 camFadeDist = 1.0f;
    bool camFadePlayerControl = false;
    s32 pickupValue = 0;                               // orig: S+0x188 (the pickup score)
    s32 lives = 5;                                     // orig: DAT_002D687C (5 after the Session constructor)
    bool quickLoading = false;                         // orig: DAT_002EAA48 (FsQuickLoad)
    void AddPickupValue(s32 v);                        // orig: 0x000C3360
    // orig: natives 319 / 318 (QuickSave.cpp, docs/analysis/quicksave_port.md): the player's state, the inventory and
    // the collected-pickup flags as "key=value" lines in `quickSave`, and the respawn that reads them back.
    void QuickSave();                                  // orig: FsQuickSave 0x000C7910 (thiscall S)
    void QuickLoad();                                  // orig: FsQuickLoad 0x000C7D40 (thiscall S)
    Intermission* intermission = nullptr;              // orig: the singleton 0x000D4E50 (LoadedLevel owns it)
    Destructable* firstDestructable = nullptr;         // orig: DAT_002DE13C (newest first, next +0x40)
    std::vector<GenericNpc*> genericNpcs;              // orig: S+0x9C list (+0x8B0 next, +0x8B4 prev)
    std::vector<SimpleNpc*> wasps;                     // orig: S+0xA0 list (+0x7F4 next, +0x7F8 prev; tail append)
    std::vector<AiAgent*> aiAgents;                    // orig: (S+0x114)+4, newest first
    std::deque<std::shared_ptr<AiNet>> aiNets;         // orig: (S+0x114)+8, newest first (not freed per stage)
    AiNet* FindAINet(const char* n) const;             // orig: 0x000313E0 (_stricmp; the searches cache link costs)
    // orig: (S+0x114)+0xC, the AI groups (natives 363-367; head insert: newest first), updated by the AI manager
    // 0x00031850 every play step; freed by the manager teardown 0x00031800 (after the agents in the port).
    std::deque<std::shared_ptr<AiGroup>> aiGroups;
    bool aiEnabled = true;                             // orig: byte [S+0x114] (the AI manager: set)
    bool aiBlind = false;                              // orig: DAT_002DBB40
    const Def* junkbotDef = nullptr;                   // orig: DAT_002DBB44 (FindDef("junkbot") by AiAgent::SetProfile)
    // orig: bytes DAT_002DD694 / DAT_002DD695: Player::Damage 0x00051AD0 returns at once while either is set. 694 is
    // cleared by every Character constructor (0x0003D5C0) and set by FsCharacterControllable (0x00037010, native 120);
    // no writer of 695 is known (L).
    u8 playerNoDamage694 = 0, playerNoDamage695 = 0;
    // orig: Ingame +0x19 (1 from the Ingame ctor 0x0002A1F0; FsDisablePausing 169 clears it). No reader is known (L).
    u8 pausingEnabled = 1;
    // orig: the global list DAT_002EAF0C of the stages' edge sets (Stage::LoadEdges 0x000CE950), newest first; read by
    // native 199 and the ledge scan (Player vf18C, Player::LedgeCheck); freed by ~Session (0x000CD170), not per stage.
    std::deque<std::shared_ptr<EdgeSet>> edgeSets;
    // orig: 0x00033EE0 (cdecl): is `c` in the character list S+0x90 (0x000C26E0 walk)?
    bool IsCharacter(const void* c) const {
        if (!c) return false;
        for (const Character* x : characters)
            if (static_cast<const void*>(x) == c) return true;
        return false;
    }
    std::vector<u16> enemyIds;                         // orig: S+0x19C (+0x1A0 data, +0x1A8 size)
    u32 enemyCount = 0;                                // orig: S+0x1AC (non-zero entries)
    std::vector<u16> killedIds;                        // orig: S+0x1CC (+0x1D0 data, +0x1D8 size; read by FsQuickSave /
    u32 killedCount = 0;                               //  FsQuickLoad 0x000C7910 / 0x000C7D40); S+0x1DC (non-zero entries)
    std::map<std::string, u8> nibblerStates;           // orig: S+0x1FC NiTStringMap ("$%03d:%03d" -> byte)
    // orig: (S+0x1FC)+0x14, the nibbler map's pending ids (NiTPrimitiveArray<u32>: data +0x18, size +0x20): appended
    // only by the freed nibbler (Nibbler::FreedStep, 0x0008BA8B -> 0x00077CC0: object_gaps.md 10.2); FsQuickSave commits them (0x00077870), FsQuickLoad drops them (0x00077900). docs/analysis/quicksave_port.md.
    std::vector<u32> nibblerPending;
    void SetNibblerState(const std::string& key, u8 v) { nibblerStates[key] = v; }   // orig: NiTStringMap::SetAt 0x0010BD70
    // orig: S+0x1B4 / S+0x1E4, FsQuickSave's copies of enemyIds / killedIds (data +0x1B8 / +0x1E8, size +0x1C0 /
    // +0x1F0, non-zero entries +0x1C4 / +0x1F4); FsQuickLoad copies them back.
    std::vector<u16> savedEnemyIds, savedKilledIds;
    u32 savedEnemyCount = 0, savedKilledCount = 0;
    // orig: DAT_002EAA40, FsQuickSave's text buffer (a heap copy of the builder's; freed by the Session ctor 0x000C3E13
    // and dtor 0x000C846C, so it lives for one level). Never written to a file. Empty = NULL.
    std::optional<std::string> quickSave;
    // orig: new(0x30) CollisionBox 0x000AF9B0 (its four callers: Moveable x2, Destructable, Static) in the level's
    // physics world (none without one).
    std::unique_ptr<phys::CollisionBox> NewCollisionBox(f32 ex, f32 ey, f32 ez, f32 mass);
    void AddEnemy(u16 id);                             // orig: 0x000C7850
    void AddKilled(u16 id);                            // orig: 0x000C7790 (AddEnemy's code on S+0x1CC)
    u8 NibblerState(u32 id, u32 b);                    // orig: 0x00077770 (inserts a missing key with 0)
    // orig: 0x000C41E0: some bound respawn point lies in a room whose first owning stage (list order) is `stage`.
    bool StageHasActiveCheckpoint(const Stage* stage) const;
    // orig: the stage's deleting destructor 0x000D1180 (Unload while still listed, unlink, ~Stage); also the
    // level teardown (0x000C85F2).
    void DeleteStage(Stage* stage);
    // A stage pointer that may be stale (the respawn stages in Level4-1): identity only, never dereferenced dead.
    bool LiveStage(const Stage* stage) const;
    // orig: the global DAT_002EA820, (re)created by the constructor through 0x000C11F0 (LightManager.h).
    std::shared_ptr<LightManager> lightManager;
    // orig: DAT_002EA828..0x002EA830, the global ambient colour the dead characters' ambient lights move towards
    // (0x000C0410); written only by natives 272 / 273 (0x000C0070 / 0x000C0240: their second vector); 0 in the image
    // (a process global: every oracle is a new process).
    ni::NiPoint3 globalAmbient;
    // orig: S+0x30 = DAT_002E9A7C, created by Session::Start (0x000C2659); the bodies' world.
    std::shared_ptr<phys::Physic> physic;
    // orig: +0xE4, the shared environment-map texture effect ("newshade.tga"; created by the effects set-up 0x000C59E0,
    // child 0 of the dynamic node; the characters' model clones are its affected nodes).
    ni::NiPointer<ni::NiTextureEffect> envTextureEffect;
    GameObject* markInstance = nullptr;                // orig: +0xA4 (the scorch-mark factory instance, level-owned)
    // Characters (Character.h; no references): every character (orig: list S+0x90, linked through +0x76C), the
    // players (S+0x98, through +0xC4C) and the first player (S+0x94).
    std::vector<Character*> characters;
    std::vector<Player*> players;
    Player* player = nullptr;
    // orig: the sound system's Sound_Load (App+0xF0), for objects that load sounds when they spawn (the player
    // classes' AddSound lists); a no-op when unset.
    std::function<u32(const std::string& path, u32 flags)> loadSound;
    // orig: +0xC0, created by the constructor (0x000DC070); destroyed after the objects and stages.
    std::shared_ptr<TriggerManager> triggers;
    // The liquid resources (Liquid.h; orig: the global cache 0x002DEDF4, a NiTPrimitiveArray: Liquid::Init takes the
    // last one of its type, 0x0006B630 adds into the first null slot, else appends). L: its lifetime (who clears the
    // original's is not read; the port keeps them with the level).
    std::vector<ni::NiPointer<LiquidResource>> liquidResources;

    // ---- kind 4 ----
    // orig: the kind-4 branch of 0x000C5BD0 (Levels\<level>\<level>.nif): the first top-level NiRoomGroup becomes
    // the room group (+0x20), NiSourceTextures are appended to the texture array, "inst_count.txt" goes to
    // ApplyLevelFileObject; then every child of the room group that is not a NiRoom moves under the scene (first
    // empty slot) with UpdateProperties, UpdateEffects and Update, then the room group itself with UpdateProperties
    // and Update (Update(0, true) = the downward pass, the upward pass 0x000B0420 is empty).
    void LoadStageStream(ni::NiStream& stream);
    ni::NiPointer<ni::NiRoomGroup> roomGroup;          // orig: +0x20
    // ---- kinds 5-7 ----
    ni::NiPointer<ni::NiAVObject> backdrop;            // orig: +0x24 "Session::m_spBackdrop" (a scene child)
    ni::NiPointer<ni::NiAVObject> cameraPath;          // orig: +0x28 "Session::m_spCameraPath" (not in the scene)
    bool backdropStatic = false;                       // orig: +0x2C (1 for backdrop_static.nif)
    // orig: a top-level NiUDSFileObject named exactly "inst_count.txt" goes to ApplyInstCount.
    void ApplyLevelFileObject(ni::NiObject* object);
    // orig: 0x000C3490: in the file's buffer (its last byte becomes NUL), each line ended by "\r" (the next character
    // is skipped too) is matched with strncmp (0x001675D0): "nibblers=" / "collectables=" -> atoi. A last line
    // without "\r" is ignored.
    void ApplyInstCount(ni::NiUDSFileObject& file);
    s32 nibblerCount = 0;                              // orig: +0x190
    s32 collectableCount = 0;                          // orig: +0x194
    s32 nibblersPickedUp = 0;                          // orig: +0x18C (FsIncNibblersPickedUp; 0 at a new load - L)
    // orig: the HUD (S+0x118; Hud.h): its HUDTimer element's state (HUDTimer(1) for "Level4-1", HUDTimer(0) for the
    // other levels but Level1-6; the HUD build 0x000C12F0).
    std::shared_ptr<HudTimer> hudTimer;
    std::shared_ptr<Hud> hud;                          // orig: S+0x118 (ctor 0x000E5470 in the Session ctor)
    // orig: the dialogue display DAT_002EBDAC (made once at the App start, 0x000A2B4D; let go at the Session's end,
    // 0x000C888F): one per level in the port (Dialogue.h).
    std::shared_ptr<DialogueDisplay> dialogue;

    // ---- stages (Stage.h) ----
    // orig: the "Stage::<name>" branch of kind 9 (0x000C651B): a new stage at the end of the list (the first one
    // becomes the current stage); its stream is requested.
    Stage* CreateStage(const std::string& stageName);
    Stage* FindStage(const std::string& stageName) const;   // orig: 0x000C3F70 (_stricmp)
    fut::level::LevelAnimations* animations = nullptr;     // (the level animations the stage files bind to)
    DefContext* defContext = nullptr;                 // (for definitions created by the spawns: the "fry" fallback)
    std::vector<std::shared_ptr<Stage>> stages;        // orig: list +0x58 (linked through Stage +0x10 / +0x14)
    Stage* currentStage = nullptr;                     // orig: +0x5C
    std::array<std::string, 3> stageNames;             // orig: +0x4C..+0x54 ("" = empty slot)
    DefRegistry* defs = nullptr;                       // orig: the definition list +0x34 (not owned; Defs.h)

    // ---- kind 10 ----
    // orig: the kind-10 branch of 0x000C5BD0 (only when the stream has exactly three top-level objects).
    void LoadDialogue(ni::NiStream& stream, const LevelServices& services);

    struct LipSyncEntry {                              // orig: 0x0C bytes (ctor 0x000D8FC0): next, name, file
        std::string name;                              // the file object's name (NULL -> "")
        ni::NiPointer<ni::NiUDSFileObject> file;
    };
    std::vector<LipSyncEntry> lipSync;                 // orig: manager at +0xE8, list head first (0x000D8DE0)
    std::map<std::string, u32> dialogueSounds;         // orig: NiTStringMap at +0x100
    std::vector<std::string> dialogueSoundOrder;       // its keys in insertion order (the bucket chains: newest first)
    std::map<std::string, LanguageDb*> databases;      // orig: NiTStringMap at +0xEC (upper-cased names)
    std::vector<std::string> databaseOrder;            // the keys in insertion order (the map's bucket chains)
    TextManager* text = nullptr;                       // orig: TextManager::Get (0x000D9D80), for the spawns
    input::InputSystem* input = nullptr;               // orig: the pads (g_pads 0x002E7024), for the player
    // orig: NiUDSStream(path) good (an archive entry or a disc file) and Sound_GetDuration 0x000C9930, for the
    // characters' IngameEvent lines; unset = no file / 0 seconds.
    std::function<bool(const std::string& path)> fileExists;
    std::function<double(u32 handle)> soundDuration;
};

}  // namespace fut::game
