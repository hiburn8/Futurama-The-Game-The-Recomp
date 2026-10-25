// The screen text object; see TextObject.h.
#include "game/text/TextObject.h"

#include "engine/ni/NiCull.h"
#include "engine/ni/NiRenderer.h"
#include "engine/ni/NiStream.h"

#include <algorithm>
#include <cstdio>

namespace fut::text {

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// orig: Font (new(0xC0), ctor 0x000984E0: prepended to the font list DAT_002E6C70, +0xB0 next / +0xAC prev, +0xA8 = 0,
// +0xB4 = 0): a font's texture and glyph data, shared by every TextObject of the font; deleted only by
// Font::DestroyAll 0x00098AA0 (the App's teardown).
struct Font {
    u32 id = 0;                                       // +0xA4
    u16 uses = 0;                                     // +0xA8 (TextObject::Create's increment; never read here)
    ni::NiPointer<ni::NiObject> texture;              // (Font::Load: the NIF's first object as a NiTexture)
    std::vector<u8> glyphs;                           // +0xBC (the .fnt file)
    bool haveGlyphs = false;
};

std::vector<std::unique_ptr<Font>>& Fonts() {         // DAT_002E6C70 (front = the head)
    static std::vector<std::unique_ptr<Font>> list;
    return list;
}

Font* CreateFont(u32 id, const TextObject::ReadFileFn& readFile) {   // orig: Font::Create 0x00098A30
    auto f = std::make_unique<Font>();                // new(0xC0) 0x000984E0
    f->id = id;
    // Font::Load 0x00098630 (font 0): fonts/Latin1.nif (0x002A1548: its first object as a NiTexture) and
    // fonts/Latin1.fnt (0x002A1534: LoadWholeFile 0x0013FBA0, the glyph table's data +0xBC)
    std::vector<u8> nif;
    if (readFile && readFile("fonts/Latin1.nif", nif)) {
        ni::NiStream s;
        if (s.Load(nif) && !s.TopLevelLinks().empty())
            f->texture = dynamic_cast<ni::NiTexture*>(s.ResolveLink(s.TopLevelLinks()[0]));
    }
    f->haveGlyphs = readFile && readFile("fonts/Latin1.fnt", f->glyphs);
    Fonts().insert(Fonts().begin(), std::move(f));    // (the ctor's prepend)
    return Fonts().front().get();
}

}  // namespace

void CreateAppFont(const TextObject::ReadFileFn& readFile) {   // orig: App::Init's first TextObject (0x000F3470)
    for (const auto& f : Fonts())
        if (f->id == 0) return;
    CreateFont(0, readFile);
}

void DestroyAllFonts() { Fonts().clear(); }           // orig: Font::DestroyAll 0x00098AA0

TextObject* TextObject::Create(u32 font, const ReadFileFn& readFile, ni::NiRenderer* renderer) {   // 0x00098AD0
    if (font != 0) {
        NotPorted("TextObject::Create: fonts other than 0 (Font::Load 0x00098630's table 0x00098A04)");
        return nullptr;
    }
    // (the TextObject new(0x124) 0x00098DF0 comes first in the original; no D3D or file work in it)
    Font* f = nullptr;                                // the font list DAT_002E6C70 by id +0xA4
    for (const auto& g : Fonts())
        if (g->id == font) {
            f = g.get();
            ++f->uses;                                // ++(u16)+0xA8
            break;
        }
    if (!f) f = CreateFont(font, readFile);           // Font::Create 0x00098A30
    if (!f->haveGlyphs) return nullptr;               // (a UB guard: no glyph data to read)
    const ni::NiPointer<ni::NiObject>& texture = f->texture;
    const std::vector<u8>& fnt = f->glyphs;
    auto* t = new TextObject;                         // 0x00098DF0 (the members' defaults)
    // Init 0x00099CA0 (mode 0, the font's texture, its glyph data, 0)
    t->fontTexture = texture;                         // +0x11C
    t->texturing = new ni::NiTexturingProperty;       // +0x18: the base texture, apply mode 2, filter 2
    t->texturing->SetBaseTexture(texture.Get());
    t->texturing->applyMode = 2;
    t->texturing->maps[0].filterMode = 2;             // 0x0001D6B0(2)
    if (!t->table.Load(fnt)) {                        // LatinGlyphTable(0x100) 0x000984B0, vf00 0x00098340
        delete t;
        return nullptr;
    }
    // the camera +0: 0x001009D0, the frustum (0, 0.1, 0.1, 0, 1, 100) (0x00011960), the port (0, 1, 1, 0), at
    // (-10, 0, 0) looking along +x; its scene the node +0x1C (a NiNode with an alpha property 0xEC | 1)
    t->camera = new ni::NiCamera;
    const f32 frustum[6] = {0.0f, 0.1f, 0.1f, 0.0f, 1.0f, 100.0f};
    t->camera->SetFrustum(frustum);
    t->camera->port[0] = 0.0f, t->camera->port[1] = 1.0f, t->camera->port[2] = 1.0f, t->camera->port[3] = 0.0f;
    t->node = new ni::NiNode;
    auto* a = new ni::NiAlphaProperty;
    a->flags = 0x00ED;
    t->node->properties.insert(t->node->properties.begin(), ni::NiPointer<ni::NiProperty>(a));   // AddHead
    t->camera->local.translate = {-10.0f, 0.0f, 0.0f};   // +0x60
    t->nextId = 1;                                    // +0x44
    t->SetZBufferTest(false);                         // 0x00099660(0)
    t->dirty = 1;                                     // +0x79
    t->SetRenderer(renderer);                         // 0x00098F80(App->vf14())
    return t;
}

void TextObject::SetRenderer(ni::NiRenderer* renderer) {   // orig: 0x00098F80
    if (sceneMode || !camera) return;                 // +0xC
    camera->renderer = renderer;                      // +0x1A8
    camera->UpdateDownwardPass(0.0f, true);           // Update(0, 1)
}

void TextObject::UseScene(ni::NiCamera* sceneCamera, ni::NiNode* sceneNode) {   // orig: 0x00099BE0
    sceneMode = true;                                 // +0xC
    camera = sceneCamera;                             // +0x00
    node = sceneNode;                                 // +0x1C
    SetZBufferTest(true);
}

void TextObject::SetZBufferTest(bool on) {            // orig: 0x00099660
    if (!node) return;                                // (the original reads +0x1C without a test)
    ni::NiZBufferProperty* z = nullptr;               // GetProperty(11) 0x00101DA0: the list's first of the type
    for (const auto& p : node->properties)
        if (auto* q = dynamic_cast<ni::NiZBufferProperty*>(p.Get())) {
            z = q;
            break;
        }
    if (!z) {                                         // new(0x1C) (vtable 0x00296C80): flags 3, function 3; | 2
        z = new ni::NiZBufferProperty;
        z->flags = 3, z->testFunction = 3;
        z->flags = u16(z->flags | 2);
        node->properties.insert(node->properties.begin(), ni::NiPointer<ni::NiProperty>(z));   // AddHead 0x00013A50
    }
    if (on) z->flags = u16(z->flags | 1);
    else z->flags = u16(z->flags & 0xFFFE);
    dirty = 1;                                        // +0x79
    zTest = on;                                       // +0x78
}

u16 TextObject::AddStringAt(std::u16string_view text, ni::NiNode* at, f32 alpha, f32 w) {   // orig: 0x0009A0A0
    return AddString(text, 0.0f, 0.0f, w == -1.0f ? width : w, alpha, at);   // FCOMP [0x00293BF0]; TEST AH,0x44
}

u16 TextObject::AddString(std::u16string_view text, f32 px, f32 py, f32 w, f32 alpha, ni::NiNode* at) {   // 0x000998C0
    if (alpha == -1.0f) alpha = style.defaultScale;   // [0x00293BF0]: +0x10
    TextMesh mesh;
    if (!LayoutString(table, style, text, px, py, w, mesh, at != nullptr)) return 0;   // (0x000998C0's checks, the layout)
    const u16 id = nextId++;                          // +0x44 (0 skipped)
    if (nextId == 0) nextId = 1;
    // BuildMesh 0x0009AE40: NiTriShapeDynamicData(4n vertices, uvs (one set), 2n triangles (0, 1, 2) (0, 2, 3) per
    // character); the layout sets the active counts (only the glyphs' quads, from the start)
    const u32 n = u32(mesh.text.size());
    auto* data = new ni::NiTriShapeData;
    data->vertexCount = mesh.activeVertices;          // (the dynamic data's vf30: the active count)
    data->vertices.assign(mesh.vertices.begin(), mesh.vertices.begin() + mesh.activeVertices);
    data->textureSets = 1;
    data->uvs.resize(mesh.activeVertices);
    for (u32 i = 0; i < mesh.activeVertices; ++i) data->uvs[i] = {mesh.uvs[i][0], mesh.uvs[i][1]};
    data->triangleCount = mesh.activeTriangles;
    data->triangleList.resize(size_t(n) * 6);
    for (u32 c = 0; c < n; ++c) {
        const u16 v = u16(c * 4);
        const u16 q[6] = {v, u16(v + 1), u16(v + 2), v, u16(v + 2), u16(v + 3)};
        std::copy(q, q + 6, data->triangleList.begin() + size_t(c) * 6);
    }
    data->bound = {{0, 0, 0}, 100000.0f};             // (data +0xC: the zero vector 0x002F915C, radius 0x47C35000)
    ni::NiPointer<ni::NiTriShape> shape(new ni::NiTriShape);   // new(0xE0) 0x00116570
    shape->modelData = data;
    if (mesh.hidden) shape->flags |= ni::kNiAVHidden;
    auto* mat = new ni::NiMaterialProperty;           // ambient = diffuse = 1, emissive = the colour, the alpha
    mat->ambient = {1, 1, 1};
    mat->diffuse = {1, 1, 1};
    mat->emissive = color;
    mat->alpha = alpha;
    auto* vc = new ni::NiVertexColorProperty;         // (0, 0)
    vc->sourceVertexMode = 0, vc->lightingMode = 0;
    for (ni::NiProperty* p : {static_cast<ni::NiProperty*>(texturing.Get()), static_cast<ni::NiProperty*>(mat),
                              static_cast<ni::NiProperty*>(vc)})
        if (p) shape->properties.insert(shape->properties.begin(), ni::NiPointer<ni::NiProperty>(p));   // AddHead
    shape->UpdateProperties();                        // 0x0009B0EF (BuildMesh's)
    shape->UpdateEffects();
    shape->UpdateDownwardPass(0.0f, true);            // Update(0, 1)
    // AddString 0x00099AD4..: an alpha property (0x00013D80, flags | 1) at the head, Update(0, 1), the states again
    auto* ap = new ni::NiAlphaProperty;
    ap->flags |= 1;
    shape->properties.insert(shape->properties.begin(), ni::NiPointer<ni::NiProperty>(ap));   // AddHead
    shape->UpdateDownwardPass(0.0f, true);            // Update(0, 1)
    shape->UpdateProperties();                        // 0x00099B33
    shape->UpdateEffects();
    x = mesh.extentX, y = mesh.extentY;               // +0x4C / +0x50 = the end cursor
    if (sceneMode && at) {                            // +0xC: under the given node, which is updated
        at->AttachChild(shape.Get(), true);           // vfA0(shape, 1)
        at->UpdateDownwardPass(0.0f, true);           // Update(0, 1)
    } else {                                          // (the scene mode without a node: the original crashes)
        node->AttachChild(shape.Get(), true);         // (+0x1C)->vfA0(shape, 1)
    }
    dirty = 1;
    stringCount = strings.empty() ? 1 : stringCount + 1;   // +0x120 (= 1 for the first string, else + 1)
    strings.insert(strings.begin(), {id, shape});     // TextString::Link 0x0009A1D0
    return id;
}

bool TextObject::RemoveString(u16 id) {              // orig: 0x00098FD0
    auto it = std::find_if(strings.begin(), strings.end(), [id](const String& s) { return s.id == id; });
    dirty = 1;                                        // +0x79 (found or not)
    if (it == strings.end()) return false;
    // the shape's parent vfA4 (DetachChild; the returned pointer released); the kanji glyphs (mode 1): NOT PORTED
    if (it->shape && it->shape->parent) static_cast<ni::NiNode*>(it->shape->parent)->DetachChild(it->shape.Get());
    strings.erase(it);                                // TextString::Unlink 0x0009ADE0
    --stringCount;                                    // +0x120
    return true;
}

void TextObject::RemoveAll() {                        // orig: 0x00099080
    if (!node) return;
    while (!strings.empty()) RemoveString(strings.front().id);   // (the list head's id each time)
    dirty = 1;
}

void TextObject::UpdateCamera() {                     // orig: 0x00098D70
    if (sceneMode || !dirty || !node || !camera) return;   // +0xC, +0x79
    node->UpdateProperties();                         // 0x00098D84
    node->UpdateEffects();
    node->UpdateDownwardPass(0.0f, true);
    camera->UpdateDownwardPass(0.0f, true);
    dirty = 0;
}

void TextObject::Click() {                            // orig: 0x00098DB0
    if (!stringCount || !camera) return;
    // 0x00100CE0: the camera's Click culls its +0x15C (a scene-mode text's camera: its owner's scene)
    ni::VisitForCulling(*camera, camera->sceneRoot ? camera->sceneRoot : node.Get());
}

}  // namespace fut::text
