// Stage::PreCache (default.xbe 0x000D11B0) and its tag effects; docs/analysis/stage_precache.md. The renderer calls
// (the geometry groups' vf18 into the stage's group +0xAC, NiXBoxRenderer vf9C: vertex / index buffers only) go to
// LevelData::renderer when there is one.
#include "game/level/Stage.h"

#include "engine/ni/NiCollision.h"
#include "game/defs/Defs.h"
#include "game/defs/EffectDefs.h"
#include "game/level/LevelAnimation.h"
#include "game/level/LevelData.h"
#include "game/object/Emitter.h"
#include "game/object/Liquid.h"

#include <cstdio>
#include <cstring>

namespace fut::game {
namespace {

int StrICmp(const char* a, const char* b) {          // orig: _stricmp (0x001684D1)
    for (;; ++a, ++b) {
        int ca = static_cast<unsigned char>(*a), cb = static_cast<unsigned char>(*b);
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca) return ca - cb;
    }
}

using ni::TagOnSelfOrAncestor;                       // orig: 0x000CEAD0 (engine/ni/NiCollision.h)

bool IsQDOutline(ni::NiAVObject* obj) {
    return dynamic_cast<ni::NiLODNode*>(obj) && !obj->name.empty() && StrICmp(obj->name.c_str(), "QDOutline") == 0;
}

// orig: the QDOutline branch: NiGeometryData::Replace 0x00143870 (count, vertices, 0, 0, 0) on every geometry under
// the LOD's first child: normals, colours and UVs dropped, the bound recomputed from the vertices.
void QDOutline(ni::NiAVObject* obj) {
    auto* nd = static_cast<ni::NiNode*>(obj);
    if (nd->Children().empty() || !nd->Children()[0]) return;
    auto* c0 = dynamic_cast<ni::NiNode*>(nd->Children()[0].Get());
    if (!c0) return;
    for (size_t i = 0; i < c0->Children().size(); ++i) {
        auto* g = dynamic_cast<ni::NiGeometry*>(c0->Children()[i].Get());
        if (!g || !g->modelData) continue;
        ni::NiGeometryData& d = *g->modelData;
        d.normals.clear();
        d.colors.clear();
        d.uvs.clear();
        d.textureSets = 0;
        if (!d.vertices.empty()) d.bound = ni::BoundOfPoints(d.vertices);   // orig: 0x0014EA10
    }
}

// orig: CamFadeRec::CamFadeRec 0x000CD680 (originalAlpha is always null here: only called without one).
std::unique_ptr<CamFadeRec> MakeCamFade(ni::NiGeometry& g) {
    auto r = std::make_unique<CamFadeRec>();
    r->geometry = &g;
    r->material = new ni::NiMaterialProperty;         // orig: ctor 0x00114B10
    r->material->ambient = r->material->diffuse = {0.5f, 0.5f, 0.5f};
    r->material->shininess = 4.0f;
    r->material->alpha = 1.0f;
    r->alpha = new ni::NiAlphaProperty;               // flags 0xEC -> 0x00ED
    r->alpha->flags = 0x00ED;
    r->originalMaterial = dynamic_cast<ni::NiMaterialProperty*>(GetProperty(g, 3));
    if (r->originalMaterial)                          // orig: NiPropertyList::Remove 0x0001E020
        for (auto it = g.properties.begin(); it != g.properties.end(); ++it)
            if (it->Get() == r->originalMaterial.Get()) {
                g.properties.erase(it);
                break;
            }
    AttachProperty(g, r->alpha.Get());
    if (const ni::NiMaterialProperty* m = r->originalMaterial.Get()) {
        r->material->ambient = m->ambient;
        r->material->diffuse = m->diffuse;
        r->material->emissive = m->emissive;
        r->material->shininess = m->shininess;        // (specular stays 0)
        r->material->revision += 4;
    }
    r->material->alpha = r->maxAlpha;
    r->material->revision += 1;
    AttachProperty(g, r->material.Get());             // properties: [material, alpha, ...]
    g.UpdateProperties();                             // 0x000CD9C6 (NOT PORTED: the global material index 0x002D792C)
    return r;
}

}  // namespace

void Stage::PreCache(ni::NiAVObject* obj) {          // orig: 0x000D11B0
    if (obj) {
        if (dynamic_cast<ni::NiOutline*>(obj)) return;
        if (IsQDOutline(obj)) QDOutline(obj);
    }
    if (animations_)
        for (const auto& la : animations_->list)
            if (la->target.Get() == obj) {
                PreCacheAnimated(obj);
                return;
            }
    if (obj) {
        if (dynamic_cast<ni::NiParticles*>(obj)) return;
        if (auto* nd = dynamic_cast<ni::NiNode*>(obj)) {
            const size_t n = nd->Children().size();  // captured
            for (size_t i = 0; i < n; ++i) PreCache(i < nd->Children().size() ? nd->Children()[i].Get() : nullptr);
        }
    }
    const bool tagged = TagEffects(obj);
    // the renderer: static vertex buffers for untagged unskinned geometry
    auto* g = obj ? dynamic_cast<ni::NiGeometry*>(obj) : nullptr;
    if (!tagged && g && g->modelData && geometryGroup && level.renderer && !g->skinInstance) {
        level.renderer->AddToGeometryGroup(geometryGroup, *g->modelData);   // DAT_0030EC70->vf18(+0xAC, data, 0, 0)
        level.renderer->PrecacheGeometry(*g, true);   // App->vf14()->vf9C(g, 1, 0, 0, 0)
    }
}

void Stage::PreCacheAnimated(ni::NiAVObject* obj) {  // orig: 0x000D0E60
    if (obj && IsQDOutline(obj)) QDOutline(obj);
    if (!precacheRenderer) return;                    // no geometry groups: the subtree is skipped
    if (obj) {
        if (dynamic_cast<ni::NiOutline*>(obj)) return;
        if (auto* nd = dynamic_cast<ni::NiNode*>(obj)) {
            const size_t n = nd->Children().size();
            for (size_t i = 0; i < n; ++i)
                PreCacheAnimated(i < nd->Children().size() ? nd->Children()[i].Get() : nullptr);
        }
    }
    if (TagEffects(obj)) return;                      // (DAT_002D6DA0 restored on every exit)
    if (!obj || dynamic_cast<ni::NiOutline*>(obj)) return;
    auto* g = dynamic_cast<ni::NiGeometry*>(obj);
    if (!g || !level.renderer) return;
    if (g->modelData && geometryGroup) level.renderer->AddToGeometryGroup(geometryGroup, *g->modelData);   // vf18
    if (g->skinInstance) return;                      // (GetName twice, unused)
    // the flag only decides whether the renderer may re-pack the buffers at a data change
    bool isStatic = true;                             // DAT_002D6DA0 (image value 1; every exit restores it)
    if (g->controller || FindAnimating(*g)) isStatic = false;   // 0x0013EC10 GetControllers, 0x000BAF50
    if (isStatic && bsp)                              // the stage's NiBsp +0x24: a collision mesh (+0x110, +0x118)
        for (const auto& m : bsp->meshes)
            if (m && m->geom.Get() == g) {
                isStatic = false;
                break;
            }
    level.renderer->PrecacheGeometry(*g, isStatic);   // App->vf14()->vf9C(g, flag, 0, 0, 0)
}

// orig: LevelAnimation::FindAnimating 0x000BAF50: the first level animation whose target is the object or its
// nearest such ancestor (FindByTargetUpward 0x000BAF00 over the level's list +0x170), one of whose clips' sequences
// names the object (NiSequence::GetObjectName 0x0012B4B0, strcmp); else the same from the target's parent up.
bool Stage::FindAnimating(const ni::NiAVObject& obj) const {
    if (obj.name.empty() || !animations_) return false;   // (GetName null)
    auto findUpward = [this](const ni::NiAVObject*& node) -> const level::LevelAnimation* {
        for (; node; node = node->parent)
            for (const auto& la : animations_->list)
                if (la && la->target.Get() == node) return la.get();
        return nullptr;
    };
    const ni::NiAVObject* node = &obj;
    for (const level::LevelAnimation* la = findUpward(node); la; la = findUpward(node)) {
        for (const auto& clip : la->clips) {          // +0x30, +0x38
            const ni::NiSequence* seq = clip ? clip->sequence.Get() : nullptr;   // (+8)
            if (!seq) continue;
            for (const ni::NiSequence::Entry& e : seq->entries)   // (L: the manager's string table: the entry names)
                if (e.name == obj.name) return true;
        }
        node = node->parent;                          // (+0x14)
    }
    return false;
}

bool Stage::TagEffects(ni::NiAVObject* obj) {        // orig: 0x000D0870
    if (TagOnSelfOrAncestor(obj, "NiData_ElectricSparks")) {
        if (Def* e = level.defs ? level.defs->Find("effect_PowerCordSparks") : nullptr) {
            const ni::NiPoint3 p = obj->world.translate;
            const ni::NiPoint3 d{-0.0f, -1.0f, -0.0f};   // orig: FCHS of (0, 1, 0)
            GameObject* o = e->SpawnEffect(level, p, d);
            if (auto* m = dynamic_cast<MeshEmitter*>(o)) m->follow = obj;   // orig: vf54 0x0006E940
        }
        return true;
    }
    if (TagOnSelfOrAncestor(obj, "NiData_Steam")) {
        if (Def* e = level.defs ? level.defs->Find("effect_SteamEmitter") : nullptr) {
            GameObject* o = e->SpawnEffect(level, {0, 0, 0}, {1, 0, 0});
            if (o)
                if (auto* nd = dynamic_cast<ni::NiNode*>(obj)) nd->AttachChild(o->GetNode(), true);   // re-parent
        }
        return true;
    }
    auto* g = dynamic_cast<ni::NiGeometry*>(obj);
    if (!g) return false;
    bool r = false;
    static constexpr const char* kLiquids[6][2] = {
        {"NiData_Lava", "Liquid_Lava"},   {"NiData_Slime", "Liquid_Slime"}, {"NiData_Water", "Liquid_Water"},
        {"NiData_Swamp", "Liquid_Swamp"}, {"NiData_Sand", "Liquid_Sand"},   {"NiData_Alien", "Liquid_Alien"}};
    for (const auto& liquid : kLiquids) {
        ni::NiStringExtraData* s = TagOnSelfOrAncestor(g, liquid[0]);
        if (!s) continue;
        f32 a = 0, b = 0, c = 0;
        // (the original does not check for the '=': a tag without one faults)
        if (const char* q = std::strchr(s->string.c_str(), '='); q && q[1])
            std::sscanf(q + 1, "%f,%f,%f", &a, &b, &c);   // a partial parse keeps the defaults
        if (auto* ld = dynamic_cast<LiquidDef*>(level.defs ? level.defs->Find(liquid[1]) : nullptr)) {
            SpawnLiquid(level, *ld, this, g, a, b, c);
            r = true;
        }
        break;
    }
    if (TagOnSelfOrAncestor(g, "NiData_CamFade")) {
        if (!GetProperty(*g, 0)) camFades.push_back(MakeCamFade(*g));
        r = true;
    }
    // the renderer: DAT_0030EC70->vf18(+0xAC, g->data, 0, 0) (buffers made at the first draw, in the stage's group)
    if (r && level.renderer && geometryGroup && g->modelData)
        level.renderer->AddToGeometryGroup(geometryGroup, *g->modelData);
    return r;
}

}  // namespace fut::game
