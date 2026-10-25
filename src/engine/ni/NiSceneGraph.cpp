// Scene-graph core: LinkObject (vtbl+0x10) for the transform-relevant classes and the world update passes
// (UpdateDownwardPass vtbl+0x64, UpdateWorldBound vtbl+0x98, UpdateWorldData vtbl+0x9C), decompiled from
// default.xbe. Verified per object against the original (tests/engine/NiSceneGraphTests.cpp).
#include "engine/ni/NiObjects.h"
#include "engine/ni/NiRenderer.h"

#include <cmath>

namespace fut::ni {
namespace {

template <class T>
T* As(NiObject* o) {
    return dynamic_cast<T*>(o);
}

}  // namespace

// ---- LinkObject -----------------------------------------------------------------------------------------------------
void NiObjectNET::LinkObject(NiStream& s) {           // orig: 0x000FE5F0
    NiObject::LinkObject(s);
    if (auto* e = As<NiExtraData>(s.ResolveLink(extraDataLink))) {
        if (!extraData) {
            extraData = e;
        } else {                                      // append to the end of the existing chain
            NiExtraData* tail = extraData.Get();
            while (tail->nextData) tail = tail->nextData.Get();
            tail->nextData = e;
        }
    }
    controller = As<NiTimeController>(s.ResolveLink(controllerLink));
}

void NiAVObject::LinkObject(NiStream& s) {            // orig: 0x00102580
    NiObjectNET::LinkObject(s);
    // orig: before 4.1.0.8 properties are appended (0x00101BB0), from 4.1.0.8 on each is pushed to the front
    // (0x00013A50), so the list ends up in reverse file order.
    const bool pushFront = s.FileVersion() >= NiVersion(4, 1, 0, 8);
    properties.clear();
    for (u32 id : propertyLinks) {
        auto* p = As<NiProperty>(s.ResolveLink(id));
        if (!p) continue;                             // both list inserts ignore null
        if (pushFront) {
            properties.insert(properties.begin(), NiPointer<NiProperty>(p));
        } else {
            properties.emplace_back(p);
        }
    }
}

void NiNode::LinkObject(NiStream& s) {                // orig: 0x001064B0
    NiAVObject::LinkObject(s);
    if (!childLinks.empty()) {
        if (children_.size() < childLinks.size()) children_.reserve(childLinks.size());   // orig: 0x00037E70
        for (u32 i = 0; i < childLinks.size(); ++i) SetAt(i, As<NiAVObject>(s.ResolveLink(childLinks[i])));
    }
    // Effects: appended before 4.1.0.8 (0x002A654C), pushed to the front from 4.1.0.8 on; each effect then gets this
    // node in its affected-node list (0x001419B0).
    const bool pushFront = s.FileVersion() >= NiVersion(4, 1, 0, 8);
    effects.clear();
    for (u32 id : effectLinks) {
        auto* e = As<NiDynamicEffect>(s.ResolveLink(id));
        if (!e) continue;
        if (pushFront) {
            effects.insert(effects.begin(), e);
        } else {
            effects.emplace_back(e);
        }
        e->AttachAffectedNode(this);
    }
}

void NiGeometry::LinkObject(NiStream& s) {            // orig: 0x00144E80
    NiAVObject::LinkObject(s);
    modelData = As<NiGeometryData>(s.ResolveLink(modelDataLink));
    skinInstance = As<NiSkinInstance>(s.ResolveLink(skinInstanceLink));
}

void NiTextureEffect::LinkObject(NiStream& s) {       // orig: 0x0011A1F0
    NiDynamicEffect::LinkObject(s);                   // 0x0011FCC0 (jmp NiAVObject::LinkObject 0x00102580)
    sourceTextureObj = s.ResolveLink(sourceTexture);  // GetObjectFromLinkID 0x00107890 -> +0x15C (old one released)
}

void NiSourceTexture::LinkObject(NiStream& s) {       // orig: 0x001181A0
    NiObjectNET::LinkObject(s);
    if (hasPixelData && !pixelDataObj) pixelDataObj = As<NiPixelData>(s.ResolveLink(pixelData));
}

void NiSourceTexture::PostLinkObject(NiStream&) {     // orig: 0x00118200
    if (g_precacheTextures) PrecacheTexture(*this);   // (tail jump 0x00118050)
}

// ---- the renderer list -------------------------------------------------------------------------------------------
bool g_precacheTextures = true;                       // orig: DAT_002D79A0 (static 1)

namespace {
std::vector<NiRenderer*>& Renderers() {               // orig: DAT_002D7BBC (a list: data, next)
    static std::vector<NiRenderer*> list;
    return list;
}
}  // namespace

NiPointer<NiRenderedTexture> CreateRenderedTexture(u32 width, u32 height, NiRenderer* r) {   // orig: 0x00140070
    if (!r || !width || (width & (width - 1)) || !height || (height & (height - 1))) return nullptr;
    NiPointer<NiRenderedTexture> t(new NiRenderedTexture);   // new(0x34) 0x00118410
    t->width = width, t->height = height;             // +0x2C / +0x30 (the preferences +0x14..+0x1C)
    if (!r->CreateRenderedTextureData(*t)) return nullptr;   // vf108
    return t;
}

NiTexture::~NiTexture() {                             // orig: 0x00118490 (+0x20, the renderer data, deleted)
    for (NiRenderer* r : Renderers()) r->PurgeTexture(*this);
}

NiRenderer::NiRenderer() { Renderers().push_back(this); }   // orig: 0x001405E0 (L: appended; one renderer)
NiRenderer::~NiRenderer() {
    auto& l = Renderers();
    for (size_t i = 0; i < l.size(); ++i)
        if (l[i] == this) {
            l.erase(l.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
}

bool PrecacheTexture(NiSourceTexture& texture) {      // orig: 0x00118050
    for (NiRenderer* r : Renderers())
        if (!r->PrecacheTexture(texture)) return false;   // vf104
    return true;
}

void NiSkinInstance::LinkObject(NiStream& s) {        // orig: 0x0010DF20
    NiObject::LinkObject(s);
    skinData = As<NiSkinData>(s.ResolveLink(skinDataLink));
    rootParent = As<NiAVObject>(s.ResolveLink(rootParentLink));
    bones.clear();
    for (u32 id : boneLinks) bones.push_back(As<NiAVObject>(s.ResolveLink(id)));
}

void NiSkinData::LinkObject(NiStream& s) {            // orig: 0x0010D9A0
    NiObject::LinkObject(s);
    if (s.FileVersion() >= NiVersion(4, 0, 0, 1)) skinPartition = As<NiSkinPartition>(s.ResolveLink(skinPartitionLink));
}

void NiExtraData::LinkObject(NiStream& s) {           // orig: 0x00109770
    NiObject::LinkObject(s);
    nextData = As<NiExtraData>(s.ResolveLink(next));
}

void NiTimeController::LinkObject(NiStream& s) {      // orig: 0x00141560
    NiObject::LinkObject(s);
    nextControllerPtr = As<NiTimeController>(s.ResolveLink(nextController));
    targetPtr = As<NiObjectNET>(s.ResolveLink(target));
}

// ---- children -------------------------------------------------------------------------------------------------------
// orig: 0x00105E60 DetachChild: finds the child, clears its parent and empties the slot (NiTArray::RemoveAt,
// 0x000CFE40: the size only shrinks when the last slot is removed).
NiPointer<NiAVObject> NiNode::DetachChild(NiAVObject* child) {
    for (u32 i = 0; i < children_.size(); ++i) {
        if (children_[i].Get() != nullptr && children_[i].Get() == child) {
            NiPointer<NiAVObject> held = children_[i];
            child->parent = nullptr;
            children_[i] = nullptr;
            --childCount_;
            if (i + 1 == children_.size()) children_.pop_back();
            return held;
        }
    }
    return {};
}

// orig: 0x00105F50 SetAt (the replaced child, returned by the original, is dropped here). A child that already has
// a parent is first detached from it (0x00101D40).
void NiNode::SetAt(u32 index, NiAVObject* child) {
    NiPointer<NiAVObject> keep(child);
    if (index < children_.size()) {
        if (NiAVObject* old = children_[index].Get()) old->parent = nullptr;
    }
    if (child) {
        if (child->parent) child->parent->DetachChild(child);
        child->parent = this;
    }
    if (index >= children_.size()) children_.resize(index + 1);
    const bool wasSet = children_[index].Get() != nullptr;
    children_[index] = child;
    if (wasSet && !child) --childCount_;
    if (!wasSet && child) ++childCount_;
}

void NiNode::AttachChild(NiAVObject* child, bool firstAvailable) {   // orig: 0x00105C90
    if (!child) return;
    NiPointer<NiAVObject> keep(child);
    if (child->parent) child->parent->DetachChild(child);   // orig: NiAVObject::AttachParent 0x00101D40
    child->parent = this;
    u32 index = static_cast<u32>(children_.size());
    if (firstAvailable)
        for (u32 i = 0; i < children_.size(); ++i)
            if (!children_[i]) {
                index = i;
                break;
            }
    if (index >= children_.size()) children_.resize(index + 1);
    children_[index] = child;
    ++childCount_;
}

void NiTextureEffect::UpdateWorldData() {             // orig: 0x0011A520
    NiAVObject::UpdateWorldData();
    UpdateProjection();
}

void NiTextureEffect::UpdateProjection() {            // orig: 0x0011A290
    using detail::F;
    const NiMatrix3& R = world.rotate;                // (the original's rows are this matrix's columns)
    if (coordGenType == 2) {                          // sphere map: one exact product each (0 * -0.5 = -0.0)
        worldProjectionTranslate = {0.5f, 0.5f, 0.0f};
        for (int r = 0; r < 3; ++r) {
            worldProjection.m[0][r] = F(double(R.m[r][2]) * 0.5f);
            worldProjection.m[1][r] = F(double(R.m[r][1]) * -0.5f);
            worldProjection.m[2][r] = 0.0f;
        }
    } else {                                          // 0x0011A34A: the model projection taken to world space
        const f32 inv = F(1.0 / double(world.scale)); // FLD 1.0f; FDIV +0xAC; FSTP
        NiMatrix3 Wt;                                 // 0x00103C60 (exact copies)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) Wt.m[r][c] = R.m[c][r];
        const NiMatrix3 M = Multiply(modelProjection, Wt);   // 0x00013B30 (this = the model projection)
        for (int r = 0; r < 3; ++r)                   // 0x000AD900 (SSE mulps: each word rounded once)
            for (int c = 0; c < 3; ++c) worldProjection.m[r][c] = M.m[r][c] * inv;
        const auto& P = worldProjection.m;
        const NiPoint3& t = world.translate;          // +0xA0
        const double t0 = (double(P[0][1]) * t.y + double(P[0][2]) * t.z) + double(P[0][0]) * t.x;
        const double t1 = (double(P[1][1]) * t.y + double(P[1][0]) * t.x) + double(P[1][2]) * t.z;
        const f32 t2 = F((double(P[2][1]) * t.y + double(P[2][0]) * t.x) + double(P[2][2]) * t.z);
        worldProjectionTranslate = {F(double(modelProjectionTranslate.x) - t0),   // (t0 / t1 unrounded)
                                    F(double(modelProjectionTranslate.y) - t1),
                                    F(double(modelProjectionTranslate.z) - t2)};
    }
    const NiPoint3& n = clippingPlane.normal;
    const f32 nx = F((double(R.m[0][1]) * n.y + double(R.m[0][2]) * n.z) + double(R.m[0][0]) * n.x);
    const f32 ny = F((double(R.m[1][0]) * n.x + double(R.m[1][1]) * n.y) + double(R.m[1][2]) * n.z);
    const f32 nz = F((double(R.m[2][0]) * n.x + double(R.m[2][1]) * n.y) + double(R.m[2][2]) * n.z);
    const NiPoint3& T = world.translate;
    worldClippingPlane.normal = {nx, ny, nz};
    worldClippingPlane.constant =
        F((((double(clippingPlane.constant) * world.scale) + double(nz) * T.z) + double(ny) * T.y) + double(nx) * T.x);
    ++revision;
}

NiNode::~NiNode() {                                  // orig: 0x00106740
    for (const auto& e : effects)                     // orig: DetachAllEffects 0x001055B0
        if (e) e->ForgetAffectedNode(this);
    // orig: DetachChildAt 0x00105D70 for every slot: clears the child's parent (+0x14) and releases it. A child that
    // outlives the node (a game object's node attached to a room, held by its object) must not keep the parent.
    for (auto& c : children_) {
        if (!c) continue;
        c->parent = nullptr;
        c = nullptr;
    }
    childCount_ = 0;
}

void NiNode::AttachEffect(NiDynamicEffect* effect) {  // orig: 0x00105530
    if (!effect) return;
    for (const auto& e : effects)
        if (e.Get() == effect) return;
    effects.insert(effects.begin(), effect);          // orig: NiTList prepend 0x00104BC0
    effect->AttachAffectedNode(this);
}

void NiDynamicEffect::AttachAffectedNode(NiNode* node) {   // orig: 0x001419B0
    for (const NiNode* n : affectedNodes)
        if (n == node) return;
    affectedNodes.insert(affectedNodes.begin(), node);
    node->AttachEffect(this);
}

void NiNode::DetachEffect(NiDynamicEffect* effect) {  // orig: 0x00105570
    for (size_t i = 0; i < effects.size(); ++i)
        if (effects[i].Get() == effect) {
            NiPointer<NiDynamicEffect> keep(effect);
            effects.erase(effects.begin() + static_cast<std::ptrdiff_t>(i));   // orig: NiTList remove 0x00104C10
            effect->DetachAffectedNode(this);
            return;
        }
}

void NiDynamicEffect::DetachAffectedNode(NiNode* node) {   // orig: 0x001419F0
    for (size_t i = 0; i < affectedNodes.size(); ++i)
        if (affectedNodes[i] == node) {
            affectedNodes.erase(affectedNodes.begin() + static_cast<std::ptrdiff_t>(i));   // 0x00104C10
            node->DetachEffect(this);
            return;
        }
}

void NiNode::DetachAllEffects() {                     // orig: 0x001055B0
    while (!effects.empty()) {
        NiDynamicEffect* e = effects.front().Get();
        effects.erase(effects.begin());               // 0x00104C10
        if (e) e->DetachAffectedNode(this);           // 0x001419F0
    }
}

void NiDynamicEffect::DetachAllAffectedNodes() {      // orig: 0x00141A30
    std::vector<NiNode*> nodes;
    nodes.swap(affectedNodes);
    for (NiNode* n : nodes)
        for (size_t i = 0; i < n->effects.size(); ++i)
            if (n->effects[i].Get() == this) {
                n->effects.erase(n->effects.begin() + i);
                break;
            }
}

void NiDynamicEffect::ForgetAffectedNode(const NiNode* node) {
    for (size_t i = 0; i < affectedNodes.size(); ++i)
        if (affectedNodes[i] == node) {
            affectedNodes.erase(affectedNodes.begin() + i);
            return;
        }
}

// ---- rooms --------------------------------------------------------------------------------------------------------
void NiRoom::LinkObject(NiStream& s) {                // orig: 0x0011EC60
    NiNode::LinkObject(s);
    std::vector<NiPointer<NiAVObject>>* lists[4] = {&list0, &list1, &list2, &fixtures};
    for (int k = 0; k < 4; ++k)
        for (u32 id : linkLists[k]) lists[k]->push_back(dynamic_cast<NiAVObject*>(s.ResolveLink(id)));
}

bool NiRoom::ContainsPoint(const NiPoint3& p) const {   // orig: 0x0011DE40
    for (const auto& e : list0) {
        const auto* w = dynamic_cast<const NiWall*>(e.Get());
        if (!w) continue;                             // (the original reads every element as a wall)
        const NiPlane& pl = w->worldPlane;
        // x87: ((n.z * p.z + n.y * p.y) + n.x * p.x) - c, compared with 0.0 (FCOMP; TEST AH,5; JNP): a NaN passes.
        double d = double(pl.normal.z) * p.z + double(pl.normal.y) * p.y;
        d += double(pl.normal.x) * p.x;
        d -= pl.constant;
        if (d < 0.0) return false;
    }
    return true;
}

void NiRoomGroup::LinkObject(NiStream& s) {           // orig: 0x0011F7C0
    NiNode::LinkObject(s);
    shellObject = s.ResolveLink(shell);
    const bool prepend = s.FileVersion() >= NiVersion(4, 1, 0, 8);
    for (u32 id : rooms) {
        auto* room = dynamic_cast<NiRoom*>(s.ResolveLink(id));
        if (!room) continue;                          // (the original stores any non-null object)
        if (prepend) {
            roomList.insert(roomList.begin(), room);
        } else {
            roomList.push_back(room);
        }
    }
}

void NiRoom::DetachFixture(NiAVObject* obj) {         // orig: 0x0011EEA0 (0x0011ED60, 0x0011D9D0)
    for (auto it = fixtures.begin(); it != fixtures.end(); ++it)
        if (it->Get() == obj) {
            fixtures.erase(it);
            UpdateChildArray();                       // orig: 0x0011DCC0
            return;
        }
}

NiRoom* NiRoomGroup::FindRoomContaining(const NiPoint3& p) const {   // orig: 0x0011F1D0
    for (const auto& r : roomList)
        if (r && r->ContainsPoint(p)) return r.Get();
    return nullptr;
}

void NiRoom::AttachFixture(NiAVObject* obj) {         // orig: 0x0011EB90
    for (const auto& f : fixtures)
        if (f.Get() == obj) return;
    fixtures.push_back(obj);
    UpdateChildArray();
}

void NiRoom::UpdateChildArray() {                     // orig: 0x0011DCC0
    for (auto& c : children_) {                       // orig: DetachChildAt (vtbl+0xA8) for every slot
        if (!c) continue;
        c->parent = nullptr;
        c = nullptr;
        --childCount_;
    }
    children_.clear();                                // orig: RemoveAll 0x00034190
    childCount_ = 0;
    for (auto* list : {&list2, &list0, &fixtures})
        for (const auto& e : *list)
            if (e) AttachChild(e.Get(), true);
}

// ---- update passes --------------------------------------------------------------------------------------------------
void NiAVObject::UpdateWorldData() {                  // orig: 0x00143770
    world = parent ? Compose(parent->world, local) : local;
}

void NiAVObject::UpdateDownwardPass(f32 time, bool updateControllers) {   // orig: 0x00101E10
    if (updateControllers) UpdateControllers(time, true);
    UpdateWorldData();
    UpdateWorldBound();
}

// Shared by 0x00104D40 and 0x00104FD0: the world bound is the merge of the visual children's bounds.
void NiNode::MergeChildBounds() {
    bool any = false;
    for (auto& c : children_) {
        NiAVObject* child = c.Get();
        if (!child || !child->IsVisualObject()) continue;
        if (any) {
            MergeBound(worldBound, child->worldBound);
        } else {
            worldBound = child->worldBound;
            any = true;
        }
    }
    if (any) {
        flags |= kNiAVVisualBound;
    } else {
        flags &= ~kNiAVVisualBound;
    }
}

void NiNode::UpdateDownwardPass(f32 time, bool updateControllers) {   // orig: 0x00104D40
    if (updateControllers) UpdateControllers(time, true);
    UpdateWorldData();
    bool any = false;
    for (u32 i = 0; i < children_.size(); ++i) {      // the child array can change size while children update
        NiAVObject* child = children_[i].Get();
        if (!child) continue;
        child->UpdateDownwardPass(time, updateControllers);
        if (!child->IsVisualObject()) continue;
        if (any) {
            MergeBound(worldBound, child->worldBound);
        } else {
            worldBound = child->worldBound;
            any = true;
        }
    }
    if (any) {
        flags |= kNiAVVisualBound;
    } else {
        flags &= ~kNiAVVisualBound;
    }
}

void NiNode::UpdateWorldBound() { MergeChildBounds(); }   // orig: 0x00104FD0

void NiBillboardNode::UpdateDownwardPass(f32 time, bool updateControllers) {   // orig: 0x0010FE90
    lastUpdateTime = time;
    lastUpdateControllers = updateControllers;
    NiNode::UpdateDownwardPass(time, updateControllers);
    UpdateWorldBound();
}

// orig: 0x0010FFF0. The billboard can rotate about its origin, so the bound is re-centred on the world position
// and grown by the old centre's distance from it.
void NiBillboardNode::UpdateWorldBound() {
    if (childCount_ == 0) return;
    const double dx = double(worldBound.center.x) - world.translate.x;
    const double dy = double(worldBound.center.y) - world.translate.y;
    const double dz = double(worldBound.center.z) - world.translate.z;
    worldBound.center = world.translate;
    worldBound.radius = static_cast<f32>(std::sqrt(dx * dx + dy * dy + dz * dz) + worldBound.radius);
}

void NiSwitchNode::UpdateDownwardPass(f32 time, bool updateControllers) {   // orig: 0x00113270
    lastUpdateControllers = updateControllers;
    lastUpdateTime = time;
    if ((flags & kNiAVSwitchUpdateOnlyActive) == 0) {
        NiNode::UpdateDownwardPass(time, updateControllers);
        return;
    }
    ++revision;
    if (index < 0) return;
    if (updateControllers) UpdateControllers(time, true);
    UpdateWorldData();
    NiAVObject* child = static_cast<u32>(index) < children_.size() ? children_[index].Get() : nullptr;
    if (!child) return;
    child->UpdateDownwardPass(time, updateControllers);
    if (childRevision.size() < children_.size()) childRevision.resize(children_.size(), 0);
    childRevision[static_cast<size_t>(index)] = revision;   // orig: 0x000A4A20 (the cull pass compares it)
    if (child->IsVisualObject()) {
        flags |= kNiAVVisualBound;
    } else {
        flags &= ~kNiAVVisualBound;
    }
    worldBound = child->worldBound;
}

void NiSwitchNode::UpdateWorldBound() {               // orig: 0x00113640
    if ((flags & kNiAVSwitchUpdateOnlyActive) == 0) {
        NiNode::UpdateWorldBound();
        return;
    }
    NiAVObject* child = index >= 0 && static_cast<u32>(index) < children_.size() ? children_[index].Get() : nullptr;
    if (child && child->IsVisualObject()) {
        worldBound = child->worldBound;
        flags |= kNiAVVisualBound;
    } else {
        flags &= ~kNiAVVisualBound;
    }
}

// ---- the selective update passes (NiAVObject::UpdateSelected 0x00101900) ------------------------------------------
void NiAVObject::UpdateSelectedDownwardPass(f32 time) {   // orig: 0x00101E40 (vf68 = vf6C for the leaves)
    UpdateControllers(time, ((flags >> 6) & 1) != 0); // byte +0xB0 >> 6 & 1
    if ((flags >> 5) & 1) {                           // selective transforms
        UpdateWorldData();                            // vf9C
        UpdateWorldBound();                           // vf98
    }
}

void NiNode::UpdateSelectedDownwardPass(f32 time) {   // orig: 0x00104E00
    UpdateControllers(time, ((flags >> 6) & 1) != 0);
    if ((flags >> 5) & 1) UpdateWorldData();          // vf9C
    bool any = false;
    for (u32 i = 0; i < children_.size(); ++i) {      // +0xCC (the high-water mark)
        NiAVObject* child = children_[i].Get();
        if (!child) continue;
        if ((child->flags >> 4) & 1) {                // selective update
            if ((child->flags >> 7) & 1) child->UpdateRigidDownwardPass(time);   // vf6C
            else child->UpdateSelectedDownwardPass(time);                       // vf68
        }
        if (!child->IsVisualObject()) continue;       // vf70 (every child, updated or not)
        if (any) {
            MergeBound(worldBound, child->worldBound);   // 0x0014EEE0
        } else {
            worldBound = child->worldBound;           // +0x20, 4 dwords
            any = true;
        }
    }
    if (any) {
        flags |= kNiAVVisualBound;                    // byte +0xB1 bit 0
    } else {
        flags &= ~kNiAVVisualBound;
    }
}

void NiNode::UpdateRigidDownwardPass(f32 time) {      // orig: 0x00104F00 (no bound pass)
    UpdateControllers(time, ((flags >> 6) & 1) != 0);
    if ((flags >> 5) & 1) UpdateWorldData();
    for (u32 i = 0; i < children_.size(); ++i) {
        NiAVObject* child = children_[i].Get();
        if (child && ((child->flags >> 4) & 1)) child->UpdateRigidDownwardPass(time);   // vf6C
    }
}

void NiBillboardNode::UpdateSelectedDownwardPass(f32 time) {   // orig: 0x0010FED0
    lastUpdateTime = time;                            // +0xE0
    NiNode::UpdateSelectedDownwardPass(time);         // 0x00104E00
    UpdateWorldBound();                               // vf98
}

void NiBillboardNode::UpdateRigidDownwardPass(f32 time) {   // orig: 0x0010FF00
    lastUpdateTime = time;
    NiNode::UpdateRigidDownwardPass(time);            // 0x00104F00
    UpdateWorldBound();
}

// orig: 0x00113360 / 0x00113470: with "update only the active child" the controllers and the world data are updated
// unconditionally (no 0x20 test) and only the active child is passed on (vf6C for the rigid pass; the selected pass
// picks vf6C / vf68 by the child's 0x80 bit), its revision recorded and its bound taken.
void NiSwitchNode::UpdateSelectedDownwardPass(f32 time) {
    lastUpdateTime = time;                            // +0xE4
    if ((flags & kNiAVSwitchUpdateOnlyActive) == 0) {
        NiNode::UpdateSelectedDownwardPass(time);     // 0x00104E00
        return;
    }
    ++revision;                                       // +0xE8
    if (index < 0) return;
    UpdateControllers(time, ((flags >> 6) & 1) != 0);
    UpdateWorldData();                                // vf9C
    NiAVObject* child = static_cast<u32>(index) < children_.size() ? children_[index].Get() : nullptr;
    if (!child) return;
    if ((child->flags >> 4) & 1) {
        if ((child->flags >> 7) & 1) child->UpdateRigidDownwardPass(time);
        else child->UpdateSelectedDownwardPass(time);
    }
    if (childRevision.size() < children_.size()) childRevision.resize(children_.size(), 0);
    childRevision[static_cast<size_t>(index)] = revision;   // 0x000A4A20
    if (child->IsVisualObject()) {
        flags |= kNiAVVisualBound;
    } else {
        flags &= ~kNiAVVisualBound;
    }
    worldBound = child->worldBound;
}

void NiSwitchNode::UpdateRigidDownwardPass(f32 time) {
    lastUpdateTime = time;
    if ((flags & kNiAVSwitchUpdateOnlyActive) == 0) {
        NiNode::UpdateRigidDownwardPass(time);        // 0x00104F00
        return;
    }
    ++revision;
    if (index < 0) return;
    UpdateControllers(time, ((flags >> 6) & 1) != 0);
    UpdateWorldData();
    NiAVObject* child = static_cast<u32>(index) < children_.size() ? children_[index].Get() : nullptr;
    if (!child) return;
    if ((child->flags >> 4) & 1) child->UpdateRigidDownwardPass(time);
    if (childRevision.size() < children_.size()) childRevision.resize(children_.size(), 0);
    childRevision[static_cast<size_t>(index)] = revision;
    if (child->IsVisualObject()) {
        flags |= kNiAVVisualBound;
    } else {
        flags &= ~kNiAVVisualBound;
    }
    worldBound = child->worldBound;
}

void NiLODNode::UpdateWorldData() {                   // orig: 0x001141D0
    NiAVObject::UpdateWorldData();
    const double s = world.scale;
    const double cx = s * lodCenter.x, cy = s * lodCenter.y, cz = s * lodCenter.z;
    const auto& M = world.rotate.m;
    worldLodCenter.x = static_cast<f32>(cx * M[0][0] + cy * M[0][1] + cz * M[0][2] + world.translate.x);
    worldLodCenter.y = static_cast<f32>(cx * M[1][0] + cy * M[1][1] + cz * M[1][2] + world.translate.y);
    worldLodCenter.z = static_cast<f32>(cx * M[2][0] + cy * M[2][1] + cz * M[2][2] + world.translate.z);
    // orig: one range per child slot (the original reads past the range array if there are fewer ranges).
    const size_t n = children_.size() < ranges.size() ? children_.size() : ranges.size();
    for (size_t i = 0; i < n; ++i) {
        const double nearW = double(ranges[i].nearDist) * world.scale;
        ranges[i].worldNearSq = static_cast<f32>(nearW * nearW);
        const double farW = double(ranges[i].farDist) * world.scale;
        ranges[i].worldFarSq = static_cast<f32>(farW * farW);
    }
}

void NiWall::UpdateWorldData() {                      // orig: 0x0011D460 (world-space plane; rotation only)
    NiAVObject::UpdateWorldData();
    const auto& M = world.rotate.m;
    const double nx = plane.normal.x, ny = plane.normal.y, nz = plane.normal.z;
    worldPlane.normal.x = static_cast<f32>(nx * M[0][0] + M[0][1] * ny + M[0][2] * nz);
    worldPlane.normal.y = static_cast<f32>(M[1][0] * nx + M[1][1] * ny + M[1][2] * nz);
    worldPlane.normal.z = static_cast<f32>(M[2][0] * nx + M[2][1] * ny + M[2][2] * nz);
    worldPlane.constant = static_cast<f32>(double(worldPlane.normal.x) * world.translate.x +
                                           double(world.translate.y) * worldPlane.normal.y +
                                           double(world.translate.z) * worldPlane.normal.z +
                                           double(plane.constant) * world.scale);
}

void NiGeometry::UpdateWorldData() {                  // orig: 0x00144740
    NiAVObject::UpdateWorldData();
    worldDataChanged = true;
    worldDataChanged2 = true;
}

void NiGeometry::UpdateWorldBound() {                 // orig: 0x00144BA0
    if (!modelData) return;                           // the original dereferences it unconditionally
    // A skinned mesh first recomputes its model-space bound (stored in the shared geometry data, as the original).
    if (skinInstance) modelData->bound = skinInstance->ComputeModelBound();
    worldBound = TransformBound(modelData->bound, world);
}

NiBound NiSkinInstance::ComputeModelBound() const {   // orig: 0x0010E140
    NiBound b;
    if (!skinData || skinData->bones.empty() || bones.empty() || !rootParent) return b;
    b = TransformBound(skinData->bones[0].bound, bones[0] ? bones[0]->world : NiTransform{});
    for (size_t i = 1; i < skinData->bones.size() && i < bones.size(); ++i)
        MergeBound(b, TransformBound(skinData->bones[i].bound, bones[i] ? bones[i]->world : NiTransform{}));
    const NiTransform toModel = Compose(skinData->rootParentToSkin, Invert(rootParent->world));
    return TransformBound(b, toModel);
}

void NiDirectionalLight::UpdateWorldData() {          // orig: 0x00112C80
    NiAVObject::UpdateWorldData();
    worldDirection = {world.rotate.m[0][0], world.rotate.m[1][0], world.rotate.m[2][0]};
    ++revision;
}

void NiPointLight::UpdateWorldData() {                // orig: 0x00117340
    NiAVObject::UpdateWorldData();
    ++revision;
}

void NiCamera::UpdateWorldBound() {                   // orig: 0x000FF410
    worldBound.center = world.translate;
}

void NiPortal::UpdateWorldData() {                    // orig: 0x00142C60 (vertices to world space)
    NiAVObject::UpdateWorldData();
    const auto& M = world.rotate.m;
    const double s = world.scale;
    for (size_t i = 0; i < vertices.size(); ++i) {
        const NiPoint3& p = vertices[i];
        worldVertices[i].x = static_cast<f32>(M[0][0] * s * p.x + M[0][1] * s * p.y + M[0][2] * s * p.z + world.translate.x);
        worldVertices[i].y = static_cast<f32>(M[1][0] * s * p.x + M[1][1] * s * p.y + M[1][2] * s * p.z + world.translate.y);
        worldVertices[i].z = static_cast<f32>(M[2][0] * s * p.x + M[2][1] * s * p.y + M[2][2] * s * p.z + world.translate.z);
    }
}

void NiPortal::UpdateWorldBound() { worldBound = TransformBound(modelBound, world); }   // orig: 0x00142B60

// ---- selective update flags -----------------------------------------------------------------------------------------
bool NiAVObject::HasTransformController() const {     // orig: 0x00101930
    for (NiTimeController* c = controller.Get(); c; c = c->nextControllerPtr.Get())
        if (c->IsTransformController()) return true;
    return false;
}

bool NiAVObject::HasPropertyController() const {      // orig: 0x00101F80
    for (const auto& p : properties)
        if (p && p->controller) return true;
    return false;
}

// orig: 0x001029C0
void NiAVObject::SetSelectiveUpdateFlags(bool& selectiveUpdate, bool selectiveTransforms, bool& rigid) {
    const bool transformCtrl = HasTransformController();
    rigid = !transformCtrl;
    if (!selectiveTransforms) selectiveTransforms = transformCtrl;
    const bool propertyCtrl = HasPropertyController();
    if (!propertyCtrl && !selectiveTransforms && !controller) {
        selectiveUpdate = false;
        flags &= ~0x0010;
        flags &= ~0x0020;
    } else {
        selectiveUpdate = true;
        flags |= 0x0010;
        if (selectiveTransforms) {
            flags |= 0x0020;
        } else {
            flags &= ~0x0020;
        }
    }
    // orig: the leaf version always ends with the rigid bit set (0xC0 with property controllers, else 0x80).
    if (propertyCtrl) {
        flags |= 0x00C0;
    } else {
        flags &= ~0x0040;
        flags |= 0x0080;
    }
}

// orig: 0x001053C0
void NiNode::SetSelectiveUpdateFlags(bool& selectiveUpdate, bool selectiveTransforms, bool& rigid) {
    const bool transformCtrl = HasTransformController();
    if (!selectiveTransforms) selectiveTransforms = transformCtrl;
    selectiveUpdate = selectiveTransforms;
    rigid = true;
    for (u32 i = 0; i < children_.size(); ++i) {
        NiAVObject* child = children_[i].Get();
        if (!child) continue;
        bool childSelective = false, childRigid = true;
        child->SetSelectiveUpdateFlags(childSelective, selectiveTransforms, childRigid);
        if (childSelective) selectiveUpdate = true;
        if (!childRigid) rigid = false;
    }
    const bool propertyCtrl = HasPropertyController();
    if (!propertyCtrl && !controller) {
        if (selectiveUpdate) {
            flags |= 0x0010;
        } else {
            flags &= ~0x0010;
        }
    } else {
        selectiveUpdate = true;
        flags |= 0x0010;
    }
    if (selectiveTransforms) {
        flags |= 0x0020;
    } else {
        flags &= ~0x0020;
    }
    if (propertyCtrl) {
        flags |= 0x0040;
    } else {
        flags &= ~0x0040;
    }
    if (rigid) {
        flags |= 0x0080;
    } else {
        flags &= ~0x0080;
    }
    if (transformCtrl) rigid = false;
}

// orig: 0x00144D80 - skinned geometry is always updated with its transforms and never rigid.
void NiGeometry::SetSelectiveUpdateFlags(bool& selectiveUpdate, bool selectiveTransforms, bool& rigid) {
    NiAVObject::SetSelectiveUpdateFlags(selectiveUpdate, selectiveTransforms, rigid);
    if (skinInstance) {
        flags |= 0x0020;
        selectiveUpdate = true;
        flags |= 0x0010;
        rigid = false;
    }
}

// ---- texturing property -----------------------------------------------------------------------------------------------
void NiTexturingProperty::LinkObject(NiStream& s) {   // orig: 0x0010A240
    NiObjectNET::LinkObject(s);
    // One "has map" count per slot was recorded by LoadBinary; present maps link their texture.
    for (u32 i = 0; i < present.size() && i < maps.size(); ++i)
        if (present[i]) maps[i].textureObj = s.ResolveLink(maps[i].texture);
}

void NiTexturingProperty::SetMap(u32 slot, const Map& map) {   // orig: 0x0010A040
    if (maps.size() <= slot) maps.resize(slot + 1);
    if (present.size() <= slot) present.resize(slot + 1, false);
    maps[slot] = map;
    present[slot] = true;
    // orig: for slot != 0 also 0x0010A000 (multi-texture bookkeeping), not needed by the port yet.
}

void NiTexturingProperty::SetBaseTexture(NiObject* texture) {   // orig: 0x0001D610
    if (!HasMap(0)) {
        Map m;                                        // Map::Map 0x0001D570
        m.clampMode = 3;
        m.filterMode = 0;
        m.textureIndex = 0;
        m.ps2L = 0;
        m.ps2K = 0xFFB5;
        if (maps.empty()) maps.resize(1);
        if (present.empty()) present.resize(1, false);
        maps[0] = m;                                  // (+0x20 SetAt(0): no multi-texture bookkeeping for slot 0)
        present[0] = true;
    }
    maps[0].textureObj = texture;
}

// ---- NiScreenTexture --------------------------------------------------------------------------------------------
NiScreenTexture::NiScreenTexture(NiObject* texture) {   // orig: 0x0010B2E0
    texturing = new NiTexturingProperty;             // new(0x3C)
    texturing->applyMode = 0;                         // +0x18
    texturing->SetBaseTexture(texture);
    texturing->maps[0].clampMode = 0;                 // 0x000506A0(0): the base map's +8
}

NiScreenTexture::~NiScreenTexture() = default;        // orig: 0x0010B030 (the renderer data's dtor is the purge)

u32 NiScreenTexture::AddRect(s16 top, s16 left, u16 width, u16 height, u16 texTop, u16 texLeft,
                             const NiColorA& color) {   // orig: 0x0010B6E0 -> 0x0010B660
    rects.push_back({top, left, width, height, texTop, texLeft, color});
    return u32(rects.size() - 1);
}

// ---- NiScreenPolygon --------------------------------------------------------------------------------------------
NiScreenPolygon::NiScreenPolygon(u16 count, const NiPoint3* v, const NiPoint2* uv, const NiColorA* c)
    : vertexCount(count) {                            // orig: 0x00106DE0
    vertices.assign(v, v + count);                    // new(12 n) + copy (the vertices are required)
    if (uv) this->uvs.assign(uv, uv + count);         // new(8 n) (0x00011740: the NiPoint2 ctor), copy
    if (c) colors.assign(c, c + count);               // new(16 n) (NiColorA ctor), copy
}

void NiScreenPolygon::AttachProperty(NiProperty* property) {
    if (!property) return;
    for (auto& p : properties)
        if (p && p->ClassName() == property->ClassName()) {
            p = property;
            return;
        }
    properties.emplace_back(property);
}

u32 NiCamera::AttachScreenPolygon(NiScreenPolygon* polygon) {   // orig: 0x00019460
    NiPointer<NiScreenPolygon> p(polygon);            // (AddRef'd for the call)
    u32 slot = 0;
    for (; slot < screenPolygonList.size(); ++slot)   // 0x000193B0 (+0x160): the first null slot
        if (!screenPolygonList[slot]) break;
    if (slot < screenPolygonList.size()) screenPolygonList[slot] = p;
    else screenPolygonList.push_back(p);              // (grown by +0x14 when the capacity is reached)
    if (p) p->flags |= 1;                             // +0x1C |= 1
    return slot;
}

void NiCamera::DetachScreenPolygon(NiScreenPolygon* polygon) {   // orig: 0x00100FC0
    for (auto& slot : screenPolygonList)
        if (slot.Get() == polygon) {
            slot = nullptr;                           // SetAt(i, 0)
            break;
        }
    CompactScreenPolygons();
}

void NiCamera::CompactScreenPolygons() {              // orig: 0x00100230
    size_t to = 0;
    for (size_t i = 0; i < screenPolygonList.size(); ++i) {
        if (!screenPolygonList[i]) continue;
        if (i != to) {
            screenPolygonList[to] = screenPolygonList[i];   // SetAt(to, [i]); SetAt(i, 0)
            screenPolygonList[i] = nullptr;
        }
        ++to;
    }
}

void NiCamera::AddScreenTexture(NiScreenTexture* texture) {   // orig: 0x0002BE40
    CompactScreenPolygons();                          // 0x00100230
    NiPointer<NiScreenTexture> p(texture);
    if (!p) return;                                   // 0x0002BD90: a null texture is not added (-1)
    for (auto& slot : screenTextureList)
        if (!slot) {
            slot = p;
            return;
        }
    screenTextureList.push_back(p);
}

void NiCamera::RemoveScreenTexture(NiScreenTexture* texture) {   // orig: 0x001002D0
    for (auto& slot : screenTextureList)
        if (slot.Get() == texture) {
            slot = nullptr;
            return;
        }
}

}  // namespace fut::ni
