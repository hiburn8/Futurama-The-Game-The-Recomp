// The front end's extras; see Extras.h.
#include "game/app/Extras.h"

#include "core/CrtString.h"
#include "engine/ni/NiKeyframeManager.h"
#include "engine/ni/NiRenderState.h"
#include "engine/ni/NiCull.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiRenderer.h"
#include "game/GameTime.h"
#include "game/app/App.h"
#include "game/app/Contexts.h"
#include "game/app/Frontend.h"
#include "game/app/MenuScene.h"
#include "game/text/LanguageDb.h"
#include "game/text/TextObject.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>

namespace fut::game::app {

using ni::detail::F;

namespace {

// A file's first top-level object as a NiSourceTexture (the IsKindOf / DynamicCast of the callers); null when the
// file does not open or load.
ni::NiPointer<ni::NiSourceTexture> LoadFirstTexture(App& app, const std::string& path) {
    std::vector<u8> bytes;
    if (!app.services.readFile || !app.services.readFile(path, bytes)) return {};   // NiUDSStream vf04
    ni::NiStream s;
    if (!s.Load(bytes) || s.TopLevelLinks().empty()) return {};   // NiStream::Load 0x00108EA0
    return dynamic_cast<ni::NiSourceTexture*>(s.ResolveLink(s.TopLevelLinks()[0]));   // (+0x54)[0]
}

// orig: GetProperty(8) 0x00101DA0 (the first property of the texturing type in the object's list).
ni::NiProperty* TexturingOf(ni::NiAVObject& o) {
    for (const auto& p : o.properties)
        if (p && ni::PropertyType(*p) == 8) return p.Get();
    return nullptr;
}

}  // namespace

// ---- unlock.txt ---------------------------------------------------------------------------------------------------

bool ExtrasItem::Locked() const { return section ? section->locked : false; }   // orig: 0x0001B620

void ExtrasSection::Refresh(const App& app) {          // orig: 0x0001B630
    if (app.extras && app.extras->allUnlocked == 1) { // CMP [0x002D9920], 1: the cheat codes 26 / 27 (CheatCodes.h)
        locked = false;
        return;
    }
    locked = !app.records.Flag(name.c_str());          // 0x000F3B10
}

void ExtrasSection::AddItem(App& app, const char* key, const char* value) {   // orig: 0x0001C580
    if (!value) return;
    char v[512];                                       // (a 0x100-byte buffer in the original: no length check)
    std::snprintf(v, sizeof v, "%s", value);
    for (char* bar = std::strchr(v, '|'); bar; bar = std::strchr(bar, '|')) {
        *bar++ = '\0';
        if (crt::StrNICmp(bar, "Language", 8) == 0) {  // [0x00293DCC]
            const std::string lang = app.text ? app.text->Language() : std::string("English");   // GetLanguage
            if (crt::StrICmp(lang.c_str(), "italian") == 0) return;   // [0x00293DB4]: the item is dropped
            char t[512];
            std::snprintf(t, sizeof t, v, lang.c_str());   // sprintf(t, v, language)
            std::snprintf(v, sizeof v, "%s", t);
            break;
        }
        if (crt::StrNICmp(bar, "Europe", 6) == 0) {    // [0x00293DC4]: App+0xE8 == 0 only (PAL)
            if (app.ntsc) return;
        } else if (crt::StrNICmp(bar, "America", 7) == 0) {   // [0x00293DBC]: App+0xE8 == 1 only
            if (!app.ntsc) return;
        }
    }
    // new(0x18) 0x0001C4A0(this, key, v): +0 key, +4 value, +0xC = 0, +0x14 = this, then 0x0001C060
    auto item = std::make_unique<ExtrasItem>();
    item->key = key ? key : "";
    item->file = v;
    item->section = this;
    // orig: 0x0001C060: the value's base (before its first '.') into +8; its extension gives the kind and the icon
    ExtrasData& x = *app.extras;
    if (const size_t dot = item->file.find('.'); dot != std::string::npos) {
        item->base = item->file.substr(0, dot);
        const std::string ext = item->file.substr(dot + 1);
        if (crt::StrICmp(ext.c_str(), "tga") == 0) {   // [0x00293DB0]
            item->kind = 3;
            item->icon = x.FindThumb(item->file.c_str());   // 0x0001BA30(+4)
            if (!item->icon) {                         // menu/extras/Thumbs_art/<base>.nif (DynamicCast; Assign)
                std::vector<u8> bytes;
                if (app.services.readFile &&
                    app.services.readFile("menu/extras/Thumbs_art/" + item->base + ".nif", bytes)) {
                    ni::NiStream s;
                    if (s.Load(bytes))
                        item->icon = s.TopLevelLinks().empty()
                                         ? nullptr
                                         : dynamic_cast<ni::NiSourceTexture*>(s.ResolveLink(s.TopLevelLinks()[0]));
                }
            }
        } else if (crt::StrICmp(ext.c_str(), "wav") == 0) {   // [0x00293D8C]
            item->kind = 2;
            item->icon = x.wavIcon;                    // DAT_002D994C
        } else if (crt::StrICmp(ext.c_str(), "avi") == 0) {   // [0x00293D88]
            item->kind = 1;
            item->icon = x.aviIcon;                    // DAT_002D99F8
        } else {
            item->kind = 0;
        }
    }
    items.push_back(std::move(item));                  // (the array grows by +0x20 when full)
}

ni::NiSourceTexture* ExtrasData::FindThumb(const char* n) const {   // orig: 0x0001BA30
    for (const auto& t : thumbs)
        if (t && !t->name.empty() && crt::StrICmp(t->name.c_str(), n) == 0) return t.Get();
    return nullptr;
}

void LoadExtrasThumbs(App& app, ni::NiStream& s) {     // orig: 0x0001CF00
    ExtrasData& x = *app.extras;
    for (u32 link : s.TopLevelLinks()) {               // (+0x54)[i], i < +0x5C
        ni::NiPointer<ni::NiSourceTexture> t(dynamic_cast<ni::NiSourceTexture*>(s.ResolveLink(link)));   // 0x002F9AAC
        if (t && !t->name.empty()) x.thumbs.push_back(t);   // GetName; SetAt(count) (grown by +0x14)
    }
}

void AcquireExtras(App& app) {                         // orig: 0x0001C900
    ExtrasData& x = *app.extras;
    ++x.users, ++x.users2;
    if (x.users == 1) {
        std::vector<u8> file;                          // NiUDSStream("menu/extras/unlock.txt", 0)
        if (!app.services.readFile || !app.services.readFile("menu/extras/unlock.txt", file)) return;   // (no db)
        file.push_back(0);                             // new(size + 1), Read, the NUL
        char* p = reinterpret_cast<char*>(file.data());
        while (p) {
            char* s = std::strchr(p, '[');
            if (!s) break;
            *s++ = '\0';
            char* e = std::strchr(s, ']');
            if (!e) return;                            // (0x0001CEB2: the stream closed, the buffer and the db skipped)
            *e++ = '\0';
            x.sections.push_back(std::make_unique<ExtrasSection>());   // new(0x24) 0x0001BA90(name)
            ExtrasSection& sec = *x.sections.back();
            sec.name = s;
            char* next = std::strchr(e, '[');          // (once per section)
            char* q = e;
            for (;;) {
                if (next && q >= next) break;
                char* k = std::strchr(q, '"');
                if (!k || (next && k >= next)) break;
                *k++ = '\0';
                char* ke = std::strchr(k, '"');
                if (!ke) return;
                if (next && ke >= next) break;
                *ke = '\0';
                q = ke + 1;
                char* eq = std::strchr(k, '=');
                if (!eq) return;
                *eq++ = '\0';
                if (crt::StrICmp(k, "@AVI_ICON") == 0) {   // [0x00293E6C]
                    x.aviIcon = x.FindThumb((std::string(eq) + ".tga").c_str());   // "%s.tga", 0x0001BA30
                    if (!x.aviIcon) x.aviIcon = LoadFirstTexture(app, "menu/extras/Thumbs_video/" + std::string(eq) + ".nif");
                } else if (crt::StrICmp(k, "@WAV_ICON") == 0) {   // [0x00293E38]
                    x.wavIcon = x.FindThumb((std::string(eq) + ".tga").c_str());
                    if (!x.wavIcon) x.wavIcon = LoadFirstTexture(app, "menu/extras/Thumbs_music/" + std::string(eq) + ".nif");
                } else {
                    sec.AddItem(app, k, eq);           // 0x0001C580
                }
            }
            p = next;
        }
    }
    if (app.text) (void)app.text->LoadDb("Language/EXTRAS.ldb");   // [0x00293E04]
}

void ReleaseExtras(App& app) {                         // orig: 0x0001C3B0
    ExtrasData& x = *app.extras;
    --x.users;
    if (--x.users2 == 0) x.thumbs.clear();             // 0x0001B750 (each released) + 0x0001BE10
    if (x.users == 0) x.sections.clear();              // each section's vf00(1): 0x0001C270 (its items deleted)
    x.aviIcon = nullptr;                               // (every call)
    x.wavIcon = nullptr;
}

// ---- the menus ------------------------------------------------------------------------------------------------------

void ExtrasMenu::ReplaceTexturing(ni::NiAVObject* o, ni::NiTexturingProperty* property) {
    if (!o) return;
    ni::NiProperty* old = TexturingOf(*o);             // GetProperty(8) (twice), IsKindOf 0x002F94AC
    if (!old || !dynamic_cast<ni::NiTexturingProperty*>(old)) return;
    ni::NiPointer<ni::NiProperty> keep(old);
    auto& list = o->properties;                        // +0x18: Remove 0x0001E020, AddHead 0x00013A50
    list.erase(std::find_if(list.begin(), list.end(), [&](const auto& p) { return p.Get() == old; }));
    list.insert(list.begin(), ni::NiPointer<ni::NiProperty>(property));
}

ni::NiTexturingProperty* ExtrasMenu::NewCellTexturing() {
    auto* t = new ni::NiTexturingProperty;             // new(0x3C) 0x00109DC0
    t->applyMode = 2;                                  // +0x18 = 2
    if (!t->HasMap(0)) t->SetBaseTexture(nullptr);     // SetBaseFilterMode(2) 0x0001D6B0 (makes the base map)
    t->maps[0].filterMode = 2;
    return t;
}

ExtrasMenu::ExtrasMenu(App& a, ni::NiStream& s) : app(a) {   // orig: 0x0001D020
    // the first NiNode among the top-level objects: +0x20, its keyframe manager new(0xC0) 0x00130D20 -> +0x18
    for (u32 link : s.TopLevelLinks())
        if (auto* n = dynamic_cast<ni::NiNode*>(s.ResolveLink(link))) {
            root = n;
            manager = new ni::NiKeyframeManager(n);
            break;
        }
    if (!root) return;
    // the sequences (RTTI 0x002FA124): "sound1", "sound2", "wheel" loop, the others clamp (0x0001BC10); AddSequence
    for (u32 link : s.TopLevelLinks()) {
        auto* seq = dynamic_cast<ni::NiSequence*>(s.ResolveLink(link));
        if (!seq) continue;
        const char* n = seq->name.c_str();             // +8
        const bool loop = crt::StrICmp(n, "sound1") == 0 || crt::StrICmp(n, "sound2") == 0 ||
                          crt::StrICmp(n, "wheel") == 0;
        SetSequenceLoop(seq, loop);
        manager->AddSequence(seq, true);
    }
    root->UpdateDownwardPass(0.0f, true);              // Update(0, 1)
    root->UpdateProperties();
    root->UpdateEffects();
    cameraNode = FindAs<ni::NiNode>(root.Get(), "CAMERA");   // +0x1C
    if (!cameraNode) return;
    camera = FindAs<ni::NiCamera>(root.Get(), "CAMERA1");    // +0x28 (RTTI 0x002F9120)
    if (!camera) return;
    camera->renderer = app.services.renderer;          // +0x1A8 = App->vf14()
    // +0x15C = the root (0x0001D377, AddRef: the Click's scene; not owning here, the root holds the camera). Read by
    // the picture splash's Update (0x000EED16: Click of this camera), which draws the grid's scene before the tiles.
    camera->sceneRoot = root.Get();
    manager->cumulativeAnimations = false;             // (+0x18)+0x4C = 0
    sequenceTime = 0;                                  // +0x40; +0x2C / +0x30 / +0x38 / +0x3C / +0x34 = 0
    // +0x50: TextObject::Create(0), SetRenderer(App->vf14()), the scene mode (+0x28, +0x20), hAlign 1, vAlign 3,
    // width 10000, no wrap, scale (1.7, 2.55), letter spacing 0.008
    title.reset(text::TextObject::Create(0, app.services.readFile, app.services.renderer));
    if (title) {
        title->SetRenderer(app.services.renderer);
        title->UseScene(camera.Get(), root.Get());     // 0x00099BE0
        title->style.hAlign = 1;                       // +0x60
        title->style.vAlign = 3;                       // +0x64
        title->width = std::bit_cast<f32>(0x461C4000u);   // +0x54 = 10000.0
        title->style.wordWrap = false;                 // +0x68
        title->SetScale(std::bit_cast<f32>(0x3FD9999Au), std::bit_cast<f32>(0x40233334u));   // (1.7, 2.55)
        title->style.letterSpacing = std::bit_cast<f32>(0x3C03126Fu);   // +4 = 0.008
    }
    // +0x54: TextObject::Create(0), SetRenderer, hAlign 0, vAlign 3, no wrap, SetZBufferTest(0), scale (0.75, 1.0)
    text.reset(text::TextObject::Create(0, app.services.readFile, app.services.renderer));
    if (text) {
        text->SetRenderer(app.services.renderer);
        text->style.hAlign = 0;
        text->style.vAlign = 3;
        text->style.wordWrap = false;
        text->SetZBufferTest(false);                   // 0x00099660(0)
        text->SetScale(std::bit_cast<f32>(0x3F400000u), std::bit_cast<f32>(0x3F800000u));
    }
    scrollLimitSound = app.LoadSound("sounds/menu/scrolllimit.wav", 0);   // +0x34
    scrollSound = app.LoadSound("sounds/menu/scroll.wav", 0);             // +0x2C
    selectSound = app.LoadSound("sounds/menu/select.wav", 0);             // +0x30
    foldUpSound = app.LoadSound("sounds/menu/Pause_fold_up.wav", 0);      // +0x38
    foldDownSound = app.LoadSound("sounds/menu/Pause_fold_down.wav", 0);  // +0x3C
    hints = app.text ? app.text->LoadDb("Language/HINTS_XBOX.ldb") : nullptr;   // +0x58
    AcquireExtras(app);                                // 0x0001C900
}

ExtrasMenu::~ExtrasMenu() {                            // orig: 0x0001C740
    // (the original reaches here only for a menu whose ctor ran to the end: the early returns leave +0x50 / +0x54
    // unset, which this dtor would read; the port tests them)
    if (title) title->RemoveAll();                     // 0x00099080
    if (text) text->RemoveAll();
    if (camera) camera->sceneRoot = nullptr;           // (+0x28)+0x15C released (the camera's scene)
    title.reset();
    text.reset();
    sound::GameSound& snd = app.Sound();               // Sound_Release +0x34, +0x2C, +0x30, +0x38, +0x3C
    for (u32 h : {scrollLimitSound, scrollSound, selectSound, foldUpSound, foldDownSound}) snd.Release(h);
    ReleaseExtras(app);                                // 0x0001C3B0
    camera = nullptr;                                  // +0x28
    field24 = nullptr;                                 // +0x24
    root = nullptr;                                    // +0x20 (the scene and its lights)
    cameraNode = nullptr;                              // +0x1C
    manager = nullptr;                                 // +0x18
}

ThumbGrid::ThumbGrid(App& a, ni::NiStream& s) : ExtrasMenu(a, s) {   // orig: 0x0001E130
    // +0xF8 the item array (capacity 1, growth 1), DAT_002D9A70 = this (the Frontend's slot), +0xF0 / +0xEC / +0xF4
    if (!root) return;
    for (s32 r = 1; r <= 5; ++r)                       // the 15 cells "frame_<c>_<r>" (else "frame<c>_<r>")
        for (s32 c = 1; c <= 3; ++c) {
            char n[32];
            ni::NiPointer<ni::NiSwitchNode>& f = frames[size_t((r - 1) * 3 + (c - 1))];
            std::snprintf(n, sizeof n, "frame_%d_%d", c, r);   // [0x00293FF8]
            f = FindAs<ni::NiSwitchNode>(root.Get(), n);
            if (f) continue;
            std::snprintf(n, sizeof n, "frame%d_%d", c, r);    // [0x00293FEC]
            f = FindAs<ni::NiSwitchNode>(root.Get(), n);
        }
    // each cell's texturing property: the geometry under every NiNode child of the cell's switch gets it
    for (size_t k = 0; k < cells.size(); ++k) {
        cells[k] = NewCellTexturing();
        ni::NiSwitchNode* f = frames[k].Get();         // (read without a test in the original)
        if (!f) continue;
        for (const auto& child : f->Children()) {      // +0xC4 / +0xCC
            auto* n = dynamic_cast<ni::NiNode*>(child.Get());   // IsKindOf 0x002F9230
            if (!n) continue;
            for (const auto& g : n->Children())
                if (g) ReplaceTexturing(g.Get(), cells[k].Get());
        }
    }
    arrowUp = FindAs<ni::NiSwitchNode>(root.Get(), "arrow_up");      // +0xD8
    arrowDown = FindAs<ni::NiSwitchNode>(root.Get(), "arrow_down");  // +0xDC
    headline = FindAs<ni::NiNode>(root.Get(), "text_headline_movie");   // +0xE0
    textNode = FindAs<ni::NiNode>(root.Get(), "text");                  // +0xE4
    root->UpdateProperties();
    // menu/extras/extra_default.nif's first object (IsKindOf NiSourceTexture) -> +0xD4, every cell's base texture
    std::vector<u8> bytes;
    if (!app.services.readFile || !app.services.readFile("menu/extras/extra_default.nif", bytes)) return;
    ni::NiStream d;
    if (!d.Load(bytes)) return;
    defaultTexture = d.TopLevelLinks().empty() ? nullptr
                                               : dynamic_cast<ni::NiSourceTexture*>(d.ResolveLink(d.TopLevelLinks()[0]));
    for (auto& c : cells) c->SetBaseTexture(defaultTexture.Get());   // 0x0001D610
    root->UpdateDownwardPass(0.0f, true);
    root->UpdateProperties();
    root->UpdateEffects();
}

ThumbGrid::~ThumbGrid() {                              // orig: 0x0001D740
    // +0xEC (the splash) deleted, DAT_002D9A70 = 0 (the Frontend's slot: its owner), +0xFC freed
    items.clear();
    textNode = nullptr, headline = nullptr, arrowDown = nullptr, arrowUp = nullptr, defaultTexture = nullptr;
    for (size_t i = cells.size(); i-- > 0;) cells[i] = nullptr;    // (the vector destructor: last first)
    for (size_t i = frames.size(); i-- > 0;) frames[i] = nullptr;
}

MusicList::MusicList(App& a, ni::NiStream& s) : ExtrasMenu(a, s) {   // orig: 0x0001F360
    // +0x7C the item array, +0x94..+0xA8 = 0, DAT_002D9B98 = this, +0xAC = 0 (no root test in the original)
    if (!root) return;
    for (s32 i = 1; i <= 4; ++i) {
        char n[32];
        std::snprintf(n, sizeof n, "box_%d", i);       // [0x002940C4] (+0x6C [i - 1], NiSwitchNode)
        boxes[size_t(i - 1)] = FindAs<ni::NiSwitchNode>(root.Get(), n);
        if (!boxes[size_t(i - 1)]) {
            std::snprintf(n, sizeof n, "box%d", i);
            boxes[size_t(i - 1)] = FindAs<ni::NiSwitchNode>(root.Get(), n);
        }
        std::snprintf(n, sizeof n, "text_%d", i);      // [0x002940B4] (+0x5C [i - 1], NiNode)
        texts[size_t(i - 1)] = FindAs<ni::NiNode>(root.Get(), n);
        if (!texts[size_t(i - 1)]) {
            std::snprintf(n, sizeof n, "text%d", i);
            texts[size_t(i - 1)] = FindAs<ni::NiNode>(root.Get(), n);
        }
    }
    playStop = FindAs<ni::NiSwitchNode>(root.Get(), "play_stop");    // +0x94
    upArrow = FindAs<ni::NiSwitchNode>(root.Get(), "up_arrow");      // +0x98
    downArrow = FindAs<ni::NiSwitchNode>(root.Get(), "down_arrow");  // +0x9C
    headline = FindAs<ni::NiNode>(root.Get(), "text_headline_audio");   // +0xA0
    thumbnail = NewCellTexturing();                    // +0xA4
    ni::NiNode* t = FindAs<ni::NiNode>(root.Get(), "thumbnail");
    if (!t) return;
    for (const auto& c : t->Children())
        if (c) ReplaceTexturing(c.Get(), thumbnail.Get());
    root->UpdateDownwardPass(0.0f, true);
    root->UpdateProperties();
    root->UpdateEffects();
    voice = 0, sample = 0;                             // +0xB4 / +0xB0
    // NiRenderedTexture::Create(256, 256, App->vf14(), the preferences 0x002D7BA8) -> +0xA8
    target = ni::CreateRenderedTexture(256, 256, app.services.renderer);
}

MusicList::~MusicList() {                              // orig: 0x0001EAF0 (DAT_002D9B98 = 0 first: the Frontend's slot)
    target = nullptr, thumbnail = nullptr, headline = nullptr;   // +0xA8, +0xA4, +0xA0
    downArrow = nullptr, upArrow = nullptr, playStop = nullptr;  // +0x9C, +0x98, +0x94
    items.clear();                                     // +0x80
    for (size_t i = boxes.size(); i-- > 0;) boxes[i] = nullptr;
    for (size_t i = texts.size(); i-- > 0;) texts[i] = nullptr;
}

// ---- the menus' updates ---------------------------------------------------------------------------------------------

namespace {

FrontendContext* Fe(App& app) {                        // orig: FindContext("Frontend") 0x000A3120 (0x00293D6C)
    return static_cast<FrontendContext*>(app.FindContext("Frontend"));
}

void ResetIdle(App& app) {                             // orig: 0x0001B690 (Frontend+0x7C = 0)
    if (FrontendContext* fe = Fe(app)) fe->idleTime = 0;
}

void SetSwitch(ni::NiSwitchNode* s, s32 index) {       // (+0xE0 = index when +0xCC > index)
    if (s && s32(s->Children().size()) > index) s->index = index;
}

}  // namespace

void ExtrasMenu::BaseOpen(s32 m) {                     // orig: 0x0001C890
    mode = m;                                          // +4
    shown = true;                                      // +9
    sequence = nullptr;                                // +0x14
    prev = -1;                                         // +0x10
    cursor = 0;                                        // +0xC
    sequenceTime = 0;                                  // +0x40
    closeRequested = false;                            // +8
    sequence = manager ? manager->GetSequence("Dialog_menu_enter") : nullptr;   // 0x0001BFC0 (0x00293DF0)
    if (sequence) {
        sequence->offset = F(-double(sequenceTime));   // FLD +0x40; FCHS; FSTP +0xA8
        manager->ActivateSequence(sequence, 0);        // 0x0012F3A0
    }
    (void)app.Sound().PlaySfx(foldUpSound, false);     // Sound_PlaySFX(+0x38, 0)
}

void ExtrasMenu::Close(const char* name) {             // orig: 0x0001C310
    sequence = manager ? manager->GetSequence(name) : nullptr;   // +0x14
    if (!sequence) return;
    sequence->offset = F(-double(sequenceTime));
    manager->ActivateSequence(sequence, 0);
}

void ExtrasMenu::StartSequence(const char* name) {     // orig: 0x0001C350
    ni::NiSequence* q = manager ? manager->GetSequence(name) : nullptr;
    if (!q) return;
    q->offset = F(-double(sequenceTime));
    manager->ActivateSequence(q, 0);
}

void ExtrasMenu::LoopSequence(const char* name, bool loop) {   // orig: 0x0001C380
    if (ni::NiSequence* q = manager ? manager->GetSequence(name) : nullptr) SetSequenceLoop(q, loop);   // 0x0001BC10
}

void ExtrasMenu::BaseUpdate() {                        // orig: 0x0001BB30
    if (!shown) return;
    if (sequence) {
        if (const ni::NiTimeController* c = sequence->ReferenceController()) {   // (+0x48)[+0x90]
            // FLD +0x20; FSUB +0x1C; FDIV +0x14; FSUB seq+0xA8; FCOMP +0x40; TEST AH,0x41; JE: running while above
            const double end = (double(c->hiKeyTime) - c->loKeyTime) / c->frequency - double(sequence->offset);
            if (end > double(sequenceTime)) {
                sequenceTime = F(double(g_frameStep) + sequenceTime);
                return;
            }
            if (manager) manager->DeactivateSequence(sequence);   // 0x0012FAB0
        }
        sequence = nullptr;
    }
    if (closeRequested) {
        // (App+0x100: Mem_ReturnTrue)
        if (title) title->RemoveAll();                 // (+0x50) 0x00099080
        Hide();                                        // vf10
        return;
    }
    sequenceTime = F(double(g_frameStep) + sequenceTime);   // FLD dt; FADD +0x40; FSTP
}

void ExtrasMenu::BaseDraw() {                          // orig: 0x0001BBB0
    if (!shown) return;
    if (title) title->UpdateCamera();                  // (+0x50) 0x00098D70
    if (camera && camera->renderer) camera->renderer->ClearBuffer(nullptr, 4);   // (+0x28)+0x1A8 vf44(0, 4)
    if (manager) manager->Update(sequenceTime);        // (+0x18)->vf38(+0x40)
    if (root) {
        root->UpdateDownwardPass(sequenceTime, true);  // (+0x20)->Update(+0x40, 1)
        root->UpdateWorldBound();                      // vf98
    }
    if (camera) ni::VisitForCulling(*camera, root.Get());   // (+0x28) Click 0x00100CE0 (+0x15C = the root)
}

void ThumbGrid::Open(s32 m) {                          // orig: 0x0001E960
    BaseOpen(m);                                       // 0x0001C890
    playing = false;                                   // +0xF4
    headlineId = 0;                                    // +0xE8
    if (title) {
        title->RemoveAll();                            // (+0x50) 0x00099080
        title->style.hAlign = 0;                       // +0x60
        title->color = ni::NiColor{1.0f, 1.0f, 0.0f};  // +0x6C..+0x74 (yellow)
        const char* id = mode == 1 ? "LABEL_019" : mode == 2 ? "LABEL_020" : mode == 3 ? "LABEL_021" : nullptr;
        if (id && app.text)                            // 0x00294004 / 0x00294010 / 0x0029401C: GetText(id, 0)
            (void)title->AddStringAt(app.text->GetText(id, nullptr), headline.Get(), -1.0f, -1.0f);   // 0x0009A0A0
        title->color = ni::NiColor{1.0f, 1.0f, 1.0f};
    }
    items.clear();                                     // (every slot nulled, count 0, the non-null count 0)
    for (const auto& sec : app.extras->sections) {     // DAT_002D5C44 (count DAT_002D5C4C)
        if (!sec) continue;
        sec->Refresh(app);                             // 0x0001B630
        for (const auto& it : sec->items)              // +0x10 (count +0x18): every item of this kind, locked too
            if (it && it->kind == mode) items.push_back(it.get());
    }
}

void ThumbGrid::Hide() {                               // orig: 0x0001DF90
    ExtrasMenu::Hide();                                // 0x0001B670
    items.clear();                                     // (+0xFC slots nulled, +0x104 = +0x108 = 0)
}

void ThumbGrid::Update() {                             // orig: 0x0001D880 (five columns, three rows visible)
    FrontendContext* fe = Fe(app);
    if (double(cooldown) > 0.0) cooldown = F(double(cooldown) - g_frameStep);   // FCOMP 0; TEST AH,0x41; JNE
    if (splash) {                                      // +0xEC: a picture is shown
        ResetIdle(app);                                // 0x0001B690
        (void)splash->Update(app, g_frameStep);        // vf04(dt)
        if (MenuInput(app, 5) && !(double(cooldown) > 0.0)) {   // TEST AH,0x41; JP: on <= 0
            cooldown = 0.5f;                           // 0x3F000000
            splash.reset();                            // vf00(1)
        }
        return;                                        // (the cursor's previous value is not updated)
    }
    if (text) text->RemoveAll();                       // (+0x54) 0x00099080
    if (playing) {                                     // +0xF4: a movie
        ResetIdle(app);
        if ((MenuInput(app, 5) || MenuInput(app, 4)) && !(double(cooldown) > 0.0))
            (void)app.PlayMovie(nullptr, 0x0001D97B);  // App vf18(0)
        if (!app.IsMoviePlaying()) {                   // App vf1C
            cooldown = 1.5f;                           // 0x3FC00000
            playing = false;
            if (fe) fe->RestartMusic();                // 0x0001B6B0
        }
        return;
    }
    BaseUpdate();                                      // 0x0001BB30
    const s32 count = s32(items.size());               // +0x104
    if (!sequence) {
        if (MenuInput(app, 5)) {
            closeRequested = true;
            Close("Dialog_manu_leave");                // 0x0001C310 (0x00293F84, sic)
            (void)app.Sound().PlaySfx(foldDownSound, false);   // +0x3C
            return;
        }
        if (count == 0) {                              // nothing of this kind: close
            closeRequested = true;
            return;
        }
        if (MenuInput(app, 4) && !(double(cooldown) > 0.0)) {
            ExtrasItem* it = items[size_t(cursor)];    // (+0xFC)[+0xC]
            if (it && !it->base.empty()) {             // (+8: the name without its extension)
                cooldown = 1.5f;
                if (it->Locked()) {                    // 0x0001B620
                    (void)app.Sound().PlaySfx(scrollLimitSound, false);   // +0x34
                } else if (it->kind == 1) {            // a movie
                    if (!playing) {
                        playing = true;
                        if (fe) fe->StopMusic();       // 0x0001B6F0
                        (void)app.PlayMovie(it->base.c_str(), 0x0001DB1B);   // App vf18(+8)
                    }
                } else if (it->kind == 3) {            // a picture
                    (void)app.Sound().PlaySfx(selectSound, false);   // +0x30
                    char path[0x104];
                    const std::string lang = app.text ? app.text->Language() : std::string("English");
                    std::snprintf(path, sizeof path, "images/extras/%s/%s.nif", lang.c_str(), it->base.c_str());   // 0x00293F6C
                    splash = std::make_unique<Splash>(app, path, 0.0f, 0.0f, camera.Get());   // new(0x3C) 0x000EFBC0(.., +0x28, 0, 0)
                }
            }
        }
        repeatTimer = F(double(repeatTimer) - g_frameStep);   // +0x48
        // a direction acts when it differs from the last one (+0x4C) or the repeat timer is below 0 (TEST AH,5; JP)
        auto ready = [&](s32 d) { return repeatDir != d || double(repeatTimer) < 0.0; };
        if (MenuInput(app, 0)) {                       // up: a row up, wrapped into the column
            if (ready(0)) {
                repeatTimer = 0.25f, repeatDir = 0;
                cursor -= 5;
                while (cursor < 0) cursor += 5;        // (JNS; ADD 5 while < 0)
            }
        } else if (MenuInput(app, 1)) {                // down: a row down
            if (ready(1)) {
                repeatTimer = 0.25f, repeatDir = 1;
                const s32 c = cursor + 5;
                cursor = c;
                if (c >= count) cursor = (c % 5 < count % 5) ? c - 5 : count - 1;   // (IDIV remainders)
            }
        } else if (MenuInput(app, 2)) {                // left: within the row
            if (ready(2)) {
                repeatDir = 2, repeatTimer = 0.25f;
                const s32 c = cursor - 1;
                cursor = c;
                if (c % 5 == 4) cursor = c + 5;
                if (cursor < 0) cursor += 5;
                if (cursor >= count) cursor = count - 1;
                if (cursor < 0) cursor = 0;
            }
        } else if (MenuInput(app, 3)) {                // right: within the row
            if (ready(3)) {
                repeatDir = 3, repeatTimer = 0.25f;
                const s32 c = cursor + 1;
                cursor = c;
                if (c % 5 == 0) cursor = c - 5;
                if (cursor >= count) cursor -= cursor % 5;
            }
        }
        if (!MenuInput(app, 0) && !MenuInput(app, 1) && !MenuInput(app, 2) && !MenuInput(app, 3)) repeatDir = 4;
    }
    if (prev != cursor) {                              // the cursor moved (or the menu opened): the visible cells
        (void)app.Sound().PlaySfx(scrollSound, false); // +0x2C
        while (cursor < top) top -= 5;                 // +0xF0: the first visible row
        while (cursor >= top + 15) top += 5;
        if (arrowUp && arrowDown) {                    // +0xD8 / +0xDC
            SetSwitch(arrowDown.Get(), u32(count - top) > 15u ? 1 : 0);   // (CMP 0xF; JBE: unsigned)
            SetSwitch(arrowUp.Get(), top > 0 ? 1 : 0);
        }
        size_t k = 0;                                  // the cells column by column (frame_<row>_<column>)
        for (s32 c = 0; c < 5; ++c)
            for (s32 r = 0; r < 15; r += 5, ++k) {
                const s32 i = top + r + c;
                ExtrasItem* it = i < count ? items[size_t(i)] : nullptr;
                if (it && it->Locked()) it = nullptr;  // (a locked item shows the default picture)
                ni::NiSwitchNode* frame = frames[k].Get();
                if (i == cursor) {
                    SetSwitch(frame, 1);
                    if (headlineId && title) title->RemoveString(headlineId);   // (+0x50) 0x00098FD0
                    headlineId = 0;
                    const char* id = it ? it->key.c_str() : "EXTRA_LOCKED";   // 0x00293F5C
                    if (title && app.text)             // GetText(id, 0) at "text" (+0xE4)
                        headlineId = title->AddStringAt(app.text->GetText(id, nullptr), textNode.Get(), -1.0f, -1.0f);
                } else {
                    SetSwitch(frame, 0);
                }
                ni::NiTexturingProperty* cell = cells[k].Get();   // (+0x98 [k])
                if (i < count) {
                    if (cell) cell->SetBaseTexture(it ? it->icon.Get() : defaultTexture.Get());   // 0x0001D610
                    if (frame) frame->flags = u16(frame->flags & ~ni::kNiAVHidden);   // +0xB0 &= ~1
                } else {
                    if (cell) cell->SetBaseTexture(nullptr);
                    if (frame) frame->flags = u16(frame->flags | ni::kNiAVHidden);    // +0xB0 |= 1
                }
            }
    }
    prev = cursor;                                     // +0x10
}

void ThumbGrid::Draw() {                               // orig: 0x0001D530
    BaseDraw();                                        // 0x0001BBB0
    if (splash) DrawHint("HINTS_EX_001");              // (+0xEC) 0x0001BCF0(0x00293F28)
}

void ExtrasMenu::DrawHint(const char* id) {            // orig: 0x0001BCF0
    const char16_t* s = app.text ? app.text->GetText(id, hints) : nullptr;   // TextManager::Get(id, +0x58)->GetText
    if (!text) return;                                 // (+0x54: the original has no test)
    text->RemoveAll();                                 // 0x00099080
    text->UpdateCamera();                              // 0x00098D70
    text->x = std::bit_cast<f32>(0x3F000000u);         // +0x4C = 0.5
    text->y = std::bit_cast<f32>(0x3DF5C28Fu);         // +0x50 = 0.12
    if (s) (void)text->AddString(s, -1.0f);            // 0x0009A070(s, -1.0)
    text->UpdateCamera();                              // 0x00098D70
    text->Click();                                     // 0x00098DB0
}

void MusicList::Open(s32 m) {                          // orig: 0x0001FAD0
    SetSwitch(playStop.Get(), 0);                      // (+0x94) the playing icon off
    sample = 0, voice = 0, top = 0;                    // +0xB0 / +0xB4 / +0xAC
    BaseOpen(m);                                       // 0x0001C890
    if (title) {
        title->RemoveAll();
        title->style.hAlign = 0;                       // +0x60
    }
    items.clear();
    for (const auto& sec : app.extras->sections) {
        if (!sec) continue;
        sec->Refresh(app);                             // 0x0001B630
        if (sec->locked) continue;                     // (+4: only unlocked sections' items are listed)
        for (const auto& it : sec->items)
            if (it && it->kind == mode) items.push_back(it.get());   // 0x000A4A20
    }
}

void MusicList::Hide() {                               // orig: 0x0001F2C0
    ExtrasMenu::Hide();                                // 0x0001B670
    items.clear();
    if (voice) app.Sound().StopVoice(voice);           // Voice_Stop(+0xB4)
    if (sample) app.Sound().Release(sample);           // Sound_Release(+0xB0)
    if (FrontendContext* fe = Fe(app)) fe->RestartMusic();   // 0x0001B6B0
}

void MusicList::Update() {                             // orig: 0x0001EC30 (four rows)
    FrontendContext* fe = Fe(app);
    if (double(cooldown) > 0.0) cooldown = F(double(cooldown) - g_frameStep);
    if (voice) {                                       // +0xB4: a track plays (as music)
        ResetIdle(app);
        if ((MenuInput(app, 5) || MenuInput(app, 4)) && !(double(cooldown) > 0.0)) {
            cooldown = 0.5f;
            app.Sound().StopVoice(voice);              // Voice_Stop
        }
        if (!app.Sound().VoiceExists(voice)) {         // Voice_Exists
            const u32 s = sample;
            voice = 0;
            if (!fe || s != fe->musicSound) app.Sound().Release(s);   // (FE+0x1AC: the front end's own music)
            sample = 0;
            SetSwitch(playStop.Get(), 0);              // (+0x94)
            LoopSequence("sound1", false);             // 0x0001C380 (0x00293F20 / 18 / 10)
            LoopSequence("sound2", false);
            LoopSequence("wheel", false);
        }
    }
    BaseUpdate();                                      // 0x0001BB30
    const s32 count = s32(items.size());               // +0x88
    if (!sequence && !voice) {
        sequenceTime = 0;                              // +0x40 = 0
        if (MenuInput(app, 5)) {
            closeRequested = true;
            Close("Dialog_manu_leave");                // 0x0001C310 (0x00293F84)
            (void)app.Sound().PlaySfx(foldDownSound, false);   // +0x3C
            return;
        }
        if (count == 0) {
            closeRequested = true;
            return;
        }
        if (MenuInput(app, 4) && !(double(cooldown) > 0.0) && items[size_t(cursor)] &&
            !items[size_t(cursor)]->base.empty()) {
            ExtrasItem* it = items[size_t(cursor)];
            (void)app.Sound().PlaySfx(selectSound, false);   // +0x30
            cooldown = 0.5f;
            char path[0x104];
            std::snprintf(path, sizeof path, "sounds/%s.wav", it->base.c_str());   // 0x00294058
            if (fe) fe->StopMusic();                   // 0x0001B6F0
            sample = app.LoadSound(path, 1);           // Sound_Load(path, 1) -> +0xB0
            voice = app.Sound().PlayMusic(sample, true);   // Sound_PlayMusic(+0xB0, 1) -> +0xB4
            if (voice) SetSwitch(playStop.Get(), 1);
            StartSequence("sound1");                   // 0x0001C350
            StartSequence("sound2");
            StartSequence("wheel");
            LoopSequence("sound1", true);              // 0x0001C380(.., 1)
            LoopSequence("sound2", true);
            LoopSequence("wheel", true);
        }
        repeatTimer = F(double(repeatTimer) - g_frameStep);   // +0x48
        auto ready = [&](s32 d) { return repeatDir != d || double(repeatTimer) < 0.0; };
        if (MenuInput(app, 0)) {
            if (ready(0)) {
                repeatTimer = 0.25f, repeatDir = 0;
                if (--cursor < 0) cursor = 0;
            }
        } else if (MenuInput(app, 1)) {
            if (ready(1)) {
                repeatTimer = 0.25f, repeatDir = 1;
                if (++cursor >= count) cursor = count - 1;
            }
        }
        if (!MenuInput(app, 0) && !MenuInput(app, 1)) repeatDir = 4;
    }
    if (prev != cursor) {
        (void)app.Sound().PlaySfx(scrollSound, false); // +0x2C
        while (cursor < top) --top;                    // +0xAC
        while (cursor >= top + 4) ++top;
        if (upArrow && downArrow) {                    // +0x98 / +0x9C
            SetSwitch(downArrow.Get(), u32(count - top) > 4u ? 1 : 0);
            SetSwitch(upArrow.Get(), top > 0 ? 1 : 0);
        }
        if (title) {
            title->RemoveAll();                        // (+0x50)
            title->color = ni::NiColor{1.0f, 1.0f, 0.0f};
            if (app.text)                              // GetText("LABEL_020", 0) (0x00294010) at the headline
                (void)title->AddStringAt(app.text->GetText("LABEL_020", nullptr), headline.Get(), -1.0f, -1.0f);
            title->color = ni::NiColor{1.0f, 1.0f, 1.0f};
        }
        for (s32 i = 0; i < 4; ++i) {
            const s32 idx = top + i;
            if (idx > count - 1) break;
            ExtrasItem* it = items[size_t(idx)];
            if (!it) continue;
            if (idx == cursor) {
                SetSwitch(boxes[size_t(i)].Get(), 1);
                if (thumbnail) {                       // (+0xA4) the front end's music shows the render target
                    if (target && crt::StrICmp(it->base.c_str(), "Music/01 - front-end") == 0)   // 0x00294040
                        thumbnail->SetBaseTexture(target.Get());
                    else
                        thumbnail->SetBaseTexture(it->icon.Get());
                }
            } else {
                SetSwitch(boxes[size_t(i)].Get(), 0);
            }
            const char* id = it->Locked() ? "EXTRA_LOCKED" : it->key.c_str();
            if (title && app.text)
                (void)title->AddStringAt(app.text->GetText(id, nullptr), texts[size_t(i)].Get(), -1.0f, -1.0f);
        }
    }
    prev = cursor;
}

void MusicList::Draw() {                               // orig: 0x0001F1A0
    if (target) {
        ni::NiPointer<ni::NiRenderedTexture> t = target;   // (a reference held; +0xA8 cleared meanwhile)
        target = nullptr;
        if (ni::NiRenderer* r = app.services.renderer) {
            r->SetRenderTarget(t.Get());               // App->vf14() vf3C(target)
            r->ClearBuffer(nullptr, 5);                // vf44(0, 5)
            if (FrontendContext* fe = Fe(app)) fe->Render();   // FindContext("Frontend")->vf0C
            r->SetRenderTarget(nullptr);               // vf3C(0)
            r->ClearBuffer(nullptr, 4);                // vf44(0, 4)
        }
        target = t;                                    // +0xA8 again
    }
    BaseDraw();                                        // 0x0001BBB0
}

}  // namespace fut::game::app
