// Liquids: the object Stage::PreCache spawns for geometry tagged "NiData_<Lava|Slime|Water|Swamp|Sand|Alien>"
// (LiquidDef::Spawn 0x0006B490, Liquid vtbl 0x0029C178, ctor 0x0006AEB0, 0xCC bytes). Its own node stays an empty,
// unnamed node under the stage node; Init works on the tagged geometry instead: its base texture becomes the
// liquid's rendered texture, and the coloured kinds get vertex colours. docs/analysis/stage_precache.md section 6.
//
// The liquids of one type share a LiquidResource (the level-wide cache, orig 0x002DEDF4): screen polygons (3 to 5
// layers with their own textures, blend modes and UV scales) that the first liquid of the type to pass PostUpdate's
// test each frame scrolls by its own velocities and renders into the resource's 128x128 rendered texture, which the
// liquids' geometries show (docs/analysis/renderer_census_port.md 2). Tick / PostUpdate's colour animation:
// inventory_batch1_port.md 4.2.
#pragma once

#include "game/object/GameObject.h"

#include <array>

namespace fut::game {

class LiquidDef;

// orig: LiquidResource (0x74 bytes, a NiRefObject: vtbl 0x0029C158 -> 0x0029C174, ctor 0x0006B700): one per liquid
// type, shared by the liquids of that type (LevelData::liquidResources); its screen polygons render into its texture.
class LiquidResource : public ni::NiRefObject {
public:
    LiquidResource(LevelData& level, u32 layers, u32 type, f32 a, f32 b);   // orig: 0x0006B700 (RET 0x10)
    u32 type = 0;                                     // +0x08 (the definition's liquid type)
    f32 a = 0, b = 0;                                 // +0x0C / +0x10 (the creating liquid's parameters)
    u32 stamp = 0;                                    // +0x14 (Liquid::PostUpdate's frame stamp)
    u32 layerCount = 0;                               // +0x18
    ni::NiPointer<ni::NiRenderedTexture> texture;     // +0x1C (null without a renderer)
    ni::NiPointer<ni::NiCamera> camera;               // +0x20 (new(0x250) 0x001009D0: never updated)
    std::array<ni::NiPointer<ni::NiScreenPolygon>, 5> polygons;        // +0x24 (NiPointer[5], 0x00166E90)
    std::array<ni::NiPointer<ni::NiTexturingProperty>, 5> texturing;   // +0x38
    std::array<ni::NiPointer<ni::NiAlphaProperty>, 5> alpha;           // +0x4C
    std::array<ni::NiPointer<ni::NiMaterialProperty>, 5> material;     // +0x60
};

class Liquid : public GameObject {
public:
    Liquid(LevelData& level, LiquidDef& def, ni::NiGeometry* geometry);   // orig: 0x0006AEB0
    // orig: 0x0006B540: a resource only the cache and this liquid still hold leaves the cache (SetAt(i, 0) 0x0012B7F0
    // on its first slot, then the compaction 0x0006B2E0); +0x3C and +0x74 released.
    ~Liquid() override;
    bool Init() override;                             // orig: 0x0006BC60
    bool Tick() override;                             // orig: vf08 0x0006A640
    bool PostUpdate() override;                       // orig: vf0C 0x0006B010 (Stage::Render)
    void UpdateColors();                              // orig: 0x0006A670

    u32 stamp = 0;                                    // orig: +0x38 (the resource's frame stamp at the last update)
    ni::NiPointer<LiquidResource> resource;           // orig: +0x3C (null until Init found the texture map)
    f32 paramA = 0, paramB = 0;                       // orig: +0x40, +0x44 (the tag's A, B: the layer velocities)
    std::array<ni::NiPoint2, 5> layerVel{};           // orig: +0x48 + 8i (u, v per second; Init's per-type table)
    f32 sinceUpdate = 0;                              // orig: +0x70 (the time since the last PostUpdate)
    std::array<ni::NiPoint2, 5> layerOfs{};           // orig: +0x78 + 8i (the scrolled offsets, wrapped to [-1, 1])
    ni::NiPointer<ni::NiGeometry> geometry;           // orig: +0x74 (released when it has no vertex-colour property)
    u32 colorMode = 0;                                // orig: +0xA0: 0 none, 1 constant (once), 3 animated, 4 + vertex y
    std::array<f32, 4> colorBase{0, 0, 0, 0};         // orig: +0xA4..+0xB0 (rgba)
    std::array<f32, 4> colorAmp{1, 1, 1, 1};          // orig: +0xB4..+0xC0
    f32 baseY = 0;                                    // orig: +0xC4 (lava with waves: the first vertex's y)
    f32 waveHeight = 0;                               // orig: +0xC8 (param C)
};

// orig: 0x0006B490 (thiscall on the definition): a liquid for `geometry`, owned by `owner` (its node under the
// owner's node), with the three parameters of the tag, then Init.
Liquid* SpawnLiquid(LevelData& level, LiquidDef& def, Owner* owner, ni::NiGeometry* geometry, f32 a, f32 b, f32 c);

}  // namespace fut::game
