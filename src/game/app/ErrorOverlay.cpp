// The App's error overlay; see ErrorOverlay.h.
#include "game/app/ErrorOverlay.h"

#include "engine/ni/NiCull.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiStream.h"
#include "game/app/App.h"
#include "game/text/LanguageDb.h"
#include "game/text/TextObject.h"

#include <bit>
#include <cstdio>
#include <vector>

namespace fut::game::app {

using ni::detail::F;

namespace {

// orig: [0x0029BC18] / [0x0029BC14]: 1 / 640 and 1 / 480 as floats (the HUD's constants)
const f32 kInv640 = std::bit_cast<f32>(0x3ACCCCCDu);
const f32 kInv480 = std::bit_cast<f32>(0x3B088889u);

}  // namespace

ErrorOverlay::ErrorOverlay(App& app) : app_(app) {   // orig: 0x000F3470
    ni::NiRenderer* renderer = app.services.renderer;   // App->vf14()
    camera = new ni::NiCamera;                        // new(0x250) 0x001009D0 -> DAT_002F0940
    // TextObject::Create(font manager, 0) -> DAT_002F094C (font 0: fonts/Latin1, the run's second texture), then
    // 0x00098F80(App->vf14()), +0x68 = 1, SetScale(1.2, 1.4) (0x3F99999A / 0x3FB33333), +0x60 = 0, +0x54 = 0.4
    text.reset(text::TextObject::Create(0, app.services.readFile, renderer));
    if (text) {
        text->SetRenderer(renderer);
        text->style.wordWrap = true;
        text->SetScale(std::bit_cast<f32>(0x3F99999Au), std::bit_cast<f32>(0x3FB33333u));
        text->style.hAlign = 0;
        text->width = std::bit_cast<f32>(0x3ECCCCCDu);
    }
    // the backdrop: new(0x24) 0x00106DE0(4, (0,1,0) (0,0,0) (1,0,0) (1,1,0), no uvs, no colours)
    static const ni::NiPoint3 kQuad[4] = {{0, 1, 0}, {0, 0, 0}, {1, 0, 0}, {1, 1, 0}};
    backdrop = new ni::NiScreenPolygon(4, kQuad, nullptr, nullptr);
    auto* vc = new ni::NiVertexColorProperty;         // new(0x20) (vtable 0x00293AD0: +0x18 = 0, +0x1C = 1), then 0 / 0
    vc->sourceVertexMode = 0, vc->lightingMode = 0;
    backdrop->AttachProperty(vc);
    alpha = new ni::NiAlphaProperty;                  // new(0x1C) (flags 0xEC) -> DAT_002F0A5C
    alpha->flags = u16(alpha->flags | 1);
    alpha->flags = u16((alpha->flags & 0xFFED) | 0x000C);
    alpha->flags = u16((alpha->flags & 0xFEFF) | 0x00E0);
    backdrop->AttachProperty(alpha.Get());
    material = new ni::NiMaterialProperty;            // new(0x5C) 0x00114B10 -> DAT_002F0ADC: black, alpha 0.75
    material->ambient = material->diffuse = material->specular = material->emissive = ni::NiColor{0, 0, 0};
    material->shininess = 0.0f;                       // +0x4C
    material->alpha = std::bit_cast<f32>(0x3F400000u);   // +0x50
    material->revision += 6;
    backdrop->AttachProperty(material.Get());
    camera->AttachScreenPolygon(backdrop.Get());      // 0x00019460
    camera->renderer = renderer;                      // +0x1A8 = App->vf14()
    // HUD\ErrorIcon.nif (NiStream::Load: its textures are created now, the run's third texture): the first top-level
    // object as a texture, 0x000F3140(texture, 128, 110, 256, 110, (1, 1, 1), 1, 0, 0, 0, camera)
    std::vector<u8> bytes;
    ni::NiStream s;
    if (app.services.readFile && app.services.readFile("HUD\\ErrorIcon.nif", bytes) && s.Load(bytes)) {
        ni::NiObject* first = s.TopLevelLinks().empty() ? nullptr : s.ResolveLink(s.TopLevelLinks()[0]);
        ni::NiPointer<ni::NiTexture> tex(dynamic_cast<ni::NiTexture*>(first));
        icon = MakeIcon(tex.Get(), 128.0f, 110.0f, 256.0f, 110.0f, ni::NiColor{1, 1, 1}, true, camera.Get());
    }
    errors = app.text ? app.text->LoadDb("Language/XBOXERROR.ldb") : nullptr;   // -> DAT_002F0954
    // NiUDSStream_SetErrorHandler(0x002D711C) (the read-error loop 0x000F3060: NOT PORTED, no read error here)
}

ErrorOverlay::~ErrorOverlay() = default;

ni::NiPointer<ni::NiScreenPolygon> ErrorOverlay::MakeIcon(ni::NiObject* texture, f32 w, f32 h, f32 x, f32 y,
                                                          const ni::NiColor& rgb, bool alphaOn,
                                                          ni::NiCamera* cam) {   // orig: 0x000F3140
    if (!cam) return {};
    static const ni::NiPoint2 kUV[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};   // (the uvs argument is null)
    const f32 W = F(double(w) * kInv640);             // FMUL [0x0029BC18]; FSTP
    const f32 H = F(double(h) * kInv480);             // FMUL [0x0029BC14]; FSTP
    const double L = double(x) * kInv640;             // (kept on the x87 stack)
    const double T = 1.0 - double(y) * kInv480;       // FSUBR [0x00293A98] 1.0
    const ni::NiPoint3 v[4] = {{F(L), F(T - H), 0}, {F(L + W), F(T - H), 0}, {F(L + W), F(T), 0}, {F(L), F(T), 0}};
    ni::NiPointer<ni::NiScreenPolygon> p(new ni::NiScreenPolygon(4, v, kUV, nullptr));   // new(0x24) 0x00106DE0
    auto* t = new ni::NiTexturingProperty;            // new(0x3C) 0x00109DC0: apply mode 2 (+0x18)
    t->applyMode = 2;
    if (!t->HasMap(0)) t->SetBaseTexture(nullptr);    // SetBaseFilterMode(1) 0x0001D6B0 (makes the base map)
    t->maps[0].filterMode = 1;
    if (texture) t->SetBaseTexture(texture);          // 0x0001D610
    p->AttachProperty(t);
    auto* m = new ni::NiMaterialProperty;             // new(0x5C): all four colours rgb, shininess 0, alpha 1
    m->ambient = m->diffuse = m->specular = m->emissive = rgb;
    m->revision += 6;
    m->shininess = 0.0f, m->alpha = 1.0f;
    p->AttachProperty(m);
    if (alphaOn) {
        auto* a = new ni::NiAlphaProperty;            // new(0x1C): (0xEC & 0xFEED) | 0xED, 0x00013CC0
        a->flags = u16((a->flags & 0xFEED) | 0xED);
        p->AttachProperty(a);
    }
    cam->AttachScreenPolygon(p.Get());                // 0x00019460
    return p;
}

void ErrorOverlay::SetMessage(const char* id) {       // orig: 0x000F2C80
    if (!errors) return;                              // DAT_002F0954
    message.reset();                                  // operator_delete(DAT_002F0950), 0
    const char16_t* t = app_.text ? app_.text->GetText(id, errors) : nullptr;   // TextManager::Get(id, db)->GetText
    if (t) message = std::make_unique<std::u16string>(t);   // new(2 (WStrLen + 1)), WStrCpy
}

void ErrorOverlay::SetMessage(const char* id, u32 n) {   // orig: 0x000F2CF0
    if (!errors) return;
    message.reset();
    const char16_t* t = app_.text ? app_.text->GetText(id, errors) : nullptr;
    // (no text: the original's 0x000D2D70 reads the null copy: a crash; the port keeps no message)
    if (!t) return;
    message = std::make_unique<std::u16string>(t);
    const size_t k = message->find(u'#');             // 0x000D2D70(copy, 0x23): the first '#', else null
    if (k != std::u16string::npos) (*message)[k] = char16_t(n + 0x30);   // MOV word [eax], cx (n + 0x30)
}

void ErrorOverlay::Draw() {                           // orig: 0x000F2D80
    // (TextManager::Get first: its result unused)
    if (message) {
        if (camera) ni::VisitForCulling(*camera, camera->sceneRoot);   // 0x00100CE0 (the backdrop and the icon)
        if (text) {
            text->x = 0.5f, text->y = 0.5f;           // +0x4C / +0x50 = 0x3F000000
            text->width = std::bit_cast<f32>(0x3ECCCCCDu);   // +0x54 = 0.4
            (void)text->AddString(*message, -1.0f);   // 0x0009A070(message, -1)
            text->UpdateCamera();                     // 0x00098D70
            text->Click();                            // 0x00098DB0
        }
        message.reset();                              // operator_delete, DAT_002F0950 = 0
        if (text) text->RemoveAll();                  // 0x00099080
    }
    // the debug byte: the level name, the player's position, the fps, the level's title (NOT PORTED: never set)
    if (debugText) std::fprintf(stderr, "NOT PORTED reached: the error overlay's debug text (0x000F2E02)\n");
}

}  // namespace fut::game::app
