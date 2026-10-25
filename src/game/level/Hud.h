// The in-game HUD (Session +0x118; docs/analysis/renderer_plan.md 12): the element list the HUD build 0x000C12F0
// fills per level (Session request kind 2), their per-step update (Session step 12, 0x000E58E0), their draw after
// the scene camera's Click (0x000E5900), the script natives' commands (0x000E5B60 / 0x000E5B90 / 0x000E5BC0 /
// 0x000E5A60) and the screen polygons the elements put on the scene camera (0x000E5C00 / 0x000E5F60).
//
// Ported: the container (ctor 0x000E5470, Find 0x000E5920, the texture cache 0x000E59E0 / 0x000E5480), the element
// base HUDObject (0x000E57F0, vtable 0x002A5118), the screen-polygon helpers, and of the elements of the default /
// "Level1" builds the parts that make or move screen polygons: HUDHead's scene polygon (0x000E94E0), HUDHealth
// (0x000E9AB0 / vf01 0x000E9CE0), HUDWeapons' backdrop (0x000ED220 / vf01 0x000ED010), HUDItems (0x000EA800 / vf01
// 0x000EAA50 / vf03 0x000EAC10), HUDNibblerCnt / HUDCollectableCnt / HUDLives with their pulse (0x000EB680 /
// 0x000E85B0 / 0x000EB070) and the flash object (0x000EBF40 / 0x000EBBD0 / 0x000EBFC0 / 0x000EB980), the elements'
// vf05 (the hide of native 384), HUDTimer (the state of LevelData.h, the text, the swamp bar 0x000ECC96.. / vf02
// 0x000EC9B0), HUDCharge (0x000E6750), HUDInfo (0x000EA100: the hint texts), HUDHead (ctor 0x000E94E0: its head camera +0x50 and mask camera +0x54, the masks 0x000E8A20,
// vf01 0x000E98D0 with the head model 0x000E8F60 (the player's HUD_Head, LipSyncFace::Create's mesh, the outline
// conversion 0x0013E5E0), the lip sync of the player's key file on the head's face (LipSync.h) and the head's turn
// 0x000E8E00, vf02 0x000E89B0), the flash texts HUDText (0x000EC2A0: HUD1Up / HUDChargeText / HUDCheckpoint with
// their shared queue), HUDWeapons whole (vf01 0x000ED010, vf02 0x000ECE50, vf03 0x000ED410 with the screen object
// 0x000E5680, vf04 0x000ECF50, vf05 0x000ED0F0), HUDItems' icon (vf03 0x000EAC10, vf01 0x000EAA50), HUDChickenWalker
// (0x000E8220). HUDCollectableCnt's Level5 absence is ported.
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fut::game {

class LevelData;
struct HudTimer;
class Hud;

// orig: HUDObject (vtable 0x002A5118; base ctor 0x000E57F0(name): strncpy(+4, name, 16), +0x13 = 0, +0x15 = 1, +0x18 =
// 0, inserted at the head of the HUD's list +4 (+0x1C next, +0x20 previous)).
class HudElement {
public:
    HudElement(Hud& hud, const char* name);
    virtual ~HudElement() = default;                  // orig: 0x000E5860 (unlinks itself)
    virtual void Update() {}                          // vf01 (+4)
    virtual void Draw() {}                            // vf02 (+8)
    virtual void Command(const char*, const char*) {} // vf03 (+0xC; 0x00011280: RET 8)
    virtual void Command(const char*, f32) {}         // vf04 (+0x10)
    virtual void SetActive(bool on) { active = on; }  // vf05 (+0x14; 0x000E5850)
    bool IsActive() const;                            // vf06 (+0x18; 0x000E58B0: +0x15 and the HUD's +0)
    Hud& hud;
    char name[16] = {};                               // +0x04
    u8 active = 1;                                    // +0x15
};

class Hud {
public:
    Hud(LevelData& level, std::function<bool(const std::string&, std::vector<u8>&)> readFile);   // orig: 0x000E5470
    ~Hud();
    // orig: 0x000C12F0 (the HUD build, Session request kind 2): the elements of the level's kind, then 0x000E5A60(1).
    void Build(const std::string& levelName);
    void Update();                                    // orig: 0x000E58E0 (each element's vf01, when +0 is set)
    void Draw();                                      // orig: 0x000E5900 (each element's vf02, when +0 is set)
    HudElement* Find(const char* name) const;         // orig: 0x000E5920 (strncmp, 15 characters)
    void SetActiveAll(bool on);                       // orig: 0x000E5A60 (each element's vf05)
    void SetActive(const char* name, bool on);        // orig: 0x000E5B60
    void Command(const char* name, const char* cmd, f32 v);           // orig: 0x000E5B90 (vf04)
    void Command(const char* name, const char* cmd, const char* arg); // orig: 0x000E5BC0 (vf03)
    // orig: 0x000E59E0: the cached texture of a file (0x000E5960 by name; else 0x000E5480: the file's first top-level
    // object as a NiTexture, the entry at the head of +8 even when it fails to load).
    ni::NiObject* Texture(const char* file);
    // orig: 0x000E5C00 (texture, w, h, x, y (pixels of 640 x 480, y down), r, g, b, alpha, &texturing, &material, uvs,
    // camera): a 4-vertex polygon (x / 640, 1 - y / 480 ...; uvs default (0, 1) (1, 1) (1, 0) (0, 0)) with a
    // NiTexturingProperty (apply mode 2, filter 1, the base texture), a NiMaterialProperty (all four colours r g b,
    // alpha 1) and, with `alpha`, a NiAlphaProperty (0xED), attached to `camera` (null: the scene camera, S+0x8C +0x30).
    ni::NiPointer<ni::NiScreenPolygon> MakePolygon(ni::NiObject* texture, f32 w, f32 h, f32 x, f32 y,
                                                   const ni::NiColor& rgb, bool alpha, const ni::NiPoint2* uvs,
                                                   ni::NiCamera* camera);
    // orig: 0x000E5F60 (element, file, w, h, x, y, alpha, &texturing, &material, uvs, camera): Texture(file), then
    // MakePolygon with the colour (1, 1, 1).
    ni::NiPointer<ni::NiScreenPolygon> MakeIcon(const char* file, f32 w, f32 h, f32 x, f32 y, bool alpha,
                                                const ni::NiPoint2* uvs, ni::NiCamera* camera);
    ni::NiCamera* SceneCamera() const;                // S+0x8C +0x30
    LevelData& level;
    u8 enabled = 1;                                   // +0x00
    std::vector<HudElement*> elements;                // +0x04 (front = the list head: the last one built)
    struct Tex {                                      // 0x10 bytes (0x000E5480): name, texture, next / previous
        std::string name;
        ni::NiPointer<ni::NiObject> texture;
    };
    std::vector<Tex> textures;                        // +0x08 (front = the head)
    std::function<bool(const std::string&, std::vector<u8>&)> readFile;
    std::vector<std::unique_ptr<HudElement>> owned;
};

// The material property of a polygon (its property state's slot), or null.
ni::NiMaterialProperty* MaterialOf(ni::NiScreenPolygon* p);

// orig: 0x000966A0 (cdecl; the HUD head's model, Impact::SetLit's clone): every geometry below the object, a
// "QDOutline" subtree skipped: the material's ambient = its diffuse, the emissive zero, the vertex-colour property's
// lighting mode 1 / source mode 0.
void PrepareLitModel(ni::NiAVObject* object);

}  // namespace fut::game
