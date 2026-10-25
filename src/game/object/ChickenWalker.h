// The level1-6 chicken walker: the object Fry owns at P+0x7B0 in level1-6 (stream owner "L16obj"). It loads the cockpit
// file Cockpit\ChickenCockpit.nif, puts the player on the AI net "ChickenNet" and walks it node by node, drives the
// cockpit's animations and the static camera at its "Camera_null". Decompiled from default.xbe;
// docs/analysis/walker_port.md (its C++ checked against the assembly; corrections in its status block).
//
// Ported: the constructor (the laser / rocket definitions, the eleven sound loads, the request), OnStream, Activate,
// Mount, Dismount, the net walk (StartNode, PrevOnPath, NextNode), Control (states 0-4, the stick, the cockpit
// transforms, the camera, the energy / health), the cockpit animation state machine (Animate, StopPrevious, Joystick,
// Blend, Keys, OnKey), the frustum side normals and RenderUpdate; since docs/analysis/explore_gaps_port.md 1 also the
// fire branch (the laser with its spread, the rockets with the process global g_rocketSide, 0x00044F0A..0x000456DF),
// the turn sound (0x000456EE: the voice id gates the next pick) and the impact 0x000436C0; the sounds through the
// level's sound service (steps, pulse, gun, rocket, turn, impact; level_sound_port.md).
// NOT PORTED: the camera's colour fade update.
#pragma once

#include "core/Types.h"
#include "engine/ni/NiKeyframeManager.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiObjects.h"
#include "engine/stream/StreamManager.h"

#include <array>
#include <memory>
#include <vector>

namespace fut::game {

class LevelData;
class Player;
class LaserDef;
class Character;
class ProjectileDef;
struct AiNet;
struct AiNetNode;

// orig: vtbl 0x002971FC (vf00 OnStream 0x000458C0, vf01 0x000AEDC0, vf02 dtor 0x000458A0), 0x17C bytes.
class ChickenWalker : public stream::Client {
public:
    ChickenWalker(LevelData& level, Player& rider);   // orig: 0x00043D80, then the request 0x000431A0
    bool OnStream(stream::Request& r) override;       // orig: vf00 0x000458C0
    void Activate(const char* name);                  // orig: 0x00044500
    void Mount();                                     // orig: 0x00043240
    void Dismount();                                  // orig: 0x00043470
    void Control(ni::NiPoint3& vel, ni::NiMatrix3& rot);   // orig: 0x000445A0 (thiscall(P, vel, rot): P is the rider)
    void RenderUpdate();                              // orig: 0x00042D80 (from Player::RenderUpdate vf0C)
    void SetRockets(u32 n) { rockets = n > 99 ? 99 : n; }   // orig: 0x000426E0
    void AddRockets(u32 n) {                          // orig: 0x00042700 (unsigned compare)
        rockets += n;
        if (rockets > 99) rockets = 99;
    }

    LevelData& level;
    Player& player;                                   // (the original reads S+0x94 everywhere but in Control)
    std::shared_ptr<stream::Request> request;         // +0x04
    u8 active = 0;                                    // +0x08 (0x00042690 returns it)
    ni::NiPointer<ni::NiNode> cockpit;                // +0x0C (the file's last top-level NiNode, "SceneNode")
    ni::NiSequence* footSeq = nullptr;                // +0x10 (the sequence whose keys Animate checks)
    std::vector<ni::NiPointer<ni::NiSequence>> seqs;  // +0x14 NiTArray (+0x18 data): the file's sequences in order
    ni::NiPointer<ni::NiKeyframeManager> joyKfm;      // +0x2C (on "Joy_stick_handle")
    ni::NiPointer<ni::NiKeyframeManager> walkKfm;     // +0x30 (on "Walker:Animation (Translational)")
    ni::NiAVObject* walkNode = nullptr;               // +0x34 "Walker:Animation (Translational)" (no reference)
    ni::NiAVObject* camNull = nullptr;                // +0x38 "Camera_null" (no reference)
    AiNetNode* node = nullptr;                        // +0x3C (the node the walker heads for)
    AiNetNode* prev = nullptr;                        // +0x40 (the node it came from)
    AiNet* net = nullptr;                             // +0x44 ("ChickenNet", "ChickenNet2", "ChickenNet3")
    s32 state = 0;                                    // +0x48: 0 start-up, 1 walking, 2 halted, 3 (no writer), 4 dead
    s32 prevState = -1;                               // +0x4C (the state of the previous Animate)
    f32 pitch = 0, yaw = 0;                           // +0x50 / +0x54 (the cockpit's stick angles)
    f32 startTime = 0;                                // +0x58 (state 1 at 2.5 s)
    f32 laserCool = 0;                                // +0x5C
    u32 speed = 0x41200000u;                          // +0x60 (10.0f; FsSetChickenSpeed 131 writes the raw dword)
    f32 gain = 0.02f;                                 // +0x64 (0x3CA3D70A)
    f32 lastMoveY = 0, lastMoveX = 0;                 // +0x68 / +0x6C (the joystick smoothing)
    // +0x70..+0xAC: sequence indices into seqs (OnStream; the ctor does not initialise them: new memory, L 0).
    s32 iStartup = 0, iBigAims = 0, iGreenline = 0, iSmallAim = 0, iWalk = 0;   // +0x70 / +0x74 / +0x78 / +0x7C / +0x80
    // +0x88..+0xA8: up_left, up, up_right, left, centered, right, down_left, down, down_right
    std::array<s32, 9> iJoy{};
    s32 iPush = 0;                                    // +0xAC
    u32 shakeAmp = 0;                                 // +0xB0 (FsSetChickenShake 133, raw dword; never read)
    f32 shakeTime = 0;                                // +0xB4 (2.0 by 133; never read)
    u8 sleep = 0;                                     // +0xB8 (FsSetChickenSleep 132)
    LaserDef* laser = nullptr;                        // +0xBC "Laser" (owned by the definition list)
    ProjectileDef* rocketDef = nullptr;               // +0xC0 "ChickRocket"
    u32 rockets = 5;                                  // +0xC4 (at most 99)
    f32 rocketCool = 0;                               // +0xC8
    s32 joyA = -1, joyB = -1;                         // +0xCC / +0xD0 (the joystick sum pair, -1 = none)
    u8 pushActive = 0;                                // +0xD4 (set once, never cleared)
    s32 stageIndex = 0;                               // +0xD8
    f32 savedHealth = 0;                              // +0xE0 (the rider's health before the ride)
    f32 energy = 500.0f;                              // +0xE4 (laser energy, [0, 500])
    u8 netEnd = 0;                                    // +0xE8 (NextNode reached the end of the net)
    std::vector<u32> stepSounds, turnSounds, impactSounds;   // +0xEC / +0x104 / +0x11C (sound lists)
    u32 rocketSound = 0, gunSound = 0, pulseSound = 0;       // +0x134 / +0x138 / +0x13C
    u32 turnVoice = 0;                                // +0x140 (the turn sound's voice, 0 when not turning)
    u32 u144 = 0;                                     // +0x144 (no reader in the class)
    std::array<ni::NiPoint3, 4> frustumSides{};       // +0x148..+0x177 (0x00042DA0; the damage indicator's)
    u8 frustumDone = 0;                               // +0x178

private:
    AiNetNode* StartNode() const;                     // orig: 0x00042950
    AiNetNode* NextNode(AiNet& n, AiNetNode& nd);     // orig: 0x000439A0
    void ComputeFrustumSides();                       // orig: 0x00042DA0
    void Animate();                                   // orig: 0x00043AF0
    void StopPrevious();                              // orig: 0x00042C90
    void Joystick(bool stop);                         // orig: 0x00042A90
    void Blend(s32 a, s32 b, f32 w);                  // orig: 0x00042A10
    void Keys(f32 t, ni::NiSequence* s);              // orig: 0x00043730
    void OnKey(f32 t, const char* name);              // orig: 0x00042800
    void FireLaser(const ni::NiMatrix3& M, const ni::NiPoint3& T);    // orig: 0x00044FB3..0x000453CF
    void FireRocket(const ni::NiMatrix3& M, const ni::NiPoint3& T);   // orig: 0x0004541D..0x000456DF
    ni::NiPoint3 ImpactSoundPosition() const;         // orig: 0x000435E0
public:
    void Impact(f32 amount, Character* attacker);    // orig: 0x000436C0
    void DamageIndicator(const ni::NiPoint3& at);     // orig: 0x00042F90 (the cw_hud's "show_indicator")
private:
    ni::NiSequence* Seq(s32 i) const {                // seqs[i] (the original indexes +0x18 without a check)
        return i >= 0 && static_cast<size_t>(i) < seqs.size() ? seqs[static_cast<size_t>(i)].Get() : nullptr;
    }
};

// orig: 0x00042740 (thiscall K, (net, goal, start), RET 0xC): walk the chain from `start` (link 0, or link 1 when link
// 0 leads back) and return the node before `goal` (null when start == goal; the last node when the chain ends).
AiNetNode* PrevOnPath(AiNet& net, const AiNetNode* goal, AiNetNode* start);

}  // namespace fut::game
