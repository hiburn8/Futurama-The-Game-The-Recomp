// The memory-card manager and its flows; see MemoryCards.h.
#include "game/app/MemoryCards.h"

#include "engine/ni/NiCull.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiObjects.h"
#include "engine/ni/NiRenderer.h"
#include "engine/ni/NiStream.h"
#include "game/GameTime.h"
#include "game/app/App.h"
#include "game/app/Breakdown.h"
#include "game/app/Contexts.h"
#include "game/app/Frontend.h"
#include "game/app/LoadPanel.h"
#include "game/app/MenuScene.h"
#include "game/app/Options.h"
#include "game/app/PauseMenu.h"
#include "core/CrtString.h"
#include "game/level/LevelLoader.h"
#include "game/level/Session.h"
#include "game/text/LanguageDb.h"
#include "game/text/TextObject.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>

namespace fut::game::app {

using ni::detail::F;

namespace {

void NotPortedFlow(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// ---- the in-game save (orig: XBox_IS_*, starter 0x000E5000) -------------------------------------------------------

// orig: XBox_IS_ExitPoint (vtable 0x002A46CC, 0x20 bytes): Enter 0x000DC930, Exit 0x000DC970; +0x1C the disappear
// started.
class IsExitPoint : public FlowState {
public:
    explicit IsExitPoint(MemoryCards& m) : FlowState(m, 0x002A46CC) {}
    void Enter() override {                           // orig: 0x000DC930
        SavePanel& p = m_.panel;                      // (+0x14)
        if (!started_) {
            started_ = true;
            if (p.state == 0) {                       // the list is shown: its disappear first
                (void)p.Show(1, m_);                  // 0x000E2BA0(1)
                return;
            }
        } else if (p.Busy()) {                        // 0x000E19A0
            return;
        }
        started_ = false;
        phase = 2;
    }
    void Exit() override {                            // orig: 0x000DC970
        m_.panel.state = 2;                           // (+0x14)+4 = 2
        g_menuInputMode = 4;                          // 0x00029F90(DAT_002D6E10: 4 in the image; L: no other writer
                                                      //  traced)
        m_.Stop();                                    // 0x000E5210
        Finish();
    }

private:
    bool started_ = false;                            // orig: +0x1C
};

// ---- the dialog states (orig: base 0x000DF7E0, 0x40 bytes, vtable 0x002A4D4C) ------------------------------------

// orig: the prompts' base: +0x1C the box (0x000DF6E0: +0 state 0 idle / 1 opening / 2 closing, +4 the dialog, +8 the
// formatted text), +0x28 the text id, +0x2C the hint id, +0x30 "the fold sound played", +0x34 / +0x38 the text's
// token and its replacement (wide copies: the ctor's token, and sprintf("%d", value) 0x00296D4C). Enter 0x000DF970
// opens the dialog with the text and waits for its animation; Exit 0x000DF9F0 closes it and waits again.
class DialogState : public FlowState {
public:
    DialogState(MemoryCards& m, u32 vtable, const char* text, const char* hint, const char* token = nullptr,
                s32 value = 0)
        : FlowState(m, vtable), text_(text ? text : ""), hint_(hint ? hint : "") {
        if (token) {                                  // 0x000D2CB0 (narrow to wide) of the token and of "%d"
            for (const char* c = token; *c; ++c) token_ += static_cast<char16_t>(static_cast<unsigned char>(*c));
            char v[32];
            std::snprintf(v, sizeof v, "%d", static_cast<int>(value));
            for (const char* c = v; *c; ++c) value_ += static_cast<char16_t>(static_cast<unsigned char>(*c));
        }
    }
    void Enter() override {                           // orig: 0x000DF970
        if (!sound_) {
            (void)m_.app.Sound().PlaySfx(m_.foldDown, false);   // Sound_PlaySFX(manager +0x9C, 0)
            sound_ = true;
        }
        if (box_ == 0) {
            // 0x000DF6F0(+0x28, dialog, +0x34, +0x38): the dialog's text: with a token and its value, the id's text
            // (GetText(id, dialog +0x3C)) with the first token replaced (0x000D2F50) into 0x000E3500; without one (or
            // a text without the token) 0x000E3570(id)
            if (!text_.empty()) {
                std::u16string formatted;
                if (!token_.empty() && !value_.empty() && m_.app.text) {
                    const char16_t* t = m_.app.text->GetText(text_, m_.dialog.textsDb);
                    const std::u16string full = t ? t : u"";
                    if (const size_t k = full.find(token_); k != std::u16string::npos)   // 0x000D2E00 (wcsstr)
                        formatted = full.substr(0, k) + value_ + full.substr(k + token_.size());
                }
                if (!formatted.empty()) (void)m_.dialog.SetString(formatted.c_str());
                else (void)m_.dialog.SetText(text_.c_str());
            }
            (void)m_.dialog.Open();                   // 0x000DF760: 0x000E4900, box state 1
            box_ = 1;
            return;
        }
        if (BoxBusy()) return;                        // 0x000DF7A0
        sound_ = false;
        phase = 1;
        // with a hint id: the dialog's hint (0x000256C0: a no-op in this build)
    }
    void Exit() override {                            // orig: 0x000DF9F0
        if (!sound_) {
            sound_ = true;
            (void)m_.app.Sound().PlaySfx(m_.foldUp, false);   // Sound_PlaySFX(manager +0xA0, 0)
        }
        text_.clear();                                // (+0x28 freed)
        if (!hint_.empty()) {                         // the dialog's hint reset 0x000E3080, +0x2C freed
            m_.dialog.ClearHint();
            hint_.clear();
        }
        token_.clear();                               // (+0x34 / +0x38 freed)
        value_.clear();
        if (box_ == 0 && !m_.dialog.flag2C) {
            (void)m_.dialog.Close();                  // 0x000DF780: 0x000E4980, box state 2
            box_ = 2;
            return;
        }
        if (!BoxBusy()) Finish();                     // 0x000DF3B0
    }

protected:
    // orig: 0x000DF7A0: false (and the box idle again) once the opening / closing animation has ended
    bool BoxBusy() {
        if ((box_ == 1 && !m_.dialog.Animating()) || (box_ == 2 && !m_.dialog.Animating())) {
            box_ = 0;
            return false;
        }
        return true;
    }
    s32 box_ = 0;                                     // orig: +0x1C
    std::string text_, hint_;                         // orig: +0x28 / +0x2C
    bool sound_ = false;                              // orig: +0x30
    std::u16string token_, value_;                    // orig: +0x34 / +0x38
};

// ---- the options load (orig: XBox_OL_*, starter 0x000E4DF0) --------------------------------------------------------

// orig: XBox_OL_ExitPoint (vtable 0x002A4AB0): Enter 0x000DC0D0, Exit 0x000DE6A0.
class OlExitPoint : public FlowState {
public:
    explicit OlExitPoint(MemoryCards& m) : FlowState(m, 0x002A4AB0) {}
    void Exit() override {                            // orig: 0x000DE6A0
        if (m_.app.frontend) m_.app.frontend->flowBusy = false;   // Frontend+0x2E = 0
        m_.Stop();                                    // 0x000E5210
        Finish();
    }
};

// orig: XBox_OL_LoadOptions (vtable 0x002A4AC4): Enter 0x000DE6D0, Update 0x000DE770, Exit 0x000DF400.
class OlLoadOptions : public FlowState {
public:
    explicit OlLoadOptions(MemoryCards& m) : FlowState(m, 0x002A4AC4) {}
    void Enter() override {                           // orig: 0x000DE6D0
        // 0x000E0B50(0): the "Futurama Options" save game exists (0x000E00D0("Futurama Options", "Options.dat"))
        if (m_.HasSave("Futurama Options", "Options.dat", nullptr)) {
            (void)m_.ReadOptions();                   // the device's vf34 0x000E1010 (synchronous)
            phase = 1;
            return;
        }
        phase = 2;
        m_.SetPending(new OlExitPoint(m_));           // 0x000E4CE0
    }
    void Update() override {                          // orig: 0x000DE770: the read's status (the device +4)
        const s32 st = m_.device.status;
        if (st == 0) {                                // read and applied
            phase = 2;
            m_.SetPending(new OlExitPoint(m_));
        } else if (st == 1) {                         // failed: DAT_002ED258 = 1 ("the options read failed")
            phase = 2;
            m_.optionsFound = true;
            m_.SetPending(new OlExitPoint(m_));
        }                                             // (2: still busy)
    }
};

// orig: XBox_OL_LoadOptionsFailed (vtable 0x002A4B28, new(0x40) 0x000DF7E0(MEMCARD_049, 0, 0, 0), +0x3C = 0): Enter
// 0x000DE820, Update 0x000DE860, Exit 0x000DF9F0.
class OlLoadOptionsFailed : public DialogState {
public:
    explicit OlLoadOptionsFailed(MemoryCards& m) : DialogState(m, 0x002A4B28, "MEMCARD_049", nullptr) {}
    void Enter() override {                           // orig: 0x000DE820
        if (!buttons_) {
            m_.dialog.SetSingle(6);                   // 0x000E3930(6): "Ok"
            m_.dialog.Select(2);
            buttons_ = true;
            m_.optionsLoaded = true;                  // DAT_002ED259 = 1 ("its prompt has been shown")
        }
        DialogState::Enter();                         // 0x000DF970
    }
    void Update() override {                          // orig: 0x000DE860 (the menu input: a fresh press)
        if (!MenuInput(m_.app, 4)) return;            // 0x00029FA0(4)
        phase = 2;
        m_.SetPending(new OlExitPoint(m_));           // new(0x1C), vtable 0x002A4AB0
    }

private:
    bool buttons_ = false;                            // orig: +0x3C
};

// orig: XBox_OL_EntryPoint (vtable 0x002A4B00, ctor 0x000DE900): Enter 0x000DE9B0, Exit 0x000DF400.
class OlEntryPoint : public FlowState {
public:
    explicit OlEntryPoint(MemoryCards& m) : FlowState(m, 0x002A4B00) {}
    void Enter() override {                           // orig: 0x000DE9B0
        if (m_.app.frontend) m_.app.frontend->flowBusy = true;   // FindContext("Frontend")+0x2E = 1
        FlowState* next = nullptr;
        if (m_.optionsFound) {                        // DAT_002ED258: the read failed
            if (!m_.optionsLoaded) next = new OlLoadOptionsFailed(m_);   // DAT_002ED259: not shown yet (MEMCARD_049)
            else next = new OlExitPoint(m_);
        } else {
            next = new OlLoadOptions(m_);
        }
        m_.SetPending(next);
        phase = 2;
    }
};

// ---- the extras load (orig: XBox_EL_*, starter 0x000E4EA0) ---------------------------------------------------------

// orig: XBox_EL_ExitPoint (vtable 0x002A4648): Enter 0x000DC0D0, Exit 0x000DC5E0.
class ElExitPoint : public FlowState {
public:
    explicit ElExitPoint(MemoryCards& m) : FlowState(m, 0x002A4648) {}
    void Exit() override {                            // orig: 0x000DC5E0
        m_.Stop();
        Finish();
    }
};

// orig: XBox_EL_LoadFailed (vtable 0x002A4698, new(0x40) 0x000DF7E0(MEMCARD_053, 0, 0, 0), +0x3C = 0): Enter
// 0x000DC600, Update 0x000DC630, Exit 0x000DF9F0.
class ElLoadFailed : public DialogState {
public:
    explicit ElLoadFailed(MemoryCards& m) : DialogState(m, 0x002A4698, "MEMCARD_053", nullptr) {}
    void Enter() override {                           // orig: 0x000DC600
        if (!buttons_) {
            m_.dialog.SetSingle(6);
            m_.dialog.Select(2);
            buttons_ = true;
        }
        DialogState::Enter();
    }
    void Update() override {                          // orig: 0x000DC630
        if (!MenuInput(m_.app, 4)) return;
        phase = 2;
        m_.SetPending(new ElExitPoint(m_));
    }

private:
    bool buttons_ = false;                            // orig: +0x3C
};

// orig: XBox_EL_EntryPoint (vtable 0x002A4670, ctor 0x000DC6C0): Enter 0x000DC5C0, Update 0x000DC770, Exit
// 0x000DC5D0.
class ElEntryPoint : public FlowState {
public:
    explicit ElEntryPoint(MemoryCards& m) : FlowState(m, 0x002A4670) {}
    void Enter() override { phase = 1; }              // orig: 0x000DC5C0
    void Update() override {                          // orig: 0x000DC770
        LevelRecords loaded, merged;                  // two 0xC0-byte blocks reset (0x000F3BF0: flags 0, times -1)
        if (!m_.HasSave("Futurama Extras", "Unlock.dat", nullptr)) {   // 0x000E0B70(0)
            m_.SetPending(new ElExitPoint(m_));
        } else if (m_.ReadExtras(loaded)) {           // 0x000E11F0(&loaded)
            // 0x000F3C20(&tmp, a = loaded, b = App+0xA4 (0x000A25B0)): per record the flags OR-ed, the time a's
            // unless a's is -1 or b's is smaller (signed): the App's -1 ("none") wins over a loaded time
            const LevelRecords& b = m_.app.records;
            for (size_t i = 0; i < 24; ++i) {
                s32 t = loaded.records[i].bestTime;   // CMP -1; JE take b; CMP b, a; JGE keep a
                if (t == -1 || b.records[i].bestTime < t) t = b.records[i].bestTime;
                merged.records[i].bestTime = t;
                merged.records[i].allNibblers = u8(loaded.records[i].allNibblers | b.records[i].allNibblers);
            }
            m_.app.records = merged;                  // 0x000F3BC0 (the copy), 0x000A25C0 (into App+0xA4)
            m_.SetPending(new ElExitPoint(m_));
        } else {
            m_.SetPending(new ElLoadFailed(m_));      // new(0x40): MEMCARD_053
        }
        phase = 2;
    }
    void Exit() override { Finish(); }                // orig: 0x000DC5D0
};

// ---- New Game's free-space check (orig: XBox_B_*, starter 0x000E50C0) ---------------------------------------------

// orig: XBox_B_ExitPoint (vtable 0x002A456C): Enter 0x000DC0D0, Exit 0x000DC2D0.
class BExitPoint : public FlowState {
public:
    explicit BExitPoint(MemoryCards& m) : FlowState(m, 0x002A456C) {}
    void Exit() override {                            // orig: 0x000DC2D0
        m_.Stop();
        m_.bootCheck = false;                         // manager +0x98 = 0
        if (m_.app.frontend) m_.app.frontend->flowBusy = false;
        Finish();
    }
};

// orig: XBox_B_InsufficientSpace (vtable 0x002A45FC, new(0x44) 0x000DF7E0(MEMCARD_047, MEMHINT_006, "#", the missing
// blocks), +0x3C = 0, +0x40 the needed blocks): Enter 0x000DC130, Update 0x000DC320, Exit 0x000DF9F0.
class BInsufficientSpace : public DialogState {
public:
    BInsufficientSpace(MemoryCards& m, s32 missingBlocks, s32 neededBlocks)
        : DialogState(m, 0x002A45FC, "MEMCARD_047", "MEMHINT_006", "#", missingBlocks), needed_(neededBlocks) {}
    void Enter() override {                           // orig: 0x000DC130
        if (!buttons_) {
            m_.dialog.SetButtons(7, 5);               // 0x000E3790(7, 5): "Dashboard" / "Continue"
            m_.dialog.Select(0);                      // the left one
            buttons_ = true;
        }
        DialogState::Enter();
    }
    void Update() override {                          // orig: 0x000DC320
        if (!MenuInput(m_.app, 4)) {
            if (m_.RepeatInput(2)) m_.dialog.Select(0);
            if (m_.RepeatInput(3)) m_.dialog.Select(1);
            return;
        }
        if (m_.dialog.selection == 0) {               // Dashboard: 0x000E4B90(+0x40): XGetDiskClusterSize("U:\"),
            // XLaunchNewImage(0, LAUNCH_DATA {2, 0, 0x55, the blocks}) (the original does not return from it)
            NotPortedFlow("memory cards: the Dashboard launch XLaunchNewImage (0x000E4B90)");
            phase = 2;
            return;
        }
        m_.SetPending(new BExitPoint(m_));            // Continue
        if (m_.CountGameSaves() == 0) m_.cardAvailable = false;   // 0x000E00B0(0): DAT_002D6E20
        phase = 2;
    }

private:
    bool buttons_ = false;                            // orig: +0x3C
    s32 needed_ = 0;                                  // orig: +0x40
};

// orig: XBox_B_MaxNoSavegamesReached (vtable 0x002A4580, new(0x40) 0x000DF7E0(MEMCARD_050, 0, 0, 0), +0x3C = 0): Enter
// 0x000DC170, Update 0x000DC1B0, Exit 0x000DF9F0.
class BMaxSaves : public DialogState {
public:
    explicit BMaxSaves(MemoryCards& m) : DialogState(m, 0x002A4580, "MEMCARD_050", nullptr) {}
    void Enter() override {                           // orig: 0x000DC170
        if (!buttons_) {
            m_.dialog.SetSingle(6);
            m_.dialog.Select(2);
            buttons_ = true;
        }
        DialogState::Enter();
    }
    void Update() override {                          // orig: 0x000DC1B0
        if (MenuInput(m_.app, 4)) {
            m_.SetPending(new BExitPoint(m_));
            phase = 2;
            return;
        }
        if (m_.RepeatInput(2)) m_.dialog.Select(0);   // (selects with the single button shown: no effect but the sound)
        if (m_.RepeatInput(3)) m_.dialog.Select(1);
    }

private:
    bool buttons_ = false;                            // orig: +0x3C
};

// orig: XBox_B_EntryPoint (vtable 0x002A45D4, ctor 0x000DC2A0): Enter 0x000DC430, Exit 0x000DF400.
class BEntryPoint : public FlowState {
public:
    explicit BEntryPoint(MemoryCards& m) : FlowState(m, 0x002A45D4) {}
    void Enter() override {                           // orig: 0x000DC430
        if (FrontendContext* fe = static_cast<FrontendContext*>(m_.app.FindContext("Frontend")))
            fe->flowBusy = true;                      // FindContext("Frontend")+0x2E = 1
        u32 missing = 0, needed = 0;
        if (!m_.EnoughSpace(&missing, &needed)) {     // 0x000E0CF0(&missing, &needed): not enough space, unless the
            // options and extras saves exist and so does a game save (0x000E0B50(0) && 0x000E0B70(0) && 0x000E0860())
            const bool keep = m_.HasSave("Futurama Options", "Options.dat", nullptr) &&
                              m_.HasSave("Futurama Extras", "Unlock.dat", nullptr) && m_.CountGameSaves() != 0;
            if (!keep) {                              // new(0x44): the blocks 0x000E0080(missing), 0x000E0080(needed)
                const s32 needBlocks = MemoryCards::Blocks(needed);
                m_.SetPending(new BInsufficientSpace(m_, MemoryCards::Blocks(missing), needBlocks));
                phase = 2;
                return;
            }
        }
        if (m_.CountGameSaves() >= 0x19) m_.SetPending(new BMaxSaves(m_));   // 0x000E0DF0: MEMCARD_050
        else m_.SetPending(new BExitPoint(m_));
        phase = 2;
    }
};

// orig: the one-button prompts: SaveOKPrompt (vtable 0x002A4708, ctor 0x000DCA70: MEMCARD_007 / MEMHINT_001) and
// SaveFailedPrompt (vtable 0x002A4734, ctor 0x000DCAA0(text): MEMCARD_006 or MEMCARD_055 / MEMHINT_001): Enter
// 0x000DC170, Update 0x000DCFC0, Exit 0x000DF9F0; +0x3C the buttons set.
class IsMessagePrompt : public DialogState {
public:
    IsMessagePrompt(MemoryCards& m, u32 vtable, const char* text) : DialogState(m, vtable, text, "MEMHINT_001") {}
    void Enter() override {                           // orig: 0x000DC170
        if (!buttons_) {
            m_.dialog.SetSingle(6);                   // 0x000E3930(6): "Ok"
            m_.dialog.Select(2);                      // 0x000E35A0(2) (its Voice_Exists / Sound_PlaySFX: sound)
            buttons_ = true;
        }
        DialogState::Enter();
    }
    void Update() override;                           // orig: 0x000DCFC0

private:
    bool buttons_ = false;                            // orig: +0x3C
};

// orig: XBox_IS_Saving (vtable 0x002A4748, ctor 0x000DCAD0: 0x000DF7E0(MEMCARD_005, 0, 0, 0), +0x3C = 0): Enter
// 0x000DF970, Update 0x000DD0B0, Exit 0x000DF9F0.
class IsSaving : public DialogState {
public:
    explicit IsSaving(MemoryCards& m) : DialogState(m, 0x002A4748, "MEMCARD_005", nullptr) {}
    void Update() override;                           // orig: 0x000DD0B0

private:
    s32 step_ = 0;                                    // orig: +0x3C (0 write the game, 1 its completion, 2 shown)
};

// orig: XBox_IS_ConfirmOverwritePrompt (vtable 0x002A4840, 0x000DF7E0(MEMCARD_016 0x002A4854, MEMHINT_002 0x002A4860,
// 0, 0)): Enter 0x000DCA30, Update 0x000DCE70, Exit 0x000DF9F0. Yes: the selected save deleted (0x000E07D0), its entry
// removed (0x000E1880), then Saving; a failed delete: SaveFailedPrompt MEMCARD_006; no: back to the list.
class IsConfirmOverwritePrompt : public DialogState {
public:
    explicit IsConfirmOverwritePrompt(MemoryCards& m) : DialogState(m, 0x002A4840, "MEMCARD_016", "MEMHINT_002") {}
    void Enter() override {                           // orig: 0x000DCA30 (OsConfirmOptions' Enter)
        if (!buttons_) {
            m_.dialog.SetButtons(1, 2);               // 0x000E3790(1, 2): "Yes" / "No"
            m_.dialog.Select(1);                      // 0x000E35A0(1)
            buttons_ = true;
        }
        DialogState::Enter();                         // 0x000DF970
    }
    void Update() override;                           // orig: 0x000DCE70

private:
    bool buttons_ = false;                            // orig: +0x3C
};

// orig: XBox_IS_SavegameListPrompt (vtable 0x002A46F4, 0x20 bytes): Enter 0x000DCA20, Update 0x000DD4B0, Exit
// 0x000DF400; +0x1C.
class IsSavegameListPrompt : public FlowState {
public:
    explicit IsSavegameListPrompt(MemoryCards& m) : FlowState(m, 0x002A46F4) {}
    void Enter() override {                           // orig: 0x000DCA20
        flag_ = true;                                 // +0x1C = 1
        phase = 1;
    }
    void Update() override {                          // orig: 0x000DD4B0
        if (m_.RepeatInput(0)) (void)m_.PanelUp();    // ((+0x10)+4) 0x000E2E40
        if (m_.RepeatInput(1)) (void)m_.PanelDown();  // ((+0x10)+4) 0x000E2D70
        if (m_.RepeatInput(4)) {                      // accept
            m_.PlayDialogSound();                     // ((+0xC)+4)+4 -> 0x000E1980: Sound_PlaySFX(the save panel's +0x80)
            MemoryCards::SaveEntry* e = m_.SelectedEntry();   // (+0x10)->0x000E17E0
            if (!e) {                                 // (the original reads entry+0x18 through the null pointer)
                NotPortedFlow("memory cards: the save list's accept without an entry (0x000E1630 on null)");
                return;
            }
            if (e->level) {                           // 0x000E1630: +0x18 set: a new save -> Saving (new(0x40)
                m_.SetPending(new IsSaving(m_));      //  0x000DF7E0(0x002A475C, 0, 0, 0), vtable 0x002A4748)
            } else {                                  // an existing save: the ConfirmOverwritePrompt (new(0x40))
                m_.SetPending(new IsConfirmOverwritePrompt(m_));
            }
            phase = 2;
            return;
        }
        if (m_.RepeatInput(5)) {                      // back
            m_.SetPending(new IsExitPoint(m_));
            phase = 2;
        }
    }

private:
    bool flag_ = false;                               // orig: +0x1C
};

void IsConfirmOverwritePrompt::Update() {              // orig: 0x000DCE70
    if (m_.RepeatInput(4)) {                          // 0x000E4D20(4)
        if (m_.dialog.selection == 0) {               // (+0xC)+0x10 +0x5C == 0: yes
            m_.PlayDialogSound();                     // ((+0xC)+4)+4 -> 0x000E1980
            MemoryCards::SaveEntry* e = m_.SelectedEntry();   // (+0x10)->0x000E17E0
            if (e && m_.DeleteGame(e->name)) {        // (+4)->0x000E07D0(entry +0xC) (no null test in the original)
                m_.RemoveEntry(e);                    // (+0x10)->0x000E1880
                m_.SetPending(new IsSaving(m_));      // new(0x40) 0x000DCAD0
            } else {
                m_.SetPending(new IsMessagePrompt(m_, 0x002A4734, "MEMCARD_006"));   // new(0x40) 0x000DCAA0(0x002A488C)
            }
        } else {
            m_.SetPending(new IsSavegameListPrompt(m_));   // new(0x20) 0x000DF370, vtable 0x002A46F4, +0x1C = 0
        }
        phase = 2;                                    // +0x18
    }
    if (m_.RepeatInput(2)) m_.dialog.Select(0);       // 0x000E35A0(0)
    if (m_.RepeatInput(3)) m_.dialog.Select(1);
}

void IsMessagePrompt::Update() {                       // orig: 0x000DCFC0
    if (!m_.RepeatInput(4)) return;                   // 0x000E4D20(4)
    m_.PlayDialogSound();                             // ((+0xC)+4)+4 -> 0x000E1980
    m_.SetPending(new IsExitPoint(m_));               // new(0x20) 0x000DF370, vtable 0x002A46CC, +0x1C = 0
    phase = 2;
}

void IsSaving::Update() {                             // orig: 0x000DD0B0
    App& app = m_.app;
    if (step_ == 0) {
        // the start: 0x000D2BA0(&t, F(Seconds(QueryCounter()))) into DAT_002ECED0
        const f32 now = F(app.Seconds(static_cast<s64>(app.qpc)));   // 0x000D2C20 / 0x000D2C10; FSTP (the argument)
        m_.savingStart = app.Ticks(now);              // FILD QPF; FMUL; _ftol
        // the device's vf20 0x000A32A0 (MOV AL,1): a card is there
        step_ = 1;
        // App vf24(0) 0x000AEDC0 (RET 4: nothing)
        if (m_.WriteGameSave()) return;               // the device's vf40 0x000E1420
        step_ = 0;
        phase = 2;
        m_.SetPending(new IsMessagePrompt(m_, 0x002A4734, "MEMCARD_006"));   // new(0x40) 0x000DCAA0(0x002A488C)
        return;
    }
    if (step_ == 1) {
        if (m_.device.status == 0) {                  // (the write's completion ran: done)
            step_ = 2;
        } else if (m_.device.status == 1) {           // failed
            step_ = 0;
            // App vf24(1): nothing
            phase = 2;
            m_.SetPending(new IsMessagePrompt(m_, 0x002A4734, "MEMCARD_006"));
        }
        if (step_ != 2) return;
        // the extras save: the level records (App+0xA4, 0x000A25B0) merged with Unlock.dat's when it exists
        LevelRecords file, merged;                    // (0x000F3BF0 twice: flags 0, times -1)
        if (m_.HasSave("Futurama Extras", "Unlock.dat", nullptr) && m_.ReadExtras(file)) {   // 0x000E0B70(0), 0x000E11F0
            for (size_t i = 0; i < 24; ++i) {         // 0x000F3C20(&tmp, &file, App+0xA4): per record
                const LevelRecords::Record& a = app.records.records[i];   // (the App's)
                const LevelRecords::Record& b = file.records[i];          // (the file's)
                s32 t = b.bestTime;
                if (t == -1 || a.bestTime < t) t = a.bestTime;   // CMP; JGE (a -1 of the App's wins: as written)
                merged.records[i].bestTime = t;
                merged.records[i].allNibblers = u8(a.allNibblers | b.allNibblers);
            }
        } else {
            merged = app.records;                     // 0x000F3BC0(App+0xA4)
        }
        app.records = merged;                         // 0x000A25C0
        if (!m_.WriteExtras(merged)) {                // 0x000E1110
            m_.SetPending(new IsMessagePrompt(m_, 0x002A4734, "MEMCARD_055"));   // 0x000DCAA0(0x002A4898)
            step_ = 0;
            phase = 2;
        }
        return;
    }
    if (step_ != 2) return;
    // shown for 3 s at least: Seconds(QueryCounter() - DAT_002ECED0) (0x000D2BC0); FCOMP [0x0029563C] 3.0; TEST AH,1
    const double shown = app.Seconds(static_cast<s64>(app.qpc) - m_.savingStart);
    if (shown < 3.0) return;
    if (m_.device.status == 0) {
        phase = 2;
        if (app.breakdown) app.breakdown->continueDirect = true;   // DAT_002E8EEC->0x000A49F0(1): +0x90 = 1
        m_.SetPending(new IsMessagePrompt(m_, 0x002A4708, "MEMCARD_007"));   // new(0x40) 0x000DCA70
    } else if (m_.device.status == 1) {
        phase = 2;
        m_.SetPending(new IsMessagePrompt(m_, 0x002A4734, "MEMCARD_055"));
    }
}

// orig: XBox_IS_LoadGames (vtable 0x002A46E0, 0x24 bytes): Enter 0x000DC9A0, Update 0x000DCD60, Exit 0x000DF400;
// +0x1C, +0x20 the Enter's step.
class IsLoadGames : public FlowState {
public:
    explicit IsLoadGames(MemoryCards& m) : FlowState(m, 0x002A46E0) {}
    void Enter() override {                           // orig: 0x000DC9A0 (one call per step until phase 1)
        if (step_ == 0) {
            if (m_.panel.state != 0) (void)m_.panel.Show(0, m_);   // (+0x14)+4 != 0: the list appears (0x000E2BA0(0))
            step_ = 1;
            return;
        }
        if (step_ != 1) return;
        if (m_.panel.Busy()) return;                  // 0x000E19A0
        m_.ClearEntries();                            // (+0x10)->0x000E18B0
        if (m_.GameSaveSpace()) m_.AddEntry(0xFF);    // 0x000E0E00(0, 0) -> the "new save" entry 0x000E1740(-1)
        m_.PanelItems();                              // (+0x14)->0x000E2B40 (the items' highlights, the rows)
        step_ = 0;
        phase = 1;
    }
    void Update() override {                          // orig: 0x000DCD60
        // (the QueryPerformanceCounter timing around it 0x000D2C20 / 0x000D2C10 / 0x000D2BA0: unused)
        m_.ListGames();                               // (+4)->0x000E05D0
        m_.PanelRows();                               // (+0x14)->0x000E2AC0
        m_.PanelItems();                              // 0x000E2B40
        m_.SetPending(new IsSavegameListPrompt(m_));  // new(0x20), +0x1C = 0
        phase = 2;
    }

private:
    s32 step_ = 0;                                    // orig: +0x20
};

// orig: XBox_IS_IsCardFull (vtable 0x002A477C, 0x1C bytes): Enter 0x000DCC70, Exit 0x000DF400.
class IsIsCardFull : public FlowState {
public:
    explicit IsIsCardFull(MemoryCards& m) : FlowState(m, 0x002A477C) {}
    void Enter() override {                           // orig: 0x000DCC70
        FlowState* next;
        if (m_.GameSaveSpace() || m_.CountGameSaves() > 0) next = new IsLoadGames(m_);   // (+4)->0x000E0E00
        else next = new IsExitPoint(m_);              // (L: the full card's prompt is the list's absence)
        m_.SetPending(next);
        phase = 2;
    }
};

// orig: XBox_IS_EntryPoint (vtable 0x002A4818, ctor 0x000DCB70, 0x20 bytes): Enter 0x000DCBF0, Exit 0x000DC920.
class IsEntryPoint : public FlowState {
public:
    explicit IsEntryPoint(MemoryCards& m) : FlowState(m, 0x002A4818) {}
    void Enter() override {                           // orig: 0x000DCBF0
        m_.SetPending(new IsIsCardFull(m_));          // new(0x1C)
        phase = 2;
    }
    void Exit() override {                            // orig: 0x000DC920 (+0x1C = 0)
        Finish();
    }
};

// ---- the options save (orig: XBox_OS_*, starter 0x000E4D40) -------------------------------------------------------

s32 g_osSavedMode = 0;                                // orig: DAT_002D6E18 (the menu input mode before the flow)
s32 g_osWait = 0;                                     // orig: DAT_002ED380 (OS_SaveOptions' step count)
s32 g_cbResult = -1;                                  // orig: DAT_002D6E1C (the confirm box's answer: 0 yes, 1 no)

FrontendContext* FrontendOf(App& app) {               // orig: FindContext("Frontend") 0x000A3120 (0x00293D6C)
    return static_cast<FrontendContext*>(app.FindContext("Frontend"));
}

// orig: XBox_OS_ExitPoint (vtable 0x002A4BE0, 0x20 bytes): Enter 0x000DEAB0, Exit 0x000DEAC0.
class OsExitPoint : public FlowState {
public:
    explicit OsExitPoint(MemoryCards& m) : FlowState(m, 0x002A4BE0) {}
    void Enter() override {                           // orig: 0x000DEAB0
        started_ = false;                             // +0x1C = 0
        phase = 2;
    }
    void Exit() override {                            // orig: 0x000DEAC0
        m_.flag8 = true;                              // MC+8: the options flow has ended
        if (FrontendContext* fe = FrontendOf(m_.app)) fe->flowBusy = false;   // (the original writes without a test)
        g_menuInputMode = g_osSavedMode;              // 0x00029F90(DAT_002D6E18)
        m_.Stop();                                    // 0x000E5210
        Finish();
    }

private:
    bool started_ = false;                            // orig: +0x1C
};

// orig: the options save's one-button prompts: OS_SaveOKPrompt (vtable 0x002A4BAC, ctor 0x000DEB00: MEMCARD_007 /
// MEMHINT_001) and OS_SaveFailedPrompt (vtable 0x002A4BC0, ctor 0x000DEB30: MEMCARD_054 / MEMHINT_001): Enter
// 0x000DC170, Update 0x000DF110, Exit 0x000DF9F0; +0x3C the buttons set.
class OsMessagePrompt : public DialogState {
public:
    OsMessagePrompt(MemoryCards& m, u32 vtable, const char* text) : DialogState(m, vtable, text, "MEMHINT_001") {}
    void Enter() override {                           // orig: 0x000DC170
        if (!buttons_) {
            m_.dialog.SetSingle(6);                   // 0x000E3930(6): "Ok"
            m_.dialog.Select(2);                      // 0x000E35A0(2)
            buttons_ = true;
        }
        DialogState::Enter();
    }
    void Update() override;                           // orig: 0x000DF110

private:
    bool buttons_ = false;                            // orig: +0x3C
};

// orig: XBox_OS_SaveOptions (vtable 0x002A4B98, 0x000DF7E0(MEMCARD_005, 0, 0, 0), +0x3C = 0): Enter 0x000DF970,
// Update 0x000DEF00, Exit 0x000DF9F0.
class OsSaveOptions : public DialogState {
public:
    explicit OsSaveOptions(MemoryCards& m) : DialogState(m, 0x002A4B98, "MEMCARD_005", nullptr) {}
    void Update() override {                          // orig: 0x000DEF00
        if (!writing_) {
            g_osWait = 0;                             // DAT_002ED380 = 0
            // the device's vf20 (Mem_ReturnTrue in this build: a card is there; its false branch makes the
            // SaveFailed prompt inline)
            writing_ = true;                          // +0x3C = 1
            // App vf24(0) 0x000AEDC0 (RET 4: nothing)
            if (m_.WriteOptions()) return;            // the device's vf30 0x000E0F30
            writing_ = false;
            phase = 2;
            m_.SetPending(new OsMessagePrompt(m_, 0x002A4BC0, "MEMCARD_054"));   // new(0x40) 0x000DEB30
            return;
        }
        const s32 n = g_osWait++;
        if (n < 0x3C) return;                         // (60 more steps in the "Saving" dialog)
        const s32 st = m_.device.status;              // (the device +4)
        if (st == 0) {
            writing_ = false;
            // App vf24(1): nothing
            phase = 2;
            m_.SetPending(new OsMessagePrompt(m_, 0x002A4BAC, "MEMCARD_007"));   // new(0x40) 0x000DEB00
        } else if (st == 1) {
            writing_ = false;
            phase = 2;
            m_.SetPending(new OsMessagePrompt(m_, 0x002A4BC0, "MEMCARD_054"));
        }
    }

private:
    bool writing_ = false;                            // orig: +0x3C
};

// orig: XBox_OS_IsCardFull (vtable 0x002A4B64, 0x1C bytes): Enter 0x000DED60, Exit 0x000DF400.
class OsIsCardFull : public FlowState {
public:
    explicit OsIsCardFull(MemoryCards& m) : FlowState(m, 0x002A4B64) {}
    void Enter() override {                           // orig: 0x000DED60
        u32 missing = 0, needed = 0;
        if (m_.OptionsSpace(&missing, &needed)) {     // (+4)->0x000E0E90
            m_.SetPending(new OsSaveOptions(m_));     // new(0x40)
        } else {
            // the CardFullPrompt (new(0x44): 0x000DF7E0(MEMCARD_003, MEMHINT_001, 0, 0), +0x40 = the blocks
            // 0x000E0080(needed), vtable 0x002A4B78): NOT PORTED, unreachable while DAT_002EDBE0 is 0 (no writer:
            // the free space is never below it); the port leaves the flow
            NotPortedFlow("memory cards: the options save's CardFullPrompt MEMCARD_003 (vtable 0x002A4B78)");
            m_.SetPending(new OsExitPoint(m_));
        }
        phase = 2;
    }
};

// orig: XBox_OS_ConfirmOptions (vtable 0x002A4CA4, 0x000DF7E0(MEMCARD_037, MEMHINT_002, 0, 0)): Enter 0x000DCA30,
// Update 0x000DEC40, Exit 0x000DF9F0.
class OsConfirmOptions : public DialogState {
public:
    explicit OsConfirmOptions(MemoryCards& m) : DialogState(m, 0x002A4CA4, "MEMCARD_037", "MEMHINT_002") {}
    void Enter() override {                           // orig: 0x000DCA30
        if (!buttons_) {
            m_.dialog.SetButtons(1, 2);               // 0x000E3790(1, 2): "Yes" / "No"
            m_.dialog.Select(1);                      // 0x000E35A0(1): the right one
            buttons_ = true;
        }
        DialogState::Enter();                         // 0x000DF970
    }
    void Update() override {                          // orig: 0x000DEC40
        if (m_.RepeatInput(4)) {
            if (m_.dialog.selection == 0) {           // (+0x10)+0x5C: yes
                m_.PlayDialogSound();                 // ((+0xC)+4)+4 -> 0x000E1980 (only here)
                m_.SetPending(new OsIsCardFull(m_));  // new(0x1C) 0x000DF370, vtable 0x002A4B64
            } else {
                m_.SetPending(new OsExitPoint(m_));   // new(0x20), +0x1C = 0
            }
            m_.flag9 = false;                         // MC+9 = 0 (both)
            phase = 2;
        }
        if (m_.RepeatInput(2)) m_.dialog.Select(0);   // 0x000E35A0(0)
        if (m_.RepeatInput(3)) m_.dialog.Select(1);
    }

private:
    bool buttons_ = false;                            // orig: +0x3C
};

// orig: XBox_OS_EntryPoint (vtable 0x002A4C7C, ctor 0x000DEBC0, 0x20 bytes): Enter 0x000DF1B0, Exit 0x000DF400.
class OsEntryPoint : public FlowState {
public:
    explicit OsEntryPoint(MemoryCards& m) : FlowState(m, 0x002A4C7C) {   // orig: 0x000DEBC0
        g_osSavedMode = g_menuInputMode;              // DAT_002D6E18 = 0x00029F80()
        g_menuInputMode = 2;                          // 0x00029F90(2)
    }
    void Enter() override {                           // orig: 0x000DF1B0
        if (FrontendContext* fe = FrontendOf(m_.app)) fe->flowBusy = true;   // FindContext("Frontend")+0x2E = 1
        if (!m_.cardAvailable) {                      // 0x000E00A0
            m_.flag9 = false;                         // MC+9 = 0
            m_.SetPending(new OsExitPoint(m_));
        } else {
            m_.SetPending(new OsConfirmOptions(m_));  // new(0x40)
        }
        started_ = false;                             // +0x1C = 0
        phase = 2;
    }

private:
    bool started_ = false;                            // orig: +0x1C
};

void OsMessagePrompt::Update() {                      // orig: 0x000DF110
    if (!m_.RepeatInput(4)) return;                   // 0x000E4D20(4)
    m_.PlayDialogSound();                             // ((+0xC)+4)+4 -> 0x000E1980
    m_.SetPending(new OsExitPoint(m_));               // new(0x20) 0x000DF370, vtable 0x002A4BE0, +0x1C = 0
    phase = 2;
}

// ---- the confirm box (orig: CB_*, starter 0x000E4F50(text)) -------------------------------------------------------

// orig: CB_ExitPoint (vtable 0x002A4CDC, 0x1C bytes): Enter 0x000DC0D0, Exit 0x000DF410.
class CbExitPoint : public FlowState {
public:
    explicit CbExitPoint(MemoryCards& m) : FlowState(m, 0x002A4CDC) {}
    void Exit() override {                            // orig: 0x000DF410
        m_.Stop();                                    // 0x000E5210
        m_.mode = g_cbResult == 0 ? 0 : 1;            // MC+0x70: 0 yes, 1 no
        Finish();
    }
};

// orig: CB_ConfirmPrompt (vtable 0x002A4D24, 0x000DF7E0(text, 0, 0, 0)): Enter 0x000DCA30, Update 0x000DF510, Exit
// 0x000DF9F0.
class CbConfirmPrompt : public DialogState {
public:
    CbConfirmPrompt(MemoryCards& m, const char* text) : DialogState(m, 0x002A4D24, text, nullptr) {}
    void Enter() override {                           // orig: 0x000DCA30
        if (!buttons_) {
            m_.dialog.SetButtons(1, 2);               // 0x000E3790(1, 2): "Yes" / "No"
            m_.dialog.Select(1);                      // 0x000E35A0(1)
            buttons_ = true;
        }
        DialogState::Enter();
    }
    void Update() override {                          // orig: 0x000DF510
        if (m_.RepeatInput(4)) {
            m_.PlayDialogSound();                     // ((+0xC)+4)+4 -> 0x000E1980
            g_cbResult = m_.dialog.selection != 0 ? 1 : 0;   // DAT_002D6E1C
            phase = 2;
            m_.SetPending(new CbExitPoint(m_));       // new(0x1C) 0x000DF370, vtable 0x002A4CDC
        }
        if (m_.RepeatInput(2)) m_.dialog.Select(0);
        if (m_.RepeatInput(3)) m_.dialog.Select(1);
    }

private:
    bool buttons_ = false;                            // orig: +0x3C
};

// orig: CB_EntryPoint (vtable 0x002A4D00, ctor 0x000DF460(text): +0x1C the text id): Enter 0x000DF650, Exit 0x000DF400.
class CbEntryPoint : public FlowState {
public:
    CbEntryPoint(MemoryCards& m, const char* text) : FlowState(m, 0x002A4D00), text_(text ? text : "") {}
    void Enter() override {                           // orig: 0x000DF650
        m_.SetPending(new CbConfirmPrompt(m_, text_.c_str()));   // new(0x40) 0x000DF7E0(+0x1C, 0, 0, 0)
        g_cbResult = -1;                              // DAT_002D6E1C = -1
        phase = 2;
    }

private:
    std::string text_;                                // orig: +0x1C
};

// ---- the load flow (orig: XBox_L_* / FL / IL, starter 0x000E5170) ------------------------------------------------

s32 g_lSavedMode = 0;                                 // orig: DAT_002D6E14 (the menu input mode before the flow)
std::optional<std::string> g_loadedLevel;             // orig: DAT_002ED000 (the loaded save's "Level=" value)

// orig: _stricmp(App+0xBC's name, "Ingame") (0x0029416C)
bool InIngame(App& app) { return app.Current() && crt::StrICmp(app.Current()->name.c_str(), "Ingame") == 0; }

// orig: 0x000DF300 (the states' +0x1C or +0x3C): the current context's save-slot list panel (Ingame: the pause's
// ((Ingame+0x14)+0x1C), else FindContext("Frontend")+0x28) and its list model 0x0001A9B0.
LoadPanel* CurrentLoadPanel(App& app) {
    if (InIngame(app)) return app.pause ? app.pause->load.get() : nullptr;
    FrontendContext* fe = FrontendOf(app);
    return fe ? fe->loadPanel.get() : nullptr;
}

// orig: XBox_L_ExitPoint (vtable 0x002A48A4, new(0x1C) 0x000DD640): Enter 0x000DDCF0, Update 0x000DC0D0, Exit 0x000DF400.
class LExitPoint : public FlowState {
public:
    explicit LExitPoint(MemoryCards& m) : FlowState(m, 0x002A48A4) {}
    void Enter() override;                            // orig: 0x000DDCF0
    void Update() override { phase = 2; }             // orig: 0x000DC0D0
};

// orig: XBox_FL_ExitPoint (vtable 0x002A48B8, new(0x28) 0x000DD660; 0x000DF290 at +0x1C: the Frontend, its load
// panel, the list): Enter 0x000DDDA0, Exit 0x000DD6B0.
class FlExitPoint : public FlowState {
public:
    explicit FlExitPoint(MemoryCards& m) : FlowState(m, 0x002A48B8), fe_(FrontendOf(m.app)) {
        panel_ = fe_ ? fe_->loadPanel.get() : nullptr;   // +0x20 (+0x24 its list)
    }
    void Enter() override {                           // orig: 0x000DDDA0
        g_menuInputMode = g_lSavedMode;               // 0x00029F90(DAT_002D6E14)
        if (FrontendContext* fe = FrontendOf(m_.app)) fe->flowBusy = false;   // FindContext("Frontend")+0x2E = 0
        if (g_loadedLevel) {
            m_.app.SetLevelName(g_loadedLevel->c_str());   // 0x000A23D0
            g_loadedLevel.reset();                    // (deleted, DAT_002ED000 = 0)
            m_.app.RequestContext("Ingame", 0x000DDE03);   // 0x000A3160
            if (fe_) fe_->stay = false;               // (+0x1C)+0x4C = 0
            m_.Stop();                                // 0x000E5210 (the load panel stays shown)
            Finish();                                 // 0x000DF3B0
            return;
        }
        phase = 2;
    }
    void Exit() override {                            // orig: 0x000DD6B0 (no game: the panel closes first)
        if (!panel_) {                                // (the original reads it without a test)
            Finish();
            return;
        }
        if (panel_->active) {
            panel_->Hide();                           // vf1C
            return;
        }
        if (panel_->closing) return;
        panel_->Deactivate();                         // vf24
        m_.Stop();
        g_menuInputMode = g_lSavedMode;
        Finish();
    }

private:
    FrontendContext* fe_ = nullptr;                   // orig: +0x1C
    LoadPanel* panel_ = nullptr;                      // orig: +0x20
};

// orig: XBox_IL_ExitPoint (vtable 0x002A48CC, new(0x2C) 0x000DD700; 0x000DF2C0 at +0x1C: the pause menu, its load
// panel +0x20, its main panel +0x24, the list +0x28): Enter 0x000DD750, Exit 0x000DD790.
class IlExitPoint : public FlowState {
public:
    explicit IlExitPoint(MemoryCards& m) : FlowState(m, 0x002A48CC), pause_(m.app.pause.get()) {}
    void Enter() override {                           // orig: 0x000DD750
        g_menuInputMode = g_lSavedMode;
        LoadPanel* load = pause_ ? pause_->load.get() : nullptr;
        if (!load) {
            phase = 2;
            return;
        }
        if (load->active) {
            load->Hide();                             // vf1C
            return;
        }
        if (!load->closing) {
            load->Deactivate();                       // vf24 (nothing)
            phase = 2;
        }
    }
    void Exit() override {                            // orig: 0x000DD790
        if (g_loadedLevel && pause_) {
            PausePanel& main = pause_->main;          // +0x24
            if (main.active) {
                main.Hide();                          // vf1C: the fold-up, the pause's fade-out
                return;
            }
            if (main.closing) return;                 // (+6: until the main panel's close ends: the pause closes)
            main.Deactivate();                        // vf24
            m_.app.SetLevelName(g_loadedLevel->c_str());   // 0x000A23D0: Ingame's next update loads it
            g_loadedLevel.reset();
        }
        g_menuInputMode = g_lSavedMode;
        m_.Stop();
        Finish();
    }

private:
    PauseMenu* pause_ = nullptr;                      // orig: +0x1C
};

void LExitPoint::Enter() {                            // orig: 0x000DDCF0
    // REPE CMPSB over 7 bytes of the current context's name with "Ingame\0" (exact)
    const bool ingame = m_.app.Current() && m_.app.Current()->name == "Ingame";
    if (ingame) m_.SetPending(new IlExitPoint(m_));   // new(0x2C) 0x000DD700
    else m_.SetPending(new FlExitPoint(m_));          // new(0x28) 0x000DD660
    phase = 2;
}

// The load prompts' common part: 0x000DF300 at +0x3C (the panel, +0x40 its list), +0x44 the buttons set.
class LPrompt : public DialogState {
public:
    LPrompt(MemoryCards& m, u32 vtable, const char* text, const char* hint)
        : DialogState(m, vtable, text, hint), panel_(CurrentLoadPanel(m.app)) {}

protected:
    void YesNo() {                                    // orig: 0x000DD9A0 (once: Yes / No, the right one)
        if (!buttons_) {
            m_.dialog.SetButtons(1, 2);               // 0x000E3790(1, 2)
            m_.dialog.Select(1);                      // 0x000E35A0(1)
            buttons_ = true;
        }
    }
    LoadPanel* panel_ = nullptr;                      // orig: +0x3C (+0x40 its list)
    bool buttons_ = false;                            // orig: +0x44
};

class LSavegameListPrompt;
class LLoad;

// orig: XBox_L_DeleteFailedPrompt (vtable 0x002A495C, new(0x48) 0x000DDA70: MEMCARD_026 / MEMHINT_001): Enter
// 0x000DDAD0, Update 0x000DE4C0, Exit 0x000DF9F0.
class LDeleteFailedPrompt : public LPrompt {
public:
    explicit LDeleteFailedPrompt(MemoryCards& m) : LPrompt(m, 0x002A495C, "MEMCARD_026", "MEMHINT_001") {}
    void Enter() override {                           // orig: 0x000DDAD0
        if (!buttons_) {
            m_.dialog.SetSingle(6);                   // 0x000E3930(6): "Ok"
            m_.dialog.Select(2);
            buttons_ = true;
        }
        DialogState::Enter();
    }
    void Update() override;                           // orig: 0x000DE4C0
};

// orig: XBox_L_NoGamesFoundPrompt (vtable 0x002A493C, new(0x44) 0x000DD9E0: MEMCARD_051 / MEMHINT_001): Enter
// 0x000DDA40, Update 0x000DE410, Exit 0x000DF9F0.
class LNoGamesFoundPrompt : public LPrompt {
public:
    explicit LNoGamesFoundPrompt(MemoryCards& m) : LPrompt(m, 0x002A493C, "MEMCARD_051", "MEMHINT_001") {}
    void Enter() override {                           // orig: 0x000DDA40 (the button every step: no "once" flag)
        m_.dialog.SetSingle(6);
        m_.dialog.Select(2);
        DialogState::Enter();
    }
    void Update() override {                          // orig: 0x000DE410
        if (!m_.RepeatInput(4)) return;
        m_.PlayDialogSound();                         // 0x000E1980
        m_.SetPending(new LExitPoint(m_));            // new(0x1C), vtable 0x002A48A4
        phase = 2;
    }
};

// orig: XBox_L_CorruptLoadFailedPrompt (vtable 0x002A491C, new(0x48) 0x000DD940: MEMCARD_052 / MEMHINT_002): Enter
// 0x000DD9A0, Update 0x000DE280, Exit 0x000DF9F0.
class LCorruptLoadFailedPrompt : public LPrompt {
public:
    explicit LCorruptLoadFailedPrompt(MemoryCards& m) : LPrompt(m, 0x002A491C, "MEMCARD_052", "MEMHINT_002") {}
    void Enter() override {                           // orig: 0x000DD9A0
        YesNo();
        DialogState::Enter();
    }
    void Update() override;                           // orig: 0x000DE280
};

// orig: XBox_L_LoadConfirmPrompt (vtable 0x002A4A74, new(0x48) 0x000DE0C0: MEMCARD_028 / MEMHINT_002): Enter
// 0x000DD9A0, Update 0x000DE180, Exit 0x000DF9F0.
class LLoadConfirmPrompt : public LPrompt {
public:
    explicit LLoadConfirmPrompt(MemoryCards& m) : LPrompt(m, 0x002A4A74, "MEMCARD_028", "MEMHINT_002") {}
    void Enter() override {
        YesNo();
        DialogState::Enter();
    }
    void Update() override;                           // orig: 0x000DE180
};

// orig: XBox_L_SavegameListPrompt (vtable 0x002A48F4, new(0x28) 0x000DD8A0; 0x000DF300 at +0x1C): Enter 0x000DC5C0,
// Update 0x000DE550, Exit 0x000DF400.
class LSavegameListPrompt : public FlowState {
public:
    explicit LSavegameListPrompt(MemoryCards& m) : FlowState(m, 0x002A48F4), panel_(CurrentLoadPanel(m.app)) {}
    void Enter() override { phase = 1; }              // orig: 0x000DC5C0
    void Update() override;                           // orig: 0x000DE550

private:
    LoadPanel* panel_ = nullptr;                      // orig: +0x1C (+0x20 its list)
};

// orig: XBox_L_Load (vtable 0x002A4908, new(0x24) 0x000DD8F0; 0x000DF300 at +0x1C): Enter 0x000DC5C0, Update
// 0x000DDF30, Exit 0x000DF400.
class LLoad : public FlowState {
public:
    explicit LLoad(MemoryCards& m) : FlowState(m, 0x002A4908), panel_(CurrentLoadPanel(m.app)) {}
    void Enter() override { phase = 1; }              // orig: 0x000DC5C0
    void Update() override {                          // orig: 0x000DDF30
        std::vector<u8> data;                         // (two locals: the data and its size)
        const SaveList::Entry* e = panel_ ? panel_->list.Selected() : nullptr;   // 0x00029A30
        if (!e) {                                     // (the original reads the entry's +8 without a test)
            m_.SetPending(new LSavegameListPrompt(m_));
            phase = 2;
            return;
        }
        if (m_.ReadGame(e->path, data)) {             // (+4)->0x000E0720(entry +8, &data, &size)
            std::optional<std::string> level = CarryLevelValue(reinterpret_cast<const char*>(data.data()), data.size());
            SetCarryState(m_.app.carry, data.data(), data.size());   // 0x000C1DC0(data, size): the WHOLE text
            // (the data freed)
            g_loadedLevel = std::move(level);         // DAT_002ED000
            m_.SetPending(new LExitPoint(m_));        // new(0x1C) 0x000DF370, vtable 0x002A48A4
        } else if (m_.device.readFailed) {            // (+4)+0x1D: no file, wrong size, read error, bad signature
            m_.SetPending(new LCorruptLoadFailedPrompt(m_));   // new(0x48) 0x000DD940
        } else {
            m_.SetPending(new LSavegameListPrompt(m_));
        }
        phase = 2;
    }

private:
    LoadPanel* panel_ = nullptr;                      // orig: +0x1C (+0x20 its list)
};

void LSavegameListPrompt::Update() {                  // orig: 0x000DE550
    if (m_.RepeatInput(0) && panel_) panel_->Up();    // 0x0001B360
    if (m_.RepeatInput(1) && panel_) panel_->Down();  // 0x0001B3F0
    if (m_.RepeatInput(4)) {
        m_.PlayDialogSound();                         // 0x000E1980
        if (InIngame(m_.app)) m_.SetPending(new LLoadConfirmPrompt(m_));   // new(0x48) 0x000DE0C0
        else m_.SetPending(new LLoad(m_));            // new(0x24) 0x000DD8F0
        phase = 2;
        return;
    }
    if (m_.RepeatInput(5)) {
        m_.SetPending(new LExitPoint(m_));            // new(0x1C), vtable 0x002A48A4
        phase = 2;
    }
}

void LLoadConfirmPrompt::Update() {                   // orig: 0x000DE180
    if (!m_.RepeatInput(4)) {
        if (m_.RepeatInput(2)) m_.dialog.Select(0);
        if (m_.RepeatInput(3)) m_.dialog.Select(1);
        return;
    }
    m_.PlayDialogSound();
    if (m_.dialog.selection == 0) m_.SetPending(new LLoad(m_));   // (+0x10)+0x5C == 0: yes
    else m_.SetPending(new LSavegameListPrompt(m_));
    phase = 2;
}

void LCorruptLoadFailedPrompt::Update() {             // orig: 0x000DE280
    if (m_.RepeatInput(4)) {
        if (m_.dialog.selection == 0) {               // yes: delete the damaged save
            m_.PlayDialogSound();
            SaveList::Entry* e = panel_ ? panel_->list.Selected() : nullptr;   // 0x00029A30
            if (e && m_.DeleteGame(e->name)) {        // (+4)->0x000E07D0(entry +0)
                m_.RemoveEntry(e->saveEntry);         // (+0x10)->0x000E1880(entry +0x14)
                panel_->list.Remove(e);               // 0x00029C70
                panel_->list.SetTop(0);
                panel_->ResetList();                  // 0x0001B2F0
                panel_->Redraw();                     // 0x0001AC10
                if (m_.CountGameSaves() > 0) m_.SetPending(new LSavegameListPrompt(m_));   // (+4)->0x000E0860
                else m_.SetPending(new LExitPoint(m_));   // new(0x1C) 0x000DD640
            } else {
                m_.SetPending(new LDeleteFailedPrompt(m_));   // new(0x48) 0x000DDA70
            }
        } else {
            m_.SetPending(new LSavegameListPrompt(m_));   // no (no confirm sound)
        }
        phase = 2;
    }
    if (m_.RepeatInput(2)) m_.dialog.Select(0);       // (also in the accepting step)
    if (m_.RepeatInput(3)) m_.dialog.Select(1);
}

void LDeleteFailedPrompt::Update() {                  // orig: 0x000DE4C0
    if (!m_.RepeatInput(4)) return;
    m_.PlayDialogSound();
    m_.SetPending(new LSavegameListPrompt(m_));
    phase = 2;
}

// orig: XBox_L_LoadGames (vtable 0x002A48E0, new(0x24) 0x000DD810; 0x000DF300 at +0x1C): Enter 0x000DD860, Update
// 0x000DDE30, Exit 0x000DF400.
class LLoadGames : public FlowState {
public:
    explicit LLoadGames(MemoryCards& m) : FlowState(m, 0x002A48E0), panel_(CurrentLoadPanel(m.app)) {}
    void Enter() override {                           // orig: 0x000DD860
        m_.ClearEntries();                            // (+0x10)->0x000E18B0
        if (panel_) {
            panel_->list.Clear();                     // 0x00029D00
            panel_->ResetList();                      // 0x0001B2F0
            panel_->list.SetTop(0);                   // 0x00029BD0(0)
            panel_->Redraw();                         // 0x0001AC10
        }
        phase = 1;
    }
    void Update() override {                          // orig: 0x000DDE30
        // (two QueryCounter reads around the listing, their difference computed and dropped)
        m_.ListGames();                               // (+4)->0x000E05D0
        if (m_.saveEntries.empty()) {                 // (+0x10)->0x000E1730: the count +0x14
            if (panel_) panel_->list.Clear();
            m_.SetPending(new LNoGamesFoundPrompt(m_));   // new(0x44) 0x000DD9E0
        } else {
            m_.SetPending(new LSavegameListPrompt(m_));   // new(0x28) 0x000DD8A0
        }
        if (panel_) {
            panel_->list.SetTop(0);
            panel_->list.Fill(m_.app);                // 0x00029D90
            panel_->Redraw();
        }
        phase = 2;
    }

private:
    LoadPanel* panel_ = nullptr;                      // orig: +0x1C (+0x20 its list)
};

// orig: XBox_L_EntryPoint (vtable 0x002A4A4C, new(0x20) 0x000DDBA0): Enter 0x000DD610, Update 0x000DDC70, Exit
// 0x000DF400; +0x1C the panel.
class LEntryPoint : public FlowState {
public:
    explicit LEntryPoint(MemoryCards& m) : FlowState(m, 0x002A4A4C) {   // orig: 0x000DDBA0
        g_lSavedMode = g_menuInputMode;               // DAT_002D6E14 = 0x00029F80()
        g_menuInputMode = 2;                          // 0x00029F90(2): the menus' own input off
        if (InIngame(m.app)) {
            panel_ = m.app.pause ? m.app.pause->load.get() : nullptr;   // ((App+0xBC)+0x14)+0x1C
        } else if (FrontendContext* fe = FrontendOf(m.app)) {
            fe->flowBusy = true;                      // FindContext("Frontend")+0x2E = 1
            panel_ = fe->loadPanel.get();             // +0x28
        }
    }
    void Enter() override {                           // orig: 0x000DD610 (every step while phase 0)
        if (!panel_) return;                          // (the original reads it without a test)
        if (panel_->active) {
            phase = 1;
            return;
        }
        if (panel_->opening) return;
        panel_->ResetList();                          // 0x0001B2F0
        panel_->Show();                               // vf18
    }
    void Update() override {                          // orig: 0x000DDC70
        m_.ClearEntries();                            // (+0x10)->0x000E18B0
        m_.device.inUse = true;                       // (+4)->0x000E0220(1)
        m_.SetPending(new LLoadGames(m_));            // new(0x24) 0x000DD810
        phase = 2;
    }

private:
    LoadPanel* panel_ = nullptr;                      // orig: +0x1C
};

}  // namespace

std::optional<std::string> CarryLevelValue(const char* text, size_t size) {   // orig: 0x000C1E10
    if (!text || size == 0) return std::nullopt;
    const char* end = text + size;
    const char* p = text;
    while (p < end && *p) {                           // (CMP BYTE [p], 0)
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p)));   // strchr(p, '\n')
        if (!nl) return std::nullopt;                 // (no test in the original)
        char line[0x100];                             // esp+0x10 (no length check in the original)
        const size_t n = std::min<size_t>(size_t(nl - p), sizeof line - 1);
        std::memcpy(line, p, n);                      // strncpy 0x001672C0
        line[n] = '\0';
        p = nl + 1;
        char* eq = std::strchr(line, '=');            // (no test in the original)
        if (!eq) continue;
        *eq = '\0';
        if (std::memcmp(line, "Level", 6) == 0) return std::string(eq + 1);   // REPE CMPSB x6 (0x002A3380); new copy
    }
    return std::nullopt;
}

void SetCarryState(CarryOver& carry, const u8* data, size_t size) {   // orig: 0x000C1DC0
    if (!data || size == 0) return;
    // DAT_002EAA28 freed, = new(size) with the bytes, DAT_002EAA2C = size (the port: the text up to its NUL)
    const char* s = reinterpret_cast<const char*>(data);
    carry.state = std::string(s, strnlen(s, size));
}

void ResetMemoryCardGlobals() {
    g_osSavedMode = 0;
    g_osWait = 0;
    g_cbResult = -1;
    g_lSavedMode = 0;
    g_loadedLevel.reset();
}

// ---- the save-game list panel ---------------------------------------------------------------------------------------

SavePanel::SavePanel() = default;                     // orig: 0x000E1E60 (the fields cleared)
SavePanel::~SavePanel() = default;
SavePanel::SavePanel(SavePanel&&) noexcept = default;
SavePanel& SavePanel::operator=(SavePanel&&) noexcept = default;

void SavePanel::Load(App& app) {                      // orig: 0x000E2080 (memcard\Savemenu.nif, a direct load)
    // the top-level objects under the scene, the sequences by name (REPE CMPSB: exact; the last of a name wins)
    std::vector<u8> bytes;
    ni::NiStream s;
    scene = new ni::NiNode;                           // +0x18 = new(0xE0) NiNode (before the load)
    if (app.services.readFile && app.services.readFile("memcard\\Savemenu.nif", bytes)) (void)s.Load(bytes);
    for (u32 link : s.TopLevelLinks()) {
        ni::NiObject* o = s.ResolveLink(link);
        if (auto* av = dynamic_cast<ni::NiAVObject*>(o); av && !av->parent) scene->AttachChild(av, true);   // vfA0
        auto* seq = dynamic_cast<ni::NiSequence*>(o); // (RTTI 0x002FA124)
        if (!seq) continue;
        for (int k = 0; k < 2; ++k)
            if (seq->name == (k == 0 ? "Savepanel_Savegame_appear" : "Savepanel_Savegame_Dissappear")) {
                sequences[size_t(k)] = seq;           // +0x30 / +0x34
                const ni::NiTimeController* c = seq->ReferenceController();   // +0x48[+0x90]
                hasSequence[k] = c != nullptr;        // (the original reads the controller without a test)
                length[k] = c ? (double(c->hiKeyTime) - c->loKeyTime) / c->frequency : 0.0;
            }
    }
    camera = FindAs<ni::NiCamera>(scene.Get(), "CAMERA1");   // +0x1C (0x00293F00; RTTI 0x002F9120)
    if (camera) camera->UpdateDownwardPass(0.0f, true);   // (+0x15C = +0x18) Update(0, 1) (no test in the original)
    sceneNode = FindAs<ni::NiNode>(scene.Get(), "SceneNode");   // +0x20 (0x002A1EE4)
    static constexpr const char* kTexts[3] = {"Text4", "Text5", "Text6"};
    for (size_t i = 0; i < 3; ++i) texts[i] = FindAs<ni::NiNode>(scene.Get(), kTexts[i]);   // +0x48 / +0x4C / +0x50
    static constexpr const char* kSlots[3] = {"Cardslot4", "Cardslot5", "Cardslot6"};
    for (size_t i = 0; i < 3; ++i) slots[i] = FindAs<ni::NiSwitchNode>(scene.Get(), kSlots[i]);   // +0x5C .. +0x64
    for (const auto& sw : slots)                      // each switch's child 0 (+0xCC > 0: +0xE0 = 0)
        if (sw && !sw->Children().empty()) sw->index = 0;
    arrowTop = FindAs<ni::NiSwitchNode>(scene.Get(), "Scrollarrow2_top");         // +0x6C
    arrowBottom = FindAs<ni::NiSwitchNode>(scene.Get(), "Scrollarrow2_bottom");   // +0x70
    for (size_t k = 0; k < 2; ++k) {                  // the managers +0x28 / +0x2C on "Savemenu" (+0x38 / +0x3C)
        targets[k] = FindAs<ni::NiNode>(scene.Get(), "Savemenu");   // (0x002A4EEC; RTTI 0x002F9230)
        managers[k] = targets[k] ? new ni::NiKeyframeManager(targets[k].Get()) : nullptr;   // new(0xC0) 0x00130D20
        ni::NiSequence* q = sequences[k].Get();       // (the original reads +0x30 / +0x34 without a test)
        if (q) q->offset = 0.0f;                      // +0xA8 = 0
        if (managers[k] && q) managers[k]->AddSequence(q, true);   // 0x0012FFB0(seq, 1)
        if (managers[k]) managers[k]->Update(0.0f);   // vf38(0)
        SetSequenceLoop(q, false);                    // 0x000E1D70(seq, 0): every controller CLAMP
        if (ni::NiNode* t = targets[k].Get()) {
            t->UpdateDownwardPass(0.0f, true);        // Update(0, 1)
            t->UpdateProperties();
            t->UpdateEffects();
        }
    }
    animated = FindAs<ni::NiNode>(sceneNode.Get(), "Allways_animated");   // +0x24 (0x0029434C, under +0x20)
    if (camera) camera->renderer = app.services.renderer;   // +0x1A8 = [0x002E7C24]->vf14()
    // +0x10: TextObject::Create(0), scale (2.3, 2.7), vAlign / hAlign 0, no wrap, SetRenderer, the scene mode on
    // (+0x1C, +0x20), letter spacing 0.01
    text.reset(text::TextObject::Create(0, app.services.readFile, app.services.renderer));
    if (text) {
        text->SetScale(std::bit_cast<f32>(0x40133333u), std::bit_cast<f32>(0x402CCCCDu));   // 0x00098C00
        text->style.vAlign = 0;                       // +0x64
        text->style.hAlign = 0;                       // +0x60
        text->style.wordWrap = false;                 // +0x68
        text->SetRenderer(app.services.renderer);     // 0x00098F80(App->vf14())
        text->UseScene(camera.Get(), sceneNode.Get());   // 0x00099BE0(+0x1C, +0x20)
        text->style.letterSpacing = std::bit_cast<f32>(0x3C23D70Au);   // +4 = 0.01
    }
    levelNamesDb = app.text ? app.text->LoadDb("Language/LEVELNAMES.ldb") : nullptr;   // +0x14 (0x00295014)
    labels = std::make_unique<MenuLabels>(scene.Get(), camera.Get());   // new(0x10) 0x00025710(+0x18, +0x1C) -> +0x8C
    labels->Init(app);                                // 0x00025850
    labels->LoadDb(app);                              // 0x000256D0
}

void SavePanel::Draw() {                              // orig: 0x000E1A00
    if (!shown) return;                               // +9
    if (sceneNode) sceneNode->UpdateDownwardPass(time, true);   // (+0x20)->Update(+0xC, 1) (no tests in the original)
    if (animated) animated->UpdateDownwardPass(time, true);     // (+0x24)
    if (ni::NiKeyframeManager* m = managers[size_t(which)].Get()) m->Update(time);   // (+0x28 + 4 * +0x40)->vf38(+0xC)
    if (text) text->UpdateCamera();                   // (+0x10) 0x00098D70
    if (camera && camera->renderer) camera->renderer->ClearBuffer(nullptr, 4);   // (+0x1C)+0x1A8 vf44(0, 4)
    if (camera) {
        camera->UpdateDownwardPass(time, true);       // Update(+0xC, 1)
        ni::VisitForCulling(*camera, scene.Get());    // Click 0x00100CE0 (+0x15C = +0x18)
    }
}

bool SavePanel::Show(s32 w, MemoryCards& m) {         // orig: 0x000E2BA0
    if (double(remaining) > 0.0) return false;        // FCOMP [0x00293AB8]; TEST AH,0x41; JNE: proceeds on <= 0
    shown = true;                                     // +9
    if (w == 0) {
        if (labels) {
            labels->LoadDb(m.app);                    // (+0x8C) 0x000256D0
            labels->Build(m.app);                     // 0x00025970
        }
        m.PanelRows();                                // 0x000E2AC0
    } else if (w == 1) {
        // (App+0x100)->0x000A32A0 (the hint: returns true)
        if (labels) labels->RemoveAll();              // 0x000256F0
    } else {                                          // (the original indexes the sequence array with -1)
        std::fprintf(stderr, "NOT PORTED reached: the save panel's sequence -1 (0x000E2BA0)\n");
        return true;
    }
    which = w;                                        // +0x40
    ni::NiSequence* q = sequences[size_t(w)].Get();   // +0x30 + 4 w
    SetSequenceLoop(q, false);                        // 0x000E1D70(seq, 0)
    if (q) q->offset = F(double(time) * -1.0);        // FLD +0xC; FMUL [0x00293BF0]; FSTP seq+0xA8 (no test)
    if (managers[size_t(w)] && q) managers[size_t(w)]->ActivateSequence(q, 0);   // (+0x28 + 4 w) 0x0012F3A0(seq, 0)
    remaining = F(hasSequence[w] ? length[w] : 0.0);  // FLD hi; FSUB lo; FDIV freq; FSTP +0x44
    return true;
}

void SavePanel::Step() {                              // orig: 0x000E2A70
    if (labels) labels->Update();                     // (+0x8C) 0x00025700
    if (!shown) return;                               // +9
    time = F(double(g_frameStep) + time);             // FLD dt; FADD +0xC
    if (double(remaining) > 0.0) {                    // FCOMP 0.0; TEST AH,0x41; JNE
        // orig: 0x000E1A70: FLD +0x44; FSUB dt; FST +0x44; FCOMP 0.0; TEST AH,5; JP (the unrounded difference)
        const double r = double(remaining) - double(g_frameStep);
        remaining = F(r);
        if (!(r < 0.0)) return;
        if (ni::NiKeyframeManager* mg = managers[size_t(which)].Get())   // (+0x28 + 4 * +0x40)
            mg->DeactivateSequence(sequences[size_t(which)].Get());   // 0x0012FAB0(+0x30 + 4 * +0x40)
        if (which == 0) {
            state = 0;                                // shown (the hint no-op HINTS_039)
        } else if (which == 1) {
            state = 1;
            shown = false;
        }
        return;
    }
    if (counter > 0) --counter;                       // +0x78
}

// ---- the save panel's rows (orig: the panel's functions over its head +0x74 and selection +0x68) ------------------

void MemoryCards::PanelRows() {                       // orig: 0x000E2AC0
    if (!panelHead && !saveEntries.empty()) panelHead = &saveEntries.front();   // +0x74 = ((App+0xF4)+4)+0x10
    for (const auto& sw : panel.slots)                // the slots' child 0
        if (sw && !sw->Children().empty()) sw->index = 0;
    PanelNames();                                     // 0x000E1C50
    if (panel.text) panel.text->UpdateCamera();       // (+0x10) 0x00098D70
    PanelArrows();                                    // 0x000E1AE0
    PanelHighlight();                                 // 0x000E1B90
}

void MemoryCards::PanelItems() {                      // orig: 0x000E2B40
    for (const auto& sw : panel.slots)
        if (sw && !sw->Children().empty()) sw->index = 0;
    PanelRows();                                      // 0x000E2AC0
    PanelHighlight();                                 // 0x000E1B90
    PanelArrows();                                    // 0x000E1AE0
}

void MemoryCards::PanelNames() {                      // orig: 0x000E1C50
    text::TextObject* t = panel.text.get();
    if (!t) return;                                   // (+0x10: no test in the original)
    for (u16 id : panel.textIds) (void)t->RemoveString(id);   // 0x00098FD0 for +0x54 / +0x56 / +0x58
    auto it = std::find_if(saveEntries.begin(), saveEntries.end(), [&](const SaveEntry& e) { return &e == panelHead; });
    for (size_t i = 0; it != saveEntries.end() && i < 3; ++i, ++it) {   // along +0x20, at most three rows
        const char16_t* s = nullptr;
        if (it->level && *it->level == "level0-1")    // +0x18, REPE CMPSB over 9 bytes with "level0-1" (0x002A4D94)
            s = app.text ? app.text->GetText(*it->level, panel.levelNamesDb) : nullptr;   // TextManager::Get(.., +0x14)
        else
            s = it->name.c_str();                     // +0xC (the save game's wide name)
        panel.textIds[i] = s ? t->AddStringAt(s, panel.texts[i].Get(), -1.0f, -1.0f) : 0;   // 0x0009A0A0 (Text4 + i)
    }
}

void MemoryCards::PanelArrows() {                     // orig: 0x000E1AE0
    for (ni::NiSwitchNode* sw : {panel.arrowTop.Get(), panel.arrowBottom.Get()})
        if (sw && !sw->Children().empty()) sw->index = 0;
    auto it = std::find_if(saveEntries.begin(), saveEntries.end(), [&](const SaveEntry& e) { return &e == panelHead; });
    if (it == saveEntries.end()) return;              // (+0x74 null)
    if (it != saveEntries.begin() && panel.arrowTop && panel.arrowTop->Children().size() > 1)   // +0x24 (previous)
        panel.arrowTop->index = 1;
    auto n = it;                                      // three more entries after the head (+0x20 three times): the
    s32 more = 0;                                     //  bottom arrow's child 1
    while (more < 3 && ++n != saveEntries.end()) ++more;
    if (more == 3 && panel.arrowBottom && panel.arrowBottom->Children().size() > 1) panel.arrowBottom->index = 1;
}

// The scrolling (the save panel's +0x78 cooldown, the rows' switches +0x5C.. and the sounds +0x7C scroll / +0x84 the
// limit; Sound_PlaySFX(.., 0)): the selection moves within the three rows, at the top / bottom row the head moves.
// The entries' +0x20 next is 0x000AED80 (no null test), +0x24 previous 0x000E1620 (null-safe).
namespace {
using EntryIt = std::list<MemoryCards::SaveEntry>::iterator;
}

bool MemoryCards::PanelDown() {                       // orig: 0x000E2D70
    if (panel.counter > 0) return true;               // +0x78 (JG)
    if (panelSelection >= 2) {                        // the bottom row: the head moves (0x000E2CE0)
        PanelScrollDown();
        panel.counter = 5;
        return true;
    }
    auto head = std::find_if(saveEntries.begin(), saveEntries.end(), [&](const SaveEntry& e) { return &e == panelHead; });
    if (head == saveEntries.end()) return false;      // +0x74 null
    EntryIt e = head;
    for (s32 i = 0; i < panelSelection && e != saveEntries.end(); ++i) ++e;   // 0x000AED80 x +0x68
    EntryIt n = e;
    if (n != saveEntries.end()) ++n;                  // 0x000AED80
    if (n == saveEntries.end()) {
        (void)app.Sound().PlaySfx(panel.limitSound, false);   // +0x84
        panel.counter = 5;
        return false;
    }
    const s32 old = panelSelection++;
    if (ni::NiSwitchNode* sw = panel.slots[size_t(old)].Get(); sw && !sw->Children().empty()) sw->index = 0;   // +0xCC > 0
    if (ni::NiSwitchNode* sw = panel.slots[size_t(panelSelection)].Get(); sw && sw->Children().size() > 1)
        sw->index = 1;                                // +0xCC > 1: +0xE0 = 1
    (void)app.Sound().PlaySfx(panel.scrollSound, false);   // +0x7C
    panel.counter = 5;
    return true;
}

bool MemoryCards::PanelUp() {                         // orig: 0x000E2E40
    if (panel.counter > 0) return true;
    if (panelSelection <= 0) {                        // the top row: the head moves (0x000E2C80)
        PanelScrollUp();
        panel.counter = 5;
        return true;
    }
    auto head = std::find_if(saveEntries.begin(), saveEntries.end(), [&](const SaveEntry& e) { return &e == panelHead; });
    if (head == saveEntries.end()) return false;
    EntryIt e = head;
    for (s32 i = 0; i < panelSelection && e != saveEntries.end(); ++i) ++e;
    if (e == saveEntries.end() || e == saveEntries.begin()) {   // 0x000E1620: no previous entry
        (void)app.Sound().PlaySfx(panel.limitSound, false);
        panel.counter = 5;
        return false;
    }
    const s32 old = panelSelection--;
    if (ni::NiSwitchNode* sw = panel.slots[size_t(old)].Get(); sw && !sw->Children().empty()) sw->index = 0;
    if (ni::NiSwitchNode* sw = panel.slots[size_t(panelSelection)].Get(); sw && sw->Children().size() > 1)
        sw->index = 1;
    (void)app.Sound().PlaySfx(panel.scrollSound, false);
    panel.counter = 5;
    return true;
}

void MemoryCards::PanelScrollDown() {                 // orig: 0x000E2CE0 (true either way)
    auto head = std::find_if(saveEntries.begin(), saveEntries.end(), [&](const SaveEntry& e) { return &e == panelHead; });
    s32 after = 0;                                    // three entries after the head (+0x20 three times)
    for (EntryIt n = head; after < 3 && n != saveEntries.end() && ++n != saveEntries.end();) ++after;
    if (head == saveEntries.end() || after < 3) {
        (void)app.Sound().PlaySfx(panel.limitSound, false);   // +0x84
        panel.counter = 5;
        return;
    }
    panelHead = &*std::next(head);                    // +0x74 = its next
    (void)app.Sound().PlaySfx(panel.scrollSound, false);   // +0x7C
    panel.counter = 5;
    PanelRows();                                      // 0x000E2AC0
}

void MemoryCards::PanelScrollUp() {                   // orig: 0x000E2C80 (true either way)
    auto head = std::find_if(saveEntries.begin(), saveEntries.end(), [&](const SaveEntry& e) { return &e == panelHead; });
    if (head == saveEntries.end() || head == saveEntries.begin()) {   // +0x74 null or no previous (0x000E1620)
        (void)app.Sound().PlaySfx(panel.limitSound, false);
        panel.counter = 5;
        return;
    }
    panelHead = &*std::prev(head);                    // +0x74 = its previous
    (void)app.Sound().PlaySfx(panel.scrollSound, false);
    panel.counter = 5;
    PanelRows();                                      // 0x000E2AC0
}

void MemoryCards::PanelHighlight() {                  // orig: 0x000E1B90
    for (const auto& sw : panel.slots)
        if (sw && !sw->Children().empty()) sw->index = 0;
    if (panelSelection >= 0 && panelSelection < 3)    // (+0x68 indexes the slots without a test)
        if (ni::NiSwitchNode* sw = panel.slots[size_t(panelSelection)].Get(); sw && sw->Children().size() > 1)
            sw->index = 1;
}

// ---- the state base ------------------------------------------------------------------------------------------------

void FlowState::Exit() { Finish(); }                  // orig: 0x000DF400 -> 0x000DF3B0

void FlowState::Finish() {                            // orig: 0x000DF3B0: manager +0x18 = 0; delete this
    // (Called on the pending state by 0x000E4C20 / 0x000E4BE0 too: the original then leaks the current state
    // (+0x18 cleared without a delete); the port frees it, nothing refers to it any more.)
    std::unique_ptr<FlowState> self;
    if (m_.current.get() == this) {
        self = std::move(m_.current);
    } else {
        if (m_.pending.get() == this) self = std::move(m_.pending);
        m_.current.reset();
    }
}

void FlowState::Step() {                              // orig: 0x000DF3D0 (the phase's slot)
    switch (phase) {
    case 0: Enter(); break;                           // vf04
    case 1: Update(); break;                          // vf08
    case 2: Exit(); break;                            // vf0C
    default: break;
    }
}

// ---- the dialog ----------------------------------------------------------------------------------------------------

void Dialog::Step(App& a) {                           // orig: 0x000E4A00
    (void)a;
    if (!open) return;                                // +0xA0
    time = F(double(g_frameStep) * 0.5f + double(time));   // FLD dt; FMUL [0x00293AA0] 0.5; FADD +0x8C
    if (!(double(anim) > -0.1f)) return;              // FCOMP [0x00293BF4]; TEST AH,0x41; JNE
    const double r = double(anim) - double(g_frameStep) * 0.5f;   // FSUBR +0x90
    anim = F(r);
    if (!(r < -0.1f)) return;                         // FCOMP -0.1; TEST AH,5; JP
    anim = -0.1f;                                     // 0xBDCCCCCD
    if (opening) {                                    // +0xA1: the open animation ended
        if (manager && enter) manager->DeactivateSequence(enter.Get());   // (+0x94) 0x0012FAB0(+0x64)
        opening = false;
        return;
    }
    // the close animation ended
    if (manager && leave) manager->DeactivateSequence(leave.Get());   // (+0x68)
    RemoveText();                                     // 0x000E3040
    open = false;                                     // +0xA0
    SetButtons(0, 0);                                 // 0x000E3790(0, 0): the old labels removed, none added
    for (ni::NiSwitchNode* s : {yesSwitch.Get(), noSwitch.Get(), okSwitch.Get()})   // +0x70 / +0x74 / +0x78: child 0
        if (s && !s->Children().empty()) s->index = 0;
    if (yesManager && yes) yesManager->DeactivateSequence(yes.Get());   // (+8)(+0x14), (+0xC)(+0x18), (+0x10)(+0x1C)
    if (noManager && no) noManager->DeactivateSequence(no.Get());
    if (okManager && ok) okManager->DeactivateSequence(ok.Get());
    selection = 3;                                    // +0x5C
    ClearButtons();                                   // 0x000E30A0
}

bool Dialog::Open() {                                 // orig: 0x000E4900
    flag2C = false;
    opening = true;                                   // +0xA1 (before the test)
    if (double(anim) > -0.1f) return false;           // FCOMP [0x00293BF4]; TEST AH,0x41; JNE: only <= -0.1 opens
    if (enter) enter->offset = F(double(time) * -1.0f);   // (+0x64)+0xA8 = +0x8C * [0x00293BF0]
    if (manager && enter) manager->ActivateSequence(enter.Get(), 0);   // (+0x94) 0x0012F3A0(+0x64, 0)
    anim = F(enterLength);                            // +0x90 (FLD 0.0, or the sequence's length)
    open = true;                                      // +0xA0
    return true;
}

bool Dialog::Close() {                                // orig: 0x000E4980
    flag2C = false;
    if (double(anim) > -0.1f) return false;
    if (leave) leave->offset = F(double(time) * -1.0f);   // (+0x68)+0xA8
    if (manager && leave) manager->ActivateSequence(leave.Get(), 0);
    anim = F(leaveLength);
    open = true;
    return true;
}

void Dialog::Select(s32 sel) {                        // orig: 0x000E35A0
    if (sel != 3 && owner && !owner->Sound().VoiceExists(selectVoice))   // Voice_Exists(+0x4C)
        selectVoice = owner->Sound().PlaySfx(selection == sel ? scrollLimitSound : scrollSound, false);   // +0x48 / +0x40
    auto hl = [](ni::NiSwitchNode* s, s32 i) {
        if (s && s32(s->Children().size()) > i) s->index = i;   // CMP +0xCC; JLE
    };
    auto off = [](ni::NiKeyframeManager* m, ni::NiSequence* q) {
        if (m && q) m->DeactivateSequence(q);
    };
    auto on = [](ni::NiKeyframeManager* m, ni::NiSequence* q) {
        if (m && q) m->ActivateSequence(q, 0);
    };
    switch (sel) {
    case 0:                                           // the left button: +0x70 child 1, the others 0
        hl(yesSwitch.Get(), 1), hl(noSwitch.Get(), 0), hl(okSwitch.Get(), 0);
        off(noManager.Get(), no.Get()), off(okManager.Get(), ok.Get()), on(yesManager.Get(), yes.Get());
        break;
    case 1:                                           // the right one
        hl(yesSwitch.Get(), 0), hl(noSwitch.Get(), 1), hl(okSwitch.Get(), 0);
        off(yesManager.Get(), yes.Get()), off(okManager.Get(), ok.Get()), on(noManager.Get(), no.Get());
        break;
    case 2:                                           // the single one
        hl(yesSwitch.Get(), 0), hl(noSwitch.Get(), 0), hl(okSwitch.Get(), 1);
        off(yesManager.Get(), yes.Get()), off(noManager.Get(), no.Get()), on(okManager.Get(), ok.Get());
        break;
    case 3:                                           // none
        hl(yesSwitch.Get(), 0), hl(noSwitch.Get(), 0), hl(okSwitch.Get(), 0);
        off(yesManager.Get(), yes.Get()), off(noManager.Get(), no.Get()), off(okManager.Get(), ok.Get());
        break;
    default:
        break;
    }
    selection = sel;                                  // +0x5C
}

bool Dialog::SetString(const char16_t* s) {           // orig: 0x000E3500
    if (textId != 0) return false;                    // +0xA2
    if (double(anim) > -0.1f) return false;           // FCOMP [0x00293BF4]; TEST AH,0x41; JNE: only when idle
    // (+0x50)->0x0009A0A0(s, +0x9C, -1, -1)
    textId = text && s ? text->AddStringAt(s, textNode.Get(), -1.0f, -1.0f) : 0;
    return true;
}

bool Dialog::SetText(const char* id) {                // orig: 0x000E3570
    const char16_t* s = owner && owner->text ? owner->text->GetText(id, textsDb) : nullptr;   // GetText(id, +0x3C)
    return SetString(s);
}

bool Dialog::RemoveText() {                           // orig: 0x000E3040
    if (textId == 0) return false;
    if (double(anim) > -0.1f) return false;
    if (text) text->RemoveString(textId);             // 0x00098FD0
    textId = 0;
    return true;
}

void Dialog::ClearHint() {                            // orig: 0x000E3080
    if (hintText) {
        hintText->RemoveAll();                        // 0x00099080
        hintText->UpdateCamera();                     // 0x00098D70
    }
    hintId = 0;                                       // +0x34
}

void Dialog::ClearButtons() {                         // orig: 0x000E30A0
    for (u16& id : buttonIds) {
        if (id && text) text->RemoveString(id);
        id = 0;
    }
}

namespace {
// the buttons' labels 0x002A504C .. 0x002A5004 (1 Yes .. 7 Dashboard)
const char* const kButtonLabels[7] = {"MEMCARD_040", "MEMCARD_041", "MEMCARD_042", "MEMCARD_043",
                                      "MEMCARD_044", "MEMCARD_045", "MEMCARD_046"};
}  // namespace

void Dialog::SetButtons(s32 left, s32 right) {        // orig: 0x000E3790
    ClearButtons();                                   // 0x000E30A0
    auto add = [this](s32 n, ni::NiNode* at) -> u16 {
        if (n < 1 || n > 7 || !text || !owner || !owner->text) return 0;   // (DEC; CMP 6; JA: no label)
        const char16_t* s = owner->text->GetText(kButtonLabels[n - 1], textsDb);
        // AddStringAt(s, node, -1, width): the width -1, 0.2 (0x3E4CCCCD) for 7 "Dashboard"
        return text->AddStringAt(s, at, -1.0f, n == 7 ? std::bit_cast<f32>(0x3E4CCCCDu) : -1.0f);
    };
    if (left >= 1 && left <= 7) buttonIds[0] = add(left, yesText.Get());     // +0x7C -> +0
    if (right >= 1 && right <= 7) buttonIds[1] = add(right, noText.Get());   // +0x80 -> +2
}

void Dialog::SetSingle(s32 n) {                       // orig: 0x000E3930
    ClearButtons();
    if (n < 1 || n > 7 || !text || !owner || !owner->text) return;
    const char16_t* s = owner->text->GetText(kButtonLabels[n - 1], textsDb);
    buttonIds[2] = text->AddStringAt(s, okText.Get(), -1.0f, -1.0f);   // +0x84 -> +4
}

Dialog::Dialog() = default;
Dialog::~Dialog() = default;

bool Dialog::Build(App& app) {                        // orig: 0x000E3B50
    owner = &app;
    textsDb = app.text ? app.text->LoadDb("Language/XBOXMEM.ldb") : nullptr;        // +0x3C
    hintsDb = app.text ? app.text->LoadDb("Language/MEMHINTS_XBOX.ldb") : nullptr;  // +0x38
    scene = new ni::NiNode;                           // +0x54 = new(0xE0) NiNode (before the load)
    std::vector<u8> bytes;
    ni::NiStream s;
    if (app.services.readFile && app.services.readFile("memcard\\Dialogmenu.nif", bytes)) (void)s.Load(bytes);
    for (u32 link : s.TopLevelLinks()) {              // (+0x54 array, +0x5C count)
        ni::NiObject* o = s.ResolveLink(link);
        if (auto* av = dynamic_cast<ni::NiAVObject*>(o); av && !av->parent)   // (RTTI 0x002F9190, no parent)
            scene->AttachChild(av, true);             // vfA0(obj, 1)
        auto* seq = dynamic_cast<ni::NiSequence*>(o); // (RTTI 0x002FA124; the names by REPE CMPSB: exact)
        if (!seq) continue;
        const ni::NiTimeController* c = seq->ReferenceController();
        const double len = c ? (double(c->hiKeyTime) - c->loKeyTime) / c->frequency : 0.0;
        if (seq->name == "Dialog_menu_enter") enterLength = len, enter = seq;   // +0x64
        if (seq->name == "Dialog_manu_leave") leaveLength = len, leave = seq;   // +0x68 (sic)
        if (seq->name == "yes") yes = seq;            // +0x14
        if (seq->name == "no") no = seq;              // +0x18
        if (seq->name == "ok") ok = seq;              // +0x1C
    }
    sceneNode = FindAs<ni::NiNode>(scene.Get(), "SceneNode");   // +0x60
    yesSwitch = FindAs<ni::NiSwitchNode>(scene.Get(), "save_yes");   // +0x70
    yesNode = FindAs<ni::NiNode>(scene.Get(), "save_yes");      // +0x20
    noSwitch = FindAs<ni::NiSwitchNode>(scene.Get(), "save_no");     // +0x74
    noNode = FindAs<ni::NiNode>(scene.Get(), "save_no");        // +0x24
    okSwitch = FindAs<ni::NiSwitchNode>(scene.Get(), "ok");     // +0x78
    okNode = FindAs<ni::NiNode>(scene.Get(), "ok");             // +0x28
    yesText = FindAs<ni::NiNode>(scene.Get(), "save_text_yes"); // +0x7C
    noText = FindAs<ni::NiNode>(scene.Get(), "save_text_no");   // +0x80
    okText = FindAs<ni::NiNode>(scene.Get(), "text_ok");        // +0x84
    camera = FindAs<ni::NiCamera>(sceneNode.Get(), "CAMERA1");  // +0x98
    if (!camera) return false;                        // (the stream released: 0x00108AC0)
    camera->sceneRoot = sceneNode.Get();              // +0x15C = +0x60 (the text's Click culls it, not +0x54)
    camera->UpdateDownwardPass(0.0f, true);           // Update(0, 1)
    textNode = FindAs<ni::NiNode>(sceneNode.Get(), "text");     // +0x9C
    animNode = FindAs<ni::NiNode>(sceneNode.Get(), "Dialogmenu:Animation (Translational)");   // +0x6C
    if (!animNode) return false;
    animNode->UpdateDownwardPass(0.0f, true);         // Update(0, 1), UpdateProperties, UpdateEffects
    animNode->UpdateProperties();
    animNode->UpdateEffects();
    // the managers new(0xC0) 0x00130D20: +0x94 on +0x6C with the enter / leave sequences (start time 0, clamp), +8 /
    // +0xC / +0x10 on the buttons' nodes with "yes" / "no" / "ok" (start time 0, loop); each Update(0) (vf38)
    auto make = [](ni::NiNode* target, std::initializer_list<std::pair<ni::NiSequence*, bool>> seqs) {
        ni::NiPointer<ni::NiKeyframeManager> m(target ? new ni::NiKeyframeManager(target) : nullptr);
        for (const auto& [q, loop] : seqs) {
            if (q) q->offset = 0.0f;                  // +0xA8 = 0
            if (m && q) m->AddSequence(q, true);
        }
        if (m) m->Update(0.0f);
        for (const auto& [q, loop] : seqs) SetSequenceLoop(q, loop);   // 0x000E3410
        return m;
    };
    manager = make(animNode.Get(), {{enter.Get(), false}, {leave.Get(), false}});
    yesManager = make(yesNode.Get(), {{yes.Get(), true}});
    noManager = make(noNode.Get(), {{no.Get(), true}});
    okManager = make(okNode.Get(), {{ok.Get(), true}});
    camera->renderer = app.services.renderer;         // +0x1A8 = App->vf14()
    // +0x50: TextObject::Create(0), +0x7A = 1, the scene mode (+0x98, +0x54), white, SetRenderer, the scene mode again,
    // hAlign / vAlign 0, width 0.8, scale (2.4, 2.7), letter spacing 0.01, word wrap
    text.reset(text::TextObject::Create(0, app.services.readFile, app.services.renderer));
    if (text) {
        text->field7A = 1;
        text->UseScene(camera.Get(), scene.Get());    // 0x00099BE0
        text->color = ni::NiColor{1, 1, 1};           // +0x6C..+0x74
        text->SetRenderer(app.services.renderer);     // 0x00098F80 (nothing in the scene mode)
        text->UseScene(camera.Get(), scene.Get());
        text->style.hAlign = 0;                       // +0x60
        text->style.vAlign = 0;                       // +0x64
        text->width = std::bit_cast<f32>(0x3F4CCCCDu);   // +0x54 = 0.8
        text->SetScale(std::bit_cast<f32>(0x4019999Au), std::bit_cast<f32>(0x402CCCCDu));   // (2.4, 2.7)
        text->style.letterSpacing = std::bit_cast<f32>(0x3C23D70Au);   // +4 = 0.01
        text->style.wordWrap = true;                  // +0x68
    }
    // +0x30: TextObject::Create(0), +0x7A = 1, white, SetRenderer, hAlign / vAlign 0, scale (0.8, 1.1), (+0x50)+0x68
    // = 1 again, x 0.5, y 0.08, SetZBufferTest(0)
    hintText.reset(text::TextObject::Create(0, app.services.readFile, app.services.renderer));
    if (hintText) {
        hintText->field7A = 1;
        hintText->color = ni::NiColor{1, 1, 1};
        hintText->SetRenderer(app.services.renderer);
        hintText->style.hAlign = 0;
        hintText->style.vAlign = 0;
        hintText->SetScale(std::bit_cast<f32>(0x3F4CCCCDu), std::bit_cast<f32>(0x3F8CCCCDu));   // (0.8, 1.1)
        if (text) text->style.wordWrap = true;
        hintText->x = 0.5f;                           // +0x4C
        hintText->y = std::bit_cast<f32>(0x3DA3D70Au);   // +0x50 = 0.08
        hintText->SetZBufferTest(false);              // 0x00099660(0)
    }
    // a z-buffer property (new(0x1C), vtable 0x00296C80: flags 3 & ~3 = 0 (no test, no write), function 3) at the head
    // of the scene node's list, UpdateProperties
    if (sceneNode) {
        auto* z = new ni::NiZBufferProperty;
        z->flags = 3, z->testFunction = 3;
        z->flags = u16(z->flags & 0xFC);              // AND byte +0x14, 0xFC
        sceneNode->properties.insert(sceneNode->properties.begin(), ni::NiPointer<ni::NiProperty>(z));   // AddHead
        sceneNode->UpdateProperties();
    }
    return true;                                      // (the sounds: 0x000E2FE0, from the manager's Init)
}

void Dialog::Draw() {                                 // orig: 0x000E33A0
    if (sceneNode) sceneNode->UpdateDownwardPass(time, true);   // (+0x60)->Update(+0x8C, 1) (no tests in the original)
    if (manager) manager->Update(time);               // (+0x94)->vf38(+0x8C)
    if (camera && camera->renderer) camera->renderer->ClearBuffer(nullptr, 4);   // (+0x98)+0x1A8 ->vf44(0, 4)
    if (camera) camera->UpdateDownwardPass(time, true);
    if (text) {
        text->UpdateCamera();                         // 0x00098D70
        text->Click();                                // 0x00098DB0
    }
}

// ---- the manager ---------------------------------------------------------------------------------------------------

void MemoryCards::Draw() {                            // orig: 0x000E5360
    panel.Draw();                                     // ((+4)+4)->vf08 0x000E1A00 (when shown, +9)
    dialog.Draw();                                    // (+0x10) 0x000E33A0
}

MemoryCards::MemoryCards(App& app) : app(app), repeat(12) {}   // orig: 0x000E4B30 (12 = _ftol(50.0 * 0.25))
MemoryCards::~MemoryCards() = default;

void MemoryCards::Init() {                            // orig: 0x000E5250
    // the save device new(0x58) 0x000E09C0 + vf00, the save data new(0x48) 0x000DFAC0 -> +4, 0x000E1810 (below), the
    // dialog new(0xA8) 0x000E3100 -> +0x10 with memcard\Dialogmenu.nif 0x000E3B50 (render), then:
    levelNames = app.text ? app.text->LoadDb("Language/LEVELNAMES.ldb") : nullptr;   // 0x000E1640 (0x00295014)
    panel = SavePanel{};                              // 0x000E1810: the panel new(0x90) 0x000E1E60 -> vf00 0x000E2080
    panel.Load(app);
    AddEntry(0);                                      // 0x000E1740(0)
    (void)dialog.Build(app);                          // 0x000E3B50: memcard\Dialogmenu.nif (a direct load)
    ReloadSounds();                                   // (the same loads, inline)
}

void MemoryCards::ReloadSounds() {                    // orig: 0x000E4C90
    foldDown = app.LoadSound("sounds/menu/Pause_fold_down.wav", 0);   // +0x9C (0x00293EA8)
    foldUp = app.LoadSound("sounds/menu/Pause_fold_up.wav", 0);       // +0xA0 (0x00293EC8)
    // 0x000E2FE0: the dialog's sounds
    dialog.scrollSound = app.LoadSound("sounds/menu/scroll.wav", 0);            // +0x40 (0x00293C7C)
    dialog.scrollLimitSound = app.LoadSound("sounds/menu/scrolllimit.wav", 0);  // +0x48 (0x00293C40)
    dialog.selectSound = app.LoadSound("sounds/menu/select.wav", 0);            // +0x44 (0x00293EE8)
    // 0x000E19C0: the save panel's ((+4)+4)
    panel.scrollSound = app.LoadSound("sounds/menu/scroll.wav", 0);             // +0x7C
    panel.limitSound = app.LoadSound("sounds/menu/scrolllimit.wav", 0);         // +0x84
    panel.selectSound = app.LoadSound("sounds/menu/select.wav", 0);             // +0x80
}

void MemoryCards::PlayDialogSound() {                 // orig: 0x000E1980 (the save panel's: (MC+4)+4)
    (void)app.Sound().PlaySfx(panel.selectSound, false);   // Sound_PlaySFX(+0x80, 0)
}

bool MemoryCards::SetPending(FlowState* s) {          // orig: 0x000E4CE0
    std::unique_ptr<FlowState> state(s);
    if (pending) return false;                        // (the new state is deleted)
    if (!state) return false;
    // state->vf10 (its name: no effect)
    pending = std::move(state);
    return true;
}

namespace {
// orig: 0x000E4C20: unless a flow runs, abort the pending / current states, reset the dialog (0x000E3080 /
// 0x000E3A00), the watcher's vf48, and mark a flow running.
void Begin(MemoryCards& m) {
    if (m.running) return;
    if (m.current || m.pending) {
        // 0x000DF3B0 on the pending state: deleted, and the current pointer cleared (the original leaks the current
        // state then; the port frees it); else the current one: vf10, 0x000DF3B0
        m.pending.reset();
        m.current.reset();
        // 0x000E3080 (the dialog's reset, render)
    }
    // 0x000E3A00 (the dialog's text), the watcher's vf48 (NOT PORTED: no hot plug)
    m.running = true;
}
}  // namespace

bool MemoryCards::StartLoadOptions() {                // orig: 0x000E4DF0
    Begin(*this);
    // (the watcher's vf38 polled 10 times: NOT PORTED)
    return SetPending(new OlEntryPoint(*this));
}

bool MemoryCards::StartLoadExtras() {                 // orig: 0x000E4EA0
    Begin(*this);
    return SetPending(new ElEntryPoint(*this));
}

bool MemoryCards::StartBootCheck() {                  // orig: 0x000E50C0
    Begin(*this);
    bootCheck = true;                                 // +0x98 = 1
    mode = 2;                                         // +0x70 = 2
    return SetPending(new BEntryPoint(*this));
}

bool MemoryCards::StartOptionsSave() {                // orig: 0x000E4D40
    Begin(*this);                                     // 0x000E4C20
    for (int i = 0; i < 10; ++i) Wait();              // the device's vf38 0x000E00C0, ten times
    return SetPending(new OsEntryPoint(*this));       // new(0x20) 0x000DEBC0 (SetPending inline)
}

bool MemoryCards::StartConfirm(const char* text) {    // orig: 0x000E4F50
    Begin(*this);                                     // 0x000E4C20
    mode = 2;                                         // +0x70 = 2
    return SetPending(new CbEntryPoint(*this, text)); // new(0x20) 0x000DF460(text)
}

bool MemoryCards::StartLoad() {                       // orig: 0x000E5170
    Begin(*this);                                     // 0x000E4C20
    mode = 2;                                         // +0x70 = 2
    return SetPending(new LEntryPoint(*this));        // new(0x20) 0x000DDBA0 (SetPending inline; vf10 its name)
}

bool MemoryCards::StartInGameSave() {                 // orig: 0x000E5000
    if (!cardAvailable) return false;                 // 0x000E00A0
    Begin(*this);                                     // 0x000E4C20
    mode = 2;                                         // +0x70
    return SetPending(new IsEntryPoint(*this));       // new(0x20) 0x000DCB70 (SetPending inline)
}

bool MemoryCards::RepeatInput(s32 action) const {     // orig: 0x000E4D20 -> 0x0002A0D0
    switch (action) {
    case 0: return repeat.Fired(input::kDPadUp) || repeat.StickUp(0);       // Fired(2) || 0x0009B1A0(0)
    case 1: return repeat.Fired(input::kDPadDown) || repeat.StickDown(0);   // Fired(3) || 0x0009B190(0)
    case 2: return repeat.Fired(input::kDPadLeft) || repeat.StickLeft(0);   // Fired(0) || 0x0009B170(0)
    case 3: return repeat.Fired(input::kDPadRight) || repeat.StickRight(0); // Fired(1) || 0x0009B180(0)
    case 4: return repeat.Fired(input::kButtonA) || repeat.Fired(input::kStart);   // Fired(7) || Fired(10)
    case 5: return repeat.Fired(input::kButtonB) || repeat.Fired(input::kBack);    // Fired(5) || Fired(11)
    case 6: return repeat.Fired(input::kStart);
    case 7: return repeat.Fired(input::kButtonX);
    default: return false;
    }
}

void MemoryCards::ClearEntries() {                    // orig: 0x000E18B0 (each entry 0x000E1590 + delete)
    saveEntries.clear();
    panelHead = nullptr;                              // (+4)+0x74 = 0
    panelSelection = 0;                               // (+4)+0x68 = 0
    saveCurrent = selectedSave = nullptr;             // (the port: no dangling entry pointers)
}

MemoryCards::SaveEntry* MemoryCards::InsertEntry(SaveEntry e, u8 kind) {   // orig: 0x000E1690
    e.kind = kind;                                    // +0x28
    auto pos = saveEntries.begin();
    if (pos != saveEntries.end()) {                   // after the head: past every next entry whose key is not above
        for (auto next = std::next(pos); next != saveEntries.end(); next = std::next(pos)) {
            if (next->key > e.key) {                  // (JG back to the reload of the same next: the original loops
                NotPortedFlow("memory cards: a save entry with a larger key (0x000E16CD loops forever)");
                break;
            }
            pos = next;
        }
        ++pos;
    }
    SaveEntry* p = &*saveEntries.insert(pos, std::move(e));
    panelHead = &saveEntries.front();                 // 0x000E1BF0: the panel's head (+0x74) and selection 0 (+0x68),
    panelSelection = 0;                               //  the three slots' child 0, 0x000E1B90; then 0x000E1720
    for (const auto& sw : panel.slots)
        if (sw && !sw->Children().empty()) sw->index = 0;
    PanelHighlight();                                 // 0x000E1B90
    return p;
}

void MemoryCards::AddEntry(u8 kind) {                 // orig: 0x000E1740
    SaveEntry e;                                      // new(0x2C), zeroed; +0x18 = new(9) "level0-1" (0x002A4D94)
    e.level = std::string("level0-1");
    (void)InsertEntry(std::move(e), kind == 0xFF ? saveDataFlag9 : kind);
}

MemoryCards::SaveEntry* MemoryCards::SelectedEntry() {   // orig: 0x000E17E0
    if (panel.Busy() || !panelHead) return nullptr;   // 0x000E19A0
    // the panel's +0x68 steps from its head along +0x20
    auto it = std::find_if(saveEntries.begin(), saveEntries.end(), [&](const SaveEntry& e) { return &e == panelHead; });
    for (s32 i = panelSelection; i > 0 && it != saveEntries.end(); --i) ++it;
    return it != saveEntries.end() ? &*it : nullptr;
}

bool MemoryCards::GameSaveSpace() {                   // orig: 0x000E0E00
    if (CountGameSaves() >= 0x19) return false;
    u32 need = kSaveKindSize;                         // DAT_002EDBDC
    const u64 freeBytes = Saves().FreeBytes(0x000E0E2B);   // GetDiskFreeSpaceEx("U:\")
    const u32 free32 = static_cast<u32>(freeBytes >> 32) | static_cast<u32>(freeBytes);   // (high | low)
    if (!HasSave("Futurama Extras", "Unlock.dat", nullptr)) need += kSaveKindSize;   // + DAT_002EDC00
    return free32 >= need;                            // CMP; JAE
}

void MemoryCards::ListGames() {                       // orig: 0x000E05D0
    SaveStorage& st = Saves();
    SaveStorage::FindData fd;
    const u32 h = st.FindFirst(fd, 0x000E0607);       // XFindFirstSaveGame("U:\", &data)
    if (h == SaveStorage::kInvalid) return;
    do {
        // 0x000D2C80: szSaveGameName narrowed (WStrLen + 1 characters, the low bytes) into a 512-byte buffer
        std::string narrow;
        for (char16_t c : fd.name) narrow += static_cast<char>(c);
        // REPE CMPSB over 0x11 / 0x10 bytes (with the NUL): exact, case-sensitive
        if (narrow == "Futurama Options" || narrow == "Futurama Extras") continue;
        // szSaveGameDirectory + "Futurama.dat" (0x002A4D78, 13 bytes with the NUL)
        const std::string path = fd.directory + "Futurama.dat";
        // 0x000DFB30(name, path): new(0x2C) 0x000E1560, +0xC = a wide copy, +8 = a copy, 0x000E1690(entry, +9)
        SaveEntry e;
        e.name = fd.name;
        e.path = path;
        (void)InsertEntry(std::move(e), saveDataFlag9);
    } while (st.FindNext(h, fd));                     // XFindNextSaveGame == 1
    st.FindClose(h);
    // (the save data's +4 = 0: the port's panel pointer is its own member)
}

bool MemoryCards::ReadGame(const std::string& path, std::vector<u8>& data) {   // orig: 0x000E0720
    device.readFailed = false;                        // +0x1D = 0
    data.clear();
    std::vector<u8> buf;
    const s32 r = ReadFile(path, buf);                // 0x000E02A0(path, &buf, &size)
    if (r == 3) {
        if (Verify(buf)) {                            // 0x000E03D0(buf, 0x1000)
            const u32 n = u32(buf[0]) | (u32(buf[1]) << 8) | (u32(buf[2]) << 16) | (u32(buf[3]) << 24);
            // *size = the word; new(word), REP MOVSD / MOVSB from +4 (the port bounds the copy by the buffer: a
            // UB-avoiding divergence for a word above 0xFFC, never written by the game)
            const size_t k = std::min<size_t>(n, buf.size() - 4);
            data.assign(buf.begin() + 4, buf.begin() + 4 + std::ptrdiff_t(k));
        } else {
            device.readFailed = true;
        }
    } else {
        device.readFailed = true;                     // (0, 1, 2: the file is missing, short or unreadable)
    }
    // (the 0x1000 buffer freed)
    return r == 3 && !device.readFailed;
}

bool MemoryCards::DeleteGame(const std::u16string& name) {   // orig: 0x000E07D0
    return Saves().DeleteSaveGame(name) == 0;         // XDeleteSaveGame("U:\", name): NEG; SBB; INC
}

void MemoryCards::RemoveEntry(SaveEntry* e) {         // orig: 0x000E1880
    if (!e) return;
    // 0x000E1590: the head moved on when it is the entry, its strings freed, unlinked; delete; the count - 1
    auto it = std::find_if(saveEntries.begin(), saveEntries.end(), [&](const SaveEntry& x) { return &x == e; });
    if (it == saveEntries.end()) return;
    if (panelHead == e) panelHead = nullptr;          // (the port: no dangling entry pointers)
    if (saveCurrent == e) saveCurrent = nullptr;
    if (selectedSave == e) selectedSave = nullptr;
    saveEntries.erase(it);
}

void MemoryCards::Stop() {                            // orig: 0x000E5210
    // the device's vf4C (NOT PORTED: hot plug); while the dialog is shown, 0x000E3A00 (its text: render)
    running = false;
}

void MemoryCards::Abort() {                           // orig: 0x000E4BE0
    pending.reset();                                  // (0x000DF3B0: the current pointer cleared too)
    current.reset();                                  // (vf10, 0x000DF3B0)
    // 0x000E3080 (the dialog's reset: render)
}

void MemoryCards::Step() {                            // orig: 0x000E5380
    dialog.Step(app);                                 // 0x000E4A00
    saveDataTime = F(double(g_frameStep) + double(saveDataTime));   // 0x000E16F0
    panel.Step();                                     // (its +4)->vf04 0x000E2A70
    repeat.Update(app.input);                         // InputRepeat::Update 0x0009B3D0
    if (!running) return;
    Wait();                                           // the device's vf38 0x000E00C0
    if (current) {
        current->Step();                              // 0x000DF3D0
        dialog.Step(app);
        return;
    }
    if (!pending && !dialog.open) {
        Stop();
        running = false;
        return;
    }
    current = std::move(pending);                     // +0x18 = +0x14 (vf10: its name)
    dialog.Step(app);
}

SaveStorage& MemoryCards::Saves() const {
    if (app.services.saves) return *app.services.saves;
    if (!ownSaves_) const_cast<MemoryCards*>(this)->ownSaves_ = std::make_unique<MemorySaveStorage>();
    return *ownSaves_;
}

// ---- the save device (orig: 0x000E09C0's class, vtable 0x002A4E50) -------------------------------------------------

bool MemoryCards::HasSave(const char* name, const char* file, std::string* path) {   // orig: 0x000E00D0
    SaveStorage& st = Saves();
    SaveStorage::FindData fd;
    const u32 h = st.FindFirst(fd, 0x000E00F1);       // XFindFirstSaveGame("U:\")
    if (h == SaveStorage::kInvalid) return false;
    std::u16string wide;                              // 0x000D2EB0 (each byte sign-extended)
    for (const char* c = name; *c; ++c) wide += static_cast<char16_t>(static_cast<s16>(static_cast<signed char>(*c)));
    bool found = false;
    for (;;) {
        if (fd.name == wide) {                        // wcscmp 0x00167E26
            if (path) *path = fd.directory + (file ? file : "");   // (the old string freed, a heap copy)
            found = true;
            break;
        }
        if (!st.FindNext(h, fd)) break;
    }
    // SetLastError(0)
    st.FindClose(h);
    return found;
}

s32 MemoryCards::CountGameSaves() {                   // orig: 0x000E0860
    SaveStorage& st = Saves();
    SaveStorage::FindData fd;
    const u32 h = st.FindFirst(fd, 0x000E087C);
    if (h == SaveStorage::kInvalid) return 0;
    s32 n = 0;
    do {                                              // every save game whose directory holds a Futurama.dat
        const u32 f = st.CreateFile(fd.directory + "Futurama.dat", 0x80000000u, 3);   // GENERIC_READ, OPEN_EXISTING
        if (f != SaveStorage::kInvalid) {
            st.CloseHandle(f);
            ++n;
        }
    } while (st.FindNext(h, fd));
    st.FindClose(h);
    return n;
}

bool MemoryCards::EnoughSpace(u32* missing, u32* needed) {   // orig: 0x000E0CF0
    const bool haveExtras = HasSave("Futurama Extras", "Unlock.dat", nullptr);
    const bool haveOptions = HasSave("Futurama Options", "Options.dat", nullptr);
    const s32 games = CountGameSaves();
    const u64 freeBytes = Saves().FreeBytes(0x000E0D3D);
    // OR of the two halves of the 64-bit byte count (EDX = high | low)
    const u32 free32 = static_cast<u32>(freeBytes >> 32) | static_cast<u32>(freeBytes);
    u32 need;
    if (!haveExtras && !haveOptions && games <= 0) {
        need = kAllSavesSize;                         // DAT_002EDC28
    } else {
        need = haveOptions ? 0u : kSaveKindSize;      // DAT_002EDBE0
        if (!haveExtras) need += kSaveKindSize;       // DAT_002EDC00
        if (games >= 0 && games < 0x19) need += kSaveKindSize;   // DAT_002EDBDC
    }
    if (free32 >= need) return true;                  // CMP; JAE
    if (missing) *missing = need - free32;
    if (needed) *needed = need;
    return false;
}

bool MemoryCards::ReadOptions() {                     // orig: vf34 0x000E1010
    device.status = 2;                                // +4
    std::string path;
    bool ok = false;
    if (!HasSave("Futurama Options", "Options.dat", &path)) {   // 0x000E00D0(0x002A4E34, 0x002A4EA4, &path)
        device.status = 1;
        return false;
    }
    std::vector<u8> buf;
    const s32 r = ReadFile(path, buf);                // 0x000E02A0
    if (r == 3) {
        if (Verify(buf)) {                            // 0x000E03D0(buf, 0x1000)
            const u32 n = u32(buf[0]) | (u32(buf[1]) << 8) | (u32(buf[2]) << 16) | (u32(buf[3]) << 24);
            // new(word), the word's bytes from +4 (bounded by the buffer: UB-avoiding), 0x000A2070 on App+0xA8
            std::vector<u8> data(buf.begin() + 4, buf.begin() + 4 + std::ptrdiff_t(std::min<size_t>(n, buf.size() - 4)));
            data.resize(std::max<size_t>(data.size(), SerializeOptions(app, nullptr)));   // (its fixed 0x214 reads)
            ok = DeserializeOptions(app, data.data());
            // (+0x34 = 0)
        } else {
            device.readFailed = true;                 // +0x1D
        }
    } else if (r == 0) {
        device.readFailed = true;                     // (CMP EAX, ESI: only the size failure marks it)
    }
    device.status = ok ? 0 : 1;
    return ok;
}

void MemoryCards::Wait() {                            // orig: 0x000E00C0: SleepEx(0, TRUE)
    SaveStorage::Completion c;
    if (!Saves().TakeCompletion(c)) return;           // (nothing queued: the wait returns at once)
    if (c.routine == 0x000E0230) WriteCompleted(c.error);   // (the only routine the game passes)
}

void MemoryCards::WriteCompleted(u32 error) {         // orig: 0x000E0230(error, bytes, overlapped)
    device.status = error != 0 ? 1 : 0;               // (manager +0xC)+4 = (error != 0)
    SaveStorage& st = Saves();
    (void)st.GetFileSize(device.file);                // (the result unused)
    st.CloseHandle(device.file);
    device.file = SaveStorage::kInvalid;
    device.buffer.clear();                            // +0x2C and +0x28 freed
}

bool MemoryCards::Sign(std::vector<u8>& buf) {        // orig: 0x000E0440(buf, size)
    SaveStorage& st = Saves();
    const u32 h = st.SignatureBegin();                // XCalculateSignatureBegin(0)
    if (h == SaveStorage::kInvalid) return false;     // (GetLastError)
    const u32 n = static_cast<u32>(buf.size()) - 0x14;
    if (!st.SignatureUpdate(h, buf.data(), n)) return false;   // (a failed update ends without End)
    std::array<u8, 20> digest{};                      // new(0x14)
    if (!st.SignatureEnd(h, digest)) return false;    // (the digest's pointer tested, not End's result)
    std::copy(digest.begin(), digest.end(), buf.begin() + n);   // REP MOVSD x5 at buf + size - 0x14
    return true;
}

bool MemoryCards::Verify(const std::vector<u8>& buf) {   // orig: 0x000E03D0(buf, size)
    SaveStorage& st = Saves();
    const u32 n = static_cast<u32>(buf.size()) - 0x14;
    const u32 h = st.SignatureBegin();
    std::array<u8, 20> digest{};
    // (a failed Begin / Update compares the stored signature with a null pointer in the original: the port fails)
    if (h == SaveStorage::kInvalid || !st.SignatureUpdate(h, buf.data(), n) || !st.SignatureEnd(h, digest))
        return false;
    return std::equal(digest.begin(), digest.end(), buf.begin() + n);   // REPE CMPSD x5
}

bool MemoryCards::CopyIcon(const std::u16string& name, s32 kind) {   // orig: 0x000E04B0(name, kind)
    SaveStorage& st = Saves();
    std::string path;
    if (st.CreateSaveGame(name, 3, 0, path, 0x000E04D3) != 0) return false;   // OPEN_EXISTING
    const char* source = kind == 0 ? "d:\\SaveGameIcons\\saveimage.xbx"            // 0x002A4E00
                         : kind == 1 ? "d:\\SaveGameIcons\\options_saveimage.xbx"  // 0x002A4DD8
                         : kind == 2 ? "d:\\SaveGameIcons\\extras_saveimage.xbx"   // 0x002A4DB0
                                     : nullptr;
    if (!source) return false;
    path += "saveimage.xbx";                          // 0x002A4DA0
    (void)st.SetFileAttributes(path, 0x80);           // FILE_ATTRIBUTE_NORMAL (the result unused)
    if (!st.CopyFile(source, path)) return false;     // CopyFileA(source, path, FALSE) (GetLastError)
    return st.SetFileAttributes(path, 0x80);
}

bool MemoryCards::WriteFile(const char* file, const std::u16string* name, s32 kind) {   // orig: 0x000E0B90
    SaveStorage& st = Saves();
    if (device.file != SaveStorage::kInvalid) st.CloseHandle(device.file);
    const std::u16string& saveAs = name ? *name : saveName;   // null: the save data's +0x40
    std::string path;
    bool ok = false;
    if (st.CreateSaveGame(saveAs, 4, 0, path, 0x000E0C03) == 0) {   // XCreateSaveGame OPEN_ALWAYS
        path += file;
        // CreateFile(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0x60000080, 0), GetLastError
        device.file = st.CreateFile(path, 0x40000000u, 2);
        if (device.file == SaveStorage::kInvalid) {
            st.CloseHandle(device.file);              // (CloseHandle(INVALID_HANDLE_VALUE): the XAPI's)
        } else {
            // GetLastError; the OVERLAPPED's offsets / event 0; SetLastError(0)
            ok = st.WriteFileEx(device.file, device.buffer.data(), 0x1000, 0x000E0230) && CopyIcon(saveAs, kind);
            if (!ok) {                                // (GetLastError)
                st.CloseHandle(device.file);
                device.file = SaveStorage::kInvalid;
            }
        }
    }
    if (ok) return true;
    device.status = 1;
    ReleaseSave();                                    // 0x000E07F0
    return false;
}

s32 MemoryCards::ReadFile(const std::string& path, std::vector<u8>& buf) {   // orig: 0x000E02A0(path, &buf, &size)
    SaveStorage& st = Saves();
    if (device.file != SaveStorage::kInvalid) {
        st.CloseHandle(device.file);
        device.file = SaveStorage::kInvalid;
    }
    buf.clear();
    // CreateFile(path, GENERIC_READ, 0, 0, OPEN_EXISTING, 0x20000080, 0), GetLastError
    device.file = st.CreateFile(path, 0x80000000u, 3);
    if (device.file == SaveStorage::kInvalid) {
        device.status = 1;
        return 2;
    }
    if (st.GetFileSize(device.file) != 0x1000) {
        device.status = 1;
        st.CloseHandle(device.file);
        device.file = SaveStorage::kInvalid;
        return 0;
    }
    // SetLastError(0); new(0x1000)
    const bool ok = st.ReadFile(device.file, 0x1000, buf);
    st.CloseHandle(device.file);
    device.file = SaveStorage::kInvalid;
    if (!ok) {                                        // (GetLastError; the buffer freed)
        buf.clear();
        device.status = 1;
        return 1;
    }
    buf.resize(0x1000);                               // (a short read: the rest of the new(0x1000) buffer, port 0)
    return 3;
}

void MemoryCards::ReleaseSave() {                     // orig: 0x000E07F0
    device.buffer.clear();                            // the device's +0x28
    saveData.clear();                                 // (+4)+0x20, +0x24 = 0
    saveFile.reset();                                 // (+4)+0x1C
    saveName.clear();                                 // (+4)+0x40
}

bool MemoryCards::WriteGameSave() {                   // orig: vf40 0x000E1420
    if (!saveDataFlag8) {                             // (+4)+8
        std::vector<std::u16string> names;            // (a local list, vtable 0x002A4E4C)
        ListSaveNames(names);                         // 0x000E1300
        BuildGameSave(names);                         // (+4)->0x000DFEE0
    }
    device.buffer.assign(0x1000, 0);                  // new(0x1000), REP STOSD
    const u32 n = static_cast<u32>(saveData.size());
    for (int i = 0; i < 4; ++i) device.buffer[size_t(i)] = u8(n >> (8 * i));   // the size word
    // the data at +4 (REP MOVS of the size: the port stops at the signature, a UB-avoiding bound)
    std::copy_n(saveData.begin(), std::min<size_t>(n, 0x1000 - 0x14 - 4), device.buffer.begin() + 4);
    if (!Sign(device.buffer)) {                       // 0x000E0440(buf, 0x1000)
        ReleaseSave();                                // 0x000E07F0
        device.status = 1;
        return false;
    }
    return WriteFile("Futurama.dat", nullptr, 0);     // 0x000E0B90(0x002A4D78, 0, 0)
}

bool MemoryCards::OptionsSpace(u32* missing, u32* needed) {   // orig: 0x000E0E90
    if (HasSave("Futurama Options", "Options.dat", nullptr)) return true;   // 0x000E00D0(0x002A4E34, 0x002A4EA4, 0)
    // GetDiskFreeSpaceExA(0x002A4E20, &free, 0, 0): its low | high dword (-1 on a failure: GetLastError)
    const u64 freeBytes = Saves().FreeBytes(0x000E0EBB);
    const u32 free32 = static_cast<u32>(freeBytes >> 32) | static_cast<u32>(freeBytes);
    const u32 kOptionsSize = kSaveKindSize;           // DAT_002EDBE0
    if (free32 >= kOptionsSize) return true;          // CMP; JAE
    if (missing) *missing = kOptionsSize - free32;
    if (needed) *needed = kOptionsSize;
    return false;
}

bool MemoryCards::WriteOptions() {                    // orig: vf30 0x000E0F30
    std::vector<u8> data;
    const u32 n = SerializeOptions(app, &data);       // App+0xA8 0x000A18F0 (the block is not freed)
    device.status = 2;                                // +4
    if (data.empty() || n > 0x1000) {                 // (null data, size < 0 or > 0x1000)
        device.status = 1;
        return false;
    }
    device.buffer.assign(0x1000, 0);                  // (+0x28 freed, new(0x1000), REP STOSD)
    for (int i = 0; i < 4; ++i) device.buffer[size_t(i)] = u8(n >> (8 * i));   // the size word
    std::copy(data.begin(), data.begin() + n, device.buffer.begin() + 4);   // REP MOVSD / MOVSB at +4
    if (!Sign(device.buffer)) {                       // 0x000E0440(buf, 0x1000)
        ReleaseSave();                                // 0x000E07F0
        device.status = 1;
        return false;
    }
    const std::u16string name = u"Futurama Options";  // 0x000D2EB0(0x002A4E34) (freed after)
    return WriteFile("Options.dat", &name, 1);        // 0x000E0B90(0x002A4EA4, name, 1)
}

bool MemoryCards::WriteExtras(const LevelRecords& r) {   // orig: 0x000E1110
    const u32 n = 0x78;                               // 0x000F3AA0: 24 x 5 bytes
    device.status = 2;
    std::vector<u8> packed;                           // new(n); 0x000F3AB0: {u8 flag, s32 best time} each
    for (const LevelRecords::Record& rec : r.records) {
        packed.push_back(rec.allNibblers);
        for (int i = 0; i < 4; ++i) packed.push_back(u8(u32(rec.bestTime) >> (8 * i)));
    }
    device.buffer.assign(0x1000, 0);
    for (int i = 0; i < 4; ++i) device.buffer[size_t(i)] = u8(n >> (8 * i));
    std::copy(packed.begin(), packed.end(), device.buffer.begin() + 4);   // (the packed copy leaks: as written)
    if (!Sign(device.buffer)) {
        ReleaseSave();
        device.status = 1;
        return false;
    }
    const std::u16string name = u"Futurama Extras";   // 0x000D2EB0(0x002A4E24) (freed after)
    return WriteFile("Unlock.dat", &name, 2);         // 0x000E0B90(0x002A4EB0, name, 2)
}

bool MemoryCards::ReadExtras(LevelRecords& r) {       // orig: 0x000E11F0
    device.status = 2;
    std::string path;
    bool ok = false;
    if (HasSave("Futurama Extras", "Unlock.dat", &path)) {   // 0x000E00D0 (the path out)
        std::vector<u8> buf;
        const s32 res = ReadFile(path, buf);          // 0x000E02A0
        if (res == 3) {
            if (Verify(buf)) {                        // 0x000E03D0(buf, 0x1000)
                const u32 n = u32(buf[0]) | (u32(buf[1]) << 8) | (u32(buf[2]) << 16) | (u32(buf[3]) << 24);
                std::vector<u8> data(buf.begin() + 4, buf.begin() + 4 + std::min<u32>(n, 0x1000 - 4));
                data.resize(std::max<size_t>(data.size(), 0x78));   // (0x000F3AE0 reads 0x78 bytes: UB-avoiding)
                for (size_t i = 0; i < 24; ++i) {     // 0x000F3AE0: returns the bytes read (0x78)
                    r.records[i].allNibblers = data[i * 5];
                    r.records[i].bestTime = s32(u32(data[i * 5 + 1]) | (u32(data[i * 5 + 2]) << 8) |
                                                (u32(data[i * 5 + 3]) << 16) | (u32(data[i * 5 + 4]) << 24));
                }
                ok = n == 0x78;
                // (the device's +0x34 = 0)
                if (ok) {
                    device.status = 0;
                    return true;
                }
                device.status = 1;
                return false;
            }
            device.readFailed = true;                 // +0x1D (a bad signature)
        } else if (res == 0) {
            device.readFailed = true;                 // (not 0x1000 bytes)
        }
    }
    device.status = 1;
    return ok;
}

void MemoryCards::ListSaveNames(std::vector<std::u16string>& names) {   // orig: 0x000E1300
    SaveStorage& st = Saves();
    SaveStorage::FindData fd;
    const u32 h = st.FindFirst(fd, 0x000E1321);
    if (h == SaveStorage::kInvalid) return;
    do {
        std::string narrow;                           // 0x000D2C80 (the low bytes)
        for (char16_t c : fd.name) narrow += static_cast<char>(c);
        if (narrow == "Futurama Options" || narrow == "Futurama Extras") continue;   // REPE CMPSB 0x11 / 0x10
        names.push_back(fd.name);                     // a wide copy into the list (grown by its +0x14)
    } while (st.FindNext(h, fd));
    st.FindClose(h);
}

void MemoryCards::BuildGameSave(const std::vector<std::u16string>& names) {   // orig: 0x000DFEE0
    if (selectedSave)                                 // +0xC: REPE CMPSB of 9 bytes with "level0-1"
        saveFlag44 = !selectedSave->level || *selectedSave->level != "level0-1";
    SaveEntry e;                                      // new(0x2C) 0x000E1560
    std::string text;
    const LoadedLevel* lvl = app.ingame ? app.ingame->Level() : nullptr;
    if (!lvl || !lvl->session) {                      // (g_level: the original calls through it)
        NotPortedFlow("memory cards: the in-game save without a level (0x000C2770 on null)");
    } else {
        (void)lvl->session->SaveText(&text, app.LevelName());   // g_level->0x000C2770(&out)
    }
    e.data.assign(text.begin(), text.end());          // +0x10 / +0x14
    // +0x1C / +0x18: 8 characters after "Level=" and a NUL (each a new(9) copy)
    std::string level = text.size() > 6 ? text.substr(6, 8) : std::string();
    if (const size_t z = level.find('\0'); z != std::string::npos) level.resize(z);
    e.level2 = level;
    e.level = level;
    saveCurrent = InsertEntry(std::move(e), saveDataFlag9);   // 0x000E1690(entry, +9); +0x30 = entry
    saveData.assign(text.begin(), text.end());        // +0x20 / +0x24 (the old one freed)
    saveFile = std::string("Futurama.dat");           // +0x1C = vf10 0x000DFB00 (the old one freed)
    for (s32 n = 1;; ++n) {                           // the first name no listed save has
        saveName = SaveName(n);                       // +0x40 = 0x000DFC10(n) (the old one freed)
        bool taken = false;                           // 0x000DFE70: WStrLen, then REPE CMPSB over 2 * len
        for (const std::u16string& o : names)
            if (o == saveName) taken = true;
        if (!taken) break;
    }
}

std::u16string MemoryCards::SaveName(s32 n) const {  // orig: 0x000DFC10
    // the level's title: TextManager::GetText((+0x30)+0x1C, all databases)
    const std::string id = saveCurrent ? saveCurrent->level2 : std::string();
    std::u16string name = app.text ? app.text->GetText(id, nullptr) : u"(Not translated)";
    name += u' ';
    const LoadedLevel* lvl = app.ingame ? app.ingame->Level() : nullptr;
    const Session* S = lvl ? lvl->session.get() : nullptr;
    auto add = [&](const std::string& s) {            // 0x000D2CB0 (narrow to wide)
        for (char c : s) name += static_cast<char16_t>(static_cast<unsigned char>(c));
    };
    // with (+4)+8 each number is "00" (0x002A4D90)
    add(saveDataFlag8 || !S ? "00" : std::to_string(S->playHours));   // _itoa(S+0x17C, 10)
    name += u"h ";
    // _itoa(_ftol(S+0x178) / 60): the magic division of the truncated seconds
    add(saveDataFlag8 || !S ? "00" : std::to_string(static_cast<s32>(static_cast<s64>(double(S->playSeconds))) / 60));
    name += u"m ";
    // _itoa(_ftol(S+0x178 + [0x00293AA0] 0.5) % 60) (the extended sum truncated; IDIV's remainder)
    add(saveDataFlag8 || !S ? "00"
                            : std::to_string(static_cast<s32>(static_cast<s64>(double(S->playSeconds) + 0.5f)) % 60));
    name += u's';
    if (n > 1) {                                      // sprintf(" (%d)", n) (0x002A4D88)
        char b[16];
        std::snprintf(b, sizeof b, " (%d)", static_cast<int>(n));
        add(b);
    }
    return name;
}

}  // namespace fut::game::app
