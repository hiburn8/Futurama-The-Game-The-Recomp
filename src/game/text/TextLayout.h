// Screen text: bitmap-font glyph tables (.fnt) and the string layout that turns a UTF-16 string into a quad mesh,
// decompiled from default.xbe (glyph tables 0x00097D10-0x000984E0, text object 0x00098DF0-0x00099CA0,
// layout 0x0009A250-0x0009AE40).
//
// .fnt (Latin tables; all shipped Latin fonts have 256 records):
//   u16 header[5] = { +6 u normaliser, +4 v normaliser, lineHeight, spaceWidth, cellSize }   (stored in that order)
//   256 x { u16 code, x, y, width, height }   pixel rectangle of each byte value's glyph in the font texture
// Glyphs are looked up by the LOW BYTE of the UTF-16 character (the table never compares `code`).
//
// Coordinates are fractions of the 640x480 screen; y grows upwards (each line moves down by lineStep). A glyph quad is
// stored as 4 vertices (depth, y, x) - the original keeps the screen x in the third component - and 4 UVs:
//   0 = (top-left) cursor, 1 = bottom-left, 2 = bottom-right, 3 = top-right.
//
// Verified bit-exactly against the original on every string of the game's language databases with all Latin fonts
// and varied alignment / wrap / spacing / scale settings (tests/game/TextLayoutTests.cpp).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiMath.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace fut::text {

class LatinGlyphTable {                               // orig: vtable 0x002A1360 (0x1C bytes)
public:
    struct Glyph {
        u16 code, x, y, width, height;
    };
    // orig: 0x00098340 (vtable slot 0). False if the data is too short.
    bool Load(std::span<const u8> fnt);

    u16 X(char16_t c) const { return glyphs_[c & 0xFF].x; }             // orig: 0x00098430 (slot 1)
    u16 Y(char16_t c) const { return glyphs_[c & 0xFF].y; }             // orig: 0x00098450 (slot 2)
    u16 Width(char16_t c) const { return glyphs_[c & 0xFF].width; }     // orig: 0x00098470 (slot 3)
    u16 Height(char16_t c) const { return glyphs_[c & 0xFF].height; }   // orig: 0x00098490 (slot 4)

    u16 vScale = 0;        // orig: +4 (file header[1])
    u16 uScale = 0;        // orig: +6 (file header[0])
    u16 lineHeight = 0;    // orig: +8
    u16 spaceWidth = 0;    // orig: +A (the layout replaces 0 by 16, permanently)
    u16 cellSize = 0;      // orig: +C

private:
    std::array<Glyph, 256> glyphs_{};                 // orig: +0x10 (count 0x100 at +0x18)
};

// Text-object settings the layout reads (orig: the 0x124-byte text object; defaults from ctor 0x00098DF0).
struct TextStyle {
    f32 letterSpacing = 0.0f;   // orig: +0x04, subtracted after every glyph
    s32 maxChars = 10;          // orig: +0x08, copied into each string (only used by the kanji mode)
    f32 defaultScale = 1.0f;    // orig: +0x10
    f32 scaleX = 1.0f;          // orig: +0x58
    f32 scaleY = 0.8f;          // orig: +0x5C
    s32 hAlign = 1;             // orig: +0x60: 0 centred on x, 1 left, 2 right-aligned at x
    s32 vAlign = 3;             // orig: +0x64: 0 centred, 4 bottom, 3 report the end cursor, others none
    bool wordWrap = true;       // orig: +0x68
};

struct TextMesh {
    std::u16string text;                  // the string after layout (missing glyphs replaced by spaces)
    std::vector<ni::NiPoint3> vertices;   // 4 per character slot; only the first activeVertices are written
    std::vector<std::array<f32, 2>> uvs;
    u16 activeVertices = 0, activeTriangles = 0;
    bool hidden = false;                  // no quad was emitted (NiAVObject flag 1)
    f32 extentX = 0.0f, extentY = 0.0f;   // orig: returned by the layout, stored at text object +0x4C/+0x50
};

// orig: TextObject::AddString 0x000998C0 (Latin mode, no external node) -> 0x0009AE40 -> layout 0x0009A320.
// Returns false (and leaves `out` empty) where the original returns string id 0: empty string, or a position
// outside [0,1]. width == 0 means "to the right screen edge" (1 - x).
// `withNode`: the string goes under a scene node (AddString's node argument): the position is not checked.
bool LayoutString(LatinGlyphTable& table, const TextStyle& style, std::u16string_view text, f32 x, f32 y, f32 width,
                  TextMesh& out, bool withNode = false);

}  // namespace fut::text
