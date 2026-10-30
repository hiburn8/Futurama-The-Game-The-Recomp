// Decompiled from default.xbe (the stream callback 0x000C5BD0, kinds 9 and 10, and 0x000C1AA0); see LevelData.h.
#include "game/level/LevelData.h"
#include "game/level/Hud.h"

#include "core/CrtString.h"

#include "game/GameTime.h"
#include "game/level/Camera.h"
#include "game/level/Dialogue.h"
#include "game/sound/GameSound.h"
#include "game/sound/SoundEvents.h"
#include "game/script/GameNatives.h"
#include "game/script/ScriptVM.h"
#include "core/memory/MemoryManager.h"
#include "game/level/LightManager.h"
#include "game/object/Character.h"
#include "game/object/GenericNpc.h"
#include "game/object/Liquid.h"
#include "game/level/ScriptTriggers.h"
#include "game/level/Stage.h"
#include "game/level/TriggerManager.h"
#include "game/physics/Physics.h"

#include "game/defs/Defs.h"
#include "game/defs/EffectDefs.h"
#include "game/level/LevelAnimation.h"
#include "game/text/LanguageDb.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fut::game {
namespace {

int StrICmp(const char* a, const char* b) {           // orig: _stricmp (0x001684D1), ASCII folding
    for (;; ++a, ++b) {
        int ca = static_cast<unsigned char>(*a), cb = static_cast<unsigned char>(*b);
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || ca == 0) return ca - cb;
    }
}

// Footstep sound pattern per surface (orig: the jump table 0x000C701C; Lava has none, Sand uses the wood sounds).
const char* FootstepPattern(u32 surface) {
    switch (surface) {
        case 1: return "sounds/footsteps/metal%d.wav";
        case 2: return "sounds/footsteps/hard%d.wav";
        case 3: return "sounds/footsteps/wood%d.wav";
        case 5: return "sounds/footsteps/wet_puddle/wet%d.wav";
        case 6: return "sounds/footsteps/wet2_%d.wav";
        case 7: return "sounds/footsteps/wet1_%d.wav";
        case 8: return "sounds/footsteps/gravel%d.wav";
        case 9: return "sounds/footsteps/wet3_%d.wav";
        case 10: return "sounds/footsteps/wood%d.wav";
        default: return nullptr;                      // 0 Nothing, 4 Lava
    }
}

}  // namespace

const std::array<const char*, 11> kSurfaceNames = {"Nothing", "Metal", "Hard", "Wood", "Lava", "Sewage waste",
                                                   "Oil", "Water", "Gravel", "Mud", "Sand"};

bool LevelData::ApplyDefsNode(ni::NiObjectNET& object, const LevelServices& services) {
    if (object.name.empty()) return false;
    const char* n = object.name.c_str();
    if (StrICmp(n, "Materials") == 0) {
        ApplyMaterials(object, services);
        return true;
    }
    if (crt::StrNICmp(n, "Instances::", 11) == 0) {   // _strnicmp
        ApplyInstances(n);
        return true;
    }
    return false;
}

void LevelData::ApplyMaterials(ni::NiObjectNET& node, const LevelServices& services) {
    const ni::NiExtraData* extra = node.extraData.Get();
    if (!extra) return;                               // orig: nothing at all without extra data
    for (u32 slot = 0; slot < footstepMaterials.size(); ++slot) {
        footstepMaterials[slot] = 0;
        if (!extra) continue;                         // port-only: the original reads past the end of the chain
        if (const auto* sx = dynamic_cast<const ni::NiStringExtraData*>(extra)) {
            u32 surface = 0;
            while (surface < kSurfaceNames.size() && StrICmp(kSurfaceNames[surface], sx->string.c_str()) != 0)
                ++surface;
            if (surface < kSurfaceNames.size()) {
                footstepMaterials[slot] = surface;
                if (const char* pattern = FootstepPattern(surface)) {
                    for (int variant = 1; variant <= 4; ++variant) {
                        char path[256];
                        std::snprintf(path, sizeof path, pattern, variant);
                        if (services.loadSound) services.loadSound(path, 0);
                    }
                }
            }
        }
        extra = extra->nextData.Get();
    }
    SetLevelTable();
}

void LevelData::ApplyInstances(const char* nodeName) {
    unsigned int count = 0;
    if (std::sscanf(nodeName + 11, "%x", &count) != 1) return;
    instanceSlots = count + 1;
    instances.assign(instanceSlots, 0);
}

std::unique_ptr<phys::CollisionBox> LevelData::NewCollisionBox(f32 ex, f32 ey, f32 ez, f32 mass) {
    auto box = std::make_unique<phys::CollisionBox>(ex, ey, ez, mass, physic ? &physic->world : nullptr);
    // HEAP-MIRROR: +0x154 is never written (heap garbage in the original); the port's default (won't do: KNOWN_ISSUES
    // "Heap layout")
    return box;
}

void LevelData::SetLevelTable() {                     // orig: 0x000C1AA0
    for (size_t i = 0; i < levelTable.size(); ++i)
        if (i != 8) levelTable[i] = -1;               // orig: +0x14C is not reset
    auto is = [&](const char* level) { return StrICmp(name.c_str(), level) == 0; };
    auto& t = levelTable;
    if (is("level1-1") || is("level5-1")) {
        t[7] = 4;
    } else if (is("level1-3")) {
        t[7] = 7;
    } else if (is("level2-2")) {
        t[0] = 8;
        t[1] = 0xB;
        t[3] = 9;
        t[4] = 0x4A;
        t[7] = 0x47;
    } else if (is("level2-4")) {
        t[7] = 6;
    } else if (is("level2-6")) {
        t[2] = 0xB;
    } else if (is("level3-4")) {
        t[12] = 0x4A;
        t[8] = 7;
    } else if (is("level4-1")) {
        t[5] = 10;
        t[6] = 9;
        t[10] = 0xB;
    } else if (is("level5-2") || is("level5-3")) {
        t[7] = 0x44;
        t[8] = 7;
    }
}

std::vector<std::string> LevelData::LoadEffectsStream(ni::NiStream& stream) {   // orig: 0x000BF980
    ni::NiPointer<ni::NiUDSFileObject> list;
    for (u32 link : stream.TopLevelLinks()) {
        ni::NiObject* obj = stream.ResolveLink(link);
        if (!obj) continue;
        if (auto* node = dynamic_cast<ni::NiNode*>(obj)) {
            effectsRoot->AttachChild(node, true);
        } else if (auto* texture = dynamic_cast<ni::NiSourceTexture*>(obj)) {
            textures.emplace_back(texture);
        } else if (auto* file = dynamic_cast<ni::NiUDSFileObject*>(obj)) {
            if (file->name == "EffectList.txt") list = file;
        }
    }
    std::vector<std::string> names;
    if (!list || list->data.empty()) return names;
    // orig: 0x000BF910, in place: '\0' over each ';'.
    char* p = reinterpret_cast<char*>(list->data.data());
    char* const end = p + list->data.size();
    size_t consumed = 0;
    while (consumed < list->data.size()) {
        char* q = static_cast<char*>(std::memchr(p, ';', static_cast<size_t>(end - p)));   // port-only bound
        if (!q) break;
        *q = 0;
        names.emplace_back(p);                        // orig: the creator 0x000BB5A0 runs here
        consumed += std::strlen(p) + 1;
        p = q + 1;
    }
    return names;
}

ni::NiAVObject* LevelData::FindEffectObject(const char* objectName) const {   // orig: 0x000C2AF0
    return GetObjectByName(effectsRoot.Get(), objectName);
}

ni::NiObjectNET* LevelData::FindTexture(const char* textureName) const {   // orig: 0x000C2A80
    for (const auto& t : textures)
        if (t && !t->name.empty() && StrICmp(t->name.c_str(), textureName) == 0) return t.Get();
    return nullptr;
}

void UpdateSound(sound::GameSound& snd, const LevelData* level) {   // orig: 0x000CBD86..0x000CBDC2
    const Camera* c = level ? level->camera.get() : nullptr;
    if (c && c->sceneCamera) snd.Update(&c->position, &c->sceneCamera->world.rotate);   // ((+0x30) is not tested)
    else snd.Update(nullptr, nullptr);
}

sound::GameSound& LevelData::Sound() const {
    return *sound;
}

LevelData::LevelData() {                              // orig: the scene part of 0x000C38B0
    scene = new ni::NiNode;
    scene->name = "Session::m_spScene";
    auto* zbuffer = new ni::NiZBufferProperty;         // orig: flags 3 (|= 3), test function 3
    zbuffer->flags = 3;
    zbuffer->testFunction = 3;
    AttachProperty(*scene, zbuffer);
    scene->UpdateProperties();                         // orig: 0x000C3C20
    dynamicNode = new ni::NiNode;
    dynamicNode->name = "Session:m_spDynamicNode";
    scene->AttachChild(dynamicNode.Get(), true);       // orig: vtbl+0xA0
    scene->UpdateDownwardPass(0.0f, true);             // orig: Update(0, true)
    lightManager = std::make_shared<LightManager>();   // orig: 0x000C11F0
    triggers = std::make_shared<TriggerManager>(*this);   // orig: 0x000DC070
    dialogue = std::make_shared<DialogueDisplay>();   // orig: DAT_002EBDAC (0x000D7760, mode 2)
}

void LevelData::LoadStageStream(ni::NiStream& stream) {   // orig: 0x000C5BD0, kind 4
    for (u32 link : stream.TopLevelLinks()) {
        ni::NiObject* object = stream.Objects()[link].Get();
        if (!object) continue;
        if (auto* group = dynamic_cast<ni::NiRoomGroup*>(object); group && !roomGroup) roomGroup = group;
        if (auto* texture = dynamic_cast<ni::NiSourceTexture*>(object)) textures.emplace_back(texture);
        ApplyLevelFileObject(object);
    }
    if (!roomGroup) return;
    const u32 count = static_cast<u32>(roomGroup->Children().size());   // orig: taken before the loop
    for (u32 i = 0; i < count; ++i) {
        ni::NiAVObject* child = i < roomGroup->Children().size() ? roomGroup->Children()[i].Get() : nullptr;
        if (!child || dynamic_cast<ni::NiRoom*>(child)) continue;
        ni::NiPointer<ni::NiAVObject> keep(child);
        scene->AttachChild(child, true);               // orig: vtbl+0xA0 (detaches it from the room group)
        child->UpdateProperties();                     // 0x000C6156
        child->UpdateEffects();
        child->UpdateDownwardPass(0.0f, true);         // Update(0, true)
    }
    scene->AttachChild(roomGroup.Get(), true);
    roomGroup->UpdateProperties();                     // 0x000C618A (no UpdateEffects)
    roomGroup->UpdateDownwardPass(0.0f, true);
}

void LevelData::ApplyLevelFileObject(ni::NiObject* object) {
    auto* file = dynamic_cast<ni::NiUDSFileObject*>(object);
    if (file && file->name == "inst_count.txt") ApplyInstCount(*file);
}

void LevelData::ApplyInstCount(ni::NiUDSFileObject& file) {   // orig: 0x000C3490
    if (file.data.empty()) return;                    // port-only: the original writes before the buffer
    char* p = reinterpret_cast<char*>(file.data.data());
    char* const end = p + file.data.size();
    end[-1] = 0;
    while (p < end) {
        char* cr = std::strchr(p, '\r');
        if (!cr) break;
        *cr = 0;
        if (std::strncmp(p, "nibblers=", 9) == 0) {
            nibblerCount = std::atoi(p + 9);
        } else if (std::strncmp(p, "collectables=", 13) == 0) {
            collectableCount = std::atoi(p + 13);
        }
        p = cr + 2;
    }
}

void LevelData::LoadSoundCache(ni::NiStream& stream, const LevelServices& services) {
    for (u32 link : stream.TopLevelLinks())
        if (auto* file = dynamic_cast<ni::NiUDSFileObject*>(stream.ResolveLink(link)))
            if (services.loadCachedSound) services.loadCachedSound(*file);   // orig: 0x000C9840
}

void LevelData::LoadDialogue(ni::NiStream& stream, const LevelServices& services) {
    const auto& top = stream.TopLevelLinks();
    if (top.size() != 3) return;
    for (u32 link : top) {
        auto* node = dynamic_cast<ni::NiNode*>(stream.ResolveLink(link));
        if (!node || node->name.empty()) continue;   // orig: otherwise only a language query
        auto children = [&](auto&& each) {
            for (u32 i = 0; i < node->Children().size(); ++i)
                if (auto* file = dynamic_cast<ni::NiUDSFileObject*>(node->Children()[i].Get())) each(*file);
        };
        if (node->name == "LipsyncDir") {
            children([&](ni::NiUDSFileObject& file) {   // orig: 0x000D8DE0 (prepends)
                lipSync.insert(lipSync.begin(), LipSyncEntry{file.name, ni::NiPointer<ni::NiUDSFileObject>(&file)});
            });
        } else if (node->name == "SoundDir") {
            children([&](ni::NiUDSFileObject& file) {
                const u32 handle = services.loadCachedSound ? services.loadCachedSound(file) : 0;
                if (!handle) return;
                if (!dialogueSounds.count(file.name)) dialogueSoundOrder.push_back(file.name);   // (SetAt keeps a key's place)
                dialogueSounds[file.name] = handle;           // orig: NiTStringMap::SetAt (0x0010BD70)
            });
        } else if (node->name == "DatabaseDir") {
            children([&](ni::NiUDSFileObject& file) {
                LanguageDb* db = services.text ? services.text->LoadDbFromMemory(file.data, file.name) : nullptr;
                if (!db) return;
                std::string key = file.name;
                for (char& c : key)
                    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);   // orig: toupper (0x00167C80)
                if (!databases.count(key)) databaseOrder.push_back(key);   // (SetAt keeps an existing key's place)
                databases[key] = db;
            });
        }
    }
}

LevelData::~LevelData() {                             // orig: 0x000C83F0 (the parts ported)
    // (the pad / rumble part 0x000C8421..0x000C8453 runs in the host's level deletion, IngameContext::DeleteLevel.)
    // The steps that release effects keep the original's order: the ids go back to the LIFO free list, so the next
    // creations' ids depend on it (docs/analysis/effect_release_order.md 3).
    // orig: 0x000C8458: the light manager deleted (0x000C11B0: ~LightManager 0x000BFFD0 releases +0x14, +0x10, +0x0C;
    // DAT_002EA820 / 824 = 0, so the targets released later do not unlink, LMTarget::~LMTarget)
    lightManager.reset();
    if (dialogue) dialogue->Release();                // orig: 0x000C8467 DAT_002EBDAC->0x000D7A10 (again at 0x000C888F)
    quickSave.reset();                                // orig: 0x000C846C: DAT_002EAA40 freed
    // orig: 0x000C8484: the carry-over texts. Unless kept (the pause's load), a replay makes the level-start copy the
    // text the next load check applies (the old text freed; DAT_002EAA2C = DAT_002EAA34), else the copy is freed; then
    // DAT_002EAA30 / 34 = 0 and both flags cleared.
    if (!carry->keep) {
        if (carry->replay) carry->state = std::move(carry->start);
        carry->start.reset();
    }
    carry->keep = false;
    carry->replay = false;
    // orig: 0x000C84D5: the level animations S+0x170 deleted, head first (the LoadedLevel owns the port's list: it is
    // emptied here, so the stages' UnbindAll and the exit function below find none, as in the original)
    if (animations) animations->DeleteAll();
    animations = nullptr;
    if (markInstance) {                               // orig: 0x000C851C: S+0xA4 vf00(1) (the scorch-mark instance)
        delete markInstance;
        markInstance = nullptr;
    }
    if (defs) {                                       // orig: 0x000C852F / 0x000C853F: S+0x40, S+0x48 vf00(1)
        defs->Delete(defs->scorchFactory);
        defs->Delete(defs->respawnEffect);
    }
    // (0x000C8550: each character definition's in-game events D+0x28 cleared, 0x000F29B0: they go with the
    // definitions below in the port)
    while (!characters.empty()) delete characters.front();   // orig: 0x000C8570: `while (S+0x90) S+0x90->vf00(1)`
    hud = nullptr;                                    // orig: 0x000C8594: S+0x118 deleted (0x000E5B20: its elements'
                                                      //  dtors, the checkpoint's Sound_Release)
    // (the AI manager S+0x114 and the camera S+0x88 are members, released after this body: L, no effect in them)
    while (!stages.empty()) DeleteStage(stages.front().get());   // orig: 0x000C85F2, head first
    DeleteObjects();                                  // orig: 0x000C8612: `while (S+4) S+4->vf00(1)`
    if (defs) defs->DeleteAll();                      // orig: 0x000C8630 / 0x000C8647 / 0x000C8660 (S+0x3C, +0x38, +0x34)
    if (script) {
        // orig: 0x000C87B6: ScriptProgram::Stop (S+0xA8): the level script's exit function runs to completion with
        // the animations, objects and stages gone and the trigger manager still there (its natives act on g_level,
        // which still is this level; boot_pause_quit frame 1248: FsPlayLevelAnim 232 on the doors, 103, 178).
        // (port guard: a level the natives do not act on, e.g. a test's earlier level, runs no exit function)
        if (script::NativeLevel() == this) script::ProgramStop(script);
        script::ProgramDestroy(script);
        mem::OperatorDelete(script);
        script = nullptr;
    }
    scriptTriggers.reset();                           // orig: 0x000C87C4: 0x000B88B0 after the script stops
    // orig: 0x000C87E8: S+0xE4 released (the environment texture effect; its other reference, the dynamic node's
    // child slot, went with the scene's children 0x000C871E: the port detaches it here)
    if (envTextureEffect) {
        if (ni::NiNode* p = envTextureEffect->parent) p->DetachChild(envTextureEffect.Get());
        envTextureEffect = nullptr;
    }
    // orig: 0x000C8943..0x000C89BF: the dialogue sounds S+0x100 released (Sound_Release 0x000C89B1) in the map's walk
    // order (buckets 0..36 by the string hash 0x000C3700, each chain newest first), the map emptied (0x0004A3D0)
    {
        auto hash = [](const std::string& k) {
            u32 h = 0;
            for (char c : k) h = h * 33 + static_cast<u32>(static_cast<s32>(static_cast<signed char>(c)));
            return h % 37;
        };
        for (u32 b = 0; b < 37; ++b)
            for (auto it = dialogueSoundOrder.rbegin(); it != dialogueSoundOrder.rend(); ++it)
                if (hash(*it) == b) Sound().Release(dialogueSounds[*it]);
        dialogueSounds.clear();
        dialogueSoundOrder.clear();
    }
    edgeSets.clear();                                 // orig: 0x000CD170 at 0x000C89C4 (each set: delete +4; delete set)
    // orig: 0x000C8A1F: the texture and pixel-data sharing lists cleared and the sharing on again
    ni::SetTextureSharing(false);                     // 0x00118930(0) / (1)
    ni::SetTextureSharing(true);
    ni::SetPixelDataSharing(false);                   // 0x001119D0(0) / (1)
    ni::SetPixelDataSharing(true);
    if (dialogue) dialogue->Release();                // orig: 0x000C888F DAT_002EBDAC->0x000D7A10
}

void LevelData::DetachObject(GameObject& object) {    // orig: 0x000C3FF0
    ni::NiNode* n = object.GetNode();
    if (n->parent) n->parent->DetachChild(n);
}

Stage* LevelData::FindStage(const std::string& stageName) const {   // orig: 0x000C3F70
    for (const auto& s : stages)
        if (!s->name.empty() && crt::StrICmp(s->name.c_str(), stageName.c_str()) == 0) return s.get();
    return nullptr;
}

bool LevelData::StageHasActiveCheckpoint(const Stage* stage) const {   // orig: 0x000C41E0
    if (!stage || !roomGroup || !triggers) return false;
    const size_t n = respawnPoints.size();            // (+0x7C read once)
    for (size_t i = 0; i < n; ++i) {
        TriggerRec* rec = respawnPoints[i];
        if (!rec || !rec->target) continue;
        ni::NiPoint3 p = triggers->Position(*rec);    // 0x000DA7E0
        ni::NiRoom* room = roomGroup->FindRoomContaining(p);
        if (!room) {
            p.y = ni::detail::F(double(p.y) + 2.0f);  // [0x00293B20]
            room = roomGroup->FindRoomContaining(p);
        }
        for (const auto& s : stages)
            if (s->ContainsRoom(room)) {              // the first match decides
                if (s.get() == stage) return true;
                break;
            }
    }
    return false;
}

void LevelData::AddPickupValue(s32 v) {               // orig: 0x000C3360
    if (quickLoading) return;
    const s32 old = pickupValue;
    pickupValue = old + v;
    for (s32 i = old + 1; i <= pickupValue; ++i)      // (signed; nothing when v <= 0)
        if (i > 0 && i % 25 == 0) {                   // CDQ; IDIV 0x19
            if (hud) hud->Command("1up", "show", -1.0f);   // 0x000E5B90(S+0x118, 0x002A3478, 0x0029A420, -1.0f)
            ++lives;
            if (player) player->SoundAt(0x7B, nullptr, 3);   // one rand() (Pickup_ExtraLife)
            return;                                   // only the first multiple of 25 per call
        }
}

AiNet* LevelData::FindAINet(const char* n) const {   // orig: 0x000313E0
    if (!n) return nullptr;
    for (const auto& net : aiNets)
        if (crt::StrICmp(net->name.data(), n) == 0) return net.get();
    return nullptr;
}

void LevelData::AddEnemy(u16 newId) {                 // orig: 0x000C7850
    for (u16 e : enemyIds)
        if (e == newId) return;
    enemyIds.push_back(newId);                        // (capacity growth 0x000C2220: +1 per add)
    if (newId) ++enemyCount;
}

void LevelData::AddKilled(u16 newId) {                // orig: 0x000C7790 (Character::Damage's death branch)
    for (u16 e : killedIds)
        if (e == newId) return;
    killedIds.push_back(newId);                       // (the Session ctor's new(2): capacity 1; growth 0x000C2220: +1)
    if (newId) ++killedCount;
}

u8 LevelData::NibblerState(u32 recId, u32 b) {        // orig: 0x00077770
    char key[0x100] = {};
    std::snprintf(key, sizeof key, "$%03d:%03d", static_cast<int>(recId), static_cast<int>(b));
    auto [it, inserted] = nibblerStates.try_emplace(key, u8{0});
    return inserted ? u8{0} : it->second;
}

bool LevelData::LiveStage(const Stage* stage) const {
    for (const auto& s : stages)
        if (s.get() == stage) return true;
    return false;
}

void LevelData::DeleteStage(Stage* st) {              // orig: 0x000D1180 -> ~Stage 0x000D0BF0
    auto it = std::find_if(stages.begin(), stages.end(), [st](const auto& p) { return p.get() == st; });
    if (it == stages.end()) return;
    st->Unload();                                     // 0x000D0C28: still listed (maybe current)
    if (currentStage == st) currentStage = nullptr;   // 0x000D0C3A
    std::shared_ptr<Stage> keep = std::move(*it);
    stages.erase(it);
    keep.reset();                                     // ~Stage: the scene detach, ResolveAll
}

Stage* LevelData::CreateStage(const std::string& stageName) {
    stages.push_back(std::make_shared<Stage>(*this, stageName));
    if (!currentStage) currentStage = stages.back().get();
    return stages.back().get();
}

// ---- the HUD timer (docs/analysis/rodent_port.md 7) ---------------------------------------------------------------
void HudTimer::Command(const char* cmd, f32 v) {      // orig: vf04 0x000EC7B0
    using ni::detail::F;
    if (std::strncmp(cmd, "play", 4) == 0) {          // 0x002A5690
        running = 1;
        paused = 0;
        return;
    }
    if (std::strncmp(cmd, "pause", 4) == 0) {         // 0x002A5688 ("paus")
        running = 1;
        paused = 1;
        return;
    }
    if (std::strncmp(cmd, "set", 4) == 0) {           // 0x002A5684
        time = v;                                     // (raw dword)
    } else if (std::strncmp(cmd, "add", 4) == 0) {    // 0x002A2104
        time = F(double(v) + time);
    } else {
        if (std::strncmp(cmd, "stop", 4) == 0) running = paused = 0;   // 0x002A567C
        return;
    }
    if (v > 0.0f) timedOut = 0;                       // TEST AH,0x41; JNE: only a positive value clears it
}

void HudTimer::Update() {                             // orig: vf01 0x000EC760
    using ni::detail::F;
    if (!running || paused) return;
    const double x = double(time) - g_frameStep;
    time = F(x);                                      // FST
    if (x < 0.0 || x == 0.0) {                        // FCOMP 0; TEST AH,0x41; JP
        time = 0;
        if (swamp && !timedOut) timedOut = 1;
    }
}

}  // namespace fut::game
