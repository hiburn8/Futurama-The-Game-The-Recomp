// Decompiled from default.xbe; see TextLayout.h. Floating point follows the original's x87 code (53-bit precision
// control, so x87 stack values are modelled as double and every `fstp dword` as F()).
#include "game/text/TextLayout.h"

#include <cstring>

namespace fut::text {
namespace {

using ni::detail::F;

u16 ReadU16(std::span<const u8> d, size_t at) {
    u16 v;
    std::memcpy(&v, d.data() + at, 2);
    return v;
}

// orig: 0x0009A250. Word-wrap look-ahead at the space at index i: true when the following word does not fit.
// The cursor is advanced by each following glyph's width only AFTER comparing, so the check is one glyph behind.
bool WrapCheck(const LatinGlyphTable& table, const TextStyle& style, const std::u16string& s, u32 i, f32 cursorX,
               f32 boxX, f32 boxWidth) {
    f32 width = boxWidth;
    if (style.hAlign == 0) width = F(double(width) + width);   // centred text may extend width to both sides
    u32 j = i + 1;
    if (s[j] == u' ') return false;
    for (;;) {
        if (j == s.size()) return false;
        if (double(width) + boxX <= cursorX) return true;      // fcomp; NaN compares false
        // orig: kanji mode also breaks after `maxChars` characters (not ported, see KNOWN_ISSUES).
        const u16 w = table.Width(s[j]);
        if (w != 0) cursorX = F(style.scaleX / double(640 / int(w)) + cursorX);
        ++j;
        if (s[j] == u' ') return false;
    }
}

}  // namespace

bool LatinGlyphTable::Load(std::span<const u8> fnt) {   // orig: 0x00098340
    if (fnt.size() < 10 + glyphs_.size() * 10) return false;   // port-only bound (shipped files are exact)
    uScale = ReadU16(fnt, 0);
    vScale = ReadU16(fnt, 2);
    lineHeight = ReadU16(fnt, 4);
    spaceWidth = ReadU16(fnt, 6);
    cellSize = ReadU16(fnt, 8);
    for (size_t i = 0; i < glyphs_.size(); ++i) {
        const size_t at = 10 + i * 10;
        glyphs_[i] = {ReadU16(fnt, at), ReadU16(fnt, at + 2), ReadU16(fnt, at + 4), ReadU16(fnt, at + 6),
                      ReadU16(fnt, at + 8)};
    }
    return true;
}

bool LayoutString(LatinGlyphTable& table, const TextStyle& style, std::u16string_view text, f32 x, f32 y, f32 width,
                  TextMesh& out, bool withNode) {
    out = {};
    // ---- orig: AddString 0x000998C0 ----
    if (!withNode && !(0.0f <= x && 0.0f <= y && x <= 1.0f && y <= 1.0f)) return false;   // only checked without a node
    const size_t nul = text.find(u'\0');                                    // wcslen
    std::u16string s(text.substr(0, nul));
    if (s.empty()) return false;
    if (width == 0.0f) width = F(1.0 - x);
    // Port-only guards: the original divides by these (a zero is an integer divide fault there).
    if (table.lineHeight == 0 || table.spaceWidth == 0) return false;

    // ---- orig: 0x0009AE40 (mesh of 4 vertices / 2 triangles per character) ----
    const u32 n = static_cast<u32>(s.size());
    out.vertices.assign(size_t(n) * 4, {});
    out.uvs.assign(size_t(n) * 4, {});
    out.activeVertices = static_cast<u16>(n << 2);
    out.activeTriangles = static_cast<u16>(n << 1);

    // ---- orig: layout 0x0009A320 (Latin mode: the glyph width comes from each glyph) ----
    std::vector<ni::NiPoint3>& v = out.vertices;
    f32 outX = x, outY = y;
    f32 curZ = 0.0f, curY = y, curX = x;
    u32 quads = 0, lines = 0, lineStart = 0, lineQuads = 0;
    f32 lineWidth = 0.0f;
    // Per-line and per-glyph steps. The depth/x line steps and the glyph's depth/y extents are 0 in both modes.
    const f32 lineStep = F(style.scaleY / double(480 / int(table.lineHeight)));
    f32 glyphW = F(style.scaleX / double(640 / int(table.spaceWidth)));

    auto alignLine = [&] {                            // shift the finished line's quads
        for (u32 q = quads - lineQuads; q < quads; ++q)
            for (u32 c = 0; c < 4; ++c) {
                f32& vx = v[q * 4 + c].z;
                if (style.hAlign == 0)
                    vx = F(double(vx) - double(lineWidth) * 0.5);
                else if (style.hAlign == 2)
                    vx = F(double(vx) - lineWidth);
            }
    };
    auto newLine = [&](u32 i) {
        curZ = F(double(curZ) - 0.0);
        curX = x;
        curY = F(double(curY) - lineStep);
        alignLine();
        lineStart = i + 1;
        ++lines;
        lineWidth = 0.0f;
        lineQuads = 0;
    };

    for (u32 i = 0; i < n; ++i) {
        const char16_t ch = s[i];
        if (ch == u'\n' || ch == 0) {
            newLine(i);
            continue;
        }
        if (ch == u' ') {
            if (style.wordWrap && WrapCheck(table, style, s, i, curX, x, width)) {
                newLine(i);
                continue;
            }
            if (table.spaceWidth == 0) table.spaceWidth = 0x10;   // orig: written back into the table
            const double advance = style.scaleX / double(640 / int(table.spaceWidth));
            curX = F(curX + advance);
            lineWidth = F(advance + lineWidth);
            continue;
        }
        u16 gw = table.Width(ch);
        if (gw == 0 || gw == 0xFFFE) {               // no glyph: draw the space glyph instead
            s[i] = u' ';
            gw = table.Width(u' ');
            if (gw == 0 || gw == 0xFFFE) gw = 10;
        }
        glyphW = F(style.scaleX / double(640 / int(gw)));   // orig: skipped in fixed-width (kanji) mode
        const char16_t g = s[i];
        const f32 u0 = F(double(table.X(g)) / int(table.uScale));
        const f32 v0 = F(double(table.Y(g)) / int(table.vScale));
        const f32 du = F(double(table.Width(g)) / int(table.uScale));
        const double dv = double(table.Height(g)) / int(table.vScale);   // stays on the x87 stack
        if (du == 0.0f && dv == 0.0) continue;

        const size_t q = size_t(quads) * 4;
        const double vBottom = dv + v0;
        const double z1 = double(curZ) - 0.0;
        const f32 yBottom = F(double(curY) - lineStep);
        const f32 xLeft = F(double(curX) - 0.0);
        const f32 uRight = F(double(du) + u0);
        const double z3 = 0.0 + curZ;
        const double y3 = 0.0 + curY;
        const f32 xRight = F(double(glyphW) + curX);
        v[q + 0] = {curZ, curY, curX};
        v[q + 1] = {F(z1), yBottom, xLeft};
        v[q + 2] = {F(z1 + 0.0), F(0.0 + yBottom), F(double(xLeft) + glyphW)};
        v[q + 3] = {F(z3), F(y3), xRight};
        out.uvs[q + 0] = {u0, v0};
        out.uvs[q + 1] = {u0, F(vBottom)};
        out.uvs[q + 2] = {uRight, F(vBottom)};
        out.uvs[q + 3] = {uRight, v0};
        lineWidth = F((double(glyphW) - style.letterSpacing) + lineWidth);   // orig: Latin mode only
        curZ = F(z3);
        curY = F(y3);
        curX = F(double(xRight) - style.letterSpacing);
        ++lineQuads;
        ++quads;
    }
    if (lineStart < n) alignLine();

    if (style.vAlign == 4) {                          // move everything down by the number of line breaks
        const double off = double(lines) * lineStep;
        const f32 offF = F(off);
        const double total = off + lineStep;
        outX = x;
        outY = F(double(outY) + total);
        for (u32 k = 0; k < quads * 4; ++k) v[k].y = F(double(offF) + v[k].y);
        curY = F(total + curY);
        curX = F(0.0 + curX);
    }
    if (style.vAlign == 0) {                          // centre vertically
        const double L = lines;
        const f32 a = F(L * lineStep);
        const f32 b = F(0.0 * L);
        const f32 cz = F((0.0 * L) * 0.5);
        const f32 cy = F(double(a) * 0.5);
        const f32 cx = F(double(b) * 0.5);
        for (u32 k = 0; k < quads * 4; ++k) {
            v[k].x = F(double(cz) + v[k].x);
            v[k].y = F(double(cy) + v[k].y);
            v[k].z = F(double(cx) + v[k].z);
        }
    }
    if (style.vAlign == 3) {
        outX = curX;
        outY = curY;
    }
    if (quads == 0) {
        out.hidden = true;
    } else {
        out.activeTriangles = static_cast<u16>(quads * 2);
        out.activeVertices = static_cast<u16>(quads * 4);
    }
    out.extentX = outX;
    out.extentY = outY;
    out.text = std::move(s);
    return true;
}

}  // namespace fut::text
