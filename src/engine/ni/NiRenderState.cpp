// The render data on the scene graph (docs/analysis/renderer_lighting_port.md 1): NiPropertyState /
// NiDynamicEffectState, UpdateProperties / UpdateEffects with their push-downs, the effect ids.
#include "engine/ni/NiRenderState.h"

#include "engine/ni/NiObjects.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace fut::ni {

// ---- the effect ids (orig: 0x002D7CAC next, the free list NiTPrimitiveArray<u16> 0x002D7CB0) ---------------------
namespace effect_ids {
u32 next = 1;                                         // (H) the data's initial value
std::vector<u16> freeList;
u32 Acquire() {                                       // orig: 0x00141910
    if (freeList.empty()) return next++;
    const u16 id = freeList.back();                   // (data[--count], the slot zeroed; the non-null count
    freeList.pop_back();                              //  0x002D7CC0: unobservable)
    return id;
}
void Release(u32 id) { freeList.push_back(u16(id)); }   // orig: 0x00141B80 (grown by +0x14 when full)
void Reset(u32 nextId, const std::vector<u16>& free) {
    next = nextId;
    freeList = free;
}
}  // namespace effect_ids

NiDynamicEffect::NiDynamicEffect() : effectId(effect_ids::Acquire()) {   // orig: 0x00141910
}

NiDynamicEffect::~NiDynamicEffect() {                 // orig: 0x00141B80
    DetachAllAffectedNodes();                         // 0x00141A30 (each affected node's list loses it)
    effect_ids::Release(effectId);
}

u32 PropertyType(const NiProperty& p) {               // orig: NiProperty vtbl+0x2C
    static constexpr std::string_view kTypes[] = {
        "NiAlphaProperty", "NiDitherProperty", "NiFogProperty", "NiMaterialProperty", "NiRendererSpecificProperty",
        "NiShadeProperty", "NiSpecularProperty", "NiStencilProperty", "NiTexturingProperty", "NiVertexColorProperty",
        "NiWireframeProperty", "NiZBufferProperty"};
    const std::string_view name = p.ClassName();
    for (u32 i = 0; i < std::size(kTypes); ++i)
        if (kTypes[i] == name) return i;
    return 0xFFFFFFFFu;
}

// ---- NiPropertyState -----------------------------------------------------------------------------------------------
namespace {
std::array<NiPointer<NiProperty>, NiPropertyState::kCount> MakeDefaults() {
    using S = NiPropertyState;
    std::array<NiPointer<NiProperty>, S::kCount> d;
    auto* alpha = new NiAlphaProperty;                // 0x001043D0: flags 0xEC, ref 0
    alpha->flags = 0xEC;
    d[S::kAlpha] = alpha;
    d[S::kDither] = new NiProperty;                   // 0x00112D40: flags 0 (no class of its own in the port)
    d[S::kFog] = new NiFogProperty;                   // 0x00104710: flags 0, depth 1, colour 0
    auto* material = new NiMaterialProperty;          // 0x00114CE0 -> ctor 0x00114B10
    material->ambient = {0.5f, 0.5f, 0.5f}, material->diffuse = {0.5f, 0.5f, 0.5f};
    material->shininess = 4.0f, material->alpha = 1.0f;
    d[S::kMaterial] = material;
    d[S::kRendererSpecific] = new NiProperty;         // 0x00117830: flags 0, +0x18 = 0
    auto* shade = new NiShadeProperty;                // 0x00117940: flags 1 (smooth)
    shade->flags = 1;
    d[S::kShade] = shade;
    d[S::kSpecular] = new NiSpecularProperty;         // 0x00118C80: flags 0
    auto* stencil = new NiStencilProperty;            // 0x00119510: off, function 4, ref 0, mask ~0, fail 0, z fail 0,
    stencil->function = 4, stencil->mask = 0xFFFFFFFFu, stencil->passAction = 3;   // pass 3, draw mode 0
    d[S::kStencil] = stencil;
    auto* tex = new NiTexturingProperty;              // 0x0010A330 -> ctor 0x00109DC0: apply mode 2, no maps
    tex->applyMode = 2;
    d[S::kTexturing] = tex;
    auto* vc = new NiVertexColorProperty;             // 0x001014E0: flags 0, source 0, lighting 1
    vc->lightingMode = 1;
    d[S::kVertexColor] = vc;
    d[S::kWireframe] = new NiProperty;                // 0x0011CDA0: flags 0
    auto* z = new NiZBufferProperty;                  // 0x0011D020: flags 3, function 3
    z->flags = 3, z->testFunction = 3;
    d[S::kZBuffer] = z;
    return d;
}
}  // namespace

NiProperty* NiPropertyState::Default(u32 type) {
    static const std::array<NiPointer<NiProperty>, kCount> defaults = MakeDefaults();
    return type < kCount ? defaults[type].Get() : nullptr;
}

NiPropertyState::NiPropertyState() {                  // orig: 0x0014D800 -> 0x0014D630
    for (u32 t = 0; t < kCount; ++t) slot[t] = Default(t);
}

NiPropertyState::NiPropertyState(const NiPropertyState& o) {   // orig: 0x0014D520
    for (u32 t = 0; t < kCount; ++t) slot[t] = o.slot[t].Get();
}

NiPropertyState::~NiPropertyState() = default;

NiPropertyState* NiScreenPolygon::State() {
    if (!state) state = new NiPropertyState;
    for (const auto& p : properties) {
        const u32 t = p ? PropertyType(*p) : 0xFFFFFFFFu;
        if (t < NiPropertyState::kCount) state->slot[t] = p.Get();
    }
    return state.Get();
}

// ---- NiDynamicEffectState -----------------------------------------------------------------------------------------
NiDynamicEffectState::NiDynamicEffectState() = default;

NiDynamicEffectState::NiDynamicEffectState(const NiDynamicEffectState& o)   // orig: 0x0014D2F0 (lists 0x0014D1F0)
    : anyTextureEffect(o.anyTextureEffect), lights(o.lights), projectedLights(o.projectedLights),
      projectedShadows(o.projectedShadows), environmentMap(o.environmentMap), fogMap(o.fogMap) {}

NiDynamicEffectState::~NiDynamicEffectState() = default;

namespace {
// orig: 0x0014D3F0 (list, effect): inserted before the first element with a larger id; an equal id: not added
template <class T>
void InsertById(std::vector<NiRawPointer<T>>& list, T* e) {
    auto it = list.begin();
    while (it != list.end() && (*it)->effectId < e->effectId) ++it;
    if (it != list.end() && (*it)->effectId == e->effectId) return;
    list.insert(it, e);
}
template <class T>
bool SameList(const std::vector<NiRawPointer<T>>& a, const std::vector<NiRawPointer<T>>& b) {   // orig: 0x0014D1C0
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].Get() != b[i].Get()) return false;
    return true;
}
}  // namespace

void NiDynamicEffectState::Add(NiDynamicEffect* e) {   // orig: 0x0014D480
    if (!e) return;
    if (e->EffectType() <= 3) {
        if (auto* l = dynamic_cast<NiLight*>(e)) InsertById(lights, l);
        return;
    }
    auto* t = dynamic_cast<NiTextureEffect*>(e);
    if (!t) return;
    switch (t->textureType) {                         // +0x168
    case 0: InsertById(projectedLights, t); break;
    case 1: InsertById(projectedShadows, t); break;
    case 2: environmentMap = t; break;
    case 3: fogMap = t; break;
    default: break;                                   // (other types: only the flag)
    }
    anyTextureEffect = true;
}

bool NiDynamicEffectState::Equals(const NiDynamicEffectState* o) const {   // orig: 0x0014D380
    if (!o) return false;
    return environmentMap.Get() == o->environmentMap.Get() && fogMap.Get() == o->fogMap.Get() &&
           SameList(lights, o->lights) && SameList(projectedLights, o->projectedLights) &&
           SameList(projectedShadows, o->projectedShadows);
}

// The geometry classes' sharing cache (DAT_0030EF10).
namespace {
NiPointer<NiDynamicEffectState> g_effectCache;
void SetEffectCache(NiDynamicEffectState* s) { g_effectCache = s; }   // orig: 0x00101D00
}  // namespace

// ---- UpdateProperties ---------------------------------------------------------------------------------------------
NiPointer<NiPropertyState> NiAVObject::MergeProperties(NiPropertyState* parentState, bool copy) const {
    // orig: 0x00101FE0
    if (properties.empty()) return parentState;
    NiPointer<NiPropertyState> s = copy ? new NiPropertyState(*parentState) : parentState;
    for (const auto& p : properties) {
        if (!p) break;                                // (a null element ends the walk)
        const u32 type = PropertyType(*p);
        if (type < NiPropertyState::kCount && s->slot[type].Get() != p.Get()) s->slot[type] = p.Get();
    }
    return s;
}

namespace {
// orig: 0x00105050: the state of an object from the root down (a new default state at the root, each level's own
// properties merged into it in place)
NiPointer<NiPropertyState> ChainProperties(const NiAVObject& o) {
    NiPointer<NiPropertyState> s = o.parent ? ChainProperties(*o.parent) : NiPointer<NiPropertyState>(new NiPropertyState);
    return o.MergeProperties(s.Get(), false);
}
NiPointer<NiDynamicEffectState> ChainEffects(const NiNode& n) {   // orig: 0x00106220
    NiPointer<NiDynamicEffectState> s;
    if (n.parent) s = ChainEffects(*n.parent);
    return n.MergeEffects(s.Get(), false);
}
}  // namespace

void NiAVObject::UpdateProperties() {                 // orig: 0x00101EA0
    NiPointer<NiPropertyState> s = parent ? ChainProperties(*parent) : NiPointer<NiPropertyState>(new NiPropertyState);
    PushProperties(s.Get());
}

void NiNode::PushProperties(NiPropertyState* state) {   // orig: 0x00105280
    const NiPointer<NiPropertyState> merged = MergeProperties(state, true);
    for (u32 i = 0; i < children_.size(); ++i)
        if (NiAVObject* c = children_[i].Get()) c->PushProperties(merged.Get());
}

void NiGeometry::PushProperties(NiPropertyState* state) {   // orig: 0x00144BF0
    const NiPointer<NiPropertyState> merged = MergeProperties(state, true);
    if (propertyState.Get() != merged.Get()) propertyState = merged.Get();
}

// ---- UpdateEffects ------------------------------------------------------------------------------------------------
NiPointer<NiDynamicEffectState> NiNode::MergeEffects(NiDynamicEffectState* parentState, bool copy) const {
    // orig: 0x00105150
    if (effects.empty()) return parentState;
    NiPointer<NiDynamicEffectState> s;
    if (!parentState) s = new NiDynamicEffectState;
    else s = copy ? new NiDynamicEffectState(*parentState) : parentState;
    for (const auto& e : effects) {
        if (!e) break;
        s->Add(e.Get());
    }
    return s;
}

void NiAVObject::UpdateEffects() {                    // orig: 0x00102190
    SetEffectCache(nullptr);
    NiPointer<NiDynamicEffectState> s;
    if (parent) s = ChainEffects(*parent);
    PushEffects(s.Get());
    SetEffectCache(nullptr);
}

void NiNode::PushEffects(NiDynamicEffectState* state) {   // orig: 0x00105320
    const NiPointer<NiDynamicEffectState> merged = MergeEffects(state, true);
    for (u32 i = 0; i < children_.size(); ++i)
        if (NiAVObject* c = children_[i].Get()) c->PushEffects(merged.Get());
}

void NiGeometry::PushEffects(NiDynamicEffectState* state) {   // orig: 0x00144CB0
    effectsPushed = true;
    if (!state) {
        effectState = nullptr;
        return;
    }
    NiDynamicEffectState* cache = g_effectCache.Get();
    if (state->Equals(cache)) {                       // (0x0014D380 of the cache: the same effects)
        if (effectState.Get() != cache) effectState = cache;
        return;
    }
    if (effectState.Get() != state) effectState = state;
    if (cache != state) SetEffectCache(state);
}

void NiParticles::PushEffects(NiDynamicEffectState* state) {   // orig: 0x0010E7F0
    effectsPushed = true;
    if (!state) {
        effectState = nullptr;
        return;
    }
    NiDynamicEffectState* cache = g_effectCache.Get();
    if (state->Equals(cache)) {
        if (effectState.Get() != cache) effectState = cache;
        return;
    }
    if (state->projectedLights.empty() && state->projectedShadows.empty() && !state->environmentMap && !state->fogMap) {
        effectState = state;
        SetEffectCache(state);
        return;
    }
    NiPointer<NiDynamicEffectState> own(new NiDynamicEffectState);   // the lights only
    effectState = own.Get();
    for (const auto& l : state->lights) own->Add(l.Get());
    SetEffectCache(own.Get());
}

}  // namespace fut::ni
