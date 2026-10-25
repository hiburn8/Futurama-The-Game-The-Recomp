// The Zoidberg rat ride (level4-1): the object the rodent player (ZoidbergRat 0x00094410, Player type 4) owns at
// P+0x7AC. It samples a B-spline through the AI net of every stage ("Start" node to the next end node) and steers the
// rat along it. Decompiled from default.xbe; docs/analysis/rodent_port.md sections 1-2.
//
// Ported: the constructor, Start, AddStage with the spline sampling, Eval / FindNearest, the per-step Update (the
// intro and the main branch), the camera presets (FsSetRodentCam / FsSetRodentSpeed). NOT PORTED: the death /
// respawn at the stage's Start (0x00061F97). The ride sound (start, fade in / out, stop) goes through the level's
// sound service (level_sound_port.md).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiMath.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace fut::game {

class LevelData;
class Player;
class Stage;

// orig: the Zoidberg rat ride (0x274 bytes, ctor 0x00061240, vtbl 0x0029B9BC), owned by the rodent player (+0x7AC).
class RatRide {
public:
    RatRide(LevelData& level, Player& rider);         // orig: 0x00061240
    void Start();                                     // orig: 0x00062EE0
    void AddStage(Stage& st);                         // orig: 0x000628E0
    void Update(ni::NiPoint3& vel, ni::NiMatrix3& rot);   // orig: 0x00061B00
    void SetCameraPreset(s32 n);                      // orig: 0x00061680 (FsSetRodentCam)
    ni::NiPoint3 Eval(f32 t, f32 u) const;            // orig: 0x000616D0
    void FindNearest(ni::NiPoint3& pos, f32& t, f32 u, f32 range) const;   // orig: 0x00061990 (0x00061AE0: 10.0)

    LevelData& level;
    Player& player;                                   // +0x08
    u8 started = 0;                                   // +0x04 (0x000611E0 returns it)
    u32 voice = 0;                                    // +0x14 (NOT initialised by the original; 0 observed)
    f32 f1C = 0, f20 = 4.0f;                          // +0x1C / +0x20 (no reader in the ride's functions)
    f32 airTime = 0, groundDist = 0;                  // +0x24 / +0x28
    f32 speed = 1.0f, step = 1.0f, steerGain = 1.5f, jumpSpeed = 40.0f;   // +0x2C / +0x30 / +0x34 / +0x38
    f32 t = 0, u = 0;                                 // +0x3C / +0x40 (the spline parameter / lateral offset)
    f32 introTime = 0;                                // +0x44 (< 1.0: the intro; then 3.0 forever)
    f32 time48 = 0;                                   // +0x48 (NOT initialised by the original; never read)
    u8 b4C = 0;                                       // +0x4C (no reader)
    s32 sampleCount = 0;                              // +0x50
    std::vector<s32> knots = std::vector<s32>(5000);  // +0x54 (new(0x4E20))
    std::vector<ni::NiPoint3> samples = std::vector<ni::NiPoint3>(5000);   // +0x58 (new(0xEA60))
    s32 pointCount = 0, pointCapacity = 0;            // +0x5C / +0x64
    std::vector<ni::NiPoint3> points;                 // +0x60 (the AI-net control points)
    ni::NiPoint3 tangent;                             // +0x68
    s32 stagesAdded = 0;                              // +0x74
    std::vector<std::unique_ptr<std::string>> names;  // +0x78 NiTPrimitiveArray<char*> (null entries are kept)
    Stage* stage = nullptr;                           // +0x90 (identity only: a stage can be unloaded)
    std::array<s32, 30> stageBase{};                  // +0x94 (the first sample of each stage)
    static const std::array<u32, 10> kCamHeight, kCamDistance;   // +0x1FC / +0x224 (the only preset tables read)

private:
    void UpdateIntro(ni::NiMatrix3& rot);             // orig: 0x00061B5B..0x00061F1B
    void UpdateMain(ni::NiPoint3& vel, ni::NiMatrix3& rot);   // orig: 0x00061F1E..0x000628CF
};

// orig: DAT_002DDF78, the smoothed steering (a static: never reset, not even by a new level). Every oracle starts a
// fresh process, so the test harness resets it per scenario.
extern f32 g_ratSteer;

}  // namespace fut::game
