// The save-slot list panel (orig: the base vtable 0x00293C94, ctor 0x0001AC30(arg, text), dtor 0x0001A9C0, the rows
// 0x0001AD60, the shared Update 0x0001B490, Up 0x0001B360 / Down 0x0001B3F0, ResetList 0x0001B2F0, the names
// 0x0001A900 / 0x0001AB80 / 0x0001AB10 / 0x0001AC10, Reset vf0C 0x0001A890) with its list model (new(0x1C) 0x00029A00,
// 0x00029A30 .. 0x00029D90), the front end's load panel (vtable 0x002940E4: ctor 0x0001FBE0, Show 0x0001FCD0, Hide
// 0x0001FD10, Activate 0x0001FE10, Deactivate 0x0001FEB0) and the pause's (CPauseContinuePanel, vtable 0x002A1E14:
// ctor 0x000A7C20, Reset 0x000A7B80, Show 0x000A7CD0, Hide 0x000A7DA0). docs/analysis/frontend_panels_port.md 4.
//
// The panel's own input (Update's up / down / accept / back) is inert while the load flow XBox_L_* runs (the flow sets
// the menu input mode 2); the flow drives Up / Down and reads the selected entry (MemoryCards.cpp).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"
#include "game/app/Frontend.h"
#include "game/app/MemoryCards.h"

#include <array>
#include <string>

namespace fut::text {
class LanguageDb;
class TextObject;
}  // namespace fut::text

namespace fut::game::app {

class App;
class PauseMenu;

// orig: the list model (new(0x1C) 0x00029A00): +0 the head, +4..+0x10 the four visible rows (pointers into the list),
// +0x14 the cursor (0..3; not written by the ctor), +0x18 Language/LEVELNAMES.ldb.
struct SaveList {
    struct Entry {                                    // orig: new(0x20), zeroed
        std::u16string name;                          // +0 (the save game's wide name, or the "nolevels" text)
        // +4: 0 for every memory-card entry and "nolevels" (the panel's accept tests it: the dead App+0xF8 == 0 path);
        // +0xC / +0x10 (the level presets' text and size): 0 likewise
        u32 preset = 0;
        std::string path;                             // +8 ("<szSaveGameDirectory>Futurama.dat")
        MemoryCards::SaveEntry* saveEntry = nullptr;  // +0x14 (the manager's entry, for the delete)
        Entry* next = nullptr;                        // +0x18
        Entry* prev = nullptr;                        // +0x1C
    };
    explicit SaveList(App& app);                      // orig: 0x00029A00
    ~SaveList();                                      // (the panel's dtor: 0x00029D00 + delete)
    SaveList(const SaveList&) = delete;
    SaveList& operator=(const SaveList&) = delete;
    Entry* Selected() const { return rows[size_t(cursor)]; }   // orig: 0x00029A30
    void Append(const std::u16string& name, const std::string& path, MemoryCards::SaveEntry* e);   // orig: 0x00029A40
    bool Down();                                      // orig: 0x00029B00
    bool Up();                                        // orig: 0x00029B70
    void SetTop(s32 n) { (void)n; cursor = 0; }       // orig: 0x00029BD0 (+0x14 = 0 whatever the argument)
    void Reset();                                     // orig: 0x00029BE0 (cursor 0, the first four entries)
    bool CanScrollDown() const { return rows[3] && rows[3]->next; }   // orig: 0x00029C30
    bool CanScrollUp() const { return rows[0] && rows[0]->prev; }     // orig: 0x00029C50
    void Remove(Entry* e);                            // orig: 0x00029C70
    void Clear();                                     // orig: 0x00029D00
    void Fill(App& app);                              // orig: 0x00029D90 (App+0xF8 = 1: the save data's entries)
    Entry* head = nullptr;                            // orig: +0
    std::array<Entry*, 4> rows{};                     // orig: +4..+0x10
    // orig: +0x14: uninitialised heap until the first SetTop / Reset (L: the front end's oracles read 0 there, the
    // pause's an arbitrary word; nothing reads it before the load flow's LoadGames sets it)
    s32 cursor = 0;
    const text::LanguageDb* levelNames = nullptr;     // orig: +0x18 (0x00295014 LEVELNAMES.ldb)
};

// orig: the panel base (0x7C bytes with the derived parts; PanelBase + +0x18 the fold time).
class LoadPanel : public PanelBase {
public:
    LoadPanel(App& app, s32 arg, text::TextObject* text);   // orig: 0x0001AC30(arg, text)
    virtual ~LoadPanel();                             // orig: 0x0001A9C0
    bool Update(f32 dt);                              // orig: vf04 0x0001B490 (false: a game was loaded, dead)
    virtual void Reset();                             // orig: vf0C 0x0001A890 (scroll / selectcontinue / scrolllimit)
    void ReleaseSounds();                             // orig: vf10 0x0001A8D0
    virtual void Show() = 0;                          // vf18
    virtual void Hide() = 0;                          // vf1C
    virtual void Activate() {}                        // vf20 (0x000A32A0 in the base)
    virtual void Deactivate() {}                      // vf24
    // orig: 0x0001AD60(scene): LM_saveslot1..4 (+0x24..), LM_uparrownull / LM_downarrownull (+0x34 / +0x38),
    // loadgame_text1..4 (+0x4C..: scale |1.0|, an alpha property 0xED at the head of their properties), the names,
    // the arrows, vf0C.
    void BuildRows(ni::NiNode* scene);
    void Up();                                        // orig: 0x0001B360
    void Down();                                      // orig: 0x0001B3F0
    void ResetList();                                 // orig: 0x0001B2F0 (row 0 highlighted, selection 0)
    void Redraw();                                    // orig: 0x0001AC10 (0x0001A900, 0x0001AB80, 0x0001AB10)
    void RemoveNames();                               // orig: 0x0001A900
    void AddNames();                                  // orig: 0x0001AB80
    void UpdateArrows();                              // orig: 0x0001AB10
    void ApplyLoaded();                               // orig: 0x0001A940 (the dead App+0xF8 == 0 path)
    static void SetIndex(ni::NiSwitchNode* s, s32 index);   // (+0xE0 = index when +0xCC > index)

    App& app;
    s32 arg = 0;                                      // orig: +0x10 (the ctor's argument)
    f32 t = 0;                                        // orig: +0x18
    ni::NiPointer<ni::NiNode> node;                   // orig: +0xC (the front end: Cam2's node; the pause: loadmenu1)
    bool gameLoaded = false;                          // orig: +0x20 (set only by the dead path)
    std::array<ni::NiPointer<ni::NiSwitchNode>, 4> rows;   // orig: +0x24..+0x30
    ni::NiPointer<ni::NiSwitchNode> upArrow, downArrow;    // orig: +0x34 / +0x38
    s32 scroll = 0;                                   // orig: +0x3C
    std::array<u16, 4> nameIds{};                     // orig: +0x40..+0x46
    text::TextObject* text = nullptr;                 // orig: +0x48
    std::array<ni::NiPointer<ni::NiNode>, 4> nameNodes;   // orig: +0x4C..+0x58
    u32 scrollSound = 0, selectSound = 0, limitSound = 0;   // orig: +0x5C / +0x60 / +0x64
    SaveList list;                                    // orig: +0x68
};

// orig: the front end's load panel (new(0x7C) 0x0001FBE0(3, frontend, +0x98): +0x70 go_back.wav, +0x74 go_to.wav,
// +0x78 the Frontend).
class FrontendLoadPanel : public LoadPanel {
public:
    FrontendLoadPanel(App& app, FrontendContext& fe, text::TextObject* text);
    ~FrontendLoadPanel() override;                    // orig: 0x0001FC70
    void Show() override;                             // orig: 0x0001FCD0
    void Hide() override;                             // orig: 0x0001FD10
    void Activate() override;                         // orig: 0x0001FE10
    void Deactivate() override;                       // orig: 0x0001FEB0
    FrontendContext& fe;                              // orig: +0x78
    u32 goBackSound = 0, goToSound = 0;               // orig: +0x70 / +0x74
};

// orig: CPauseContinuePanel (new(0x7C) 0x000A7C20(0, pause, +0x54): +0x70 the pause menu, +0x74 pause_go_to.wav,
// +0x78 pause_go_back.wav; its fold: t 1.6666666 .. 1.875 (Show), 2.0833333 .. 2.2916667 (Hide) at half steps).
class PauseLoadPanel : public LoadPanel {
public:
    PauseLoadPanel(App& app, PauseMenu& pause, text::TextObject* text);
    ~PauseLoadPanel() override;                       // orig: 0x000A7BB0
    void Reset() override;                            // orig: 0x000A7B80
    void Show() override;                             // orig: 0x000A7CD0
    void Hide() override;                             // orig: 0x000A7DA0
    PauseMenu& pause;                                 // orig: +0x70
    u32 goToSound = 0, goBackSound = 0;               // orig: +0x74 / +0x78
};

}  // namespace fut::game::app
