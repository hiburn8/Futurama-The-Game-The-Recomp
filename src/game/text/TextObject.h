// The screen text object (0x124 bytes; TextObject::Create 0x00098AD0, ctor 0x00098DF0, Init 0x00099CA0): a font
// (glyph table + texture), its own camera looking at a node of string meshes (one NiTriShape of 4 vertices / 2
// triangles per character each, TextString::BuildMesh 0x0009AE40 with the layout of TextLayout.h), drawn by its
// camera's Click (0x00098DB0). Ported: the Latin mode (font 0, fonts/Latin1), the scene mode of the menus (0x00099BE0:
// the strings go under the menu scene's nodes and are drawn by its camera), the z-buffer switch (0x00099660); NOT
// PORTED: the kanji mode (font mode 1: KNOWN_ISSUES #12), the per-string alpha setter (0x00098ED0).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"
#include "game/text/TextLayout.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fut::ni {
class NiRenderer;
}

namespace fut::text {

class TextObject {
public:
    using ReadFileFn = std::function<bool(const std::string&, std::vector<u8>&)>;
    // orig: 0x00098AD0 (font index): the font (the font list 0x002E6C70, else Font::Create 0x00098A30 -> Font::Load
    // 0x00098630: font 0 = fonts/Latin1.nif + fonts/Latin1.fnt), new(0x124) 0x00098DF0, Init 0x00099CA0 (the
    // camera, the node, the texturing property), then 0x00098F80(App->vf14()). Null for a font other than 0 (L:
    // not ported) or when the glyph table does not load.
    static TextObject* Create(u32 font, const ReadFileFn& readFile, ni::NiRenderer* renderer);
    void SetRenderer(ni::NiRenderer* renderer);       // orig: 0x00098F80 (unless +0xC: camera +0x1A8, Update(0, 1))
    // orig: 0x00099BE0 (camera, node): the scene mode (+0xC = 1): the camera and the node replace the text's own,
    // then SetZBufferTest(1) (on the scene's node).
    void UseScene(ni::NiCamera* sceneCamera, ni::NiNode* sceneNode);
    // orig: 0x00099660 (on): the node's z-buffer property (a new one at the head of its list when it has none:
    // flags 3, function 3, | 2), its flag bit 0 = on; +0x79 = 1, +0x78 = on.
    void SetZBufferTest(bool on);
    // orig: 0x0009A0A0 (text, node, alpha, width): AddString(text, 0, 0, width -1 = +0x54, node, alpha).
    u16 AddStringAt(std::u16string_view text, ni::NiNode* at, f32 alpha, f32 w);
    void SetScale(f32 sx, f32 sy) { style.scaleX = sx, style.scaleY = sy; }   // orig: 0x00098C00
    // orig: 0x0009A070 (text, alpha): AddString(text, +0x4C, +0x50, +0x54, no node, alpha).
    u16 AddString(std::u16string_view text, f32 alpha) { return AddString(text, x, y, width, alpha); }
    // orig: 0x000998C0 (text, x, y, width, node, alpha; alpha -1 = +0x10): the string's mesh under the text's node
    // (+0x1C; in the scene mode under `at`, which is then updated) and the end cursor in +0x4C / +0x50 (vAlign 3); 0
    // for an empty string or (without `at`) a position outside [0, 1].
    u16 AddString(std::u16string_view text, f32 x, f32 y, f32 width, f32 alpha, ni::NiNode* at = nullptr);
    // orig: 0x00098FD0 (id): the string's shape detached from its parent, unlinked, +0x120 - 1; +0x79 = 1 either way;
    // false when no string has the id.
    bool RemoveString(u16 id);
    void RemoveAll();                                 // orig: 0x00099080 (each string 0x00098FD0)
    void UpdateCamera();                              // orig: 0x00098D70 (unless +0xC, when +0x79: the node's and
                                                      //  the camera's Update)
    void Click();                                     // orig: 0x00098DB0 (when +0x120: the camera's Click)

    ni::NiPointer<ni::NiCamera> camera;               // +0x00
    bool sceneMode = false;                           // +0x0C (0x00099BE0)
    TextStyle style;                                  // +0x04 .. +0x68
    ni::NiPointer<ni::NiTexturingProperty> texturing; // +0x18
    ni::NiPointer<ni::NiNode> node;                   // +0x1C
    LatinGlyphTable table;                            // +0x38
    u16 nextId = 1;                                   // +0x44
    f32 x = 0.1f, y = 0.5f, width = 0.0f;             // +0x4C / +0x50 / +0x54
    ni::NiColor color{1, 1, 1};                       // +0x6C
    bool zTest = false;                               // +0x78
    u8 dirty = 0;                                     // +0x79
    u8 field7A = 0;                                   // +0x7A (set by the menus; no reader in the text code)
    ni::NiPointer<ni::NiObject> fontTexture;          // +0x11C (the font's NiSourceTexture)
    u32 stringCount = 0;                              // +0x120 (AddString: 1 into an empty list, else + 1; RemoveString - 1)
    struct String {                                   // TextString (0x38 bytes, ctor 0x0009ACE0)
        u16 id = 0;                                   // +0x20
        ni::NiPointer<ni::NiTriShape> shape;          // +0x24
    };
    std::vector<String> strings;                      // +0x48 (a list; front = the head)
};

// orig: App::Init 0x000D2730's error overlay (0x000F3470) makes the first TextObject, so font 0 (fonts/Latin1, the
// texture right after the renderer's clipper) exists before any level; every later TextObject reuses it from the
// font list. The App's error overlay (game/app/ErrorOverlay.cpp) makes that TextObject; the harnesses without an App
// (the render census, the viewer) call this for the font alone. Idempotent.
void CreateAppFont(const TextObject::ReadFileFn& readFile);
// orig: Font::DestroyAll 0x00098AA0 (the App's teardown 0x000D29B0): every font of the list.
void DestroyAllFonts();

}  // namespace fut::text
