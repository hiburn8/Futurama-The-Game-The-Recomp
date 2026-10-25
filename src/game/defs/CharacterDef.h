// Character definitions, decompiled from default.xbe (translation unit 0x0003E220-0x00042950).
//
// A level's defs.nif names its characters as "CharacterDef::<name>" nodes. The level loader creates one definition
// per name (PlayerDef for the player characters, SimpleNPCDef for "kamikaze", GenericNPCDef otherwise; see
// Session::OnStreamItemLoaded kind 9) and streams Chars\<name>.ucf. When that file is loaded, the definition's
// stream callback (0x00041E30) assembles the character:
//   * the file's top-level NiNodes are attached to a new root and renamed "<path>:<k>", texture effects are attached
//     too (and removed again later), data files (NiUDSFileObject) are taken: char.cfg (movement values and the
//     animation set), "lipsync*", "IngameEvent", "HUD_Head" and lip-sync key files;
//   * without a char.cfg every top-level NiSequence becomes an animation;
//   * the root is named "CharacterDef::<name>" and its materials / z-buffer / vertex-colour state is prepared.
// Every animation keeps its sequence, playback speed, sound events (from the sequence's text keys) and a
// transition to every other animation (blend parameters from char.cfg).
//
// Verified against the original code under emulation for every character of the shipped levels
// (tests/game/CharacterDefTests.cpp, oracle tools/difftest/chars_oracle.py).
#pragma once

#include "engine/stream/StreamManager.h"
#include "game/data/ConfigFile.h"
#include "game/defs/Defs.h"
#include "game/defs/IngameEvent.h"

#include <array>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fut::game {

class Liquid;

struct AnimEntry;

struct AnimEvent {                                     // orig: 0x14 bytes, vtable 0x00296C4C
    std::string text;                                  // orig: +0x04, the text key ("morph:jland", ...)
    u32 handle = 0;                                    // orig: +0x08, the playing sound (set at run time)
    s32 soundEvent = 0;                                // orig: +0x0C, sound event of "<character>:<text + 6>"
    f32 time = 0.0f;                                   // orig: +0x10, text key time
};

struct AnimTransition {                                // orig: 0x14 bytes, vtable 0x00296C50 (created by 0x00040100)
    AnimEntry* target = nullptr;                       // orig: +0x04
    // orig: +0x08, NULL until char.cfg sets it: "blend:" -> the given name, "morph:" / "immed:" -> "".
    std::string type;
    bool hasType = false;
    f32 time = 0.0625f;                                // orig: +0x0C
    u32 mode = 1;                                      // orig: +0x10: 0 immediate, 1 blend, 2 morph
    // orig: 0x0003EDD0. A time <= 0 becomes 0.0625.
    void Set(AnimEntry* to, const char* typeName, f32 t, u32 m);
};

struct AnimEntry {                                     // orig: 0x44 bytes, vtable 0x00296CB4, ctor 0x0003F4A0
    std::vector<AnimEvent> events;                     // orig: NiTPrimitiveArray at +0x04 (data +0x08, size +0x10)
    std::vector<AnimTransition> transitions;           // orig: NiTPrimitiveArray at +0x1C (data +0x20, size +0x28)
    f32 speed = 1.0f;                                  // orig: +0x34, frequency of the sequence's main controller
    ni::NiPointer<ni::NiSequence> sequence;            // orig: +0x38
    u8 unknown3C = 0;                                  // orig: +0x3C
    // orig: +0x40, an 8-byte block allocated by the first "damage" / "damagetype" line of the animation's char.cfg
    // block. The other half stays uninitialised heap memory in the original; the port keeps it 0.
    bool hasDamage = false;
    f32 damage = 0.0f;                                 // block +0
    s32 damageType = 0;                                // block +4

    const char* Name() const;                          // orig: 0x0003F0B0 (the sequence's name)
    AnimTransition* TransitionTo(const AnimEntry* to); // orig: 0x0003F1E0 returns its +0x10 (mode)
};

// Lip-sync / facial key file ("LipSyncKeyFile v1.2" or "v1.3"; orig: 0x2C bytes, vtable 0x002A15E8 over NiRefObject,
// ctor 0x0009CB90, loader 0x0009CBD0, dtor 0x0009D170). The records keep the file's words (the floats as bits):
//   records1: the mouth phonemes {code, time b, weight c} in time order (the face's 'N' / 'A' / 'H' shapes);
//   records2: the expression / head-turn channels {code, centre T v[0], amplitude v[1], hold v[2], attack v[3],
//             release v[4]} (codes 'A' / 'H' turn the head, the others are the face's 'E' shapes);
//   records3: unused by the evaluation (L: event data); records4 (v1.3): the eyes' uv offset {u, v, time}.
struct LipSyncKeyFile : ni::NiRefObject {
    struct Record1 { u8 a = 0; u32 b = 0, c = 0; };                   // orig: 0x0C bytes (file: 9), 0x0009D550
    struct Record2 { u8 a = 0; std::array<u32, 5> v{}; };            // orig: 0x18 bytes (file: 21), 0x0009D5A0
    struct Record3 { u32 field0 = 0, field4 = 0; std::vector<u8> data; };   // orig: 0x10 bytes, 0x0009D620
    struct Record4 { std::array<u32, 3> v{}; };                       // orig: 0x0C bytes, 0x0009D6C0 (v1.3 only)
    u32 version = 0;                                   // port-only: 1 = v1.2, 2 = v1.3
    std::vector<Record1> records1;                     // orig: +0x08 / count +0x0C
    std::vector<Record2> records2;                     // orig: +0x10 / count +0x14
    std::vector<Record3> records3;                     // orig: +0x18 / count +0x1C
    std::vector<Record4> records4;                     // orig: +0x20 / count +0x24
    u32 unknown28 = 0;                                 // orig: +0x28
    bool Load(std::span<const u8> data);               // orig: 0x0009CBD0

    // orig: 0x0009CA40: the largest record-1 time and record-2 end (T + release + hold), from 0 (the track length).
    f32 Duration() const;
    // orig: 0x0009CAB0: the record-1 index whose time is at or before t (the last one from its time on), -1 before
    // the first.
    s32 FindPhoneme(f32 t) const;
    // orig: 0x0009CFD0: the channels of the record-2 envelopes active at t, summed by code (at most `max`).
    s32 Channels(char* codes, f32* values, s32 max, f32 t) const;
    // orig: 0x0009D0C0: the records-4 uv offset at t (linear between the keys, (0, 0) before the first).
    ni::NiPoint2 EyeOffset(f32 t) const;
};

// The animation names indexed by animation id (orig: pointer table 0x002D6228, count 0x002D6608): read from the
// user's default.xbe (core/XbeImage.h). AnimationName(id): the name, "" for an id outside 0..kAnimationCount-1.
constexpr u32 kAnimationCount = 248;
const char* AnimationName(u32 id);

class CharacterDef : public ObjectDef, public stream::Client {   // orig: ctor 0x000409C0, 0x144 bytes, vtable 0x00296CEC
public:
    explicit CharacterDef(std::string characterName);  // orig: 0x000409C0
    std::string_view ClassName() const override { return "CharacterDef"; }
    bool IsType(u32 t) const override { return (t & 0x1) == t; }   // orig: 0x00040D50
    const char* TypeName() const override { return "StreamDefinition"; }

    // orig: slot 1 of the stream interface (0x0003FE00): requests Chars\<name>.ucf when there is no stream item.
    void RequestStream();
    // orig: the stream client at +0x18 (0x00041E30): the path, then OnStreamLoaded with `streamContext`.
    bool OnStream(stream::Request& r) override;
    std::shared_ptr<stream::Request> request;          // orig: +0x24
    DefContext* streamContext = nullptr;
    // orig: the success branch of the stream callback 0x00041E30 (stream item +0x1C != 0) with the loaded file.
    void OnStreamLoaded(ni::NiStream& stream, DefContext& ctx);

    AnimEntry* AddAnimation(ni::NiSequence* sequence, DefContext& ctx);   // orig: 0x000407E0
    AnimEntry* FindAnimation(const char* sequenceName) const;            // orig: 0x0003F340 (strcmp)
    AnimEntry* FindAnimationById(s32 id) const;                          // orig: 0x0003F280
    bool IsAnimationNameFree(const char* sequenceName) const;            // orig: 0x0003F220 (stricmp)
    // orig: 0x00041A50 (char.cfg as a NiUDSFileObject) / 0x000411A0 (the parsed file).
    bool LoadConfig(ni::NiUDSFileObject& file, ni::NiStream* stream, DefContext& ctx);
    bool ApplyConfig(const ConfigFile& cfg, ni::NiStream* stream, DefContext& ctx);

    // ---- data (orig offsets in the definition object) ----
    f32 maxHealth = 100.0f;                            // orig: +0x60 (char.cfg "maxhealth", an integer)
    f32 jumpSpeed = 45.0f, jumpWalkSpeed = 45.0f, jumpRunSpeed = 45.0f;   // orig: +0x64 +0x68 +0x6C
    f32 sneakSpeed = 5.0f, walkSpeed = 10.0f, runSpeed = 15.0f;           // orig: +0x70 +0x74 +0x78
    // orig: +0x7C, not set by the constructor (heap garbage until char.cfg sets it); the port starts at 0.
    f32 runJumpSpeed = 0.0f;
    f32 ladderClimbSpeed = 0.0f, ledgeSlideSpeed = 0.0f;                  // orig: +0x80 +0x84
    f32 rotSpeed = 4.0f, aimSpeed = 1.0f;                                 // orig: +0x88 +0x8C
    f32 boundWidth = 1.5f, boundHeight = 4.5f, boundDepth = 1.5f;         // orig: +0x90 +0x94 +0x98
    f32 boundCenter = 4.5f, fpsHeight = 4.0f;                             // orig: +0x9C +0xA0
    f32 ledgeGrabDistance = 1.0f, ledgeGrabHeight = 1.0f;                 // orig: +0xA4 +0xA8
    f32 stepHeight = 1.8f;                                                // orig: +0xAC
    f32 cameraHeight = 6.5f, lookAtHeight = 3.5f;                         // orig: +0xB0 +0xB4
    f32 cameraDistance = 15.0f, cameraLookAhead = 0.0f;                   // orig: +0xB8 +0xBC
    std::vector<std::unique_ptr<AnimEntry>> animations;                   // orig: NiTArray at +0xC0 (data +0xC4)
    // Number of variants present (orig: counted by ApplyConfig from the animation-name table).
    s32 idleCount = 1;                                 // orig: +0xD8, ids 0-8 ("bender": 2; at least 1)
    s32 longIdleCount = 0;                             // orig: +0xDC, ids 9-12
    s32 dyingCount = 1;                                // orig: +0xE0, ids 77-81
    s32 damageCount = 0;                               // orig: +0xE4, ids 71-73
    s32 control = 0;                                   // orig: +0xE8 (char.cfg "control")
    s32 attack1Count = 1;                              // orig: +0xEC, ids 47-51
    s32 attack2Count = 1;                              // orig: +0xF0, 1 + ids 52-56
    s32 closeAttack1Count = 0;                         // orig: +0xF4, ids 59-60
    s32 closeAttack2Count = 0;                         // orig: +0xF8, ids 61-62
    std::string path;                                  // orig: +0x108 "Chars\<name>.ucf", "" once loaded
    ni::NiPointer<ni::NiUDSFileObject> lipSyncFile;    // orig: +0x10C (name containing "lipsync")
    ni::NiPointer<ni::NiUDSFileObject> ingameEventFile;   // orig: +0x110 ("IngameEvent")
    IngameEventSet ingameEvents;                       // orig: +0x28 (parsed by the first instance's Init)
    ni::NiPointer<ni::NiUDSFileObject> hudHeadFile;    // orig: +0x114 ("HUD_Head")
    // orig: hash map at +0x134 (37 buckets): key files by mode, 0 = "single_blink.key", 2 = "electric.key" (mode 1
    // compares "single_blink.key" again and cannot be reached).
    std::map<u32, ni::NiPointer<LipSyncKeyFile>> keyFiles;
    struct SoundEntry {                                // orig: 8-byte records
        u32 sound = 0;                                 // the loaded sound's handle
        u32 id = 0;                                    // what the sound is for (attack 10, impact 2, death 0, ...)
    };
    std::vector<SoundEntry> sounds;                    // orig: NiTArray at +0x11C
    void AddSound(const char* path, u32 id, DefContext& ctx);   // orig: 0x000403F0 (Sound_Load(path, 0))
    // orig: +0x118, the file's texture effect while the stream callback runs (a top-level one, then the last one
    // RemoveTextureEffects detaches); released at the end of the callback (0x000424D0), so the characters always use
    // the level's environment map. Kept for BuildModel's branch that would clone it.
    ni::NiPointer<ni::NiTextureEffect> envEffect;
    // orig: 0x00040450 AcquireModel(&out, idx): a clone of the model root's child `idx` (names copied exactly),
    // reusing a cached clone nobody else holds; nullptr without a model.
    ni::NiPointer<ni::NiAVObject> AcquireModel(u32 idx);
    std::vector<ni::NiPointer<ni::NiAVObject>> modelCache;   // orig: NiTArray at +0x48 (data +0x4C, size +0x54)

private:
    void CreateEvents(AnimEntry& entry, DefContext& ctx);                 // orig: 0x000401B0
    bool TakeDataFile(ni::NiAVObject* object, ni::NiStream* stream, DefContext& ctx);   // orig: 0x00041B00
    void RemoveTextureEffects(ni::NiNode* node);                         // orig: 0x0003F730
    ni::NiNode* EnsureRoot();
};

class PlayerDef : public CharacterDef {                // orig: ctor 0x0005E610, 0x144 bytes (no own fields)
public:
    using CharacterDef::CharacterDef;
    std::string_view ClassName() const override { return "PlayerDef"; }
    bool IsType(u32 t) const override { return (t & 0x3) == t; }   // orig: 0x0005E670
    // orig: 0x0005E6B0 (Character.cpp): the player class by name, owner, Init when the model is loaded, label.
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
};

class SimpleNPCDef : public CharacterDef {             // orig: ctor 0x0005F630, 0x164 bytes ("kamikaze" only)
public:
    using CharacterDef::CharacterDef;
    std::string_view ClassName() const override { return "SimpleNPCDef"; }
    // orig: the 0x0005F630 part after the CharacterDef constructor. +0x144 is set to 0 only for "kamikaze" (the only
    // name the level loader gives this class); the wasp sounds load when it is 0.
    void Construct(DefContext& ctx);
    u32 type144 = 0;                                   // orig: +0x144
    std::array<u32, 4> sounds148{};                    // orig: +0x148, +0x14C, +0x158, +0x15C: wasp hover / dive /
                                                       // warning / launch sound handles
    f32 value160 = 100.0f;                             // orig: +0x160 (500 with the wasp sounds)
    // orig: +0x150 / +0x154, never written by the constructor (heap garbage; SimpleNpc::Damage tests +0x150 with
    // Sound_HasVoices): 0 here (a UB-avoiding divergence).
    u32 sample150 = 0, value154 = 0;
    // orig: vf14 0x00060320 (SimpleNpc.cpp): a SimpleNpc, owner, Init when the model is loaded, label.
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
};

class ProjectileDef;
class FlareDef;
class SpriteEmitterDef;
class LevelData;

// orig: ctor 0x0004B5B0, 0x1BC bytes. The constructor sets defaults, then a per-name block (a _stricmp chain over 45
// names) sets values, adds sounds and creates the weapon definitions (lasers, projectiles, muzzle flares, particle
// emitters; they append themselves to the level's definition list). What the values mean is not identified yet; they
// are named by offset.
class GenericNPCDef : public CharacterDef {
public:
    using CharacterDef::CharacterDef;
    std::string_view ClassName() const override { return "GenericNPCDef"; }
    bool IsType(u32 t) const override { return (t & 0x200001) == t; }   // orig: 0x0004E910
    // The part after the CharacterDef constructor. `levelName` is the level object's name (g_level +0x0C).
    void Construct(DefContext& ctx, const LevelData& level, const std::string& levelName,
                   std::vector<std::unique_ptr<Def>>& defs);

    u32 value144 = 0, value148 = 0;                    // orig: +0x144, +0x148
    f32 value14C = 250.0f, value150 = 2.0f, value154 = 4.0f;   // orig: +0x14C..+0x154
    ProjectileDef* weapon158 = nullptr;                // orig: +0x158 (a LaserDef or a ProjectileDef)
    ProjectileDef* projectile15C = nullptr;            // orig: +0x15C
    FlareDef* flare160 = nullptr;                      // orig: +0x160
    SpriteEmitterDef* particles164 = nullptr;          // orig: +0x164
    s32 value168 = 30, value16C = 10;                  // orig: +0x168, +0x16C
    f32 value174 = 3.5f, value178 = 3.5f;              // orig: +0x174, +0x178
    // orig: bytes +0x170..+0x18E; +0x18D and +0x18E default to 1 (+0x172, +0x173 and +0x17F are not set).
    std::array<u8, 0x1F> flags170{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1};
    f32 value190 = 100.0f, value194 = 0.33f;           // orig: +0x190, +0x194 (0x3EA8F5C3)
    f32 value198 = 100.0f, value19C = 50.0f;           // orig: +0x198, +0x19C
    u8 flag1A0 = 1;                                    // orig: +0x1A0
    // orig: +0x1A4 (0 from the constructor): the lavaman's Liquid, made by the first lavaman spawn of the level
    // (GenericNPCDef::Spawn 0x0004A997) and pointed at each lavaman model's "TM_MeshShape:0" by its BuildModel. A
    // plain pointer as in the original (the liquid is owned by the level object; L: it is never deleted while a
    // lavaman lives, its geometry being the lavaman's mesh).
    Liquid* liquid1A4 = nullptr;
    // orig: +0x1A8 a NiTPointerMap with 37 buckets (empty after construction), +0x1B8 = 1.
    u8 flag1B8 = 1;
    // orig: 0x0004A430 (GenericNpc.cpp): the NPC class by definition name (Nibbler and Tank ported).
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
};

// orig: the class choice of the level loader (0x000C62EF).
std::unique_ptr<CharacterDef> CreateCharacterDef(const std::string& name);

// orig: 0x0003F8D0: the whole model: material emissive = diffuse, z-buffer and shade properties removed, vertex
// colours from the vertices without lighting (a new property when there is none), node effects detached; the
// children of a "QDOutline" node are left alone.
void PrepareCharacterProperties(ni::NiAVObject* object);
// orig: 0x0003FB30: one object (the root's children): material emissive = diffuse, z-buffer test and write on (a new
// property when there is none), shade flag 1 off, vertex colours from the material without lighting.
void PrepareCharacterPartProperties(ni::NiAVObject* object);

}  // namespace fut::game
