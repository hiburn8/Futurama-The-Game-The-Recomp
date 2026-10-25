// The save-slot list panel; see LoadPanel.h.
#include "game/app/LoadPanel.h"

#include "engine/ni/NiMath.h"
#include "game/GameTime.h"
#include "game/app/App.h"
#include "game/app/MenuScene.h"
#include "game/app/PauseMenu.h"
#include "game/text/LanguageDb.h"
#include "game/text/TextObject.h"

#include <bit>
#include <cmath>
#include <cstdio>

namespace fut::game::app {

using ni::detail::F;

// ---- the list model -------------------------------------------------------------------------------------------------

SaveList::SaveList(App& app) {                        // orig: 0x00029A00
    // +0 / +4..+0x10 = 0; +0x18 = TextManager::Get()->LoadDb("Language/LEVELNAMES.ldb") (0x00295014)
    levelNames = app.text ? app.text->LoadDb("Language/LEVELNAMES.ldb") : nullptr;
}

SaveList::~SaveList() { Clear(); }

void SaveList::Append(const std::u16string& name, const std::string& path, MemoryCards::SaveEntry* e) {   // 0x00029A40
    Entry* n = new Entry;                             // new(0x20), zeroed
    n->name = name;                                   // +0 (WStrLen, new, WStrCpy)
    n->path = path;                                   // +8 (strlen + 1, new, copy)
    n->saveEntry = e;                                 // +0x14
    if (!head) {
        head = n;
        return;
    }
    Entry* last = head;                               // after the last entry (+0x18 until null)
    while (last->next) last = last->next;
    n->prev = last;                                   // +0x1C
    last->next = n;
}

bool SaveList::Down() {                               // orig: 0x00029B00
    if (cursor == 3) {
        if (!rows[3] || !rows[3]->next) return false;
        for (Entry*& r : rows)                        // (each row that is set moves to its next)
            if (r) r = r->next;
        return true;
    }
    if (cursor > 3) return false;                     // (JGE: above 3)
    if (!rows[size_t(cursor) + 1]) return false;
    ++cursor;
    return true;
}

bool SaveList::Up() {                                 // orig: 0x00029B70
    if (cursor != 0) {
        if (cursor < 0) return false;                 // (JLE)
        --cursor;
        return true;
    }
    if (!rows[0] || !rows[0]->prev) return false;
    for (Entry*& r : rows)
        if (r) r = r->prev;
    return true;
}

void SaveList::Reset() {                              // orig: 0x00029BE0
    cursor = 0;
    rows = {};
    if (!head) return;
    rows[0] = head;
    if (!head->next) return;
    rows[1] = head->next;
    if (!head->next->next) return;
    rows[2] = head->next->next;
    if (!head->next->next->next) return;
    rows[3] = head->next->next->next;
}

void SaveList::Remove(Entry* e) {                     // orig: 0x00029C70
    if (head == e) {                                  // only the head is unlinked in the original
        head = e->next;
        if (head) head->prev = nullptr;
        e->next = e->prev = nullptr;
    } else if (e) {
        // (the original deletes an entry other than the head without unlinking it: its neighbours keep pointing at
        // the freed entry. UB-avoiding divergence: the port unlinks it; no oracle deletes a non-head entry.)
        if (e->prev) e->prev->next = e->next;
        if (e->next) e->next->prev = e->prev;
        e->next = e->prev = nullptr;
    }
    delete e;                                         // 0x000299A0 (its strings) + delete
    cursor = 0;                                       // +0x14 = 0, the rows from the head again
    rows = {};
    if (!head) return;
    rows[0] = head;
    if (!head->next) return;
    rows[1] = head->next;
    if (!head->next->next) return;
    rows[2] = head->next->next;
    if (!head->next->next->next) return;
    rows[3] = head->next->next->next;
}

void SaveList::Clear() {                              // orig: 0x00029D00 (the cursor is kept)
    for (Entry* e = head; e;) {
        Entry* next = e->next;
        delete e;                                     // (+0xC, +4, +0, +8 freed, unlinked, deleted)
        e = next;
    }
    rows = {};
    head = nullptr;
}

void SaveList::Fill(App& app) {                       // orig: 0x00029D90
    if (!app.useMemoryCardFlows) {                    // App+0xF8 == 0: the 22-level preset list (dead: main sets 1)
        std::fprintf(stderr, "NOT PORTED reached: the save list's level presets (0x00029D90, App+0xF8 == 0)\n");
        Reset();
        return;
    }
    s32 n = 0;
    for (MemoryCards::SaveEntry& e : app.memoryCards->saveEntries) {   // ((App+0xF4)+4)+0x10 along +0x20
        Append(e.name, e.path, &e);                   // 0x00029A40(+0xC, +8, entry)
        ++n;
    }
    if (n == 0) {                                     // one entry with the "nolevels" text as its name (+4 = 0)
        Entry* x = new Entry;
        const char16_t* s = app.text ? app.text->GetText("nolevels", levelNames) : nullptr;   // 0x0029502C
        x->name = s ? s : u"";
        x->next = head;                               // +0x18 = the old head (null), the new head
        head = x;
    }
    Reset();                                          // 0x00029BE0
}

// ---- the panel base -------------------------------------------------------------------------------------------------

void LoadPanel::SetIndex(ni::NiSwitchNode* s, s32 index) {
    if (!s) return;                                   // (the original writes without a test)
    if (s32(s->Children().size()) > index) s->index = index;   // CMP +0xCC; JLE
}

LoadPanel::LoadPanel(App& a, s32 n, text::TextObject* t) : app(a), arg(n), text(t), list(a) {   // orig: 0x0001AC30
    // 0x000298A0(arg) (the PanelBase), the rows / arrows / text nodes cleared, +0x3C = 0, +0x48 = the text object, the
    // list model new(0x1C) 0x00029A00 -> +0x68; App+0xF8 == 0: its 0x00029D90 (dead); +0x20 = 0
    if (!app.useMemoryCardFlows) list.Fill(app);
}

LoadPanel::~LoadPanel() {                             // orig: 0x0001A9C0
    list.Clear();                                     // (+0x68: 0x00029D00 and delete)
    RemoveNames();                                    // 0x0001A900
    for (size_t i = 0; i < rows.size(); ++i) rows[i] = nullptr, nameNodes[i] = nullptr;
    upArrow = nullptr, downArrow = nullptr;           // +0x34 / +0x38
    sound::GameSound& snd = app.Sound();              // Sound_Release +0x5C, +0x60, +0x64
    snd.Release(scrollSound);
    snd.Release(selectSound);
    snd.Release(limitSound);
}

void LoadPanel::Reset() {                             // orig: 0x0001A890
    scrollSound = app.LoadSound("sounds/menu/scroll.wav", 0);          // +0x5C (0x00293C7C)
    selectSound = app.LoadSound("sounds/menu/selectcontinue.wav", 0);  // +0x60 (0x00293C5C)
    limitSound = app.LoadSound("sounds/menu/scrolllimit.wav", 0);      // +0x64 (0x00293C40)
}

void LoadPanel::ReleaseSounds() {                     // orig: 0x0001A8D0
    sound::GameSound& snd = app.Sound();
    snd.Release(scrollSound);
    snd.Release(selectSound);
    snd.Release(limitSound);
}

void LoadPanel::BuildRows(ni::NiNode* scene) {        // orig: 0x0001AD60(scene)
    static const char* const kRows[4] = {"LM_saveslot1", "LM_saveslot2", "LM_saveslot3", "LM_saveslot4"};
    static const char* const kTexts[4] = {"loadgame_text1", "loadgame_text2", "loadgame_text3", "loadgame_text4"};
    for (size_t i = 0; i < rows.size(); ++i) rows[i] = FindAs<ni::NiSwitchNode>(scene, kRows[i]);   // (RTTI 0x002F9820)
    scroll = 0;                                       // +0x3C
    upArrow = FindAs<ni::NiSwitchNode>(scene, "LM_uparrownull");     // +0x34
    downArrow = FindAs<ni::NiSwitchNode>(scene, "LM_downarrownull"); // +0x38
    for (size_t i = 0; i < nameNodes.size(); ++i) nameNodes[i] = FindAs<ni::NiNode>(scene, kTexts[i]);   // (0x002F9230)
    const f32 one = F(std::fabs(1.0));                // FLD QWORD [0x00293CC0]; FABS; FSTP
    for (size_t i = 0; i < nameNodes.size(); ++i) {
        nameIds[i] = 0;                               // +0x40 [i]
        ni::NiNode* n = nameNodes[i].Get();
        if (!n) continue;                             // (the original writes without a test)
        n->local.scale = one;                         // +0x6C
        auto* a = new ni::NiAlphaProperty;            // new(0x1C) (vtable 0x00293B54): flags 0xEC, +0x18 = 0
        a->flags = u16(a->flags | 1);
        n->properties.insert(n->properties.begin(), ni::NiPointer<ni::NiProperty>(a));   // AddHead 0x00013A50
        n->UpdateProperties();
        n->UpdateEffects();
        n->UpdateDownwardPass(0.0f, true);            // Update(0, 1)
    }
    AddNames();                                       // 0x0001AB80
    UpdateArrows();                                   // 0x0001AB10
    Reset();                                          // vf0C
    // (+0x6C = 0: a byte without a reader found)
}

void LoadPanel::RemoveNames() {                       // orig: 0x0001A900
    for (u16& id : nameIds) {
        if (!id) continue;
        if (text) text->RemoveString(id);             // 0x00098FD0
        id = 0;
    }
    if (text) text->UpdateCamera();                   // 0x00098D70
}

void LoadPanel::AddNames() {                          // orig: 0x0001AB80
    for (size_t i = 0; i < nameNodes.size(); ++i) {
        if (const SaveList::Entry* e = list.rows[i]; e && text)   // (+0x68)+4 [i]
            nameIds[i] = text->AddStringAt(e->name, nameNodes[i].Get(), -1.0f, -1.0f);   // 0x0009A0A0(name, node, -1, -1)
        if (ni::NiNode* n = nameNodes[i].Get()) {     // (no test in the original)
            n->UpdateDownwardPass(0.0f, true);        // Update(0, 1)
            n->UpdateProperties();
            n->UpdateEffects();
        }
    }
}

void LoadPanel::UpdateArrows() {                      // orig: 0x0001AB10
    // the down arrow (+0x38) on while the list scrolls down (0x00029C30), the up arrow (+0x34) while it scrolls up
    SetIndex(downArrow.Get(), list.CanScrollDown() ? 1 : 0);
    SetIndex(upArrow.Get(), list.CanScrollUp() ? 1 : 0);
}

void LoadPanel::Redraw() {                            // orig: 0x0001AC10
    RemoveNames();
    AddNames();
    UpdateArrows();
}

void LoadPanel::ResetList() {                         // orig: 0x0001B2F0
    SetIndex(rows[0].Get(), 1);                       // row 0 highlighted, the others not
    for (size_t i = 1; i < rows.size(); ++i) SetIndex(rows[i].Get(), 0);
    selection = 0;                                    // +8
    scroll = 0;                                       // +0x3C
    Redraw();
}

void LoadPanel::Up() {                                // orig: 0x0001B360
    if (!list.Up()) {                                 // 0x00029B70
        (void)app.Sound().PlaySfx(limitSound, false); // Sound_PlaySFX(+0x64, 0)
        delay = 10;
        return;
    }
    (void)app.Sound().PlaySfx(scrollSound, false);    // +0x5C
    if (selection > 0) {
        SetIndex(rows[size_t(selection)].Get(), 0);   // the old row off, the new one on
        --selection;
        SetIndex(rows[size_t(selection)].Get(), 1);
    }
    delay = 10;
    Redraw();
}

void LoadPanel::Down() {                              // orig: 0x0001B3F0
    if (!list.Down()) {                               // 0x00029B00
        (void)app.Sound().PlaySfx(limitSound, false);
        delay = 10;
        return;
    }
    (void)app.Sound().PlaySfx(scrollSound, false);
    if (selection < 3) {
        SetIndex(rows[size_t(selection)].Get(), 0);
        ++selection;
        SetIndex(rows[size_t(selection)].Get(), 1);
    }
    delay = 10;
    Redraw();
}

void LoadPanel::ApplyLoaded() {                       // orig: 0x0001A940 (the dead App+0xF8 == 0 path's preset load)
    // rows[cursor]'s +0xC (a preset text) and +0x10 (its size) are 0 for every memory-card entry: nothing happens.
    // (With them set: DAT_002EAA38 = 1 (0x000C1280), App+0xAC = the text's "Level=" value, the carry text = the
    // preset; not ported: no entry of the port has a preset.)
}

bool LoadPanel::Update(f32 dt) {                      // orig: vf04 0x0001B490
    if (opening) {
        t = F(double(dt) + t);                        // FLD dt; FADD +0x18; FSTP
        Show();                                       // vf18
    }
    if (closing) {
        t = F(double(dt) + t);
        Hide();                                       // vf1C
    }
    if (delay != 0) {
        --delay;
        return true;
    }
    if (!active) return true;
    auto menuMode = [] { return g_menuInputMode == 0 || g_menuInputMode == 3; };   // 0x00029F80 (0 or 3)
    if (MenuInput(app, 5)) {                          // back
        if (!menuMode()) return true;                 // (three more 0x00029F80 calls: no effect)
        if (Busy()) {                                 // 0x00029880
            delay = 10;
            return true;
        }
        Hide();                                       // vf1C (the hint no-op)
        return true;
    }
    if (MenuInput(app, 0)) {                          // up
        if (!menuMode()) return true;
        Up();
    }
    if (MenuInput(app, 1)) {                          // down
        if (!menuMode()) return true;
        Down();
    }
    if (!MenuInput(app, 4)) return true;              // accept
    if (!menuMode()) return true;
    if (Busy()) {
        delay = 10;
        return true;
    }
    if (list.head && list.head->preset == 0) return true;   // (+0x68)->head +4: 0 for a memory-card entry
    (void)app.Sound().PlaySfx(selectSound, false);    // +0x60
    delay = 10;
    // (the hint no-op)
    gameLoaded = true;                                // +0x20
    Hide();                                           // vf1C
    return false;
}

// ---- the front end's load panel ------------------------------------------------------------------------------------

FrontendLoadPanel::FrontendLoadPanel(App& a, FrontendContext& f, text::TextObject* t)   // orig: 0x0001FBE0(3, fe, t)
    : LoadPanel(a, 3, t), fe(f) {
    selection = 0;                                    // +8
    goToSound = app.LoadSound("sounds/menu/go_to.wav", 0);      // +0x74 (0x0029410C)
    goBackSound = app.LoadSound("sounds/menu/go_back.wav", 0);  // +0x70 (0x002940CC)
    gameLoaded = false;                               // +0x20
}

FrontendLoadPanel::~FrontendLoadPanel() {             // orig: 0x0001FC70
    app.Sound().Release(goBackSound);                 // Sound_Release(+0x70), (+0x74)
    app.Sound().Release(goToSound);
}

void FrontendLoadPanel::Show() {                      // orig: 0x0001FCD0
    if (!opening) {
        fe.MoveCamera(4);                             // 0x00023940(4): Camera_Continue_there
        (void)app.Sound().PlaySfx(goToSound, false);  // +0x74
        opening = true;
        return;
    }
    if (fe.cameraMoving) return;                      // Frontend+0x6C
    Activate();                                       // vf20
    opening = false;
}

void FrontendLoadPanel::Hide() {                      // orig: 0x0001FD10
    if (gameLoaded) {                                 // +0x20 (the dead path)
        closing = false;
        ApplyLoaded();                                // 0x0001A940
        return;
    }
    if (!closing) {
        Deactivate();                                 // vf24
        fe.currentPanel = fe.mainPanel.get();         // Frontend+0x34 = +0x1C
        fe.MoveCamera(5);                             // Camera_Continue_back
        (void)app.Sound().PlaySfx(goBackSound, false);   // +0x70
        closing = true;
        // (App+0x100: Mem_ReturnTrue)
    }
    if (fe.cameraMoving) return;
    LoadPanel& p = *fe.loadPanel;                     // (Frontend+0x28: this panel)
    SaveList& l = p.list;                             // 0x0001A9B0
    app.memoryCards->ClearEntries();                  // ((App+0xF4)+4)->0x000E18B0
    l.Clear();                                        // 0x00029D00
    app.memoryCards->ClearEntries();
    p.ResetList();                                    // 0x0001B2F0
    l.SetTop(0);                                      // 0x00029BD0(0)
    p.ResetList();
    p.Redraw();                                       // 0x0001AC10
    fe.mainPanel->Show();                             // (Frontend+0x34)->vf18: the main panel
    closing = false;
}

void FrontendLoadPanel::Activate() {                  // orig: 0x0001FE10
    active = true;
    for (const auto& r : rows) SetIndex(r.Get(), 0);  // the four rows off
    selection = 0;                                    // +8
    SetIndex(rows[0].Get(), 1);                       // row 0 on
    // (the hint 0x000256C0("HINTS_014"): a no-op)
    if (app.memoryCards->flag0) app.memoryCards->flag0 = false;   // (App+0xF4)+0 (L: the device's "changed" byte)
    app.input.Update(g_frameStep, app.services.readPad);   // Input_Update 0x0009BEA0
}

void FrontendLoadPanel::Deactivate() {                // orig: 0x0001FEB0
    active = false;
    for (const auto& r : rows) SetIndex(r.Get(), 0);
    // (App+0x100: Mem_ReturnTrue)
}

// ---- the pause's load panel -----------------------------------------------------------------------------------------

PauseLoadPanel::PauseLoadPanel(App& a, PauseMenu& p, text::TextObject* tx)   // orig: 0x000A7C20(0, pause, +0x54)
    : LoadPanel(a, 0, tx), pause(p) {
    t = std::bit_cast<f32>(0x3FD55555u);              // +0x18 = 1.6666666
    goToSound = app.LoadSound("sounds/menu/pause_go_to.wav", 0);      // +0x74 (0x002A1DF8)
    goBackSound = app.LoadSound("sounds/menu/pause_go_back.wav", 0);  // +0x78 (0x002A1DD8)
    LoadPanel::Reset();                               // 0x0001A890
}

PauseLoadPanel::~PauseLoadPanel() {                   // orig: 0x000A7BB0
    app.Sound().Release(goToSound);                   // +0x74, +0x78
    app.Sound().Release(goBackSound);
    ReleaseSounds();                                  // 0x0001A8D0 (the base dtor releases them again: as written)
}

void PauseLoadPanel::Reset() {                        // orig: 0x000A7B80
    goToSound = app.LoadSound("sounds/menu/pause_go_to.wav", 0);
    goBackSound = app.LoadSound("sounds/menu/pause_go_back.wav", 0);
    LoadPanel::Reset();                               // 0x0001A890
}

void PauseLoadPanel::Show() {                         // orig: 0x000A7CD0
    if (!opening) {
        t = std::bit_cast<f32>(0x3FD55555u);          // 1.6666666
        opening = true;
        selection = 0;
        for (const auto& r : rows) SetIndex(r.Get(), 0);
        SetIndex(rows[size_t(selection)].Get(), 1);
        (void)app.Sound().PlaySfx(goToSound, false);  // +0x74
        // (Mem_ReturnTrue)
        return;
    }
    if (!(double(t) > std::bit_cast<f32>(0x3FF00000u))) return;   // FCOMP [0x002A1E3C] 1.875; TEST AH,0x41; JNE
    opening = false;
    active = true;
    // (the hint 0x000256C0("HINTS_014"): a no-op)
}

void PauseLoadPanel::Hide() {                         // orig: 0x000A7DA0
    if (!closing) {
        t = std::bit_cast<f32>(0x40055555u);          // 2.0833333
        closing = true;
        active = false;
        delay = 10;
        (void)app.Sound().PlaySfx(goBackSound, false);   // +0x78
        for (const auto& r : rows) SetIndex(r.Get(), 0);
        // (App+0x100: Mem_ReturnTrue)
        return;
    }
    if (!(double(t) > std::bit_cast<f32>(0x4012AAABu))) return;   // FCOMP [0x002A1E40] 2.2916667
    LoadPanel& p = *pause.load;                       // ((+0x70)+0x1C: this panel)
    SaveList& l = p.list;
    app.memoryCards->ClearEntries();
    l.Clear();
    app.memoryCards->ClearEntries();
    p.ResetList();
    l.SetTop(0);
    p.ResetList();
    p.Redraw();
    closing = false;
    active = false;
    if (gameLoaded) pause.main.Hide();                // ((+0x70)+0x18)->vf1C (the dead path)
    else pause.main.Activate();                       // vf20
    if (text) text->RemoveAll();                      // (+0x48) 0x00099080
}

}  // namespace fut::game::app
