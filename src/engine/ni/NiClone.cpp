// NiObject::Clone; see NiClone.h. Per-class functions of docs/analysis/ni_clone.md (CreateClone = vtable slot 2 on
// the source, CopyMembers chains, ProcessClone = slot 9 on the SOURCE, finding its clone through the map).
#include "engine/ni/NiClone.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace fut::ni {
namespace {

constexpr f32 kFltMax = 3.4028234663852886e38f;

struct CloneMap {                                     // orig: NiTPointerMap (37 buckets) at 0x002F90B0
    std::unordered_map<const NiObject*, NiObject*> map;   // last insert wins (0x0010BD70)
    std::vector<std::string>* unsupported = nullptr;
    void Set(const NiObject* key, NiObject* value) { map[key] = value; }   // orig: NiObject::CopyMembers 0x000FE1E0
    NiObject* Find(const NiObject* key) const {
        const auto it = map.find(key);
        return it == map.end() ? nullptr : it->second;
    }
    NiObject* Unsupported(const NiObject& o) {
        if (unsupported) unsupported->emplace_back(o.ClassName());
        static std::vector<std::string> seen;          // (a class whose CreateClone is not ported: dropped)
        const std::string what(o.ClassName());
        for (const std::string& s : seen)
            if (s == what) return nullptr;
        seen.push_back(what);
        std::fprintf(stderr, "NOT PORTED reached: CreateClone of %s (the clone lacks it)\n", what.c_str());
        return nullptr;
    }
};

NiObject* CreateClone(NiObject* src, CloneMap& m);
void ProcessClone(NiObject* src, CloneMap& m);

// ---- CopyMembers chains -----------------------------------------------------------------------------------------------
void CopyNET(const NiObjectNET& s, NiObjectNET& d, CloneMap& m) {   // orig: 0x000FE8A0
    m.Set(&s, &d);
    d.name = s.name;                                  // clone name mode 1 (0x002F90C0): strdup
    for (NiExtraData* ed = s.extraData.Get(); ed; ed = ed->nextData.Get()) {
        auto* c = dynamic_cast<NiExtraData*>(CreateClone(ed, m));
        if (!c) continue;
        c->nextData = d.extraData;                    // prepend: the chain comes out reversed
        d.extraData = c;
    }
    if (s.controller) d.controller = dynamic_cast<NiTimeController*>(CreateClone(s.controller.Get(), m));
}

void CopyAV(const NiAVObject& s, NiAVObject& d, CloneMap& m) {   // orig: 0x00102320
    CopyNET(s, d, m);
    d.flags = s.flags;
    d.local = s.local;                                // 0x40 bytes: rotate, translate, scale
    for (const auto& p : s.properties) {              // orig: 0x001022A0: same order, shared unless cloned
        auto* mapped = dynamic_cast<NiProperty*>(m.Find(p.Get()));
        d.properties.push_back(mapped ? mapped : p.Get());
    }
}

void CopyNode(const NiNode& s, NiNode& d, CloneMap& m) {   // orig: 0x00105880
    CopyAV(s, d, m);
    const auto& kids = s.Children();
    for (u32 i = 0; i < kids.size(); ++i) {
        if (!kids[i]) continue;                       // empty slot: SetAt not called
        auto* c = dynamic_cast<NiAVObject*>(CreateClone(kids[i].Get(), m));
        d.SetAt(i, c);                                // (a null clone still extends the array)
    }
}

void CopySwitch(const NiSwitchNode& s, NiSwitchNode& d, CloneMap& m) {   // orig: 0x00112F50
    CopyNode(s, d, m);
    d.index = s.index;
}

void CopyDynamicEffect(const NiDynamicEffect& s, NiDynamicEffect& d, CloneMap& m) {   // orig: 0x001418C0
    CopyAV(s, d, m);
    d.revision = s.revision;                          // (+0xC8 too, not modelled)
}

void CopyLight(const NiLight& s, NiLight& d, CloneMap& m) {   // orig: 0x00145720
    CopyDynamicEffect(s, d, m);
    d.dimmer = s.dimmer;
    d.ambient = s.ambient;
    d.diffuse = s.diffuse;
    d.specular = s.specular;
}

void CopyExtra(const NiExtraData& s, NiExtraData& d, bool exactClass, CloneMap& m) {   // orig: 0x00109660
    m.Set(&s, &d);
    d.size = s.size;
    if (s.size && exactClass) d.data = s.data;        // binary payload, exact NiExtraData only
    d.nextData = s.nextData ? dynamic_cast<NiExtraData*>(CreateClone(s.nextData.Get(), m)) : nullptr;
}

void CopyController(const NiTimeController& s, NiTimeController& d, CloneMap& m) {   // orig: 0x0013EE00 + 0x00141090
    m.Set(&s, &d);
    d.flags = s.flags;
    if (s.targetPtr) {
        NiObject* t = m.Find(s.targetPtr);
        d.targetPtr = t && t->ClassName() == s.targetPtr->ClassName() ? dynamic_cast<NiObjectNET*>(t) : nullptr;
    }
    if (s.nextControllerPtr)
        d.nextControllerPtr = dynamic_cast<NiTimeController*>(CreateClone(s.nextControllerPtr.Get(), m));
    d.frequency = s.frequency;
    d.phase = s.phase;
    d.loKeyTime = s.loKeyTime;
    d.hiKeyTime = s.hiKeyTime;
    d.startTime = s.startTime;
    d.lastTime = s.lastTime;
    d.accumulatedTime = s.accumulatedTime;
}

// orig: the group step of 0x00126F20 / 0x001299A0: first key earlier than lo (strict), last key later than hi.
template <class Keys>
void WidenRange(const Keys& keys, f32& lo, f32& hi) {
    if (keys.empty()) return;
    if (keys.front().time < lo) lo = keys.front().time;
    if (keys.back().time > hi) hi = keys.back().time;
}

void RecomputeKeyframeRange(NiKeyframeController& c) {   // orig: 0x00126F20
    c.loKeyTime = kFltMax;
    c.hiKeyTime = -kFltMax;
    const auto* data = c.Data();
    if (!data) return;
    if (data->rotations.type == KeyType::Euler) {
        if (!data->rotations.euler.empty())
            for (const FloatKeys& axis : data->rotations.euler[0].axes) WidenRange(axis.keys, c.loKeyTime, c.hiKeyTime);
    } else {
        WidenRange(data->rotations.keys, c.loKeyTime, c.hiKeyTime);
    }
    WidenRange(data->positions.keys, c.loKeyTime, c.hiKeyTime);
    WidenRange(data->scales.keys, c.loKeyTime, c.hiKeyTime);
}

void RecomputeVisRange(NiVisController& c) {          // orig: 0x001299A0
    c.loKeyTime = kFltMax;
    c.hiKeyTime = -kFltMax;
    if (const auto* data = dynamic_cast<const NiVisData*>(c.dataPtr.Get())) WidenRange(data->keys, c.loKeyTime, c.hiKeyTime);
    c.lastIndex = 0;
}

void RecomputeUVRange(NiUVController& c) {            // orig: 0x001285A0 (the four groups in order)
    c.loKeyTime = kFltMax;
    c.hiKeyTime = -kFltMax;
    if (const auto* data = dynamic_cast<const NiUVData*>(c.dataPtr.Get()))
        for (const FloatKeys& g : data->groups) WidenRange(g.keys, c.loKeyTime, c.hiKeyTime);
}

// ---- CreateClone (vtable slot 2), by exact class -------------------------------------------------------------------
NiObject* CreateClone(NiObject* src, CloneMap& m) {
    if (!src) return nullptr;
    const std::string_view cls = src->ClassName();

    // Nodes.
    if (cls == NiNode::kClassName || cls == NiWall::kClassName || cls == NiRoom::kClassName ||
        cls == NiRoomGroup::kClassName) {             // orig: 0x00106350 (rooms / walls lose their own data)
        auto* d = new NiNode;
        CopyNode(*static_cast<NiNode*>(src), *d, m);
        return d;
    }
    if (cls == NiBillboardNode::kClassName) {         // orig: 0x001102B0
        auto* d = new NiBillboardNode;
        d->lastUpdateTime = 0;
        d->lastUpdateControllers = true;
        CopyNode(*static_cast<NiNode*>(src), *d, m);  // (flags copied, so the billboard mode bits too)
        return d;
    }
    if (cls == NiSwitchNode::kClassName) {            // orig: 0x00113800 (ctor 0x00112FC0)
        auto* d = new NiSwitchNode;
        d->index = -1;
        d->revision = 1;
        d->lastUpdateControllers = true;
        CopySwitch(*static_cast<NiSwitchNode*>(src), *d, m);
        return d;
    }
    if (cls == NiFltAnimationNode::kClassName) {      // orig: 0x00113F10
        auto* s = static_cast<NiFltAnimationNode*>(src);
        auto* d = new NiFltAnimationNode;
        d->index = -1;
        d->revision = 1;
        d->lastUpdateControllers = true;
        d->period = 1.0f;
        CopySwitch(*s, *d, m);
        d->period = s->period;
        return d;
    }
    if (cls == NiLODNode::kClassName) {               // orig: 0x00114730 / 0x00114610
        auto* s = static_cast<NiLODNode*>(src);
        auto* d = new NiLODNode;
        d->index = -1;
        d->revision = 1;
        d->lastUpdateControllers = true;
        CopySwitch(*s, *d, m);
        d->lodCenter = s->lodCenter;
        d->ranges = s->ranges;                        // (the world near / far fields too; not worldLodCenter)
        return d;
    }
    if (cls == NiBSPNode::kClassName) {               // orig: 0x001127F0
        auto* s = static_cast<NiBSPNode*>(src);
        auto* d = new NiBSPNode;
        CopyNode(*s, *d, m);
        d->plane = s->plane;
        return d;
    }

    // Geometry.
    if (cls == NiTriShape::kClassName) {              // orig: 0x001166F0 -> 0x00144DC0
        auto* s = static_cast<NiTriShape*>(src);
        auto* d = new NiTriShape;
        d->modelData = s->modelData;                  // shared (SetModelData)
        CopyAV(*s, *d, m);
        return d;
    }
    if (cls == NiTriStrips::kClassName) {             // orig: 0x0011C480 -> 0x00144DC0 (as NiTriShape's)
        auto* s = static_cast<NiTriStrips*>(src);
        auto* d = new NiTriStrips;
        d->modelData = s->modelData;                  // shared (SetModelData)
        CopyAV(*s, *d, m);
        return d;
    }

    // Lights.
    if (cls == NiAmbientLight::kClassName) {          // orig: 0x0010F0B0
        auto* d = new NiAmbientLight;
        CopyLight(*static_cast<NiLight*>(src), *d, m);
        return d;
    }
    if (cls == NiDirectionalLight::kClassName) {      // orig: 0x00112BA0
        auto* s = static_cast<NiDirectionalLight*>(src);
        auto* d = new NiDirectionalLight;
        CopyLight(*s, *d, m);
        d->worldDirection = s->worldDirection;
        return d;
    }
    if (cls == NiPointLight::kClassName) {            // orig: 0x00117350
        auto* s = static_cast<NiPointLight*>(src);
        auto* d = new NiPointLight;
        CopyLight(*s, *d, m);
        d->constantAtten = s->constantAtten;
        d->linearAtten = s->linearAtten;
        d->quadraticAtten = s->quadraticAtten;
        return d;
    }

    // Cameras (the explosion meshes' files carry one under their root).
    if (cls == NiCamera::kClassName) {                // orig: 0x00101050 (new(0x250), ctor 0x001009D0) -> 0x00100360
        auto* s = static_cast<NiCamera*>(src);
        auto* d = new NiCamera;
        CopyAV(*s, *d, m);                            // 0x00102320
        for (u32 i = 0; i < 16; ++i) d->projection[i] = s->projection[i];   // +0xC4..+0x103 (not +0x108 / +0x10C)
        d->world.translate = s->world.translate;      // +0xA0..+0xA8 (the world translate only)
        d->worldDir = s->worldDir;                    // +0x110
        d->worldUp = s->worldUp;                      // +0x11C
        d->worldRight = s->worldRight;                // +0x128
        for (u32 i = 0; i < 6; ++i) d->frustum[i] = s->frustum[i];   // +0x134
        for (u32 i = 0; i < 4; ++i) d->port[i] = s->port[i];         // +0x14C
        d->lodAdjust = s->lodAdjust;                  // +0x244
        for (u32 i = 0; i < 6; ++i) d->frustumPlanes[i] = s->frustumPlanes[i];   // +0x1B0's entries (count +0x1C4)
        // The screen polygons +0x160 and screen textures +0x190: each slot's CreateClone (NiScreenPolygon 0x00106F80,
        // NiScreenTexture 0x0010B930) set at the same index (SetAt 0x0012B7F0), a null slot set null.
        d->screenPolygonList.resize(s->screenPolygonList.size());
        for (size_t i = 0; i < s->screenPolygonList.size(); ++i)
            d->screenPolygonList[i] = dynamic_cast<NiScreenPolygon*>(CreateClone(s->screenPolygonList[i].Get(), m));
        d->screenTextureList.resize(s->screenTextureList.size());
        for (size_t i = 0; i < s->screenTextureList.size(); ++i)
            d->screenTextureList[i] = dynamic_cast<NiScreenTexture*>(CreateClone(s->screenTextureList[i].Get(), m));
        d->scene = kNullLink;                         // dest +0x15C (the scene) and +0x1A8 (the renderer) released, null
        d->renderer = nullptr;
        return d;
    }

    // Game classes.
    if (cls == NiUDSNode::kClassName || cls == NiUDSObject::kClassName || cls == NiBsp::kClassName)
        return nullptr;                               // orig: 0x00013DB0
    if (cls == NiUDSFileObject::kClassName) {         // orig: 0x0011FB30: name and data only, not registered
        auto* s = static_cast<NiUDSFileObject*>(src);
        auto* d = new NiUDSFileObject;
        d->name = s->name;
        d->data = s->data;
        return d;
    }

    // Extra data.
    if (cls == NiExtraData::kClassName) {             // orig: 0x00109840
        auto* d = new NiExtraData;
        CopyExtra(*static_cast<NiExtraData*>(src), *d, true, m);
        return d;
    }
    if (cls == NiStringExtraData::kClassName) {       // orig: 0x00119BB0 / 0x00119A90
        auto* s = static_cast<NiStringExtraData*>(src);
        auto* d = new NiStringExtraData;
        CopyExtra(*s, *d, false, m);
        d->string = s->string;
        return d;
    }
    if (cls == NiTextKeyExtraData::kClassName) {      // orig: 0x0012A240 / 0x0012A030
        auto* s = static_cast<NiTextKeyExtraData*>(src);
        auto* d = new NiTextKeyExtraData;
        CopyExtra(*s, *d, false, m);
        d->keys = s->keys;
        return d;
    }

    // Controllers.
    if (cls == NiKeyframeController::kClassName) {    // orig: 0x001279B0
        auto* s = static_cast<NiKeyframeController*>(src);
        auto* d = new NiKeyframeController;           // indices 0, priority -1, +0x34 = 0 (+0x3F never written)
        CopyController(*s, *d, m);
        if (s->dataPtr) {                             // orig: SetData 0x001273B0: shared, indices reset, range
            d->dataPtr = s->dataPtr;
            d->lastRotIndex = d->lastPosIndex = d->lastScaleIndex = 0;
            RecomputeKeyframeRange(*d);
        }
        return d;
    }
    if (cls == NiVisController::kClassName) {         // orig: 0x00129EA0
        auto* s = static_cast<NiVisController*>(src);
        auto* d = new NiVisController;
        d->computeScaledFirst = true;
        CopyController(*s, *d, m);
        if (s->dataPtr) {
            d->dataPtr = s->dataPtr;
            RecomputeVisRange(*d);
        }
        return d;
    }
    if (cls == NiAlphaController::kClassName) {       // orig: 0x001205B0 / 0x00152D90
        auto* s = static_cast<NiAlphaController*>(src);
        auto* d = new NiAlphaController;
        CopyController(*s, *d, m);
        d->dataPtr = s->dataPtr;                      // shared (the original has no null check)
        return d;
    }
    if (cls == NiMaterialColorController::kClassName) {   // orig: 0x00133900
        auto* s = static_cast<NiMaterialColorController*>(src);
        auto* d = new NiMaterialColorController;
        CopyController(*s, *d, m);
        d->dataPtr = s->dataPtr;
        return d;
    }
    if (cls == NiUVController::kClassName) {          // orig: 0x001290D0 (new(0x70), the ctor's values) -> 0x00128EC0
        auto* s = static_cast<NiUVController*>(src);
        auto* d = new NiUVController;                 // +0x34 = 1, textureSet +0x54 = 0 (not copied below)
        CopyController(*s, *d, m);                    // 0x00141090
        d->appliedOffset[0] = s->appliedOffset[0], d->appliedOffset[1] = s->appliedOffset[1];   // +0x5C / +0x60
        d->appliedTiling[0] = s->appliedTiling[0], d->appliedTiling[1] = s->appliedTiling[1];   // +0x64 / +0x68
        if (s->dataPtr) {                             // the key caches +0x44..+0x50: copied, then zeroed here
            d->dataPtr = s->dataPtr;                  // shared (AddRef)
            for (u32& i : d->lastIndex) i = 0;
            RecomputeUVRange(*d);                     // 0x001285A0
        } else {
            for (u32 i = 0; i < 4; ++i) d->lastIndex[i] = s->lastIndex[i];
        }
        return d;
    }

    // Shared data (orig: 0x000114B0 `return this`).
    if (dynamic_cast<NiGeometryData*>(src) || dynamic_cast<NiKeyframeData*>(src) || dynamic_cast<NiVisData*>(src) ||
        dynamic_cast<NiFloatData*>(src) || dynamic_cast<NiPosData*>(src) || dynamic_cast<NiUVData*>(src) ||
        dynamic_cast<NiMorphData*>(src) || dynamic_cast<NiSkinData*>(src) || dynamic_cast<NiSkinPartition*>(src) ||
        dynamic_cast<NiPixelData*>(src) || dynamic_cast<NiTexture*>(src) || dynamic_cast<NiOutlineData*>(src))
        return src;

    return m.Unsupported(*src);
}

// orig: NiSkinInstance::CreateClone 0x0010E290: shared (this) unless its root parent or one of its bones was cloned
// (0x0010DD60); a new instance shares the skin data and points at the cloned root / bones (the others unchanged).
NiSkinInstance* CloneSkinInstance(NiSkinInstance& s, CloneMap& m) {
    const size_t n = s.skinData ? s.skinData->bones.size() : 0;   // orig: skin data +0x50
    bool any = m.Find(s.rootParent) != nullptr;
    for (size_t i = 0; i < n && !any; ++i) any = i < s.bones.size() && m.Find(s.bones[i]);
    if (!any) return &s;
    auto* d = new NiSkinInstance;
    m.Set(&s, d);                                     // orig: NiObject::CopyMembers
    d->skinData = s.skinData;
    auto map = [&](NiAVObject* o) {
        auto* c = dynamic_cast<NiAVObject*>(m.Find(o));
        return c ? c : o;
    };
    d->rootParent = map(s.rootParent);
    d->bones.resize(n);
    for (size_t i = 0; i < n; ++i) d->bones[i] = map(i < s.bones.size() ? s.bones[i] : nullptr);
    return d;
}

// ---- ProcessClone (vtable slot 9), on the source --------------------------------------------------------------------
void ProcessClone(NiObject* src, CloneMap& m) {
    // orig: 0x0013ECB0 first walks the source controller chain, whose ProcessClone does nothing else for the
    // controller classes ported (the look-at controller's would re-target, not ported).
    if (!dynamic_cast<NiObjectNET*>(src)) return;
    if (auto* s = dynamic_cast<NiNode*>(src)) {       // orig: 0x001063D0
        auto* clone = dynamic_cast<NiNode*>(m.Find(s));
        if (clone)                                    // orig: 0x00105900, tail first (so the order is kept)
            for (auto it = s->effects.rbegin(); it != s->effects.rend(); ++it) {
                if (!*it) continue;
                auto* e = dynamic_cast<NiDynamicEffect*>(m.Find(it->Get()));
                clone->AttachEffect(e ? e : it->Get());   // (a shared effect gains the clone node)
            }
        for (const auto& c : s->Children())
            if (c) ProcessClone(c.Get(), m);
        return;
    }
    if (auto* g = dynamic_cast<NiGeometry*>(src)) {   // orig: 0x00144DF0
        if (NiSkinInstance* s = g->skinInstance.Get()) {
            auto* clone = dynamic_cast<NiGeometry*>(m.Find(g));
            auto* skin = dynamic_cast<NiSkinInstance*>(m.Find(s));
            if (!skin) skin = CloneSkinInstance(*s, m);   // orig: vtbl+8 0x0010E290
            if (clone) clone->skinInstance = skin;
        }
    }
}

}  // namespace

NiPointer<NiSequence> CloneSequence(NiSequence& s, std::vector<std::string>* unsupported) {
    // orig: NiSequence::CreateClone 0x0012F050 -> CopyMembers 0x0012CF00, called directly (not through Clone 0x000FE120):
    // the controllers' CreateClone finds none of their targets in the map, so they start without one.
    CloneMap m;
    m.unsupported = unsupported;
    NiPointer<NiSequence> d(new NiSequence);
    m.Set(&s, d.Get());
    d->name = s.name;                                 // (name mode 1: exact)
    d->textKeys = s.textKeys;
    d->textKeysObj = s.textKeysObj;                   // shared
    d->textKeyReference = s.textKeyReference;
    for (const NiSequence::Entry& e : s.entries) {    // orig: AddController(name, controller->CreateClone(), 1)
        NiSequence::Entry n;
        n.name = e.name;
        if (e.controllerObj) n.controllerObj = CreateClone(e.controllerObj.Get(), m);
        d->entries.push_back(std::move(n));
    }
    return d;
}

void CopyNodeMembers(NiNode& src, NiNode& dest, std::vector<std::string>* unsupported) {
    CloneMap m;
    m.unsupported = unsupported;
    CopyNode(src, dest, m);                           // 0x00105880
    ProcessClone(&src, m);                            // vf24 of the source
}

NiPointer<NiObject> Clone(NiObject* root, std::vector<std::string>* unsupported) {   // orig: 0x000FE120
    CloneMap m;
    m.unsupported = unsupported;
    NiPointer<NiObject> clone(CreateClone(root, m));
    ProcessClone(root, m);
    return clone;
}

}  // namespace fut::ni
