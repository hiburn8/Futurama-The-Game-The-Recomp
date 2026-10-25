// Characters: the objects character definitions spawn (the player classes, later NPCs). Decompiled from default.xbe;
// docs/analysis/player_spawn.md, docs/analysis/player_spawn_deps.md.
//
// Ported (the load-time state of the player): PlayerDef::Spawn (class switch), the Character / Player constructors'
// state the load reads (lists, node, health, bounds, type, the player's physics body, the rotation),
// Init (ambient light, keyframe manager with the definition's sequences, model clone with the environment-map effect,
// the subtree flag pass, PhysicController, light-manager target) and the lip-sync face (LipSync.h).
// The IngameEvent file is parsed by the first instance (IngameEvent.h, with its rand() calls). NOT PORTED: the
// camera,
// HUD, input, AI agent, the projected shadow's texture effect (the shadow's camera, texture, accumulator and pass
// are ported: RenderShadow), the level-specific extras other than the rat ride (RatRide.h).
#pragma once

#include "engine/ni/NiRenderer.h"
#include "engine/stream/StreamManager.h"
#include "game/input/Pad.h"
#include "game/object/GameObject.h"
#include "game/object/LipSync.h"
#include "game/physics/Physics.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace fut::ni {
class NiKeyframeManager;
class NiSequence;
struct RayHit;
}

namespace fut::game {

class CharacterDef;
class CannonCtrl;
class ChickenWalker;
class Weapon;
class RatRide;
class Stage;
struct ScriptTrigger;
struct AnimEntry;
class Character;
struct IngameEventContext;

// orig: PhysicController (ctor 0x00037730, vtbl 0x00295950, 0x70 bytes): an NiTimeController that pins a root bone's
// translation; appended after the keyframe manager, inactive at load.
class PhysicController : public ni::NiTimeController {
public:
    static constexpr std::string_view kClassName = "PhysicController";
    std::string_view ClassName() const override { return kClassName; }
    explicit PhysicController(Character& c);         // orig: 0x00037730
    void Update(f32 time) override;                   // orig: vf14 0x000377B0
    void SetBone(const char* boneName);               // orig: vf54 0x00037820

    Character* character;                             // orig: +0x3C
    u8 applied = 0;                                   // orig: +0x40
    ni::NiPoint3 saved;                               // orig: +0x50 (the bone's local translation at SetBone)
    ni::NiAVObject* bone = nullptr;                   // orig: +0x68 (no reference)
    u8 byte6C = 0, keepY = 0;                         // orig: +0x6C / +0x6D
};

// orig: MasterPropertyAccumulator (ctor 0x00038000, vtable 0x00295B68; Character +0x5B4): the player's shadow pass hands
// it the visible geometry, which it draws at once with its own property list swapped in.
class MasterPropertyAccumulator : public ni::NiAccumulator {
public:
    void FinishAccumulating(ni::NiCamera&) override { camera = nullptr; }   // orig: vf30 0x00031E40
    bool RegisterObject(ni::NiGeometry& geometry) override;   // orig: vf34 0x0003B080
    std::vector<ni::NiPointer<ni::NiProperty>> properties;    // orig: +0x0C (NiPropertyList, head first)
};

class Character : public GameObject {                 // orig: base ctor 0x0003D220, vtbl 0x00295DD8
public:
    Character(LevelData& level, CharacterDef& def);   // orig: 0x0003D220
    ~Character() override;
    bool Init() override;                             // orig: 0x00038070 (slot 1 thunk 0x0004F7E0)
    // orig: SetPosition 0x000353B0 / SetRotation 0x00033D20: only the physics body and +0x570, never the node.
    void SetPosition(const ni::NiPoint3& p) override;
    void SetRotation(const ni::NiMatrix3& m) override;
    // orig: vf20 0x00035400: the step's displacement d becomes body velocity (d / g_frameStep added; no body test).
    void Translate(const ni::NiPoint3& d) override;
    ni::NiPoint3 GetWorldPosition() const override;   // orig: 0x00035440 (body minus the height offset)
    // vf38: +0x570 copied out (12 dwords: the console's 48-byte matrix), not the node's world rotation.
    ni::NiMatrix3 GetWorldRotation() const override { return rotation; }   // orig: 0x00033D40
    // vf3C: with +0x60, the model's collision meshes into the new owner stage's BSP (jmp 0x000350B0).
    void OnAttach() override;                         // orig: 0x00035270
    // vf40: with +0x60 and a model, out of the old owner stage's BSP (owner vf0C(2), the BSP, 0x000FC2D0).
    void OnDetach() override;                         // orig: 0x00035280
    // vf70: body ? body vf48 0x000AF1F0 (byte +0x45, supported) : 0 (the players: 0x0004FFE0).
    virtual bool OnGround() const;                    // orig: 0x00033D10
    virtual void BuildModel();                        // orig: vf64 0x0003DF70
    virtual bool ShouldUnloadModel() const;           // orig: vf6C 0x00035020
    virtual void UnloadModel();                       // orig: vf68 0x0003B7D0
    // orig: 0x000350B0 (thiscall, RET 0; BuildModel 0x0003E0CA when +0x60): the model's collision meshes added to the
    // owner stage's BSP by definition name ("patrol_guard" / "spinbot": four named parts, "chickenwalker": none,
    // "tentacle_dude": the model with 0x80, else the model with 8). docs/analysis/npc_scripted_port.md 2.0.1.
    void AddModelCollisionMeshes();
    // orig: 0x00035230 (thiscall, RET 0; UnloadModel 0x0003B7FC when +0x60): the first record of each NiTriShape of the
    // model removed from the owner stage's BSP again (0x000FC2D0).
    void RemoveModelCollisionMeshes();
    void DeleteInventoryEffects();                    // orig: 0x00034A50 (inventory objects with IsType(0x8000))
    // orig: 0x00035080 (thiscall(float), RET 4): the height offset follows the node scale (the body is not moved).
    void SetScale(f32 s);
    // orig: 0x00033CF0 (thiscall(int), RET 4): vf68 then +0x784.
    void SetModelVariant(u32 v) {
        UnloadModel();
        modelVariant = v;
    }
    virtual void AfterAnimation() {}                  // orig: vf60 (players: 0x000B0420)
    // orig: vf78 0x0003D7A0 -> 0x0003D7C0: UpdateRoom's point for characters (the body's centre height).
    ni::NiPoint3 CharacterPoint() const override;
    // orig: 0x000364C0 (thiscall(key, b), RET 8): the pose does not advance this step; with a text-key name the current
    // sequence (and a summed one) is set to the time b after that key (docs/analysis/npc_scripted_port.md 2.0.3).
    void HoldAnimation(const char* key, f32 b);
    void AddDefSequences();                           // orig: 0x0003DEA0
    void AddSequence(ni::NiSequence* seq);            // orig: 0x0003DCE0

    // ---- per-step update (CharacterUpdate.cpp; docs/analysis/character_update.md 2) ----
    // orig: vf58 0x0003EA90, pass 0 before the physics step, pass 1 after it; false = delete the character (+0x628).
    // (virtual: SimpleNpc replaces it, vf58 0x00060480)
    virtual bool Update(f32 dt, int pass);
    virtual void Think(f32 dt) { (void)dt; }          // orig: vf54 (NPC AI: NOT PORTED)
    virtual void UpdateAnimation(f32 dt) { (void)dt; }   // orig: vf5C (NPC behaviours: NOT PORTED)
    virtual void OnTextKey(f32 keyTime, const char* text) { (void)keyTime, (void)text; }   // orig: vfE8
    void PrepareRender();                             // orig: 0x00039890 (the pose, once per rendered frame)
    bool IsDead() const { return health <= 0.0f; }    // orig: vfA8 0x00033D80 (NaN: alive)
    bool IsDyingAnim() const;                         // orig: vfAC 0x00035DD0
    bool IsIdling() const;                            // orig: vfA0 0x00035BD0
    virtual bool WalkRunPlaying() const;              // orig: vf9C 0x00035AE0 (ids 13-17, 21-23)
    virtual bool IsFallAnim() const { return false; }   // orig: vf140 (players: 0x000501A0)
    // The holder of a player (Player +0x7A0, read by the shared character code only for a definition of type 2);
    // null for every other character (a port accessor: the original reads the player's field).
    virtual Character* HeldBy() const { return nullptr; }
    // orig: 0x000345B0 (thiscall(out), RET 4): vf34 raised by F(+0x4D4 + +0x4D0) (y = F(that + pos.y)).
    ni::NiPoint3 TopPoint() const;
    ni::NiSequence* SeqById(s32 anim) const { return anim >= 0 && anim < 256 ? byAnimation[anim] : nullptr; }   // 0x00033E00
    bool IsAnimPlaying(s32 anim) const;                 // orig: 0x00033E20 (current or overlay)
    ni::NiSequence* FindSequence(const char* name) const;   // orig: 0x00039BA0 (the name map)
    bool IsPlayingName(const char* name) const;       // orig: 0x00039D60 (FindSequence(name) is current or overlay)
    void PlayOverlay(ni::NiSequence* seq, bool loop); // orig: 0x00036780 (the upper-body slot +0xB0, priority 1)
    void ClearOverlay();                              // orig: 0x000363C0
    // orig: 0x0003A130 (sequence, play once, interruptible, force, start time in character time).
    void PlayAnimation(ni::NiSequence* seq, bool once, bool interruptible, bool force, f32 start);
    void PlayAnimById(s32 anim, bool once, bool intr, bool force, f32 start) {   // orig: 0x0003CE70
        PlayAnimation(SeqById(anim), once, intr, force, start);
    }
    void StopAnimation();                             // orig: 0x00039D90
    // orig: 0x0003A4B0 (thiscall(seq, loop), RET 8; docs/analysis/npc_scripted_port.md 2.0.2): the sequence as the
    // only animation, without 0x0003A130's transition logic (null: everything deactivated).
    void PlaySequenceDirect(ni::NiSequence* seq, bool loop);
    void FinishTransitions();                         // orig: 0x000363F0
    void SumBlend(ni::NiSequence* a, ni::NiSequence* b, f32 w);   // orig: 0x00039ED0
    void PhaseSync(ni::NiSequence* from, ni::NiSequence* to);   // orig: 0x00036700
    void AnimTick(f32 dt);                            // orig: 0x0003B5E0
    AnimEntry* EntryOf(const ni::NiSequence* seq) const;   // orig: CharacterDef::FindAnimationBySequence 0x0003FD20
    static void SetFrequency(ni::NiSequence* seq, f32 f);   // orig: 0x00036240
    // orig: 0x0003ABA0 (thiscall(NiPoint3*), RET 4): face the point (y ignored) at once (vf24).
    void TurnToPoint(const ni::NiPoint3& p);
    // orig: 0x0003AD90 (thiscall(NiPoint3*, f32 rate), RET 8): turn towards the point, at most 1.5 * rate radians.
    void TurnToward(const ni::NiPoint3& p, f32 rate);
    void BeginRootMotion();                           // orig: 0x000368A0
    void EndRootMotion();                             // orig: 0x000369E0
    // orig: CharacterDef 0x0003FEA0 (id, position, flags, index, attached node): the definition's sounds for `id`;
    // flags & 2 picks one with rand(), else `index`; attached / 2D (no position) / 3D, flags & 4 = loop. The voice id.
    s32 SoundAt(u32 sound, const ni::NiPoint3* pos, u32 flags, u32 index = 0, ni::NiAVObject* attach = nullptr);
    // The voice tests the game code inlines at every site (the level's sound service, LevelData::Sound()):
    // `v && Voice_Exists(v)` (0x000C9900), and the stop `if (v && Voice_Exists(v)) { Voice_Stop(v); v = 0; }`.
    bool VoiceLive(u32 v) const;
    void StopVoiceAndClear(u32& v);
    // `if (Voice_Exists(v)) { Voice_Stop(v); [v = 0;] }` without the null test (Voice_Exists(0) is called).
    void StopVoiceIfExists(u32& v, bool clear);
    // `Voice_SetMaxDistance(v, d)` (0x000C9CA0) after a SoundAt: unconditional at every site (a 0 id finds no voice).
    void VoiceMaxDistance(s32 v, f32 d);
    // vf88 (the players' slot 0x0005A770 only aligns the stack and calls 0x000357E0).
    bool Heal(f32 amount);                            // orig: 0x000357E0 / 0x0005A770
    // orig: 0x000358D0 (FsCharacterSay 146 / 147): builds the line's dialogue (a lip track on the character's
    // lip-sync player from the level's key files, a sound track from the dialogue sound map) and hands it to the
    // dialogue display (Dialogue.h) with the priority; returns its duration (0 when nothing was built).
    f32 Say(const char* textId, const char* dbName, s32 priority);
    // orig: vfC8 (players 0x00051250): a pickup arrived in the inventory.
    virtual void OnPickup(GameObject& obj) { (void)obj; }
    // What IngameEventSet::Parse / Trigger read from the level (the original's globals).
    IngameEventContext IngameContext() const;

    // ---- damage and the melee hit window (docs/analysis/combat_ai_port.md 9) ----
    // orig: vf7C (Character 0x000354B0; Player 0x00051AD0; GenericNpc 0x00047240 and the NPC classes' own slots).
    virtual void Damage(f32 amount, Character* attacker);
    // orig: vfA4 0x00033D60 (type 7 never; else alive). GenericNpc 0x00047880, Player 0x000502C0.
    virtual bool CanBePushed() const { return type != 7 && !IsDead(); }
    // orig: vfB0 0x00033DA0 (players too): alive and type not 1 / 2 / 3 (a candidate of Player::FindTarget).
    // GenericNpc 0x00047830, Nibbler 0x0008B1C0.
    virtual bool IsTargetable() const { return !IsDead() && type != 1 && type != 2 && type != 3; }
    // orig: vfCC (thiscall(Character* t, f32 amount)): a landed hit's damage (base 0x00011280: nothing).
    virtual void DealDamage(Character* t, f32 amount) { (void)t, (void)amount; }
    // orig: vfD4 (thiscall(GameObject* t, NiPoint3* dir, u32 n)): a hit target (base 0x00033DD0: nothing).
    virtual void ApplyHit(GameObject* t, ni::NiPoint3& dir, u32 n) { (void)t, (void)dir, (void)n; }
    // orig: vfD0 (thiscall(ScriptTrigger* t, NiPoint3* dir), RET 8; base 0x00011280: nothing; Player 0x000519F0): a
    // hit sweep's sphere inside a health trigger (HitSweep 0x0003CCF2). dir is not written by any ported slot.
    virtual void HitTrigger(ScriptTrigger& t, ni::NiPoint3& dir) { (void)t, (void)dir; }
    virtual void HitSpheres(u32 n) { (void)n; }       // orig: vfD8 (base 0x000AEDC0: nothing; GenericNpc 0x00046C40)
    // orig: vfB8 (thiscall(Character* other, f32 amount, bool flag), RET 0xC; base 0x00033DD0: nothing): a character
    // standing on this character's collision mesh (UpdateGroundContacts: (this, 0, 0)) or a laser hit on it
    // (Laser::OnMeshHit: (shooter, 100, 1)). GenericNpc 0x00046DF0.
    virtual void OnMeshContact(Character* other, f32 amount, bool flag) { (void)other, (void)amount, (void)flag; }
    // orig: vf80 (slot 32; Character 0x000AEDC0: nothing; Weasel 0x00093430 / WeaselRoll 0x00093DC0): called by
    // Player::ApplyHit for a hit with n & 4 (Bender's charge punch). The name is a placeholder (L).
    virtual void Vf80(Character* attacker) { (void)attacker; }
    // orig: vf84 (slot 33; Character 0x000357B0: vf7C(100, attacker), +0x778 = 3; Weasel 0x000934E0: vf7C(+0x4F4)
    // instead; WeaselRoll 0x000935D0: its vf80): Bender's charge punch landing on the character (PushOffCharacters).
    virtual void Kill(Character* attacker);
    void HitStart() { hitActive = 1; }                // orig: vfDC 0x00033DF0 (not overridden by the ported classes)
    void HitEnd();                                    // orig: vfE0 0x000360B0 (not overridden by the ported classes)
    // orig: 0x0003C090 (thiscall(name, a, b, n, box), RET 0x14): the hit spheres from the character point to the named
    // node, one physics object query per sphere; each object's owner is hit once per hit window (+0x5B8).
    void HitSweep(const char* name, f32 a, f32 b, u32 n, bool box);
    // orig: 0x0003C405..0x0003C5EF (inline in HitSweep): the sweep's wall ray hit: Smack_Impact sparks on the wall,
    // vfD4(0, N, n) and a player's rumble.
    // orig: 0x0003CC16..0x0003CDBE (inline in HitSweep, after each sphere's object query): every health trigger the
    // sphere (S, r) touches, once per hit window: vfD0 and the Smack_Impact at T + 0.1 dir (dir: the slot esp+0x28).
    void HitHealthTriggers(const ni::NiPoint3& S, f32 r, const ni::NiPoint3& T, ni::NiPoint3& dir);
    // Returns the wall's normal (the sweep's direction slot esp+0x28 keeps it: 0x0003C450).
    ni::NiPoint3 WallSparks(const ni::RayHit& hit, const ni::NiPoint3& q, const ni::NiPoint3& d, f32 dist, u32 n);
    // orig: 0x00034A10 (thiscall(f32 t, NiAVObject* tip), RET 8): the tip's selective flags, then the node's
    // selective pass at t (NiAVObject::UpdateSelected 0x00101900).
    void TipToKeyTime(f32 t, ni::NiAVObject* tip);

    // orig: +0x38, the character's inventory (vtbl 0x00295DC0): collected pickups (their nodes stay in their room).
    class Inventory : public Owner {
    public:
        explicit Inventory(Character& c) : holder(c) {}
        ~Inventory() override { DeleteObjects(); }
        void AttachObject(GameObject& object) override;   // orig: 0x000352D0
        void DetachObject(GameObject& object) override;   // orig: 0x00035350
        bool IsOwnerType(u32 t) const override { return (t & 4) == t; }   // orig: 0x00077220
        ni::NiNode* GetNode() const override;         // orig: 0x0003D800 (the holder's node)
        Character& holder;
    };
    Inventory inventory{*this};
    std::unique_ptr<struct AiAgent> agent;            // orig: +0x58 (NPCs: GenericNpc ctor; players: Player::Player)

    CharacterDef& cdef;
    std::unique_ptr<phys::CharBody> body;             // orig: +0x5C (players: Player ctor)
    ni::NiPoint3 bodyPosition;                        // (the position of a character without a body)
    ni::NiMatrix3 rotation;                           // orig: +0x570
    f32 heightOffset = 0;                             // orig: +0x4D0 (D boundCenter)
    f32 boundHeight = 0;                              // orig: +0x4D4
    f32 health = 0, health4F8 = 0, maxHealth = 0;     // orig: +0x4F4 / +0x4F8 / +0x4FC
    u32 type = 0;                                     // orig: +0x774 (1 fry, 2 leela, 3 bender, 4 rodent, ...)
    ni::NiPointer<ni::NiKeyframeManager> keyframes;   // orig: +0x558
    ni::NiPointer<PhysicController> physic;           // orig: +0x560
    ni::NiPointer<ni::NiAVObject> model;              // orig: +0x564 (the clone of the definition's model)
    ni::NiPointer<ni::NiTextureEffect> envEffect;     // orig: +0x5A4
    // The projected shadow of a player (Character::Init 0x000382DB; drawn by RenderShadow): the texture effect that
    // projects the shadow texture onto the room the character stands in (+0x780).
    ni::NiPointer<ni::NiTextureEffect> shadowEffect;  // orig: +0x5A0 (type 1 projected shadow, clipped)
    ni::NiRoom* shadowRoom = nullptr;                 // orig: +0x780 (the room the effect affects; not referenced)
    ni::NiPointer<ni::NiRenderedTexture> shadowTexture;   // orig: +0x5A8 (128 x 128; null without a renderer)
    ni::NiPointer<ni::NiCamera> shadowCamera;         // orig: +0x5B0
    std::unique_ptr<MasterPropertyAccumulator> shadowAccumulator;   // orig: +0x5B4
    // orig: 0x0003B960 (players' vf0C, `draw` = not first person): the shadow camera above the character, the pass
    // into the shadow texture (renderer_plan.md 9).
    void RenderShadow(bool draw);
    std::vector<ni::NiSequence*> sequences;           // orig: +0x52C (+0x544 name map)
    std::vector<ni::NiPointer<ni::NiSequence>> sequenceRefs;   // (the array's references)
    std::array<ni::NiSequence*, 256> byAnimation{};   // orig: +0xC0 (animation id -> sequence)
    std::vector<ni::NiNode*> tips;                    // orig: +0x788 ("tip" weapon attachment nodes)
    std::string animName = "none";                    // orig: +0x66C (UnloadModel: the animation to restore)
    f32 animOffset = 0;                               // orig: +0x668 (UnloadModel: its sequence offset +0xA8)
    ni::NiPointer<ni::NiTimeController> aim55C;       // orig: +0x55C (the Tank's AimController; UnloadModel removes it)
    u32 modelVariant = 0;                             // orig: +0x784

    // ---- per-step state (orig offsets; initial values from the constructor 0x0003D220) ----
    f32 updateDistance = 250.0f;                      // orig: +0x64
    f32 lodStep = 1.0f;                               // orig: +0x70 (steps since the last render, LOD weighted)
    f32 animTime = 0;                                 // orig: +0x78 (character time t)
    f32 renderTime = 0;                               // orig: +0x7C
    f32 animStart = 0;                                // orig: +0x80 (t of the current sequence's start)
    f32 transitionTime = 0.5f;                        // orig: +0x84
    f32 overlayTransition = 0;                        // orig: +0x88 (NOT initialised by the constructor)
    u8 flag60 = 0;                                    // orig: +0x60
    u8 interruptible = 1;                             // orig: +0x91
    u8 blending = 0, morphing = 0, summing = 0, overlayBlending = 0;   // orig: +0x93 / +0x94 / +0x95 / +0x96
    u8 looping = 0;                                   // orig: +0x98
    u8 overlayLooping = 0;                            // orig: +0x99
    u8 finished = 0, overlayFinished = 0;             // orig: +0x9B / +0x9C
    u8 holdPose = 0, animUpdated = 0;                 // orig: +0x9E / +0x9F
    ni::NiSequence* curSeq = nullptr;                 // orig: +0xA4
    ni::NiSequence* prevSeq = nullptr;                // orig: +0xA8 (blend / morph source)
    ni::NiSequence* sumSeq = nullptr;                 // orig: +0xAC
    ni::NiSequence* overlaySeq = nullptr;             // orig: +0xB0
    ni::NiSequence* overlayPrev = nullptr;            // orig: +0xB4
    f32 overlayStart = 0;                             // orig: +0x4C0 (NOT initialised by the constructor)
    u8 rootStarted = 0, rootFlags = 0, rootKeep = 0;  // orig: +0x4CC / +0x4CD / +0x4CE
    ni::NiPoint3 velBefore;                           // orig: +0x4E4 (the body velocity before the step)
    f32 moveX = 0, moveY = 0, strafe = 0;             // orig: +0x4D8 / +0x4DC / +0x4E0 (move intent)
    f32 idleTime = 0;                                 // orig: +0x4C8
    f32 desiredSpeed = 0;                             // orig: +0x4F0
    u8 attack1 = 0, attack2 = 0, b508 = 0, b509 = 0;  // orig: +0x506..+0x509 (attack requests)
    f32 cameraDistance = 0;                           // orig: +0x500 (NOT initialised by the constructor)
    // orig: +0x5B8 NiTPrimitiveArray (data +0x5BC, count +0x5C4, non-null count +0x5C8): the physics objects and the
    // health triggers (HitSweep 0x0003CCA7) hit in this window
    std::vector<const void*> hitList;
    u8 b5D0 = 0;                                      // orig: +0x5D0 (1 after a sweep; reader not traced)
    u8 hitActive = 0;                                 // orig: +0x5D1 (the hit window: AnimTick sweeps every step)
    f32 f5D4 = 2.0f;                                  // orig: +0x5D4 (2.0 at the ctor, 0 after a sweep; reader not traced)
    ni::NiPoint3 hitTip;                              // orig: +0x5D8 (the last sweep's end point)
    f32 hitRadius = 0;                                // orig: +0x5E4 (the last sweep's radius)
    u8 deleteMe = 0;                                  // orig: +0x628
    u8 splashed = 0;                                  // orig: +0x629
    f32 rippleTimer = 0;                              // orig: +0x62C
    u8 hasControl = 1;                                // orig: +0x630 (also enables the +0x634 timer)
    f32 timer634 = 0;                                 // orig: +0x634
    u8 directRoot = 0;                                // orig: +0x638
    ni::NiPoint3 groundUp;                            // orig: +0x63C (NOT initialised by the constructor)
    ni::NiPoint3 groundPos;                           // orig: +0x648 (NOT initialised by the constructor)
    f32 deathFade = 0;                                // orig: +0x660
    ni::NiPoint3 rootPrev;                            // orig: +0x654 (the root bone's previous parent-space offset)
    f32 rootY = 0;                                    // orig: +0x664
    u8 animLock = 0, pcActive = 0;                    // orig: +0x77C / +0x77D
    u32 deathKind = 0;                                // orig: +0x778 (the death animation's kind: 7, 0x11, ...)
    // orig: +0x40, the lip-sync player (LipSync.h): its face (+0x48, the model's, made by Init), its key file (+0x44)
    // and state (+0x50: 1 stopped, 0 playing, 2 paused) set by the dialogue display's lip tracks (Dialogue.h).
    LipSyncPlayer lip;
    Character* killer = nullptr;                      // orig: +0x68 (Damage's death branch; reader not traced)

protected:
    void UpdateTimers(f32 dt);                        // orig: 0x00034AA0
    void UpdateCameraDistance();                      // orig: 0x00034DF0
    void UpdateBodyShape();                           // orig: 0x00034EE0
    void ApplyRootMotion();                           // orig: 0x0003E240
    void UpdateGroundContacts();                      // orig: 0x00038880
    void SurfaceDeathKind(u32 m);                     // orig: 0x000373E0 (the death kind of a surface's flag byte)
    void SyncNodeTransform();                         // orig: 0x00039530
    void PushOffCharacters();                         // orig: 0x00034600
    void FireTextKeys(ni::NiSequence* seq);           // orig: 0x00039C50
    void UpdateAnimFinished();                        // orig: 0x00036180 (current and overlay)
};

// orig: FPView (vtbl 0x002973EC, ctor 0x00045F60 (thiscall(which): 0 Bender, 1 Leela), dtor 0x000460F0): the
// player's first-person vision overlay (P+0x7B4), its file streamed through the request vf02 0x00046210 (the Player
// constructor). Ported: the constructor's camera, the stream callback vf00 0x000462B0 (the file's root under a node of
// its own, its z-buffer property off, the camera's scene) and the destructor's release order. NOT PORTED: its draw
// 0x00045F10 (the camera's Click), the text object +0x20. The level1-6 chicken walker's cockpit file is
// ChickenWalker's own request.
class ViewFile : public stream::Client {
public:
    ViewFile(std::string path, const char* owner, s32 which, ni::NiRenderer* renderer);
    ~ViewFile() override;                             // orig: 0x000460F0
    bool OnStream(stream::Request& r) override;       // orig: vf00 0x000462B0
    std::string path;                                 // (orig: sprintf'd by vf00 from +0x14: 0x00297434 / 0x00297418)
    std::shared_ptr<stream::Request> request;         // orig: +0x04
    ni::NiPointer<ni::NiNode> root;                   // orig: +0x08 (the file's first top-level object, a NiNode)
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x0C (new(0x250) 0x001009D0)
    ni::NiPointer<ni::NiNode> node;                   // orig: +0x10 (the root's parent)
    ni::NiPointer<ni::NiNode> cameraScene;            // orig: (+0x0C)+0x15C, the camera's scene = +0x10
    s32 which = 0;                                    // orig: +0x14
    bool fpView = false;                              // the FPView object (always, now that the cockpit is ChickenWalker)
    s32 variant = 0;                                  // orig: FPView +0x24 (0x00045EF0: rand() % 2 per frame)
    u8 dirty = 1;                                     // orig: FPView +0xA8 (1 from the ctor)
    u8 active = 0;                                    // orig: FPView +0x18 (read by the draw 0x00045F10)
    f32 time = 0;                                     // orig: FPView +0x1C (the draw's timer)
    std::array<char, 0x40> collectKey{};              // orig: +0x28 = "BD_COLLECT" (11 bytes of 0x00297404)
    std::array<char, 0x40> killsKey{};                // orig: +0x68 = "BD_KILLS" (9 bytes of 0x002973F8)
    void SetActive(u8 v) {                            // orig: 0x00045ED0 (thiscall(u8), RET 4)
        active = v;
        time = 0;
    }
};

// The player's action map (orig: P+0x7BC, 0x245 bytes, ctor 0x0005E2B0; character_update.md 3.1): pad bindings per
// action and the press / release / hold histories (the jump buffer).
struct ActionMap {
    ActionMap();                                      // orig: 0x0005E2B0
    void Update(const input::Pad* pad);               // orig: 0x0005E0D0 (once per step)
    bool MatchNewest(u8 action) const { return press[0] == action; }   // orig: 0x0005DFC0 with n = 1
    u8 Binding(u32 i) const { return rows[0][i % 33]; }                // orig: 0x0005DFA0
    std::array<std::array<u8, 33>, 10> rows{};        // +0x000: row 0 active, rows 1..9 presets
    std::array<u8, 33> held{};                        // +0x14A
    std::array<f32, 33> holdTime{};                   // +0x16C
    std::array<u8, 10> press{}, release{}, hold{};    // +0x1F0 / +0x1FA / +0x204 (newest first)
    f32 clock = 0, lastPress = 0, lastRelease = 0, lastHold = 0;   // +0x228 / +0x22C / +0x230 / +0x234
    f32 unknown238 = -1.0f;                           // +0x238
    f32 walkThreshold = 0.5f, runThreshold = 0.95f;   // +0x23C / +0x240
    u8 tank = 0;                                      // +0x244 (camera-relative control when 0)
};

// The player's ten rumble channels (orig: P+0xA04, 0xCC bytes; camera_modes_bender_port.md 5). The ctor 0x0005EFD0
// zeroes only `time` and `disabled`; the other arrays are heap leftovers read only for a channel Add wrote.
struct Rumble {
    std::array<f32, 10> left{}, right{}, time{}, invDuration{};   // +0x00 / +0x28 / +0x50 (remaining) / +0x78
    std::array<s32, 10> kind{};                       // +0xA0 (0 constant, 1 sine, 2 rising, 3 falling)
    u8 disabled = 0;                                  // +0xC8 (0x0005EFF0)
    void Add(f32 l, f32 r, f32 duration, s32 k);      // orig: 0x0005F000 (thiscall, RET 0x10)
    void Update(input::Pad& pad);                     // orig: 0x0005F070 (thiscall(Pad*), RET 4)
};

// The class ctors call 0x0005D6F0 and set their vtables (Fry also zeroes +0xD68 / +0xD6C); their dtors set the
// vtables, call 0x0003B7D0 and the Player dtor 0x00050830, which calls 0x0003B7D0 first itself.
class Player : public Character {                     // orig: 0x0005D6F0
public:
    // orig: 0x0005D6F0 / 0x00083280 (Fry) / 0x00089700 (Leela) / 0x000804D0 (Bender) / 0x00094410 (ZoidbergRat)
    Player(LevelData& level, CharacterDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot, std::string cls);
    // orig: 0x00050830 / 0x000832D0 (Fry) / 0x00089740 (Leela) / 0x00080510 (Bender) / 0x00094450 (ZoidbergRat)
    ~Player() override;
    std::string className;                            // "Fry", "Leela", "Bender", "ZoidbergRat", "Player"
    std::unique_ptr<ViewFile> view;                   // orig: +0x7B4 (FPView)
    std::unique_ptr<CannonCtrl> cannon;               // orig: +0x7A8 (Bender in level2-5: the cannons, Cannon.h)
    std::unique_ptr<RatRide> ride;                    // orig: +0x7AC (the rodent's rat ride, RatRide.h)
    std::unique_ptr<ChickenWalker> chicken;           // orig: +0x7B0 (Fry in level1-6: the chicken walker, ChickenWalker.h)
    void OnStageLoaded(Stage& stage);                 // orig: 0x0004F8D0 (end of Stage::OnStream)
    // orig: vf64 0x0003DF70 (Fry, Leela, the rodent) / 0x00081710 (Bender: the "Ciggar_null" and "Beer_Bottle"
    // nodes hidden, the animation 0xBC's damage 50)
    void BuildModel() override;
    f32 startY = 0, spawnY = 0;                       // orig: +0xB80 (jump start height) / +0xB84 (last supported)

    // ---- the per-step player code (PlayerControl.cpp; character_update.md 3 / 4) ----
    void Think(f32 dt) override;                      // orig: Player::Control 0x000530C0 (vf54)
    void UpdateAnimation(f32 dt) override;            // orig: 0x0005A790 (vf5C)
    void OnTextKey(f32 keyTime, const char* text) override;   // orig: vfE8 0x00052190 / 0x00085D40 (Fry)
    bool IsFallAnim() const override;                 // orig: vf140 0x000501A0
    bool IsJumpAnim() const;                          // orig: vf12C 0x00050010
    bool IsAttackAnim() const;                        // orig: vf124 0x0004FBB0
    bool OnLadder() const { return ladder || ladderB91 || ladderB92; }   // orig: vf138 0x00050140
    bool OnLedge() const { return ledge || ledgeB8F || ledgeB90; }   // orig: vf13C 0x00050170
    bool OnGround() const override;                   // orig: vf70 0x0004FFE0
    // orig: vf130 0x000500F0: vf12C, or vf34's y at or above +0xB84 (TEST AH,1: NaN fails) and vf140.
    bool JumpingOrFalling() const;
    // orig: vf134 0x000500C0: "death_fall" (0x66) playing, or dead (vfA8) with the death kind +0x778 7.
    bool IsDeathFall() const;
    // orig: vf158 0x000511B0 (thiscall(GameObject* o), RET 4; no caller in the game): an inventory object whose
    // definition name equals o's (strcmp; a null o, definition or name: false).
    bool HasInventoryLike(const GameObject* o) const;
    // orig: vf160 0x00050460 (thiscall(f32 v), RET 4; no caller): v <= +0xAF8 (TEST AH,0x41) -> +0xAF8 -= v, true.
    bool SpendCharge(f32 v);
    bool CanTakeCharge() const { return charges < 4.0f; }   // orig: vf16C 0x000504B0 ([0x00293C1C]; TEST AH,5; JP)
    // orig: vf170 0x00050540 (thiscall(slot), RET 4; no caller): ammo[slot] < the maximum (table 0x0029A254).
    bool AmmoBelowMax(s32 slot) const;
    // orig: vf174 0x000505D0 (the cheat code 26 of 0x0002C530, game/app/CheatCodes.h): +0x4F4 55555.0f <-> 100.0f.
    void ToggleInvulnerable();
    // orig: vf178 0x00055B50 (no caller in the game: only the player vtables hold it): for each weapon type 0..3
    // (vf7C) found in the list from +0x528 (next +8), a new "hammer" / "pistol" / "shotgun" / "railgun" (FindDef, vf14
    // at a stack point and matrix nothing writes: L zeros) into the inventory (vf10(+0x38)); then every ammo slot 999.
    void CheatWeapons();
    // orig: vfC4 0x0004F7F0 (thiscall(W), RET 4; FsGetAmmoPlayer): the ammo of W's type 1..4 (+0xAD0..), else 0.
    s32 WeaponAmmo(const Weapon* w) const;
    // orig: 0x0004F850 (stdcall(W), RET 4; FsGetMaxAmmoPlayer): W's type 1..4 -> 500 / 200 / 300 / 400, else 0.
    static s32 WeaponMaxAmmo(const Weapon* w);
    // orig: the maximum ammo per slot (dword table 0x0029A254; slot 4 the hammer: 0).
    static constexpr s32 kMaxAmmo[5] = {500, 200, 300, 400, 0};
    bool WalkRunPlaying() const override;             // orig: vf9C 0x0004FA80
    bool IsLandAnim() const;                          // orig: vf144 0x00050220
    ni::NiPoint3 CharacterPoint() const override;     // orig: vf78 0x00054A50
    void OnPickup(GameObject& obj) override;          // orig: vfC8 0x00051250
    bool AddAmmo(s32 slot, s32 n);                    // orig: vf8C 0x000504D0
    bool AddCharge(f32 v);                            // orig: vf15C 0x00050420
    f32 ChargeLevel() const { return charges; }       // orig: vf164 0x00050490
    void SetChargeLevel(f32 v) { charges = v; }       // orig: vf168 0x000504A0 (a raw dword store)
    bool HasCockpit() const { return chicken != nullptr; }   // orig: P+0x7B0 (the level1-6 chicken walker object)
    bool WeaponOverlayPlaying() const;                // orig: vf128 0x00050F00
    // orig: vfB4 0x00035F70 (thiscall(W), RET 4; every player class): the old weapon put away (vf60) and detached from
    // the "R_HAND" node, the new one attached there and taken out (vf64); false without the hand node.
    bool SetWeapon(Weapon* W);
    void UseAmmo(s32 slot, s32 n);                    // orig: vf90 0x00050560 (ammo[slot] -= n; 0 if negative)
    s32 GetAmmo(s32 slot) const;                      // orig: vf94 0x00050AF0 (ammo[slot])
    // orig: 0x00051D50 (thiscall(slot, n), RET 8; FsQuickLoad): ammo[slot] = n, then the HUD's "weapons" "ammo" for
    // the weapon being taken out (render only).
    void SetAmmo(s32 slot, s32 n);
    // orig: 0x00050580 (FsQuickLoad): the weapon put away (vfB4(0)), the transitions finished, the attack bytes
    // +0x504 / +0x505 and the pending weapon +0x524 cleared, then every inventory object deleted (vf00(1)).
    void ClearInventory();
    void Damage(f32 amount, Character* attacker) override;   // orig: vf7C 0x00051AD0
    bool CanBePushed() const override;                // orig: vfA4 0x000502C0
    // orig: vfD8 0x000514E0 (the base: Fry, the rodent) / 0x0008A9F0 (Leela) / 0x00081440 (Bender).
    void HitSpheres(u32 n) override;
    void ApplyHit(GameObject* t, ni::NiPoint3& dir, u32 n) override;   // orig: vfD4 0x00051520
    void HitTrigger(ScriptTrigger& t, ni::NiPoint3& dir) override;     // orig: vfD0 0x000519F0
    bool IsPunchSeq(const ni::NiSequence* seq) const; // orig: 0x0004FEB0 (Leela's punch sequences)
    // orig: vfCC 0x000503E0 (thiscall(Character* t, f32 amount), RET 8; every player class): the target's vf7C(amount,
    // this) when it is a live character and amount > 0 (a player's projectile hit, Laser::PostUpdate 0x00078589).
    void DealDamage(Character* t, f32 amount) override;
    bool IsKickSeq(const ni::NiSequence* seq) const;  // orig: 0x0004FF40 (Leela's kick sequences)
    bool IsSecondAttack(const ni::NiSequence* seq) const;   // orig: 0x0004FD90
    void RenderUpdate();                              // orig: vf0C 0x00050B00 (Session::Render)
    // orig: the view overlay at the end of Session::Render (+0x7B4: mode 3 ? draw 0x00045F10 : 0x00045EF0); realDt
    // = the render frame time DAT_002E7C28.
    void RenderView(f32 realDt);
    // orig: vf154 0x00058FA0 (Fry, +0x774 == 1; RenderUpdate): the lock-on marker. Built on first use (the
    // "Targeting" node under the scene S+0x18 with "CrossHairNode" (a 9-vertex textured quad grid) and "ZoomTargetNode"
    // ("ZoomHair", 16 vertices: the closing brackets)); each rendered frame placed at the aim point +0xB04, its
    // crosshair texture chosen by the held weapon's type, its colour by +0xB01, the brackets closing over +0xC2C.
    void UpdateLockMarker();
    void BuildLockMarker();                           // orig: 0x00058FE8..0x00059E9A (the +0xC20 == 0 branch)

    ActionMap actions;                                // orig: +0x7BC
    const input::Pad* pad = nullptr;                  // orig: +0x7B8
    u8 special50A = 0, special50B = 0, special50C = 0;   // orig: +0x50A..+0x50C
    u8 attacking504 = 0, attacking505 = 0;            // orig: +0x504 / +0x505
    u8 lockOn = 0;                                    // orig: +0xB00
    ni::NiPoint3 inputDir;                            // orig: +0xB1C (world input direction, unit)
    f32 lookY = 0, lookX = 0;                         // orig: +0xB28 / +0xB2C
    f32 aValue = 0, xValue = 0;                       // orig: +0xB30 / +0xB34
    f32 orbitX = 0, orbitY = 0;                       // orig: +0xB38 / +0xB3C (camera stick)
    f32 ladderSpeed = 0, b44 = 0;                     // orig: +0xB40 / +0xB44 (the ladder climb speed)
    ni::NiPoint3 thinkVel;                            // orig: +0xB48
    f32 sinceAttack = -10.0f;                         // orig: +0xB54 (Reset)
    f32 lastSurfaceDamage = -10.0f;                   // orig: +0xB58 (Reset; UpdateGroundContacts' slow damage time)
    f32 ledgeTimerA = 0, ledgeTimerB = 0;             // orig: +0xB70 / +0xB74 (vf18C: letting go / grabbing)
    f32 slopeTime = 0, airTime = 0;                   // orig: +0xB78 / +0xB7C
    u8 firstPerson = 0, jumpRequest = 0, jumpPending = 0, b8B = 0;   // orig: +0xB88 / +0xB89 / +0xB8A / +0xB8B
    // +0xB8C / +0xB91 / +0xB92: the ladder states (vf188 scans the stages' "Ladders" records); +0xB8D / +0xB8F / +0xB90:
    // the ledge states (vf18C scans the edge sets). player_moves_port.md 7 (fry_leela_port.md 8.1 had them swapped).
    u8 ladder = 0, ledge = 0, locked = 0, ledgeB8F = 0, ledgeB90 = 0;   // orig: +0xB8C / +0xB8D / +0xB8E / +0xB8F / +0xB90
    u8 ladderB91 = 0, ladderB92 = 0, b93 = 0, b94 = 0;  // orig: +0xB91..+0xB94
    u8 roll95 = 0, roll96 = 0, roll97 = 0, roll98 = 0;   // orig: +0xB95..+0xB98
    // +0xB9E: an electric surface (UpdateGroundContacts: the contact byte equals S+0x12C[7]).
    u8 b9A = 0, aHeld = 0, xHeld = 0, b9D = 0, shock = 0, tentacle = 0;   // orig: +0xB9A..+0xB9F
    u16 actionBits = 0;                               // orig: +0xBA0
    u32 shockVoice = 0;                               // orig: +0xC14 (the electric shock's sound 0x4D)
    u32 spinVoice = 0;                                // orig: +0xC18 (Bender's spin sounds 0x3F / 0x40; not written by the ctor: L 0)
    u16 actionBitsHigh = 0;                           // orig: +0xBA2 (never written: leftover bytes, display only)
    f32 runTime = 0, prevRunTime = 0;                 // orig: +0xBD8 / +0xBDC
    f32 ladderGate = 1.0f, ledgeGate = 1.0f;          // orig: +0xBE4 (vf188) / +0xBE8 (vf18C)
    u8 footstepRight = 0;                             // orig: +0xBEC
    // ---- Fry / Leela (docs/analysis/fry_leela_port.md 6) ----
    s32 kickIndex = 0, punchIndex = 0;                // orig: +0x51C / +0x520 (the position in the combo row)
    Weapon* pendingWeapon = nullptr;                  // orig: +0x524 (the weapon being taken out)
    Weapon* weapon = nullptr;                         // orig: +0x528 (the weapon in the hand)
    f32 charges = 0, chargesShown = 0;                // orig: +0xAF8 / +0xAFC (special-attack charges / HUD shadow)
    u8 lockHasTarget = 0, lockB02 = 0;                // orig: +0xB01 / +0xB02
    ni::NiPoint3 aimPoint;                            // orig: +0xB04
    // orig: +0xB68 / +0xB6C, the end times of the last A / X move: NOT initialised by the original (L: zeroed heap).
    f32 attackStart = 0, attack2Start = 0;
    s32 punchSteps = 0, kickSteps = 0;                // orig: +0xBA4 / +0xBA8 (moves in the current row)
    s32 punchHits = 0, kickHits = 0;                  // orig: +0xBAC / +0xBB0 (hits landed in the row)
    s32 bb4 = 0;                                      // orig: +0xBB4
    u8 kickRow = 0, punchRow = 0;                     // orig: +0xBB8 / +0xBB9 (written before read)
    u8 kickTable[2][5] = {{0, 1, 2, 3, 4}, {2, 3, 0, 1, 4}};   // orig: +0xBBA (Player ctor 0x0005DE61)
    u8 punchTable[4][5] = {{0, 1, 0, 1, 6}, {2, 3, 2, 3, 6}, {0, 1, 4, 5, 6}, {2, 3, 4, 5, 6}};   // orig: +0xBC4
    f32 damageSide = 0;                               // orig: +0xBE0 (the side of the last hit: Damage vf7C 0x00051D05)
    s32 kickCount = 0, punchCount = 0, specialCount = 0;   // orig: +0xC00 / +0xC04 / +0xC08
    s32 hitsA = 0, hitsX = 0;                         // orig: +0xC0C / +0xC10 (the hit handler 0x0005190C)
    s32 kills = 0;                                    // orig: +0xBF0 (ApplyHit's kills; no other reader seen)
    s32 deaths = 0;                                   // orig: +0xBF4 (Damage's deaths; read by the App 0x000A66E7)
    Weapon* stowedWeapon = nullptr;                   // orig: +0xC1C (put away while hanging / climbing / locked)
    s32 shots = 0;                                    // orig: +0xBF8 (the fire key's shot counter; no reader traced)
    std::array<s32, 5> ammoShown{};                   // orig: +0xAE4 (UpdateAnimation's HUD shadow of +0xAD0; ctor 0)
    f32 lockTime = 0;                                 // orig: +0xC2C (0 at a new lock; vf154 0x00058FA0 advances it)
    f32 tapTimer = 0;                                 // orig: +0xC30
    s32 tapCount = 0;                                 // orig: +0xC34
    Character* lockTarget = nullptr;                  // orig: +0xC3C (the locked character; ctor 0x0005D8F5: 0)
    ni::NiPoint3 lockDir;                             // orig: +0xC40 (the facing when a lock found no character)
    ni::NiPointer<ni::NiNode> marker;                 // orig: +0xC20 (vf154: the lock-on marker, "Targeting")
    ni::NiPointer<ni::NiMaterialProperty> markerMaterial;   // orig: +0xC28 (the marker's shared material)
    ni::NiTexturingProperty* markerTexturing = nullptr;   // (the marker's shared texturing property: the original
                                                      //  finds it as the CrossHairNode's child 0's property 8)
    f32 fireHold = 0, chargeTime = 0;                 // orig: +0xD60 / +0xD64 (Fry; NOT initialised by the original)
    u32 fryVoiceD68 = 0, fryVoiceD6C = 0;             // orig: +0xD68 / +0xD6C (Fry ctor 0x0008329B)
    std::array<s32, 5> ammo{};                        // orig: +0xAD0..+0xAE0 (per weapon slot; FsQuickSave saves five)
    Rumble rumble;                                    // orig: +0xA04 (ctor 0x0005EFD0)

    // orig: Player::Reset 0x00057680 (a respawn: the animations stopped, the velocity cleared, full health, the move
    // and attack states cleared, the weapon put away). The constructor's own call is the reduced form in Player::Player.
    void Reset();
    // orig: 0x00057930 (thiscall(NiPoint3* pos, NiMatrix3* rot), RET 8): vf1C, vf24, Reset, +0xB84 = pos.y.
    void Respawn(const ni::NiPoint3& pos, const ni::NiMatrix3& rot);

private:
    void SampleInput(ni::NiPoint3& vel, ni::NiMatrix3& rot);   // orig: vfFC 0x00057960
    void Walk(ni::NiPoint3& vel, ni::NiMatrix3& rot);          // orig: vf100 0x00054AE0
    ni::NiPoint3 Steer(f32 ts, ni::NiMatrix3& rot);            // orig: 0x000536D0
    void TryJump();                                   // orig: vf104 0x00050D10
    void Recentre();                                  // orig: vf108 0x00050E80
    void Attack(bool second, f32 value, ni::NiMatrix3& rot);   // orig: vf114 / vf118 (base 0x0004F910 / 0x0004F9D0)
    void AttackFryA(f32 value);                       // orig: 0x00084B90
    void AttackFryX(f32 value, ni::NiMatrix3& rot);   // orig: 0x00084C50
    void AttackLeelaA(f32 value);                     // orig: 0x0008A190 (kicks)
    void AttackLeelaX(f32 value);                     // orig: 0x0008A6B0 (punches)
    void AttackBenderA(f32 value);                    // orig: 0x00080FD0 (CHARGE / the charge punch from a jump)
    void AttackBenderX(f32 value);                    // orig: 0x000811D0 (SPIN / spinair with Bender_Spin)
    void LockOn(ni::NiMatrix3& rot);                  // orig: vf150 0x00058A70
    // orig: 0x00053D60 (thiscall(f32* dot, s32 skip, bool setAim), RET 0xC): the nearest targetable character in
    // front that no physics object or level geometry hides (after `skip` accepted ones), or null.
    Character* FindTarget(f32* dot, s32 skip, bool setAim);
    // orig: 0x00054440 (thiscall(f32* dot, NiPoint3* out, bool setAim), RET 0xC): the nearest unbroken destructable
    // or player-hittable health trigger in front (scored by distance and angle) that the physics objects / the
    // trigger's box and the level geometry do not hide: *out its point along the ray, *dot the ray's dot with the
    // facing.
    bool FindObjectTarget(f32* dot, ni::NiPoint3& out, bool setAim);
    bool LockRetarget();                              // orig: 0x000552B0 (thiscall(NiPoint3*), RET 4: first person)
    void LockAcquire(bool held, u8& aimed);           // orig: 0x00054F90 (thiscall(held, u8*, NiPoint3*), RET 0xC)
    bool LockAim(u8& aimed);                          // orig: 0x00050FE0 (thiscall(u8*, NiPoint3*), RET 8)
    void Aim(ni::NiMatrix3& rot);                   // orig: vfFC 0x00058324..0x000585B8 (inline)
    void WeaponCycle(bool next);                      // orig: vf10C (Fry 0x00085B70)
    void PreHookBender();                             // orig: Bender vfEC 0x000805B0
    void PreHookFry();                                // orig: 0x00083450
    void PreHookLeela();                              // orig: 0x0008AC00
    void PreHookRat(ni::NiPoint3& vel, ni::NiMatrix3& rot);    // orig: ZoidbergRat vfEC 0x000944F0
    // orig: 0x00052C20 (thiscall(u8* flag, desiredSpeed, velY, side), RET 0x10): the rodent's animation states.
    void RodentStates(u8& flag, f32 dSpeed, f32 velY, f32 side);
    void PostHookFry(ni::NiPoint3& vel, ni::NiMatrix3& rot);     // orig: 0x00084460
    void PostHookLeela(ni::NiPoint3& vel, ni::NiMatrix3& rot);   // orig: 0x000897E0
    void PostHookBender(ni::NiPoint3& vel, ni::NiMatrix3& rot);  // orig: 0x00080720
    void SpinFlameLoop(ni::NiPoint3& vel);            // orig: 0x00080C5B..0x00080F8A (inline in Bender's vfF8)
    void PostHookClimbBlocks(ni::NiPoint3& vel, ni::NiMatrix3& rot);   // (0x29 / 0xF7 / 0x2E, the same in both)
    bool AttackStatesFry(bool hurt, bool reload);     // orig: 0x000839D0 (the flag argument is never used)
    bool IsChargingAnim() const;                      // orig: 0x00083330 (0x90 / 0x83 / 0xA5 / 0xA7 playing)
    bool IsChargeAnimPlaying() const;                 // orig: 0x00083380 (those or 0x91 / 0x84 / 0xA6 / 0xA8)
    bool RequestFire(u32 kind);                       // orig: 0x00083570 (thiscall(kind), RET 4; 1 = A, 2 = X)
    void FireKey();                                   // orig: 0x00084EF0 (Fry's "morph:firew")
    void CycleWeapon(bool next);                      // orig: vfE4 0x00083860 (next) / 0x000836F0 (previous)
    void FirstPersonToggle();                         // orig: vf11C 0x00058940
    void FaceKeyDialogue(u32 mode);                   // orig: 0x000574F0
    void LadderCheck(ni::NiPoint3& vel, ni::NiMatrix3& rot);    // orig: vf188 0x00055F00
    void LedgeCheck(ni::NiPoint3& vel, ni::NiMatrix3& rot);   // orig: vf18C 0x00056990
    void PreHook(ni::NiPoint3& vel, ni::NiMatrix3& rot);       // orig: vfEC (per class)
    void PostHook(ni::NiPoint3& vel, ni::NiMatrix3& rot);      // orig: vfF8 (per class)
    void AttackStates(u8& flag, bool hurt, bool reload);         // orig: vf184 (Player 0x00052480, Fry 0x000839D0)
    void Footstep(u32 kind, bool left);               // orig: vf17C 0x00051DA0
    // orig: LANDING 0x0005BEDF (4.2.6).
    void Landing(u8& flag, double fallH, f32 hSpeed, f32 thrWalk, f32 fwd, f32 side, const ni::NiPoint3& pos);
    // orig: 0x0005AF39..0x0005B3D0 (UpdateAnimation phase A, vfA8 dead): the death animations by +0x778.
    void DyingStates();
    // orig: 0x0005B0FF..0x0005B180: Bender's death effect (the model hidden, an ImpactDef's SpawnUp along +X).
    void BenderDeathEffect(const char* defName, bool hide);
public:
    // orig: 0x00054DA0 (thiscall(u8 enter), RET 4) with enter = 0: back from first person (the yaw of the camera's
    // rotation, the camera follows the player again with a 90-degree field of view). (Also TurdManBig's grab.)
    void LeaveFirstPerson();
    // orig: 0x00054DA0(1): the camera's first-person mode on the player (0x00018050), the view overlay on, the HUD
    // off for every class but Fry.
    void EnterFirstPerson();
    ni::NiPoint3 EyePoint() const;                    // orig: 0x00054A80 (vf78's first-person point)
    // ---- the held state (docs/analysis/object_gaps.md 16: the sewer tentacle, turdman, the carnivorous plant) ----
    Character* HeldBy() const override { return heldBy; }
    bool IsHeldBy(const Character* c) const { return c ? heldBy == c : heldBy != nullptr; }   // orig: 0x000503B0
    void HeldHurt() {                                 // orig: 0x00050390
        if (heldBy) heldHurt = 1;
    }
    // orig: 0x00055370 (thiscall(Character* by, NiNode* tip), RET 8): the player's node attached to the holder's tip.
    void Grab(Character* by, ni::NiNode* tip);
    // orig: 0x00055880 (thiscall, RET 0): back to the level node, facing along the holder's x axis. `leftover` stands
    // for a non-null +0x7A0 that is not a character (the constructor's leftover bytes, KNOWN_ISSUES #33).
    void Release(bool leftover = false);
    // orig: 0x00050360 (thiscall(f32 dmg, Character* by, u8 back), RET 0xC): vf7C(dmg, by), +0xB9F = back + 1.
    void TentacleHit(f32 dmg, Character* by, bool back) {
        Damage(dmg, by);
        tentacle = static_cast<u8>(back ? 2 : 1);
    }
    Character* heldBy = nullptr;                      // orig: +0x7A0 (the holder; Player::Reset 0)
    // orig: +0x7A4 (0x00050390: a hit while held, "0xBF"), +0x7A5 (Bender released: dizzy), +0x7A6 (grabbed: 0xD8),
    // +0x7A7 (grabbed by turdman: 0xD7)
    u8 heldHurt = 0, releaseDizzy = 0, grabStart = 0, grabTurd = 0;

private:
    void HeldUpdate(u8& flag);                        // orig: vf180 0x00055D60 (thiscall(u8* flag), RET 4)
};

// orig: 0x00036210 (cdecl): the length of one cycle of the sequence (0 for none), in ST0.
double SequenceDuration(const ni::NiSequence* seq);

// orig: 0x000362D0 (thiscall on a character, unused): every controller of the sequence loops or clamps
// (CharacterUpdate.cpp).
void SetCycleType(ni::NiSequence* seq, bool loop);

// orig: 0x00033E60 (cdecl): does the key time cross [t, t + dt)? (CharacterUpdate.cpp)
bool KeyCrossed(f32 key, f32 t, f32 ofs, f32 period, bool loop);

// orig: 0x00034100 (cdecl, ST0): acos for |c| < 1, PI at or below -1, 0 at or above 1 (PlayerControl.cpp).
double AcosClamped(f32 c);

// orig: Footstep_Play 0x00033F20 (CharacterUpdate.cpp).
void FootstepPlay(LevelData& level, u32 kind, const ni::NiPoint3& pos);

// orig: 0x00036A20 (cdecl; CharacterUpdate.cpp): for the geometry and then each of its ancestors, the first character
// of S+0x90 (0x000C26E0 walk) whose node (vf30) it is; null if none (the owner of a collision mesh hit).
Character* CharacterOwningMesh(LevelData& level, ni::NiAVObject* geom);

// orig: 0x000C0410 (cdecl; CharacterUpdate.cpp): the colour a step (F(step + step)) towards the global ambient `g`.
ni::NiColor AmbientStep(const ni::NiColor& c, f32 step, const ni::NiPoint3& g);

}  // namespace fut::game
