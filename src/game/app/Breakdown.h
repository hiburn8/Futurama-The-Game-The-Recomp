// The end-of-level menu ("breakdown", orig: DAT_002E8EEC, new(0x94) 0x000A4E70 at the first FsChangeLevel,
// persistent): Open 0x000A6340, Update 0x000A59D0, Draw 0x000A4C40, the sequence start 0x000A5810. Its four items:
// Save (the in-game save flow), Continue (the next level, App+0xAC), Replay (the level again with its start state),
// Quit (to the Frontend). docs/analysis/frontend_port.md 1.11, 2.5.
//
// The scene (round 7): pause\breakdown.nif's first NiNode with its keyframe manager (every sequence, "klinga" looping),
// the camera, the four items and "collect", glow1Shape / glowShape2 hidden, the pre-cache 0x000CEAC0, the items'
// highlights, the enter / leave and "klinga" sequences, Open's three black frames, Draw 0x000A4C40 (with the yes / no
// dialog's draw while a confirm runs), the eight sounds and the music. The statistics, the rating and the items' texts
// (round 8): Open's 0x000A65A8 .. 0x000A6F1B through AddText 0x000A6010 into the text records +0x74 (TextRecord:
// 0x000A4AC0 / 0x000A4BC0 / 0x000A5880 / 0x000A4DC0), updated every step by Update's loop 0x000A5BBC.
#pragma once

#include "core/Types.h"
#include "engine/ni/NiKeyframeManager.h"
#include "engine/ni/NiObjects.h"

#include <array>
#include <bit>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fut::text {
class TextObject;
class LanguageDb;
}  // namespace fut::text

namespace fut::game::app {

class App;

// orig: a text record of the breakdown (0x4C bytes, ctor 0x000A4AC0, dtor 0x000A4BC0): one string of the scene-mode
// text object under a node of pause\breakdown.nif, added by the first Update after Open (mode 0) or typed in one
// character per 0.1 s (mode 1); colour mode 2 pulses its colour (0x000A4DC0).
struct TextRecord {
    // orig: 0x000A4AC0 (text, text object, scale, hAlign, node, x, y, mode): the colour white, +0xC = the text
    // object's letter spacing, the typed buffer empty
    TextRecord(std::u16string_view text, text::TextObject* to, f32 scale, s32 hAlign, ni::NiNode* node, f32 x, f32 y,
               s32 mode);
    ~TextRecord();                                    // orig: 0x000A4BC0 (the string removed from the text object)
    s32 Update();                                     // orig: 0x000A5880 (0 nothing / done, 1 waiting, 2 typed, 3 added)
    void ApplyColour();                               // orig: 0x000A4DC0 (the node's children's material emissive)

    f32 time = 0;                                     // +0x00
    f32 accum = 0;                                    // +0x04 (the typing timer)
    f32 interval = std::bit_cast<f32>(0x3CCCCCCDu);   // +0x08 (0.1 s per character)
    f32 spacing = 0;                                  // +0x0C (the text object's +4 to add with)
    s32 mode = 0;                                     // +0x10 (0 the whole string, 1 typed)
    s32 colourMode = 0;                               // +0x14 (2: the pulse)
    u16 id = 0;                                       // +0x18 (the text object's string)
    u32 typedCount = 0;                               // +0x1C
    f32 x = 0, y = 0;                                 // +0x20 / +0x24
    ni::NiColor colour{1, 1, 1};                      // +0x28
    std::u16string typed;                             // +0x34 (new(2 n + 2), [0] = 0)
    std::u16string full;                              // +0x38 (a copy of the text)
    text::TextObject* text = nullptr;                 // +0x3C
    f32 scale = 0;                                    // +0x40 (no reader)
    s32 hAlign = 0;                                   // +0x44 (the text object's +0x60 at AddText)
    ni::NiPointer<ni::NiNode> node;                   // +0x48
};

class Breakdown {
public:
    explicit Breakdown(App& app);                     // orig: 0x000A4E70 (pause\breakdown.nif: its sequences)
    ~Breakdown();
    void Open();                                      // orig: 0x000A6340 (the lives top-up for a new chapter)
    void Update();                                    // orig: 0x000A59D0 (one call per step while open)
    void Draw();                                      // orig: 0x000A4C40 (Ingame::Render while open)

    App& app;
    bool closing = false;                             // orig: +0
    bool open = false;                                // orig: +1
    s32 selection = 0;                                // orig: +4 (0 Save, 1 Continue, 2 Replay, 3 Quit)
    s32 highlighted = 0;                              // orig: +8
    s32 confirm = 0;                                  // orig: +0xC (1 replay, 2 quit, 3 continue)
    f32 time = 0;                                     // orig: +0x5C
    f32 repeatTimer = 0;                              // orig: +0x60
    s32 repeatDir = 0;                                // orig: +0x64
    bool continueDirect = false;                      // orig: +0x90 (set by a successful in-game save)

    // orig: +0x10, the running enter / leave sequence (NiKeyframeManager +0x14's lookup 0x0001BFC0 by name: null when
    // the file has none) and its +0xA8 (= -time at its start, 0x000A5827 / 0x000A6439)
    struct Sequence {
        std::string name;
        bool hasController = false;                   // (its reference controller +0x48[+0x90])
        double length = 0;                            // (hi - lo) / frequency of that controller
    };
    const Sequence* sequence = nullptr;
    f32 sequenceOffset = 0;

    // the scene (render)
    ni::NiPointer<ni::NiKeyframeManager> manager;     // orig: +0x14 (on the scene)
    ni::NiPointer<ni::NiNode> cameraNode;             // orig: +0x18 ("CAMERA")
    ni::NiPointer<ni::NiNode> scene;                  // orig: +0x1C (the file's first NiNode)
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x24 ("CAMERA1")
    std::array<ni::NiPointer<ni::NiSwitchNode>, 4> items;   // orig: +0x28..+0x34 (save / continue / replay / quit)
    ni::NiPointer<ni::NiSwitchNode> collect;          // orig: +0x38 ("collect")
    // orig: +0x3C cursor, +0x40 select, +0x44 toggleerror, +0x48 Pause_fold_up, +0x4C Pause_fold_down, +0x50
    // toggleerror, +0x54 toggleok, +0x58 EndOfLevel_theme.wav (music)
    std::array<u32, 8> sounds{};
    std::unique_ptr<text::TextObject> text;           // orig: +0x68 (scene mode on the camera)
    const text::LanguageDb* levelNames = nullptr;     // orig: +0x6C
    const text::LanguageDb* texts = nullptr;          // orig: +0x70 (Language/BREAKDOWN.ldb)
    // orig: +0x74, the text records (NiTPrimitiveArray: data +0x78, size +0x80; +0x8C the update loop's index)
    std::vector<std::unique_ptr<TextRecord>> records;

private:
    void StartSequence(const char* name);             // orig: 0x000A5810
    // orig: 0x000A6010 (cdecl: this, "node|x|y", scale, colour, colour mode, mode, text id, format, ...): the node of
    // the scene (vf54, a NiNode), x = atof(x) * 1.1, y = atof(y) * -0.085; the text: the id's string (TextManager
    // Find, else the id itself widened) or the formatted one; a new record appended to +0x74 (none without a node).
    void AddText(const char* spec, f32 scale, const ni::NiColor& colour, s32 colourMode, s32 mode, const char* textId,
                 const char* format, ...);
    void DeleteRecords();                             // orig: 0x000A6526 / 0x000A5C84 (each deleted, then RemoveAll)
    void Confirm(s32 mode);                           // orig: the confirm dialog of Continue / Replay / Quit
    const Sequence* FindSequence(const char* name) const;   // orig: 0x0001BFC0
    std::vector<Sequence> sequences_;                 // (the file's top-level NiSequences, in order)
    std::vector<ni::NiPointer<ni::NiSequence>> niSequences_;   // (the same, as the manager holds them)
    ni::NiSequence* NiSequenceOf(const Sequence* q) const;
};

}  // namespace fut::game::app
