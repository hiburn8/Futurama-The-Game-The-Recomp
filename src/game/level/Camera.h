// The game camera: the 0x760-byte "Ingame" object Session::Start creates (ctor 0x00019DB0, S+0x88 list / S+0x8C
// current camera). docs/analysis/camera.md (section numbers in Camera.cpp).
//
// Ported: the constructor's state, SetFollowTarget, PlaceBehindTarget, the per-step update with the follow mode 0
// (without lock-on) and its collision helpers (push-out, ray casts, magnets), the shake (camera_modes_bender_port.md
// 4), and the apply step with NiCamera::LookAt, the static modes 1 / 2 and the path mode 6
// (docs/analysis/level2-2_port.md 4), the fixed-vector mode 8 (object_gaps.md 12), the rail mode 9 (object_gaps.md
// 17), the first-person mode 3 (0x00018050 / 0x00018250 / 0x00014150), the path-to-chase mode 5 and the side mode 7.
// NOT PORTED yet: the free fly mode 4 (0x00015110) and the frustum wobble (rendering only).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"

#include <array>

namespace fut::game {

class Character;
class LevelData;
struct TriggerRec;
struct AiNet;

// orig: NiCamera::LookAt 0x000FFF40 (thiscall(target*, up*)): the camera looks along its local +X at `target` with
// `up`; false (nothing changed) when the target is at the camera or `up` is parallel to the view.
bool NiCameraLookAt(ni::NiCamera& cam, const ni::NiPoint3& target, const ni::NiPoint3& up);

class Camera {
public:
    explicit Camera(LevelData& level);                // orig: 0x00019DB0 (renderer, "Ingame", clone 0)
    void SetFollowTarget(Character* c);               // orig: 0x00018FB0
    void Update(f32 dt);                              // orig: 0x00019C80 (per fixed step)
    void Apply();                                     // orig: 0x00012210 (per rendered frame)
    // orig: 0x00012410 ("cam:click"): NiCamera::Click of the scene camera; the port keeps only the cull pass's
    // counters and render-time side effects (NiCull.h).
    void Click();
    // The script natives' set-ups (camera natives 234-239): the FOV lines, the static cameras, the snap behind.
    void ResetFov();                                  // orig: inline in 0x00019110 / 0x00018860 / 0x00018B50
    void SetFov(f32 deg);                             // orig: 0x000110E0 (the level1-6 chicken walker: 50)
    void SetStatic(const ni::NiPoint3* pos, const ni::NiMatrix3* rot);   // orig: 0x00015040 (mode 1)
    void SetStaticTarget(const ni::NiPoint3* pos, TriggerRec* rec, f32 easing);   // orig: 0x00014550 (mode 2)
    // orig: 0x00014240 (mode 6): a quadratic path from the position through `a`'s reflection to `b` in `time`
    // seconds, the look-at turning from its current point to `rec`'s position (native 245).
    void SetPathToStaticTarget(const ni::NiPoint3& a, const ni::NiPoint3& b, TriggerRec* rec, f32 time);
    // orig: 0x00014990 (thiscall(name, NiPoint3* v), RET 8; mode 8, native 241): the camera follows `name`'s object
    // from the offset -v, its look-at blending to the point v ahead (docs/analysis/object_gaps.md 12).
    void SetFixedVector(const char* name, const ni::NiPoint3& v);
    // orig: 0x00014F40 (thiscall(name, net), RET 8; mode 9, native 243 FsSetRailCam): the camera rides the AI net
    // `net` ahead of a node (the fog node for "FOGNODE", else the target's model) (docs/analysis/object_gaps.md 17).
    void SetRail(const char* name, const char* net);
    // orig: 0x00014690 (thiscall(target, a, b), RET 0xC; mode 7, native 240 FsSetSideCam 0x00018DE0): the side
    // camera: beside the target, on the line from a's position through b's at the target's height, looking at the
    // target's head (nothing unless all three are set).
    void SetSide(Character* t, TriggerRec* a, TriggerRec* b);
    // orig: 0x000143A0 (thiscall(a*, b*, Character* c, f32 time, u8 follow), RET 0x14; mode 5, native 244
    // FsSetPathToChaseCam 0x00018A40 with follow 0): the mode-6 path from the position through a's reflection to b in
    // `time` seconds (with `follow` the end moves with c), the look-at turning to c's pivot; at the end the follow
    // camera on c (0x00018FB0).
    void SetPathToChase(const ni::NiPoint3& a, const ni::NiPoint3& b, Character* c, f32 time, u8 follow);
    // orig: 0x00018050 (thiscall(Character* c), RET 4; mode 3, Player 0x00054DA0(1)): the first-person camera at c's
    // eye (D+0xA0 above its position) along its horizontal facing; the transform pushed (the mode to return to in
    // +0x160), c's model hidden (its hidden bit kept in +0x1C0).
    void EnterFirstPerson(Character* c);
    // orig: 0x00011120 (thiscall(NiPoint3 by value), RET 0xC): the position, translate dirty.
    void SetPosition(const ni::NiPoint3& p) {
        position = p;                                 // +0x48
        dirtyTranslate = 1;                           // +0xA1
    }
    f32 fpPitch = 0, fpYaw = 0;                       // orig: +0x190 / +0x194 (mode 3's smoothed look; ctor 0)
    u8 modelHidden = 0;                               // orig: +0x1C0 (the target model's hidden bit before mode 3)
    u8 pathFollow = 0;                                // orig: +0x114 (mode 5)
    ni::NiPoint3 pathTargetStart;                     // orig: +0x118 (c's position at the set-up)
    TriggerRec* sideA = nullptr;                      // orig: +0x130 (mode 7)
    TriggerRec* sideB = nullptr;                      // orig: +0x134
    // orig: 0x00012FB0 (native 262 with a5 = 0): the colour fade's state (the fade update 0x000198E0 and its screen
    // polygon are NOT PORTED: rendering only).
    void SetFade(f32 r, f32 g, f32 b, f32 time, f32 a5);
    // orig: 0x00013130 (thiscall, 7 floats, RET 0x1C): a fade from the current start colour (+0x1FC) to (r, g, b, a);
    // the level1-6 chicken walker's fade from black. State only (NOT PORTED: the fade update, as SetFade's).
    void FadeTo(f32 r, f32 g, f32 b, f32 a, f32 time, f32 v244, f32 v248);
    void ScriptPlaceBehindTarget() { PlaceBehindTarget(); }   // (native 235)
    // orig: 0x00013060 (thiscall(r, g, b, time), RET 0x10): native 261 FsFadeOut.
    void FadeOut(f32 r, f32 g, f32 b, f32 time);
    // orig: 0x000132F0 / 0x00011310: the transform stack (+0x250 + 0x50 i, 16 entries, top +0x750).
    void PushTransform() {                            // orig: 0x000132F0
        if (stackTop >= 15) return;                   // CMP 0xF; JGE
        ++stackTop;
        saved[static_cast<size_t>(stackTop)] = {position, rotation, mode};   // +0x00 / +0x10 / +0x40
    }
    void PopTransform() {                             // orig: 0x00011310 (the mode is not restored)
        if (stackTop < 0) return;                     // TEST; JL
        position = saved[static_cast<size_t>(stackTop)].position;
        dirtyTranslate = 1;                           // +0xA1
        rotation = saved[static_cast<size_t>(stackTop)].rotation;
        dirtyRotate = 1;                              // +0xA3
        --stackTop;
    }
    struct Saved {                                    // orig: 0x50 bytes
        ni::NiPoint3 position;                        // +0x00
        ni::NiMatrix3 rotation;                       // +0x10
        u32 mode = 0;                                 // +0x40
    };
    std::array<Saved, 16> saved{};                    // orig: +0x250
    s32 stackTop = -1;                                // orig: +0x750 (ctor 0x0001A85C: -1)
    void ApplyPresets(s32 k);                         // orig: 0x00011380 (native 250 FsSetCamPreset)
    // The shake setters (camera_modes_bender_port.md 4.2; the per-step shake is Shake 0x00012E40): shaking = amp > 0
    // (TEST AH,0x41; JNE: NaN 0), amp and time as raw dwords.
    void SetShake(f32 amp, f32 time) {                // orig: 0x00011290 (thiscall, RET 8): native 266
        shaking = amp > 0.0f ? 1 : 0;
        shakeAmp = amp;
        shakeTime = time;
    }
    void SetShakeForever(f32 amp) {                   // orig: 0x00012E00 (thiscall, RET 4): native 265
        shaking = amp > 0.0f ? 1 : 0;
        shakeAmp = amp;
        shakeTime = -47.0f;                           // 0xC23C0000 ("forever")
    }

    LevelData& level;
    u8 active = 1;                                    // orig: +0x00
    u8 recentre = 0;                                  // orig: +0x01 (0x000111A0)
    ni::NiPoint3 pivotCopy;                           // orig: +0x04
    s32 kind = 0;                                     // orig: +0x10 (preset kind)
    u8 shaking = 0;                                   // orig: +0x14
    f32 shakeTime = -47.0f;                           // orig: +0x18 (-47 = forever)
    f32 shakeAmp = 0;                                 // orig: +0x1C
    ni::NiPoint3 shakeOffset;                         // orig: +0x20
    ni::NiPointer<ni::NiCamera> sceneCamera;          // orig: +0x30 (attached to the scene root)
    TriggerRec* tracked = nullptr;                    // orig: +0x40 (modes 2 / 6)
    Character* target = nullptr;                      // orig: +0x44
    ni::NiPoint3 position;                            // orig: +0x48
    ni::NiPoint3 lookAt;                              // orig: +0x54
    ni::NiPoint3 goal;                                // orig: +0x60 (mode 2)
    ni::NiMatrix3 rotation;                           // orig: +0x70 (the non-look-at modes)
    u8 dirtyFrustum = 1, dirtyTranslate = 1, dirtyLookAt = 0, dirtyRotate = 1;   // orig: +0xA0..+0xA3
    f32 halfWidth = 1.0f;                             // orig: +0xA4 (FPTAN(pi/4) rounded)
    f32 halfHeight = 0.750000059604644775f;           // orig: +0xA8 (0x3F400001: the unrounded tan * 0.75)
    f32 fov = 90.0f, unknownB0 = -47.47f, farPlane = 2000.0f;   // orig: +0xAC / +0xB0 / +0xB4
    // The fog: the NiFogProperty on the scene root (the renderer's FOGCOLOR / FOGENABLE; its depth decides the clear
    // flags) and the camera's own copies.
    ni::NiPointer<ni::NiFogProperty> fogProperty;     // orig: +0x1C8 (ctor 0x0001A651: flags 0, depth 1, colour 0)
    ni::NiPoint3 fogColor{0.1f, 0.2f, 0.0f};          // orig: +0x1CC (0x3DCCCCCD, 0x3E4CCCCD, 0); also fog +0x1C
    f32 fogDistance = 100.0f;                         // orig: +0x1D8
    f32 defaultFar = 2000.0f;                         // orig: +0x1DC (the far plane to restore)
    u8 fogEnabled = 0;                                // orig: +0x1E0
    ni::NiPointer<ni::NiNode> fogNode;                // orig: +0x1E4 (FsFogSetRelativeNode)
    f32 fogOffset = 0;                                // orig: +0x1E8 (not initialised by the ctor; read only with fogNode)
    f32 fogDepth = 1.0f;                              // orig: fog property +0x18 (renderer: clear flags 5 when 1.0)
    void SetFogIntensity(f32 v);                      // orig: 0x00012C70
    void SetFogColor(f32 r, f32 g, f32 b);            // orig: 0x00012CC0
    void SetFogDistance(f32 d);                       // orig: 0x00011230
    void SetFogRelativeNode(const char* name, const char* child, f32 offset);   // orig: 0x00018520
    void EnableFog();                                 // orig: 0x00012D20
    void DisableFog();                                // orig: 0x00012DB0
    f32 height = 0;                                   // orig: +0xD0 (desired camera height)
    struct Follow {                                   // orig: +0xE4 (0x8C bytes), Reset 0x00011A90
        ni::NiPoint3 facing;                          // +0x00 (smoothed target facing)
        f32 yawRate = 0, heightRate = 0, distRate = 0;   // +0x0C / +0x10 / +0x14
        f32 swingTime = 0;                            // +0x18
        ni::NiPoint3 swingDir;                        // +0x1C
        f32 recheck = 0, occluded = 0;                // +0x28 / +0x2C
        f32 unknown30 = 1.0f;                         // +0x30
        u8 magnet = 0;                                // +0x44
        std::array<f32, 8> on{}, off{};               // +0x48 / +0x68 (the flag timers of 2.6.1)
        u8 keep = 0;                                  // +0x88
    } follow;
    s32 updates = 0;                                  // orig: +0x14C
    f32 pitchOffset = 0, yawVelocity = 0;             // orig: +0x150 / +0x154
    f32 radius = 1.0f;                                // orig: +0x158
    u32 mode = 0;                                     // orig: +0x15C (0 follow, 3 first person, ...)
    u32 prevMode = 0;                                 // orig: +0x160
    u8 flush = 0;                                     // orig: +0x164
    // orig: +0x168..+0x184, the presets of kind 0 (0x00011380).
    f32 distance = 14.2f, presetHeight = 5.89f, preset170 = 0.8777f, pivotHeight = 5.52588f;
    f32 yawGain = 35.0f, preset17C = 0.8225f, pitchGain = 50.0f, preset184 = 0.1f;
    f32 targetExtent = 0;                             // orig: +0x188
    f32 easing = 0;                                   // orig: +0x18C (mode 2)
    // Mode 8 (0x00014990 / 0x00014B00): the unit offset +0xD4 and its length +0xE0; the anchor (the look-at goal)
    // and the height goal live in the follow block's +0x34..+0x40 (written only by mode 8; Reset 0x00011A90 keeps
    // them, so the port keeps them outside `follow`).
    ni::NiPoint3 fixedDir;                            // orig: +0xD4
    f32 fixedLength = 0;                              // orig: +0xE0
    ni::NiPoint3 fixedAnchor;                         // orig: (+0xE4)+0x34
    f32 fixedHeight = 0;                              // orig: (+0xE4)+0x40
    s32 modeFrames = 0;                               // orig: +0x198
    // Mode 9 (0x00014F40 / 0x000125B0): the AI net and the node the camera rides ahead of; +0x148 is cleared by the
    // set-up and not read by the update.
    AiNet* railNet = nullptr;                         // orig: +0x138
    ni::NiPointer<ni::NiAVObject> railNode;           // orig: +0x13C
    f32 rail148 = 0;                                  // orig: +0x148
    // Mode 6 (0x00014240 / 0x00018C00; the constructor zeroes +0xE8..+0x130).
    ni::NiPoint3 pathStart, pathControl, pathEnd;     // orig: +0xE8 / +0xF4 / +0x100
    f32 pathTime = 0, pathDuration = 0;               // orig: +0x10C / +0x110
    ni::NiPoint3 pathLook;                            // orig: +0x124 (the look-at at the path's start)
    // The colour fade (0x00012FB0): colours +0x1FC / +0x20C / +0x21C / +0x22C, phases +0x240..+0x248, elapsed +0x24C.
    std::array<f32, 4> fadeColor{}, fadeColor20C{}, fadeColor21C{}, fadeColor22C{};
    f32 fadeTime = 0, fade244 = 0, fade248 = 0, fadeElapsed = 0;
    // orig: 0x000198E0 (thiscall(dt), per step from Update): the fade's colour from its phases (from +0x21C to +0x20C
    // over +0x240, held +0x244, to +0x22C over +0x248; the elapsed time +0x24C first), clamped at 0, onto the fade
    // polygon, which is on the overlay camera while the alpha is above 0. False when it is not shown.
    bool UpdateFade(f32 dt);
    // orig: +0x34 the overlay camera (0x001009D0, no scene; renderer App->vf14()), Clicked by 0x00012450 after the HUD.
    ni::NiPointer<ni::NiCamera> overlayCamera;
    ni::NiPointer<ni::NiScreenPolygon> fadePolygon;   // orig: +0x1F8 (0x000194D0)
    u8 fadeAttached = 0;                              // orig: +0x23C
    void DrawOverlay();                               // orig: 0x00012450 (+0x34 Click)
    ni::NiPoint3 pivotAtFollow, positionAtFollow;     // orig: +0x19C / +0x1A8

private:
    void PlaceBehindTarget();                         // orig: 0x00015350 (2.4)
    void FollowMode(f32 dt);                          // orig: 0x00015990 (2.6)
    void StaticTargetMode();                          // orig: 0x00012460 (mode 2; dt not read)
    void PathToStaticTargetMode(f32 dt);              // orig: 0x00018C00 (mode 6)
    void FixedVectorMode(f32 dt);                     // orig: 0x00014B00 (mode 8)
    void RailMode();                                  // orig: 0x000125B0 (mode 9; dt not read)
    void SideMode();                                  // orig: 0x00014700 (mode 7; dt not read)
    void PathToChaseMode(f32 dt);                     // orig: 0x00019180 (mode 5)
    void FirstPersonMode();                           // orig: 0x00018250 (mode 3; thiscall(dt), RET 4; dt not read)
    // orig: 0x00014150 (thiscall(u32 newMode), RET 4; SetFollowTarget's 0x00014150(0)): false when leaving first
    // person returned to the pushed mode instead.
    bool LeaveModeCheck(u32 newMode);
    // orig: the setters' inline restore (0x00014240 / 0x000143A0 / 0x00014550 / 0x00014690 / 0x00014990 / 0x00015040
    // / 0x00018050 / 0x00018FB0 / 0x00014150): in mode 3 with a target, its model's hidden bit back from +0x1C0.
    void RestoreTargetModel();
    // orig: 0x00013360 (thiscall(const NiPoint3* dir, NiPoint3* out), RET 8): the camera point `distance` (+0x168)
    // from the target along -dir at the height +0xD0, pulled in by the collision rays from the pivot.
    void SideProbe(const ni::NiPoint3& dir, ni::NiPoint3& out);
    void Shake(f32 dt);                               // orig: 0x00012E40 (thiscall(f32 dt), RET 4)
    bool PushOut(ni::NiPoint3& p);                    // orig: 0x000129C0
    s32 Magnets(ni::NiPoint3& des);                   // orig: 0x000136C0
};

// The camera's process statics as a new process has them (the follow mode's lock slide point 0x002D96EC, its guard
// bit 0x002D96F8): for the tests that run several levels in one process.
void ResetCameraStatics();

}  // namespace fut::game
