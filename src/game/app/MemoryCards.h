// The memory-card manager (orig: App+0xF4, ctor 0x000E4B30, 0xA4 bytes) and its flows, the yes / no dialog
// (manager +0x10, ctor 0x000E3100) and the save-game storage seam. docs/analysis/frontend_port.md 1.8.
//
// A flow is a chain of small state objects (each 0x1C bytes or more, base ctor 0x000DF370, vtable [0] dtor, [1] Enter,
// [2] Update, [3] Exit, [4] Name): the manager's step 0x000E5380 runs the current state's phase (0x000DF3D0: +0x18 = 0
// Enter, 1 Update, 2 Exit); a state queues its successor as the manager's pending state (0x000E4CE0) and its Exit
// (0x000DF3B0) clears the current one; the next step makes the pending state current. While a flow runs ((+0xA) set)
// the pause cannot open and the menus ignore input. The states are identified by their original vtables (the boot
// oracle's MC lines).
//
// Ported: the manager's step and starters, the dialog's step 0x000E4A00 and its state, the flows of the empty-card
// paths the boot oracles take (the options load XBox_OL_*, the extras load XBox_EL_*, New Game's free-space check
// XBox_B_*), the in-game save XBox_IS_* (the list, the Saving state with the Futurama.dat and Unlock.dat writes, the
// save images, the SaveOK / failure prompts), the 0x40-byte dialog-state base 0x000DF7E0, the save device +0xC
// (0x000E09C0, the doc's "device watcher": the save games' file layer over SaveStorage.h, its vf38 the alertable wait
// that runs a write's completion), the options save XBox_OS_* and the confirm box CB_*, the load flow XBox_L_* / FL /
// IL (the save-slot list of game/app/LoadPanel.h, the listing 0x000E05D0, the read 0x000E0720, the corrupt save's
// delete, the carry text 0x000C1DC0 / 0x000C1E10). NOT PORTED (reported when reached): the other prompts (full card,
// the options prompt), ConfirmOverwritePrompt (an existing save), the device's memory-unit hot plug (vf48 / vf4C: the
// host storage has one fixed card).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiKeyframeManager.h"
#include "engine/ni/NiObjects.h"
#include "game/app/SaveStorage.h"
#include "game/input/Pad.h"

#include <array>
#include <list>
#include <optional>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fut::text {
class LanguageDb;
class TextObject;
}  // namespace fut::text

namespace fut::game {
struct CarryOver;
}

namespace fut::game::app {

class App;
class MemoryCards;
struct LevelRecords;

// orig: 0x000C1E10(text): the value of the first "Level=" line (each line "\n"-terminated, split at its first '='; the
// key compared over 6 bytes with "Level\0"), a heap copy; none = null. (No NULL tests in the original: a line without
// '\n' or '=' is UB there; the port stops.)
std::optional<std::string> CarryLevelValue(const char* text, size_t size);
// orig: 0x000C1DC0(data, size): DAT_002EAA28 = a copy of `size` bytes (the old one freed), DAT_002EAA2C = size (both
// unchanged when data or size is 0). The port's text stops at the first NUL (CarryOver keeps the text without it).
void SetCarryState(CarryOver& carry, const u8* data, size_t size);
// (port) the flows' process globals reset (DAT_002D6E14 / 18 / 1C, DAT_002ED000 / 380): every oracle run is a process.
void ResetMemoryCardGlobals();

// orig: a flow state (base vtable 0x002A454C, ctor 0x000DF370: +0xC the manager, +4 the save device, +8 the dialog,
// +0x10 the save data, +0x14 its +4, +0x18 the phase 0).
class FlowState {
public:
    FlowState(MemoryCards& m, u32 vtable) : m_(m), vtable(vtable) {}
    virtual ~FlowState() = default;
    virtual void Enter() { phase = 2; }               // vf04 (default 0x000DC0D0: straight to the exit)
    virtual void Update() {}                          // vf08 (0x000A32A0: nothing)
    virtual void Exit();                              // vf0C (default 0x000DF400 -> 0x000DF3B0)
    void Step();                                      // orig: 0x000DF3D0
    const u32 vtable;                                 // (the original's vtable: the state's identity)
    s32 phase = 0;                                    // orig: +0x18

protected:
    void Finish();                                    // orig: 0x000DF3B0 (manager +0x18 = 0; delete this)
    MemoryCards& m_;
};

// orig: the yes / no dialog (manager +0x10, new(0xA8) 0x000E3100, memcard\Dialogmenu.nif 0x000E3B50).
struct Dialog {
    Dialog();
    ~Dialog();
    // orig: 0x000E3B50: Language/XBOXMEM.ldb / MEMHINTS_XBOX.ldb, memcard\Dialogmenu.nif's objects under a new node,
    // its sequences, nodes, camera, keyframe managers, the two text objects, the z-buffer property, the sounds.
    // False when CAMERA1 or "Dialogmenu:Animation (Translational)" is missing (the rest is then skipped).
    bool Build(App& app);
    // orig: 0x000E33A0 (App::Frame while a flow runs): the scene node and the manager at +0x8C, a z clear on the
    // camera's renderer (vf44(0, 4)), the camera's update, the text's UpdateCamera and Click (its strings: the scene).
    void Draw();
    // orig: 0x000E4A00 (each manager step, twice while a flow runs): the open / close animation.
    void Step(App& app);
    bool Animating() const { return double(anim) > -0.1f; }   // orig: 0x000E3020 (FCOMP [0x00293BF4])
    // orig: 0x000E35A0: the selection (0 the left button, 1 the right one, 2 the single one, 3 none): unless 3 or its
    // last sound still plays, the scroll sound (+0x48 when already selected, else +0x40); the three switches'
    // highlights and the three highlight sequences.
    void Select(s32 sel);
    // orig: 0x000E3570(id): TextManager::GetText(id, XBOXMEM.ldb) into 0x000E3500: the text's string under the "text"
    // node (+0xA2) unless one is shown or the dialog animates.
    bool SetText(const char* id);
    bool SetString(const char16_t* s);                // orig: 0x000E3500
    bool RemoveText();                                // orig: 0x000E3040 (the string +0xA2, when idle)
    void ClearHint();                                 // orig: 0x000E3080 (the hint text's RemoveAll and UpdateCamera)
    // orig: 0x000E3790(left, right) / 0x000E3930(single): the buttons' labels (1 MEMCARD_040 .. 7 MEMCARD_046, 0 none)
    // under save_text_yes / save_text_no / text_ok, after 0x000E30A0 removes the old ones (+0 / +2 / +4).
    void SetButtons(s32 left, s32 right);
    void SetSingle(s32 n);
    void ClearButtons();                              // orig: 0x000E30A0
    App* owner = nullptr;                             // (Build's App)
    std::array<u16, 3> buttonIds{};                   // orig: +0x00 / +0x02 / +0x04
    u16 textId = 0;                                   // orig: +0xA2
    s32 selectVoice = 0;                              // orig: +0x4C
    u16 hintId = 0;                                   // orig: +0x34 (the hint string)
    bool Open();                                      // orig: 0x000E4900 (the enter sequence)
    bool Close();                                     // orig: 0x000E4980 (the leave sequence)
    // the lengths of memcard\Dialogmenu.nif's sequences "Dialog_menu_enter" (+0x64) and "Dialog_manu_leave" (+0x68)
    // ((hi - lo) / frequency of the reference controller, 0 without one)
    double enterLength = 0, leaveLength = 0;
    // orig: +0x54 (new NiNode: the file's top-level objects; its lights keep their effect ids), +0x64 / +0x68 the
    // enter / leave sequences (the others, +0x14 / +0x18 / +0x1C, are not kept by the port)
    ni::NiPointer<ni::NiNode> scene;
    ni::NiPointer<ni::NiSequence> enter, leave;
    ni::NiPointer<ni::NiKeyframeManager> yesManager, noManager, okManager;   // orig: +0x08 / +0x0C / +0x10
    ni::NiPointer<ni::NiSequence> yes, no, ok;        // orig: +0x14 / +0x18 / +0x1C ("yes" / "no" / "ok")
    ni::NiPointer<ni::NiNode> yesNode, noNode, okNode;   // orig: +0x20 / +0x24 / +0x28 ("save_yes" / "save_no" / "ok")
    std::unique_ptr<text::TextObject> hintText;       // orig: +0x30
    const text::LanguageDb* hintsDb = nullptr;        // orig: +0x38 (Language/MEMHINTS_XBOX.ldb)
    const text::LanguageDb* textsDb = nullptr;        // orig: +0x3C (Language/XBOXMEM.ldb)
    u32 scrollSound = 0, selectSound = 0, scrollLimitSound = 0;   // orig: +0x40 / +0x44 / +0x48
    std::unique_ptr<text::TextObject> text;           // orig: +0x50 (scene mode: the dialog's camera and +0x54)
    ni::NiPointer<ni::NiNode> sceneNode;              // orig: +0x60 ("SceneNode")
    ni::NiPointer<ni::NiNode> animNode;               // orig: +0x6C ("Dialogmenu:Animation (Translational)")
    ni::NiPointer<ni::NiSwitchNode> yesSwitch, noSwitch, okSwitch;   // orig: +0x70 / +0x74 / +0x78
    ni::NiPointer<ni::NiNode> yesText, noText, okText;   // orig: +0x7C / +0x80 / +0x84 ("save_text_yes" ..)
    ni::NiPointer<ni::NiKeyframeManager> manager;     // orig: +0x94 (on +0x6C: the enter / leave sequences)
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x98 ("CAMERA1"; +0x15C = +0x60)
    ni::NiPointer<ni::NiNode> textNode;               // orig: +0x9C ("text")
    bool flag2C = false;                              // orig: +0x2C
    f32 time = 0;                                     // orig: +0x8C (+= dt * 0.5 while +0xA0)
    f32 anim = -0.1f;                                 // orig: +0x90 (the animation's remaining time; 0xBDCCCCCD idle)
    s32 selection = 3;                                // orig: +0x5C (0 yes, 1 no, 3 none)
    bool open = false;                                // orig: +0xA0 (shown)
    bool opening = false;                             // orig: +0xA1 (the open animation runs)
};

// orig: the save-game list panel of the save data ((manager +4)+4, new(0x90) 0x000E1E60, vtable 0x002A4ED4: vf00
// 0x000E2080 loads memcard\Savemenu.nif, vf04 0x000E2A70 the step): its appear / disappear animation, the part the
// flows wait on, and since 2026-10-19 its scene, rows, names and draw. Since then its scrolling too
// (0x000E2D70 / 0x000E2E40: more than three entries).
struct MenuLabels;

// orig: the save panel (the save data's +4, new(0x90) 0x000E1E60, vtable 0x002A4ED4; memcard\Savemenu.nif): the
// in-game save's list of three card slots. Its rows' head +0x74 and selection +0x68 are MemoryCards::panelHead /
// panelSelection (the save data's list lives there); MemoryCards' Panel* functions are the panel's row functions.
struct SavePanel {
    SavePanel();
    ~SavePanel();
    SavePanel(SavePanel&&) noexcept;
    SavePanel& operator=(SavePanel&&) noexcept;
    // orig: 0x000E2080: the scene (the file's top-level objects under a new node), the sequences, CAMERA1, SceneNode,
    // Text4..6, Cardslot4..6, the scroll arrows, the two keyframe managers on "Savemenu", Allways_animated, the
    // camera's renderer, the text object, LEVELNAMES.ldb and the labels object.
    void Load(App& app);
    // orig: 0x000E2BA0 (0 appear: the labels' database and texts, the rows 0x000E2AC0; 1 disappear: the labels removed;
    // false while animating), then the sequence from -time activated and its length.
    bool Show(s32 which, MemoryCards& m);
    bool Busy() const { return double(remaining) > 0.0; }   // orig: 0x000E19A0 (FCOMP 0.0; TEST AH,0x41)
    void Step();                                      // orig: 0x000E2A70 (+ 0x000E1A70)
    // orig: 0x000E1A00 (vf08, App::Frame while a flow runs, when shown): the scene node, Allways_animated and the
    // running manager at +0xC, the text's UpdateCamera, a z clear, the camera's Update and Click.
    void Draw();
    s32 state = 2;                                    // orig: +4 (2 at construction, 0 once shown, 1 once hidden)
    bool shown = false;                               // orig: +9
    f32 time = 0;                                     // orig: +0xC
    s32 which = 0;                                    // orig: +0x40 (the running sequence: 0 appear, 1 disappear)
    f32 remaining = 0;                                // orig: +0x44
    s32 counter = 0;                                  // orig: +0x78
    // the sequences +0x30 "Savepanel_Savegame_appear" / +0x34 "Savepanel_Savegame_Dissappear" (sic): (hi - lo) /
    // frequency of their reference controller (the last top-level sequence of the name wins)
    bool hasSequence[2] = {false, false};
    double length[2] = {0, 0};
    std::array<ni::NiPointer<ni::NiSequence>, 2> sequences;   // orig: +0x30 / +0x34
    std::array<ni::NiPointer<ni::NiKeyframeManager>, 2> managers;   // orig: +0x28 / +0x2C (on +0x38 / +0x3C)
    std::array<ni::NiPointer<ni::NiNode>, 2> targets; // orig: +0x38 / +0x3C (both "Savemenu")
    std::unique_ptr<text::TextObject> text;           // orig: +0x10 (the rows' names, scene mode)
    const text::LanguageDb* levelNamesDb = nullptr;   // orig: +0x14 (Language/LEVELNAMES.ldb)
    ni::NiPointer<ni::NiNode> scene;                  // orig: +0x18 (new NiNode: the file's top-level objects)
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x1C ("CAMERA1"; +0x15C = +0x18, +0x1A8 the renderer)
    ni::NiPointer<ni::NiNode> sceneNode;              // orig: +0x20 ("SceneNode")
    ni::NiPointer<ni::NiNode> animated;               // orig: +0x24 ("Allways_animated" under +0x20)
    std::array<ni::NiPointer<ni::NiNode>, 3> texts;   // orig: +0x48 / +0x4C / +0x50 ("Text4" .. "Text6")
    std::array<u16, 3> textIds{};                     // orig: +0x54 .. +0x58 (the rows' string ids; L: 0 at first)
    std::array<ni::NiPointer<ni::NiSwitchNode>, 3> slots;   // orig: +0x5C / +0x60 / +0x64 ("Cardslot4" .. "Cardslot6")
    ni::NiPointer<ni::NiSwitchNode> arrowTop, arrowBottom;  // orig: +0x6C / +0x70 ("Scrollarrow2_top" / "_bottom")
    std::unique_ptr<MenuLabels> labels;               // orig: +0x8C (new(0x10) 0x00025710(+0x18, +0x1C))
    // orig: 0x000E19C0 (the manager's Init, last): scroll.wav +0x7C, select.wav +0x80 (the flows' confirm sound
    // 0x000E1980), scrolllimit.wav +0x84
    u32 scrollSound = 0, selectSound = 0, limitSound = 0;
};

class MemoryCards {
public:
    explicit MemoryCards(App& app);
    ~MemoryCards();
    void Init();                                      // orig: 0x000E5250 (the manager's parts, App::Init)
    // orig: 0x000E4C90 (a level's kind-3 handler, before the pause's panel reset): the fold sounds +0x9C / +0xA0,
    // the dialog's 0x000E2FE0 and the save panel's 0x000E19C0 loaded again (Sound_Load).
    void ReloadSounds();
    void Step();                                      // orig: 0x000E5380 (App vf28, every fixed step)
    // orig: 0x000E5360 (App::Frame while +0xA): the save panel's draw ((+4)+4 vf08 0x000E1A00, when shown) and the
    // dialog's 0x000E33A0.
    void Draw();
    // The starters (each: 0x000E4C20, then the flow's first state as the pending one when none is pending).
    bool StartLoadOptions();                          // orig: 0x000E4DF0 (XBox_OL_EntryPoint)
    bool StartLoadExtras();                           // orig: 0x000E4EA0 (XBox_EL_EntryPoint)
    bool StartBootCheck();                            // orig: 0x000E50C0 (XBox_B_EntryPoint)
    bool StartInGameSave();                           // orig: 0x000E5000 (XBox_IS_EntryPoint: FsGameSave, the breakdown)
    bool StartOptionsSave();                          // orig: 0x000E4D40 (XBox_OS_EntryPoint: the options' Apply)
    bool StartConfirm(const char* text);              // orig: 0x000E4F50 (CB_EntryPoint(text): MC+0x70 = 2)
    bool StartLoad();                                 // orig: 0x000E5170 (XBox_L_EntryPoint: MC+0x70 = 2)
    // orig: 0x000E1980 (the save panel's 0x000E1980 through (MC+4)+4): Sound_PlaySFX(+0x80 select.wav)
    void PlayDialogSound();
    // orig: 0x000E0E90 (&missing, &needed): an Options save exists or the free space holds one (DAT_002EDBE0)
    bool OptionsSpace(u32* missing, u32* needed);
    bool WriteOptions();                              // orig: vf30 0x000E0F30 (Options.dat, "Futurama Options")
    // orig: 0x000E0E00 (out parameters unused by the callers ported): room for one more game save (fewer than 25
    // saves, the free space against DAT_002EDBDC (+ DAT_002EDC00 without an extras save)). Traced.
    bool GameSaveSpace();
    // orig: 0x000E05D0: the game saves into the save data's list (the empty card: no entry). Traced.
    void ListGames();
    // The save panel's row functions (the panel's head +0x74 = panelHead, selection +0x68 = panelSelection):
    void PanelRows();                                 // orig: 0x000E2AC0 (the head, the slots, names, arrows, highlight)
    void PanelItems();                                // orig: 0x000E2B40 (the slots' child 0, 0x000E2AC0, highlight, arrows)
    void PanelNames();                                // orig: 0x000E1C50 (Text4..6: up to three entries' names)
    void PanelArrows();                               // orig: 0x000E1AE0 (the scroll arrows' child 1: more above / below)
    void PanelHighlight();                            // orig: 0x000E1B90 (the selected slot's child 1)
    // orig: 0x000E2D70 / 0x000E2E40 (the list prompt's down / up, on the panel's cooldown +0x78): the selection to the
    // next / previous row while an entry is there, at the last / first row the head moves (0x000E2CE0 / 0x000E2C80:
    // down only with three entries after the head); the scroll / limit sounds.
    bool PanelDown();
    bool PanelUp();
    void PanelScrollDown();                           // orig: 0x000E2CE0
    void PanelScrollUp();                             // orig: 0x000E2C80
    // orig: 0x000E4D20 -> 0x0002A0D0(action, +0x1C): the menu input on the manager's repeat (0 up, 1 down, 2 left,
    // 3 right, 4 accept A / Start, 5 back B / Back, 6 Start, 7 X)
    bool RepeatInput(s32 action) const;
    // orig: the save data's list ((manager +4)+0x10, count +0x14; new(0x2C) entries, +0x20 next / +0x24 previous):
    // 0x000E18B0 clears it, 0x000E1740(kind) adds an entry with a copy of "level0-1", 0x000DFEE0 the save being
    // written; 0x000E1690 inserts after the last entry whose key is not above the new one's (the head never compared)
    struct SaveEntry {
        s32 key = 0;                                  // orig: +0 (0: no writer in the paths ported)
        std::string path;                             // orig: +8 (a listed game: "<directory>Futurama.dat", 0x000DFB30)
        std::u16string name;                          // orig: +0xC (a listed game: its save game's wide name)
        std::vector<u8> data;                         // orig: +0x10 / +0x14 (the save's text)
        std::optional<std::string> level;             // orig: +0x18 (0x000E1630: set = a new save)
        std::string level2;                           // orig: +0x1C (the level name: the save name's title id)
        u8 kind = 0;                                  // orig: +0x28
    };
    void ClearEntries();                              // orig: 0x000E18B0
    void RemoveEntry(SaveEntry* e);                   // orig: 0x000E1880 (0x000E1590: unlinked, deleted; count - 1)
    void AddEntry(u8 kind);                           // orig: 0x000E1740
    SaveEntry* InsertEntry(SaveEntry e, u8 kind);     // orig: 0x000E1690 (+ the panel's 0x000E1BF0)
    SaveEntry* SelectedEntry();                       // orig: 0x000E17E0 (null while the panel animates)
    void Stop();                                      // orig: 0x000E5210 (+0xA = 0)
    void Abort();                                     // orig: 0x000E4BE0 (the Frontend's exit)
    bool SetPending(FlowState* s);                    // orig: 0x000E4CE0 (deletes `s` when one is pending)

    // ---- the save device (orig: +0xC, new(0x58) 0x000E09C0, vtable 0x002A4E50) and the save data's writes ----
    // Every XAPI call goes to SaveStorage (traced there: the boot oracle's SAVE lines, `site` = the return address).
    bool HasSave(const char* name, const char* file, std::string* path);   // orig: 0x000E00D0
    s32 CountGameSaves();                             // orig: 0x000E0860
    // orig: 0x000E0CF0(&missing, &needed): the free space (its low | high dword) against the saves still missing (all
    // three kinds without any, else the options / extras saves not there and a game save while fewer than 25 exist)
    bool EnoughSpace(u32* missing = nullptr, u32* needed = nullptr);
    // orig: the static initialisers 0x0020F050 .. 0x0020F0F0: DAT_002EDBAC = XGetDiskClusterSize("U:\") (0x000F8111;
    // a stock partition's 16 KB clusters: the oracle's model), each kind's size DAT_002EDBE0 (options) / DAT_002EDC00
    // (extras) / DAT_002EDBDC (a game) = ((c + 0xFFF) / c * 2 + (c + 0x29) / c + 1) * c, DAT_002EDC28 = their sum
    static constexpr s32 kClusterSize = 0x4000;
    static constexpr u32 kSaveKindSize =
        u32(((kClusterSize + 0x29) / kClusterSize + ((kClusterSize + 0xFFF) / kClusterSize) * 2 + 1) * kClusterSize);
    static constexpr u32 kAllSavesSize = kSaveKindSize + kSaveKindSize + kSaveKindSize;
    static s32 Blocks(u32 bytes) { return s32(kClusterSize + s32(bytes) - 1) / kClusterSize; }   // orig: 0x000E0080
    bool ReadOptions();                               // orig: vf34 0x000E1010 (Options.dat: the read, verify, apply)
    void Wait();                                      // orig: vf38 0x000E00C0: SleepEx(0, TRUE) (a completion runs)
    void WriteCompleted(u32 error);                   // orig: 0x000E0230 (the WriteFileEx completion routine)
    bool Sign(std::vector<u8>& buf);                  // orig: 0x000E0440 (the signature into the last 20 bytes)
    bool Verify(const std::vector<u8>& buf);          // orig: 0x000E03D0
    bool CopyIcon(const std::u16string& name, s32 kind);   // orig: 0x000E04B0 (0 game, 1 options, 2 extras)
    // orig: 0x000E0B90(file, name, kind): XCreateSaveGame OPEN_ALWAYS, CreateFile CREATE_ALWAYS, one WriteFileEx of
    // the 0x1000-byte buffer, the save image; null name = the save data's display name
    bool WriteFile(const char* file, const std::u16string* name, s32 kind);
    s32 ReadFile(const std::string& path, std::vector<u8>& buf);   // orig: 0x000E02A0 (3 read, 2 / 1 / 0 failed)
    void ReleaseSave();                               // orig: 0x000E07F0 (the buffer and the save data's parts)
    bool WriteGameSave();                             // orig: vf40 0x000E1420 (Futurama.dat)
    bool WriteExtras(const LevelRecords& r);          // orig: 0x000E1110 (Unlock.dat)
    bool ReadExtras(LevelRecords& r);                 // orig: 0x000E11F0
    // orig: 0x000E0720(path, &data, &size): +0x1D = 0; the read 0x000E02A0 and the verify 0x000E03D0; ok: `data` = the
    // size word's bytes from +4; any failure: +0x1D ("damaged") = 1, false.
    bool ReadGame(const std::string& path, std::vector<u8>& data);
    bool DeleteGame(const std::u16string& name);      // orig: 0x000E07D0 (XDeleteSaveGame("U:\", name) == 0)
    void ListSaveNames(std::vector<std::u16string>& names);   // orig: 0x000E1300 (not Options / Extras)
    void BuildGameSave(const std::vector<std::u16string>& names);   // orig: 0x000DFEE0 (the save data)
    std::u16string SaveName(s32 n) const;             // orig: 0x000DFC10 (title + " 0h 0m 2s" [+ " (n)"])
    SaveStorage& Saves() const;                       // AppServices::saves (or an empty card of the manager's)

    App& app;
    bool flag0 = false;                               // orig: +0 (cleared by the load panel's Activate; L: no setter traced)
    bool flag8 = false, flag9 = false;                // orig: +8 / +9
    bool running = false;                             // orig: +0xA ("a flow is running")
    Dialog dialog;                                    // orig: +0x10
    std::unique_ptr<FlowState> pending;               // orig: +0x14
    std::unique_ptr<FlowState> current;               // orig: +0x18
    input::InputRepeat repeat;                        // orig: +0x1C (period _ftol(DAT_002D67E4 50.0 * 0.25) = 12)
    s32 mode = 0;                                     // orig: +0x70
    bool bootCheck = false;                           // orig: +0x98
    // orig: DAT_002D6E20 (0x000E00A0; 1 in the image): the save item's availability (its writer 0x000E00B0 is the
    // device's hot plug: NOT PORTED)
    bool cardAvailable = true;
    // the save device's state (+0xC)
    struct Device {
        s32 status = 0;                               // orig: +4 (0 done, 1 failed, 2 busy)
        bool inUse = false;                           // orig: +0x1C (0x000E0220: set by L_EntryPoint; no reader traced)
        bool readFailed = false;                      // orig: +0x1D
        u32 file = SaveStorage::kInvalid;             // orig: +0x20
        std::vector<u8> buffer;                       // orig: +0x28 (the 0x1000-byte file image)
    } device;
    // the save data (+4, new(0x48) 0x000DFAC0)
    f32 saveDataTime = 0;                             // orig: (+4)+0x18 (0x000E16F0: += dt)
    SavePanel panel;                                  // orig: (+4)+4
    std::list<SaveEntry> saveEntries;                 // orig: (+4)+0x10
    SaveEntry* panelHead = nullptr;                   // orig: ((+4)+4)+0x74 (0x000E1BF0: the list's head)
    s32 panelSelection = 0;                           // orig: ((+4)+4)+0x68
    bool saveDataFlag8 = false;                       // orig: (+4)+8 (set: the name's numbers "00", no new data; L:
                                                      //  its writer is the load path, not ported)
    u8 saveDataFlag9 = 0;                             // orig: (+4)+9 (L: its writers are not traced)
    SaveEntry* selectedSave = nullptr;                // orig: (+4)+0xC (L: no writer in the paths ported)
    std::optional<std::string> saveFile;              // orig: (+4)+0x1C (vf10 0x000DFB00: "Futurama.dat")
    std::vector<u8> saveData;                         // orig: (+4)+0x20 / +0x24
    SaveEntry* saveCurrent = nullptr;                 // orig: (+4)+0x30
    std::u16string saveName;                          // orig: (+4)+0x40 (the save game's display name)
    bool saveFlag44 = false;                          // orig: (+4)+0x44 (the selected entry is not "level0-1")
    const text::LanguageDb* levelNames = nullptr;     // orig: (+4)+0x2C (Language/LEVELNAMES.ldb)
    u32 foldDown = 0, foldUp = 0;                     // orig: +0x9C / +0xA0 (sounds\menu\Pause_fold_*.wav)
    // orig: DAT_002ED258 / DAT_002ED259 (the options flow's results: an Options save was found / loaded)
    bool optionsFound = false, optionsLoaded = false;
    // orig: DAT_002ECED0 (0x000DD0B0's static: the Saving state's start, in ticks)
    s64 savingStart = 0;

private:
    std::unique_ptr<MemorySaveStorage> ownSaves_;     // (AppServices::saves unset)
};

}  // namespace fut::game::app
