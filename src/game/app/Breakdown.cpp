// The end-of-level menu; see Breakdown.h.
#include "game/app/Breakdown.h"

#include "core/CrtString.h"
#include "engine/ni/NiCull.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiObjects.h"
#include "engine/ni/NiRenderer.h"
#include "engine/ni/NiStream.h"
#include "game/app/MenuScene.h"
#include "game/text/LanguageDb.h"
#include "game/text/TextObject.h"

#include <bit>
#include "game/GameTime.h"
#include "game/app/App.h"
#include "game/app/Contexts.h"
#include "game/app/Frontend.h"
#include "game/app/MemoryCards.h"
#include "game/level/LevelData.h"
#include "game/defs/Defs.h"
#include "game/level/LevelLoader.h"
#include "game/level/Session.h"
#include "game/object/Character.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fut::game::app {

using ni::detail::F;

namespace {
s32 FtoL(double v) { return static_cast<s32>(static_cast<s64>(v)); }   // _ftol 0x00167110 (truncation)
}  // namespace

// ---- the text records --------------------------------------------------------------------------------------------
TextRecord::TextRecord(std::u16string_view str, text::TextObject* to, f32 sc, s32 align, ni::NiNode* at, f32 px, f32 py,
                       s32 m)                         // orig: 0x000A4AC0 (thiscall, RET 0x20)
    : mode(m), x(px), y(py), text(to), scale(sc), hAlign(align), node(at) {
    // +0x28..+0x30 = 1.0, +0x14 = 0, +0xC = the text object's +4, +0 = +4 = 0, +8 = 0.1, +0x18 = 0, +0x1C = 0; +0x38 =
    // a copy of the text (new(2 WStrLen + 2), WStrCpy), +0x34 = new(the same size) with [0] = 0
    spacing = text ? text->style.letterSpacing : 0.0f;
    full.assign(str.begin(), std::find(str.begin(), str.end(), u'\0'));
}

TextRecord::~TextRecord() {                           // orig: 0x000A4BC0
    // +0x38 and +0x34 freed; the string removed (0x00098FD0) when it was added; the node +0x48 released
    if (id && text) text->RemoveString(id);
}

void TextRecord::ApplyColour() {                      // orig: 0x000A4DC0
    if (!node) return;                                // +0x48
    if (colourMode == 2) {                            // +0x14: the pulse
        // FLD +0; FMUL [0x00295D58] 3.1; FCOS; FMUL [0x00293AA8] 0.15; FADD [0x0029A268] 0.85; (+0x2C = 1.0) FLD +0;
        // FMUL [0x002A1968] 7.4; FSIN; FMUL [0x00293B28] 0.4; FADD [0x002976C0] 0.6; FXCH; FSTP +0x28; FSTP +0x30
        const double r = std::cos(double(time) * 3.1f) * 0.15f + 0.85f;   // (KNOWN_ISSUES #35: the C library's)
        colour.g = 1.0f;
        const double b = std::sin(double(time) * 7.4f) * 0.4f + 0.6f;
        colour.r = F(r);
        colour.b = F(b);
    }
    // each child (+0xCC re-read per iteration): its material property (GetProperty(3) 0x00101DA0): emissive +0x40 =
    // the colour, ++revision +0x54
    for (size_t i = 0; i < node->Children().size(); ++i) {
        ni::NiAVObject* c = node->Children()[i].Get();
        if (!c) continue;
        if (auto* m = dynamic_cast<ni::NiMaterialProperty*>(game::GetProperty(*c, 3))) {
            m->emissive = colour;
            ++m->revision;
        }
    }
}

s32 TextRecord::Update() {                            // orig: 0x000A5880
    text->style.hAlign = hAlign;                      // (+0x3C)+0x60 = +0x44
    text->SetScale(std::bit_cast<f32>(0x3FD9999Au), std::bit_cast<f32>(0x40233334u));   // (1.7, 2.55) for every record
    time = F(double(g_frameStep) + time);             // FLD [0x002D67E8]; FADD +0; FSTP
    if (mode == 0) {
        if (id) {                                     // added: the colour only
            ApplyColour();
            return 0;
        }
        text->style.letterSpacing = spacing;          // (+0x3C)+4 = +0xC
        id = text->AddString(full, x, y, text->width, -1.0f, node.Get());   // 0x0009A140(+0x38, +0x20, +0x24, +0x48, -1)
        if (!id) return 0;
        ApplyColour();
        return 3;
    }
    if (mode != 1) return 0;
    // FLD dt; FADD +4; FCOM +8; FST +4; TEST AH,5; JP: below the interval -> 1 (the sum stored)
    const double sum = double(g_frameStep) + accum;
    accum = F(sum);
    if (sum < double(interval)) return 1;
    accum = F(sum - double(interval));                // FSUB +8; FSTP +4
    const char16_t ch = typedCount < full.size() ? full[typedCount] : u'\0';   // +0x38[+0x1C]
    if (ch == u'\0') return 0;
    typed.resize(typedCount);
    typed.push_back(ch);                              // +0x34[+0x1C] = ch, [+0x1C + 1] = 0
    ++typedCount;
    if (id) text->RemoveString(id);                   // 0x00098FD0
    id = text->AddString(typed, x, y, text->width, -1.0f, node.Get());   // 0x0009A140(+0x34, ...)
    ApplyColour();
    return 2;
}

void Breakdown::DeleteRecords() {                     // orig: 0x000A6526 (Open) / 0x000A5C84 (the close)
    for (size_t i = 0; i < records.size(); ++i) records[i].reset();   // 0x000A4BC0 + delete, SetAt(i, 0)
    records.clear();                                  // 0x000FA6C0
}

void Breakdown::AddText(const char* spec, f32 sc, const ni::NiColor& c, s32 colourMode, s32 mode, const char* textId,
                        const char* format, ...) {    // orig: 0x000A6010
    // the spec copied; "node|x|y" split at the '|' (strchr 0x00167050; a missing part reads "0" 0x002A1AA8)
    char buf[0x200];
    std::snprintf(buf, sizeof buf, "%s", spec);
    const char* px = "0";
    const char* py = "0";
    if (char* bar = std::strchr(buf, '|')) {
        *bar = '\0';
        px = bar + 1;
    }
    if (char* bar = std::strchr(const_cast<char*>(px), '|')) {
        *bar = '\0';
        py = bar + 1;
    }
    // the node: (+0x1C) vf54 GetObjectByName (twice), kept when it is a NiNode (RTTI 0x002F9230 walk)
    ni::NiNode* at = scene ? dynamic_cast<ni::NiNode*>(GetObjectByName(scene.Get(), buf)) : nullptr;
    const f32 x = F(std::atof(px) * double(std::bit_cast<f32>(0x3F8CCCCDu)));   // atof; FMUL [0x00295644] 1.1; FSTP
    const f32 y = F(std::atof(py) * double(std::bit_cast<f32>(0xBDAE147Bu)));   // FMUL [0x002A1AA4] -0.085; FSTP
    if (!at || !text) return;                         // (port guard: no text object, as a failed font load leaves)
    std::u16string str;
    if (textId) {
        // TextManager::Find(id, 0) 0x000D9C30: the entry's string (+4) when it has one, else the id widened
        // (0x000D2CB0, MOVSX per byte)
        const text::LanguageDb::Entry* e = app.text ? app.text->Find(textId, nullptr) : nullptr;
        if (e && e->hasText) {
            str = e->text;
        } else {
            for (const char* p = textId; *p; ++p) str.push_back(char16_t(s16(s8(*p))));
        }
    } else if (format) {
        char out[0x400];                              // (esp+0x21C: vsprintf 0x00167185)
        va_list ap;
        va_start(ap, format);
        std::vsnprintf(out, sizeof out, format, ap);
        va_end(ap);
        for (const char* p = out; *p; ++p) str.push_back(char16_t(s16(s8(*p))));
    } else {
        return;
    }
    // new(0x4C) 0x000A4AC0(text, +0x68, scale, (+0x68)+0x60, node, x, y, mode); +0xC = (+0x68)+4, +0x14 = the colour
    // mode, +0x28..+0x30 = the colour; appended (SetAtGrow at the size: 0x000A5850 / 0x000A4A20)
    auto r = std::make_unique<TextRecord>(str, text.get(), sc, text ? text->style.hAlign : 0, at, x, y, mode);
    r->spacing = text ? text->style.letterSpacing : 0.0f;
    r->colourMode = colourMode;
    r->colour = c;
    records.push_back(std::move(r));
}

Breakdown::Breakdown(App& a) : app(a) {               // orig: 0x000A4E70
    // +0x14..+0x24 = 0, the item array +0x28 (4), the text records +0x74.. (+0x78 = new(4)), +0xC / +0x10 / +8 / +4 /
    // +1 / +0 = 0, DAT_002E8EEC = this; then pause\breakdown.nif by a direct NiStream::Load
    std::vector<u8> bytes;
    ni::NiStream s;
    if (!app.services.readFile || !app.services.readFile("pause\\breakdown.nif", bytes) || !s.Load(bytes)) return;
    for (u32 link : s.TopLevelLinks()) {              // the first NiNode (RTTI 0x002F9230): the scene +0x1C
        if (auto* n = dynamic_cast<ni::NiNode*>(s.ResolveLink(link))) {
            scene = n;
            manager = new ni::NiKeyframeManager(n);   // new(0xC0) 0x00130D20 -> +0x14
            break;
        }
    }
    if (!scene) return;                               // (the stream released: nothing else is made)
    PreCacheScene(app.services.renderer, scene.Get());   // 0x000CEAC0(+0x1C)
    for (u32 link : s.TopLevelLinks()) {              // every NiSequence (RTTI 0x002FA124)
        auto* seq = dynamic_cast<ni::NiSequence*>(s.ResolveLink(link));
        if (!seq) continue;
        if (crt::StrICmp(seq->name.c_str(), "klinga") != 0) SetSequenceLoop(seq, false);   // 0x000A4CE0(seq, 0)
        manager->AddSequence(seq, true);              // 0x0012FFB0(seq, 1)
        Sequence q;                                   // (the port's copy of what the update's gate reads)
        q.name = seq->name;
        if (const ni::NiTimeController* c = seq->ReferenceController()) {   // +0x48[+0x90]
            q.hasController = true;
            q.length = (double(c->hiKeyTime) - c->loKeyTime) / c->frequency;   // FLD +0x20; FSUB +0x1C; FDIV +0x14
        }
        sequences_.push_back(std::move(q));
        niSequences_.push_back(seq);
    }
    scene->UpdateDownwardPass(0.0f, true);            // Update(0, 1)
    scene->UpdateProperties();
    scene->UpdateEffects();
    cameraNode = FindAs<ni::NiNode>(scene.Get(), "CAMERA");   // +0x18 (0x00293F08)
    if (!cameraNode) return;
    camera = FindAs<ni::NiCamera>(scene.Get(), "CAMERA1");    // +0x24 (RTTI 0x002F9120)
    if (!camera) return;
    for (const char* g : {"glow1Shape", "glowShape2"})    // (RTTI 0x002F9190) hidden: +0xB0 |= 1
        if (auto* o = FindAs<ni::NiAVObject>(scene.Get(), g)) o->flags = u16(o->flags | ni::kNiAVHidden);
    camera->renderer = app.services.renderer;         // +0x1A8 = App->vf14(); +0x15C = the scene (the Click's)
    static const char* const kItems[4] = {"save_breakdown", "continue_breakdown", "replay_breakdown", "quit_breakdown"};
    for (size_t i = 0; i < items.size(); ++i) items[i] = FindAs<ni::NiSwitchNode>(scene.Get(), kItems[i]);   // +0x28..
    collect = FindAs<ni::NiSwitchNode>(scene.Get(), "collect");   // +0x38
    manager->cumulativeAnimations = false;            // (+0x14)+0x4C = 0
    time = 0;                                         // +0x5C; the sounds +0x3C.. = 0
    // +0x68: TextObject::Create(0), SetRenderer(App->vf14()), the scene mode (+0x24, +0x1C), hAlign 1, vAlign 3,
    // width 10000, no word wrap; Language/LEVELNAMES.ldb +0x6C, Language/BREAKDOWN.ldb +0x70; +0x8C = 0, +0x90 = 0
    text.reset(text::TextObject::Create(0, app.services.readFile, app.services.renderer));
    if (text) {
        text->SetRenderer(app.services.renderer);     // 0x00098F80
        text->UseScene(camera.Get(), scene.Get());    // 0x00099BE0
        text->style.hAlign = 1;                       // +0x60
        text->style.vAlign = 3;                       // +0x64
        text->width = std::bit_cast<f32>(0x461C4000u);   // +0x54 = 10000.0
        text->style.wordWrap = false;                 // +0x68
    }
    levelNames = app.text ? app.text->LoadDb("Language/LEVELNAMES.ldb") : nullptr;   // 0x00295014
    texts = app.text ? app.text->LoadDb("Language/BREAKDOWN.ldb") : nullptr;         // 0x002A196C
    continueDirect = false;                           // +0x90
}

Breakdown::~Breakdown() {                             // (the App's teardown: the original keeps the menu)
    DeleteRecords();                                  // (before the text object they refer to)
    if (text) text->RemoveAll();
}

const Breakdown::Sequence* Breakdown::FindSequence(const char* name) const {   // orig: 0x0001BFC0
    // (the keyframe manager's name map 0x00108220; L: the first of equal names)
    for (const Sequence& q : sequences_)
        if (q.name == name) return &q;
    return nullptr;
}

ni::NiSequence* Breakdown::NiSequenceOf(const Sequence* q) const {
    if (!q) return nullptr;
    const size_t i = size_t(q - sequences_.data());
    return i < niSequences_.size() ? niSequences_[i].Get() : nullptr;
}

void Breakdown::StartSequence(const char* name) {     // orig: 0x000A5810
    sequence = FindSequence(name);                    // +0x10
    if (!sequence) return;
    sequenceOffset = F(-double(time));                // FLD +0x5C; FCHS; FSTP seq+0xA8
    if (ni::NiSequence* q = NiSequenceOf(sequence)) {
        q->offset = sequenceOffset;
        if (manager) manager->ActivateSequence(q, 0); // (+0x14) 0x0012F3A0(seq, 0)
    }
}

void Breakdown::Draw() {                              // orig: 0x000A4C40
    if (!open) return;                                // +1
    if (camera && camera->renderer) camera->renderer->ClearBuffer(nullptr, 4);   // (+0x24)+0x1A8 vf44(0, 4)
    if (manager) manager->Update(time);               // (+0x14)->vf38(+0x5C)
    if (scene) {
        scene->UpdateDownwardPass(time, true);        // (+0x1C)->Update(+0x5C, 1)
        scene->UpdateWorldBound();                    // vf98
    }
    if (camera) ni::VisitForCulling(*camera, scene.Get());   // (+0x24) Click 0x00100CE0
    if (text) text->UpdateCamera();                   // (+0x68) 0x00098D70
    if (confirm == 1 || confirm == 2 || confirm == 3 || app.memoryCards->dialog.Animating())   // 0x000E3020
        app.memoryCards->dialog.Draw();               // ((App+0xF4)+0x10) 0x000E33A0
}

void Breakdown::Open() {                              // orig: 0x000A6340
    if (ni::NiRenderer* r = app.services.renderer) {  // App->vf14(): the clear colour black (vf6C), three blank frames
        r->SetBackgroundColor(ni::NiColor{0.0f, 0.0f, 0.0f});
        for (int i = 0; i < 3; ++i) {
            r->ClearBuffer(nullptr, 1);               // vf44(0, 1)
            r->DisplayFrame();                        // vf40
        }
    }
    app.Sound().PauseAll();                           // Sound_PauseAll 0x000CAFD0
    app.input.SetRumbleEnabled(false, 0.0f);          // Input_SetRumbleEnabled(0, 0)
    confirm = 0;                                      // +0xC
    continueDirect = false;                           // +0x90
    closing = false;                                  // +0
    time = 0;                                         // +0x5C
    sequence = nullptr;                               // +0x10
    highlighted = 0;                                  // +8
    selection = 0;                                    // +4
    repeatDir = 0;                                    // +0x64
    repeatTimer = 0;                                  // +0x60
    if (manager) manager->DeactivateAll();            // (+0x14) 0x00131170
    auto setItem = [](ni::NiSwitchNode* s, s32 index) {
        if (s && s32(s->Children().size()) > index) s->index = index;   // (+0xCC > index: +0xE0)
    };
    for (s32 i = 0; i < 4; ++i) {                     // the other items off, each Update(0, 1)
        if (i == selection) continue;
        setItem(items[size_t(i)].Get(), 0);
        if (items[size_t(i)]) items[size_t(i)]->UpdateDownwardPass(0.0f, true);
    }
    setItem(items[size_t(selection)].Get(), 1);       // the selection's on, Update(0, 1)
    if (items[size_t(selection)]) items[size_t(selection)]->UpdateDownwardPass(0.0f, true);
    StartSequence("Dialog_menu_enter");               // 0x0001BFC0 + seq+0xA8 = -time (inline at 0x000A6425)
    if (const Sequence* k = FindSequence("klinga")) { // (0x002A19EC: activated the same way, not kept)
        if (ni::NiSequence* q = NiSequenceOf(k)) {
            q->offset = F(-double(time));
            if (manager) manager->ActivateSequence(q, 0);
        }
    }
    static const char* const kSounds[7] = {"sounds/menu/cursor.wav",      "sounds/menu/select.wav",
                                           "sounds/menu/toggleerror.wav", "sounds/menu/Pause_fold_up.wav",
                                           "sounds/menu/Pause_fold_down.wav", "sounds/menu/toggleerror.wav",
                                           "sounds/menu/toggleok.wav"};
    for (size_t i = 0; i < 7; ++i) sounds[i] = app.LoadSound(kSounds[i], 0);   // +0x3C .. +0x54
    sounds[7] = app.LoadSound("sounds/music/EndOfLevel_theme.wav", 1);         // +0x58 (0x002A1CB8)
    (void)app.Sound().PlayMusic(sounds[7], true);     // Sound_PlayMusic(+0x58, 1)
    UpdateSound(app.Sound(), app.CurrentLevel());     // App+0xF0 vf04 (Update 0x000CBD40); vf08 (a nop)
    DeleteRecords();                                  // +0x8C = 0, +0x78's records deleted, 0x000FA6C0
    (void)app.Sound().PlaySfx(sounds[3], false);      // Sound_PlaySFX(+0x48 Pause_fold_up, 0)
    if (text) text->RemoveAll();                      // (+0x68) 0x00099080
    // 0x000A65A8: the statistics, with the values a run without a level shows (the locals' initialisers): nibblers 5 of
    // 5, kills 13 of 23, gold 11 of 103, the best time 65120 s, the level time 65123 s, the game time 174312 s, 3
    // deaths, "The Sewers" / "The Subway", the rating id buffer "Rock n roll baby!!" (0x002A1CA4, overwritten below)
    s32 picked = 5, nibblers = 5, kills = 13, enemies = 23, gold = 11, goldTotal = 103, best = 0xFE60, levelTime = 0xFE63;
    s32 gameTime = 0x2A8E8, deaths = 3;
    std::string levelId = "The Sewers", nextId = "The Subway";   // (0x002A1C98 / 0x002A1C8C)
    char rating[0x40] = "Rock n roll baby!!";
    s32 killPct = 0, goldPct = 0;
    LoadedLevel* l = app.ingame ? app.ingame->Level() : nullptr;
    LevelData* level = l && l->level ? l->level.get() : nullptr;   // g_level (DAT_002DAC64)
    auto lives = [&] { return level ? level->lives : app.lives; };   // 0x000C12B0 (DAT_002D687C)
    if (level) {
        picked = level->nibblersPickedUp;             // S+0x18C
        nibblers = level->nibblerCount;               // S+0x190
        (void)lives();                                // 0x000C12B0 (the result unused)
        levelId = level->name;                        // S+0xC
        nextId = app.LevelName();                     // App+0xAC
        best = app.records.BestTime(level->name.c_str());   // App+0xA4 0x000F3B60 (-1 none)
        if (Player* P = level->player) {              // S+0x94
            kills = s32(level->killedIds.size());     // S+0x1D8
            enemies = s32(level->enemyIds.size());    // S+0x1A8
            if (crt::StrICmp(level->name.c_str(), "Level3-4") == 0) ++kills;   // 0x002A1C80
            gold = level->pickupValue;                // S+0x188
            goldTotal = level->collectableCount;      // S+0x194
            deaths = P->deaths;                       // P+0xBF4
            killPct = enemies > 0 ? FtoL(double(kills) * 100.0f / double(enemies)) : 100;   // FILD; FMUL; FIDIV; _ftol
        } else {
            killPct = FtoL(double(kills) * 100.0f / double(enemies));   // 0x000A672E (the initialisers)
        }
    } else {
        killPct = FtoL(double(kills) * 100.0f / double(enemies));
    }
    goldPct = goldTotal > 0 ? FtoL(double(gold) * 100.0f / double(goldTotal)) : 100;   // [0x00295610] 100.0
    if (level) {
        // the rating: (kills % 0.8 + gold % 0.5, else gold % 1.2, else 100) - deaths 10, / 20, clamped to 0 .. 4
        double score;
        if (enemies > 0) score = double(killPct) * 0.8f + double(goldPct) * 0.5f;   // [0x00293C18], [0x00293AA0]
        else if (goldTotal > 0) score = double(goldPct) * 1.2f;   // [0x0029A520]
        else score = 100.0f;
        score -= double(deaths) * 10.0f;              // FILD; FMUL [0x00293C2C]; FSUBP
        s32 r = FtoL(score) / 20;                     // (the magic 0x66666667 division)
        if (r > 4) r = 4;
        else if (r < 0) r = 0;
        // the character by the level's name (strstr): Level2 Bender, Level3 Leela, Level4 Zoidberg, Level5-1 Bender,
        // Level5-2 Leela, else Fry; the same index for the "collect" switch (FRY 3, BEND 2, ZOID 1, LEEL 0)
        const char* who = "FRY";                      // 0x00295AA0
        s32 index = 3;
        const char* n = levelId.c_str();
        if (std::strstr(n, "Level2")) who = "BEND", index = 2;          // 0x002A1C78, 0x00295AAC
        else if (std::strstr(n, "Level3")) who = "LEEL", index = 0;     // 0x002A1C70, 0x00295AA4
        else if (std::strstr(n, "Level4")) who = "ZOID", index = 1;     // 0x002A1C68, 0x002A1C5C
        else if (std::strstr(n, "Level5-1")) who = "BEND", index = 2;   // 0x002A1C50
        else if (std::strstr(n, "Level5-2")) who = "LEEL", index = 0;   // 0x002A1C44
        std::snprintf(rating, sizeof rating, "BD_%s_%02d", who, r);   // 0x002A1C38
        if (collect && index >= -1 && index < s32(collect->Children().size())) collect->index = index;   // +0x38 +0xE0
        // the times: the level's S+0x180 / +0x184 and the game's S+0x178 / +0x17C, seconds + 0.5 truncated plus the
        // hours; the game time is at least the level time
        const Session* S = l->session.get();
        if (S) {
            levelTime = FtoL(double(S->playSeconds2) + 0.5f) + S->playHours2 * 3600;   // FADD [0x00293AA0]; _ftol
            gameTime = FtoL(double(S->playSeconds) + 0.5f) + S->playHours * 3600;
            if (levelTime > gameTime) gameTime = levelTime;
        }
        const char* next = nextId.c_str();            // (esp+0x34)
        if (lives() < 5 &&                            // 0x000C12B0; CMP 5; JGE
            (crt::StrICmp(next, "level1-1") == 0 || crt::StrICmp(next, "level2-1") == 0 ||
             crt::StrICmp(next, "level3-1") == 0))
            level->lives = 5;                         // 0x000C12C0(5): a new chapter starts with 5 lives
    }
    // 0x000A694F: the texts (the text object's +0x60 (hAlign) set before each; the colour (0.78, 1, 0.69) =
    // 0x3F47AE14, 1.0, 0x3F30A3D7; the scales 1.7 = 0x3FD9999A, 2.125 = 0x40080000, 2.55 = 0x40233334)
    const ni::NiColor white{1.0f, 1.0f, 1.0f};
    const ni::NiColor green{std::bit_cast<f32>(0x3F47AE14u), 1.0f, std::bit_cast<f32>(0x3F30A3D7u)};
    const f32 s17 = std::bit_cast<f32>(0x3FD9999Au), s2125 = std::bit_cast<f32>(0x40080000u);
    const f32 s255 = std::bit_cast<f32>(0x40233334u);
    auto align = [&](s32 a) {
        if (text) text->style.hAlign = a;             // (+0x68)+0x60
    };
    align(1);
    AddText("text_level", s2125, white, 0, 0, levelId.c_str(), nullptr);   // the level's title (LEVELNAMES)
    if (killPct != -1) {
        align(1);
        AddText("text_1|0|0", s17, green, 0, 0, "BD_KILLS", nullptr);
        align(0);
        AddText("text_1|1|0", s17, green, 0, 0, nullptr, "%d / %d", kills, enemies);   // 0x002A1C18
    }
    align(1);
    AddText("text_1|0|1", s17, green, 0, 0, "BD_PLDEATHS", nullptr);
    align(0);
    AddText("text_1|1|1", s17, green, 0, 0, nullptr, "%d", deaths);   // 0x00296D4C
    const bool allNibblers = picked != 0 && picked >= nibblers;   // (esp+0x13)
    align(2);
    AddText("text_toaster", s255, green, 0, 0, nullptr, "%d", lives());
    AddText("text_gold", s255, green, 0, 0, nullptr, "%d / %d", gold, goldTotal);
    if (allNibblers) {                                // the pulsing colour (mode 2) and the unlock line
        AddText("text_nibbler", s255, green, 2, 0, nullptr, "%d / %d", picked, nibblers);
        align(1);
        AddText("text_extras", s255, green, 2, 0, "BD_UNLOCK", nullptr);
    } else {
        AddText("text_nibbler", s255, green, 0, 0, nullptr, "%d / %d", picked, nibblers);
    }
    // the times as "%02d:%02d:%02d" (0x002A1B84) of t / 3600, t / 60 % 60, t % 60 (signed: -1 shows 00:00:-1)
    align(1);
    AddText("text_1|0|2", s17, green, 0, 0, "BD_LEVELTIME", nullptr);
    align(0);
    AddText("text_1|1|2", s17, green, 0, 0, nullptr, "%02d:%02d:%02d", levelTime / 3600, levelTime / 60 % 60,
            levelTime % 60);
    align(1);
    AddText("text_1|0|3", s17, green, 0, 0, "BD_BESTTIME", nullptr);
    align(0);
    AddText("text_1|1|3", s17, green, 0, 0, nullptr, "%02d:%02d:%02d", best / 3600, best / 60 % 60, best % 60);
    align(1);
    AddText("text_coment", s2125, white, 0, 0, rating, nullptr);   // "BD_<char>_<rating>"
    align(1);
    AddText("text_2|0|0", s17, green, 0, 0, "BD_GAMETIME", nullptr);
    align(0);
    AddText("text_2|1|0", s17, green, 0, 0, nullptr, "%02d:%02d:%02d", gameTime / 3600, gameTime / 60 % 60,
            gameTime % 60);
    align(1);
    AddText("text_2|0|1", s17, green, 0, 0, "BD_NEXTLEVEL", nullptr);
    align(0);
    AddText("text_2|1|1", s17, green, 0, 0, nextId.c_str(), nullptr);   // the next level's title
    align(0);
    // the items with the letter spacing 0.01 (0x3C23D70A; the old value restored after); the save item grey (0.5)
    // without the memory card (0x000E00A0: DAT_002D6E20)
    const f32 spacing = text ? text->style.letterSpacing : 0.0f;   // (+0x68)+4
    if (text) text->style.letterSpacing = std::bit_cast<f32>(0x3C23D70Au);
    const ni::NiColor grey{0.5f, 0.5f, 0.5f};
    AddText("text_save", s17, app.memoryCards->cardAvailable ? white : grey, 0, 0, "BD_SAVE", nullptr);
    AddText("text_continue", s17, white, 0, 0, "BD_CONTINUE", nullptr);
    AddText("text_replay", s17, white, 0, 0, "BD_REPLAY", nullptr);
    AddText("text_quit", s17, white, 0, 0, "BD_QUIT", nullptr);
    if (text) text->style.letterSpacing = spacing;
}

void Breakdown::Confirm(s32 mode) {                   // orig: 0x000A5E06 / 0x000A5E58 / 0x000A5EA6
    Dialog& dlg = app.memoryCards->dialog;            // (App+0xF4)+0x10
    // 0x000E3500(TextManager::GetText("BD_CONFIRM_CONTINUE" / "_REPLAY" / "_QUIT", 0)), 0x000E3790(1, 2)
    const char* id = mode == 3 ? "BD_CONFIRM_CONTINUE" : mode == 1 ? "BD_CONFIRM_REPLAY" : "BD_CONFIRM_QUIT";
    (void)dlg.SetString(app.text ? app.text->GetText(id, nullptr) : nullptr);
    dlg.SetButtons(1, 2);
    dlg.Select(1);                                    // 0x000E35A0(1): "no"
    dlg.Open();                                       // 0x000E4900
    confirm = mode;                                   // +0xC
    (void)app.Sound().PlaySfx(sounds[1], false);      // Sound_PlaySFX(+0x40 select)
}

void Breakdown::Update() {                            // orig: 0x000A59D0
    if (!open) return;                                // +1
    // (the hint no-ops 0x000256C0: HINTS_BD_001 and one per selection)
    bool busy = false;                                // BL
    if (sequence) {                                   // +0x10
        if (sequence->hasController) {
            // FLD hi; FSUB lo; FDIV freq; FSUB seq+0xA8; FCOMP +0x5C; TEST AH,0x41; JNE: busy only while greater
            if (sequence->length - double(sequenceOffset) > double(time)) {
                busy = true;
            } else {
                // NiKeyframeManager::DeactivateSequence 0x0012FAB0 (+0x14, the sequence); +0x10 = 0
                if (manager)
                    if (ni::NiSequence* q = NiSequenceOf(sequence)) (void)manager->DeactivateSequence(q);
                sequence = nullptr;
            }
        } else {
            sequence = nullptr;                       // (no controller: +0x10 = 0 without the deactivation)
        }
    }
    MemoryCards& mc = *app.memoryCards;               // App+0xF4
    busy = busy || mc.running;                        // OR BL, +0xA
    Dialog& dlg = mc.dialog;                          // +0x10
    if (confirm == 1 || confirm == 2 || confirm == 3 || dlg.Animating()) {   // 0x000E3020
        busy = true;
        if (!dlg.Animating()) {
            if (MenuInput(app, 2)) dlg.Select(0);     // 0x00029FA0(2) left: "yes"
            if (MenuInput(app, 3)) dlg.Select(1);     // right: "no"
            if (MenuInput(app, 4)) {                  // accept
                if (dlg.selection == 0) {             // +0x5C
                    if (confirm == 1) {               // Replay
                        LoadedLevel* l = app.ingame ? app.ingame->Level() : nullptr;
                        // 0x000C2320: App+0xAC = S+0xC, DAT_002EAA39 = 1
                        if (l && l->level) app.SetLevelName(l->level->name.c_str());
                        app.carry.replay = true;
                        closing = true;               // +0
                        StartSequence("Dialog_menu_leave");
                        (void)app.Sound().PlaySfx(sounds[4], false);   // Sound_PlaySFX(+0x4C Pause_fold_down, 0)
                    } else if (confirm == 3) {        // Continue (App+0xAC holds the next level)
                        closing = true;
                        StartSequence("Dialog_menu_leave");
                        (void)app.Sound().PlaySfx(sounds[4], false);
                    } else {                          // Quit
                        app.carry.state.reset();      // 0x000C1290: DAT_002EAA28 freed
                        app.ingame->Leave("Frontend");   // DAT_002DAC60 0x0002A220
                        app.SetLevelName("");         // 0x000A23D0(0x00295254)
                        closing = true;
                        StartSequence("Dialog_menu_leave");
                        (void)app.Sound().PlaySfx(sounds[4], false);   // Sound_PlaySFX(+0x4C, 0)
                    }
                }
                confirm = 0;
                mc.PlayDialogSound();                 // ((App+0xF4)+4)+4 -> 0x000E1980
                dlg.Close();                          // 0x000E4980
            }
        }
    }
    for (size_t i = 0; i < records.size(); ++i)       // 0x000A5BBC: +0x8C over +0x80 (the results unused)
        if (records[i]) (void)records[i]->Update();   // 0x000A5880
    if (busy) {
        time = F(double(g_frameStep) + time);         // FLD dt; FADD +0x5C; FSTP
        return;
    }
    if (closing) {                                    // the leave has ended
        app.input.SetRumbleEnabled(true, 0.0f);       // Input_SetRumbleEnabled(1, 0)
        app.ActivePad()->SetRumble(0.0f, 0.0f, false);   // Pad::SetRumble(0, 0, 0)
        open = false;                                 // +1 = BL (0): Ingame's next step loads App+0xAC or leaves
        for (u32 h : sounds) app.Sound().Release(h);  // Sound_Release(+0x3C .. +0x58) 0x000C9880
        // (App+0x100 0x000A32A0: returns true)
        DeleteRecords();                              // 0x000A5C84: each deleted, SetAt(i, 0); 0x000FA6C0
        if (text) text->RemoveAll();                  // (+0x68) 0x00099080
        return;                                       // (no time update)
    }
    const bool press = app.ActivePad()->JustPressedUnlatched(input::kStart) |   // (both read: OR BL, AL)
                       app.ActivePad()->JustPressedUnlatched(input::kButtonA);
    const bool left = app.ActivePad()->IsDownUnlatched(input::kDPadLeft) |
                      (app.ActivePad()->StickX(0) < -0.1f);   // [0x00293BF4]; TEST AH,5; JP
    const bool right = app.ActivePad()->IsDownUnlatched(input::kDPadRight) |
                       (app.ActivePad()->StickX(0) > 0.1f);   // [0x00293AB0]; TEST AH,0x41; JNE
    if (press) {
        switch (selection) {                          // the jump table 0x000A5FFC (each case returns: no time update)
        case 0:                                       // Save
            if (!mc.cardAvailable) {                  // 0x000E00A0 (DAT_002D6E20)
                (void)app.Sound().PlaySfx(sounds[2], false);   // Sound_PlaySFX(+0x44 toggleerror, 0)
                return;
            }
            mc.StartInGameSave();                     // 0x000E5000
            (void)app.Sound().PlaySfx(sounds[1], false);   // Sound_PlaySFX(+0x40 select, 0)
            return;
        case 1:                                       // Continue
            if (continueDirect) {                     // +0x90: after a successful save, no confirm dialog
                closing = true;
                StartSequence("Dialog_menu_leave");
                (void)app.Sound().PlaySfx(sounds[1], false);   // Sound_PlaySFX(+0x40, 0)
                return;
            }
            Confirm(3);                               // "BD_CONFIRM_CONTINUE"
            return;
        case 2: Confirm(1); return;                   // "BD_CONFIRM_REPLAY"
        case 3: Confirm(2); return;                   // "BD_CONFIRM_QUIT"
        default:
            (void)app.Sound().PlaySfx(sounds[1], false);   // Sound_PlaySFX(+0x40, 0)
            return;
        }
    }
    // the repeat: FLD +0x60; FSUB dt; FST +0x60 (the difference stays on the x87 stack for the right test)
    const double r = double(repeatTimer) - double(g_frameStep);
    repeatTimer = F(r);
    if (right && (repeatDir != 1 || r < 0.0)) {       // FCOMP [0x00293AB8] 0.0; TEST AH,5; JP: on <
        repeatTimer = 0.25f;                          // 0x3E800000
        repeatDir = 1;
        ++selection;
    }
    if (left && (repeatDir != -1 || double(repeatTimer) < 0.0)) {   // FLD +0x60 (the stored float)
        repeatTimer = 0.25f;
        repeatDir = -1;
        --selection;
    }
    if (!right && !left) repeatDir = 0;
    if (selection > 3) selection = 0;                 // AND with ((3 < sel) - 1)
    if (selection < 0) selection = 3;
    if (highlighted != selection) {                   // the old item's highlight off, the new one's on
        if (ni::NiSwitchNode* o = items[size_t(highlighted)].Get(); o && !o->Children().empty()) o->index = 0;
        if (ni::NiSwitchNode* n = items[size_t(selection)].Get(); n && n->Children().size() > 1) n->index = 1;
    }
    highlighted = selection;                          // +8
    time = F(double(g_frameStep) + time);             // FLD dt; FADD +0x5C; FSTP
}

}  // namespace fut::game::app
