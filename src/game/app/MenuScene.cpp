// The menus' scene helpers; see MenuScene.h.
#include "game/app/MenuScene.h"

#include "core/CrtString.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiRenderer.h"
#include "game/app/App.h"
#include "game/app/MemoryCards.h"
#include "game/text/LanguageDb.h"
#include "game/text/TextObject.h"

#include <bit>
#include <cstdio>

namespace fut::game::app {

using ni::detail::F;

void PreCacheScene(ni::NiRenderer* renderer, ni::NiAVObject* object, bool isStatic) {   // orig: 0x000CE9C0
    if (!object) return;
    if (dynamic_cast<ni::NiOutline*>(object)) return;   // IsKindOf(0x0030456C)
    if (auto* node = dynamic_cast<ni::NiNode*>(object)) {   // DynamicCast 0x002F9230
        const size_t n = node->Children().size();     // +0xCC (read once)
        for (size_t i = 0; i < n; ++i)
            PreCacheScene(renderer, i < node->Children().size() ? node->Children()[i].Get() : nullptr, isStatic);
        return;
    }
    auto* g = dynamic_cast<ni::NiGeometry*>(object);  // DynamicCast 0x0030EC0C
    if (!g || !renderer) return;
    // GetControllers 0x0013EC10: animated geometry is never static
    renderer->PrecacheGeometry(*g, g->controller ? false : isStatic);   // vf9C(g, flag, 0, 0, 0)
}

void SetSequenceLoop(ni::NiSequence* seq, bool loop) {   // orig: 0x00022460
    if (!seq) return;
    const u32 type = loop ? 0u : 2u;                  // SETNE; DEC; AND 2
    for (u32 i = 0; i < seq->ControllerCount(); ++i) {
        ni::NiTimeController* c = seq->Controller(i);  // (+0x48)[i] as RTTI 0x0030E628
        if (!c) continue;
        c->flags = u16((c->flags & 0xFFF9) | (type << 1));   // SetCycleType 0x0013EBA0
    }
}

// ---- the labels -----------------------------------------------------------------------------------------------------

MenuLabels::MenuLabels(ni::NiNode* sceneNode, ni::NiCamera* sceneCamera)
    : node(sceneNode), camera(sceneCamera) {}         // orig: 0x00025710 (+0 = 0, +0xC = 0)

MenuLabels::~MenuLabels() = default;                  // orig: 0x00025900 (UnloadDb(+0xC): the App's manager keeps it)

void MenuLabels::Init(App& app) {                     // orig: 0x00025850
    text.reset(text::TextObject::Create(0, app.services.readFile, app.services.renderer));   // Create(0)
    if (!text) return;
    text->SetScale(std::bit_cast<f32>(0x42233333u), std::bit_cast<f32>(0x42200000u));   // (40.8, 40.0)
    text->UseScene(camera.Get(), node.Get());         // 0x00099BE0(+8, +4)
    text->style.vAlign = 4;                           // +0x64
    text->style.hAlign = 0;                           // +0x60
    text->style.wordWrap = false;                     // +0x68
    text->SetZBufferTest(true);                       // 0x00099660(1)
    text->SetRenderer(app.services.renderer);         // 0x00098F80(App->vf14()): nothing in the scene mode
    LoadDb(app);                                      // TextManager::LoadDb("Language/FRONTEND.ldb") -> +0xC
}

void MenuLabels::LoadDb(App& app) {                   // orig: 0x000256D0
    db = app.text ? app.text->LoadDb("Language/FRONTEND.ldb") : nullptr;
}

void MenuLabels::Update() {                           // orig: 0x00025700
    if (text) text->UpdateCamera();                   // 0x00098D70
}

void MenuLabels::RemoveAll() {                        // orig: 0x000256F0
    if (text) text->RemoveAll();                      // 0x00099080
}

void MenuLabels::Build(App& app) {                    // orig: 0x00025970
    text::TextObject* t = text.get();
    if (!t) return;
    text::TextManager* tm = app.text.get();
    auto setColor = [t](f32 r, f32 g, f32 b) { t->color = ni::NiColor{r, g, b}; };   // +0x6C / +0x70 / +0x74
    auto get = [&](const char* key) -> const char16_t* { return tm ? tm->GetText(key, db) : nullptr; };
    auto add = [t](const char16_t* s, ni::NiNode* at) {   // 0x0009A0A0(text, node, -1, -1)
        if (s) t->AddStringAt(s, at, -1.0f, -1.0f);   // (a null text: AddString returns 0 at once)
    };
    auto find = [this](const char* name) { return FindAs<ni::NiNode>(node.Get(), name); };   // (+4)->vf54, NiNode
    const bool german = tm && crt::StrICmp("german", tm->Language().c_str()) == 0;   // GetLanguage; _stricmp
    setColor(1, 1, 0);
    t->style.letterSpacing = std::bit_cast<f32>(0x3E4CCCCDu);   // +4 = 0.2
    const f32 sx = t->style.scaleX, sy = t->style.scaleY;   // +0x58 / +0x5C
    if (german) t->SetScale(F(double(sx) - std::bit_cast<f32>(0x3E99999Au)), sy);   // FSUB [0x00293B18] 0.3
    // the memory-card save panel's "Text_savegame" ((App+0xF4)+4 -> +4 -> +0x18, the save list's scene)
    if (ni::NiNode* at = app.memoryCards ? FindAs<ni::NiNode>(app.memoryCards->panel.scene.Get(), "Text_savegame")
                                         : nullptr) {
        const f32 spacing = t->style.letterSpacing;
        const f32 k = std::bit_cast<f32>(0x3D958106u);   // [0x0029499C] 0.073
        const f32 ny = F(double(sy) * k);              // (the y argument first)
        t->style.letterSpacing = std::bit_cast<f32>(0xBA83126Fu);   // -0.001
        const f32 nx = F(double(sx) * k);
        t->SetScale(nx, ny);
        setColor(1, 1, 0);
        add(get("LABEL_024"), at);
        setColor(1, 1, 1);
        t->SetScale(sx, sy);
        t->style.letterSpacing = spacing;
    }
    // ebx: the last text fetched (the "on" / "off" items reuse it)
    const char16_t* last = nullptr;
    auto item = [&](const char* name, const char* key) {
        if (ni::NiNode* at = find(name)) {
            last = get(key);
            add(last, at);
        }
    };
    setColor(1, 1, 0);
    item("text_headline_loadgame", "LABEL_014");
    setColor(1, 1, 0);
    item("text_headline_pause", "LABEL_016");
    setColor(1, 1, 0);
    item("text_headline_options", "LABEL_001");
    setColor(1, 1, 1);
    item("text_newgame", "LABEL_000");
    item("text_resume", "LABEL_015");
    item("text_options", "LABEL_001");
    item("text_optionsnull", "LABEL_001");
    item("text_loadgame", "LABEL_002");
    item("text_extras", "LABEL_003");
    item("text_quit", "LABEL_017");
    item("text_on", "LABEL_005");
    if (ni::NiNode* at = find("text_on1")) {          // the previous text, then LABEL_006 for the "off" items
        add(last, at);
        last = get("LABEL_006");
    }
    if (ni::NiNode* at = find("text_off1")) add(last, at);
    if (ni::NiNode* at = find("text_off2")) add(last, at);
    auto own = [&](const char* name, const char* key) {   // (the later items push their own text)
        if (ni::NiNode* at = find(name)) add(get(key), at);
    };
    own("text_soundvolume", "LABEL_007");
    own("text_mucisvolume", "LABEL_008");             // (sic)
    own("text_speechvolume", "LABEL_009");
    own("text_vibration", "LABEL_011");
    own("text_adjustscreen", "LABEL_012");
    {
        const f32 sx2 = t->style.scaleX, sy2 = t->style.scaleY;
        if (german) t->SetScale(F(double(sx2) - 4.5f), sy2);   // FSUB [0x002947A0] 4.5
        if (ni::NiNode* at = find("text_restoredefault")) {
            add(get("LABEL_013"), at);
            t->SetScale(sx2, sy2);
        }
    }
    own("text_invertaim", "LABEL_010");
    own("text_apply", "LABEL_023");
    own("text_movies", "LABEL_019");
    own("text_music", "LABEL_020");
    own("text_gallery", "LABEL_021");
    if (ni::NiNode* at = find("text_headline_extras")) {
        setColor(1, 1, 0);
        add(get("LABEL_003"), at);
        setColor(1, 1, 1);
    }
}

}  // namespace fut::game::app
