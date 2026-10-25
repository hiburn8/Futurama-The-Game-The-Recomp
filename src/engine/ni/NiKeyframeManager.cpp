// NiKeyframeManager and the NiSequence runtime; see NiKeyframeManager.h. Floating point follows the original
// instruction by instruction (NiMath.h): x87 values are `double`, F() marks every store to a float.
#include "engine/ni/NiKeyframeManager.h"

#include "engine/ni/NiQuaternion.h"

#include <climits>
#include <cmath>
#include <cstring>

namespace fut::ni {
namespace {

using detail::F;
constexpr f32 kMaxFloat = 3.4028234663852886e38f;    // orig: DAT_002A6984

NiAVObject* AsAV(NiObjectNET* o) { return dynamic_cast<NiAVObject*>(o); }

// In this NetImmerse build NiVisController and NiUVController derive from NiKeyframeController (their RTTI objects
// are registered with parent 0x002F9FF4 by the static initializers 0x0022B2C0 / 0x0022B140), so every keyframe
// IsKindOf test below includes them. The port's class tree keeps them apart; this helper restores the original test.
bool IsKeyframeKind(const NiTimeController* c) {
    return dynamic_cast<const NiKeyframeController*>(c) || dynamic_cast<const NiVisController*>(c) ||
           dynamic_cast<const NiUVController*>(c);
}

NiTextKeyExtraData* TextKeys(const NiSequence* s) { return dynamic_cast<NiTextKeyExtraData*>(s->textKeysObj.Get()); }

void Record(NiTransformRecord& r, NiAVObject* obj) {
    r.object = obj;
    r.rotate = obj->local.rotate;
    r.translate = obj->local.translate;
    r.scale = obj->local.scale;
}

// orig: the overlap check shared by MorphActivate and the two SumActivates: same controller count and, for every
// index, the same target (as NiAVObject).
bool SameTargets(const NiSequence* a, const NiSequence* b) {
    if (a->ControllerCount() != b->ControllerCount()) return false;
    for (u32 i = 0; i < a->ControllerCount(); ++i)
        if (AsAV(a->Controller(i)->targetPtr) != AsAV(b->Controller(i)->targetPtr)) return false;
    return true;
}

// Blend of two transforms into `target` (shared x87 pattern of 0x0012C8F0 / 0x0012D4B0): weight w for `b`.
void BlendInto(NiAVObject* target, f32 w, const NiPoint3& ta, NiQuaternion qa, f32 sa, const NiPoint3& tb,
               const NiQuaternion& qb, f32 sb) {
    const f32 omw = F(1.0 - double(w));
    const f32 x = F(double(omw) * ta.x + double(w) * tb.x);
    const f32 y = F(double(F(double(omw) * ta.y)) + double(w) * tb.y);
    const f32 z = F(double(F(double(omw) * ta.z)) + double(F(double(w) * tb.z)));
    const double dot = ((double(qb.z) * qa.z + double(qb.y) * qa.y) + double(qb.x) * qa.x) + double(qb.w) * qa.w;
    if (dot < 0.0) qa = {-qa.w, -qa.x, -qa.y, -qa.z};   // orig: 0x001517B0
    const NiQuaternion r = QuatSlerp(w, qa, qb);
    target->local.translate = {x, y, z};
    target->local.rotate = QuatToRotation(r);
    target->local.scale = F(std::fabs(double(omw) * sa + double(w) * sb));
}

}  // namespace

// ---- controller chains ----------------------------------------------------------------------------------------------
void NiObjectNET::PrependController(NiTimeController* c) {   // orig: 0x000FE480
    if (!c) return;
    c->nextControllerPtr = controller.Get();
    if (controller.Get() != c) controller = c;
}

void NiObjectNET::RemoveController(NiTimeController* c) {    // orig: 0x000FE4C0
    if (!c || !controller) return;
    // A reference is held only while unlinking a controller that is in the chain (one that is not, e.g. a
    // manager still being constructed with no references yet, must not be touched).
    if (controller.Get() == c) {
        NiPointer<NiTimeController> keep(c);
        controller = c->nextControllerPtr.Get();
        c->nextControllerPtr = nullptr;
        return;
    }
    for (NiTimeController* p = controller.Get(); p->nextControllerPtr; p = p->nextControllerPtr.Get())
        if (p->nextControllerPtr.Get() == c) {
            NiPointer<NiTimeController> keep(c);
            p->nextControllerPtr = c->nextControllerPtr.Get();
            c->nextControllerPtr = nullptr;
            return;
        }
}

void NiTimeController::Stop() {                       // orig: 0x00141060
    SetActive(false);
    lastTime = -kMaxFloat;
    if (flags & 1) startTime = -kMaxFloat;
}

void NiTimeController::Start(f32 time) {              // orig: 0x00141030
    SetActive(true);
    lastTime = -kMaxFloat;                            // FLD [0x002A6984]; FCHS; FSTP +0x28
    if (flags & 1) startTime = time;                  // GetAnimType 0x0013EB90 == 1 (app-init): +0x24 = time
}

namespace {
template <class F>
void WalkAnimations(NiObjectNET* object, const F& each) {   // (0x0013EF40 / 0x0013F090's walk)
    for (NiTimeController* c = object->controller.Get(); c; c = c->nextControllerPtr.Get()) each(c);
    auto* av = dynamic_cast<NiAVObject*>(object);
    if (!av) return;
    for (const auto& p : av->properties)              // (+0x18's list)
        if (p && p->controller) WalkAnimations(p.Get(), each);
    if (auto* n = dynamic_cast<NiNode*>(object))
        for (size_t i = 0; i < n->Children().size(); ++i)   // (+0xC4, the count +0xCC re-read)
            if (NiAVObject* c = n->Children()[i].Get()) WalkAnimations(c, each);
}
}  // namespace

void StartAnimations(NiObjectNET* object) {           // orig: 0x0013EF40
    if (!object) return;
    WalkAnimations(object, [](NiTimeController* c) { c->Start(-kMaxFloat); });   // vf2C(-[0x002A6984])
}

void StopAnimations(NiObjectNET* object) {            // orig: 0x0013F090
    if (!object) return;
    WalkAnimations(object, [](NiTimeController* c) { c->Stop(); });   // vf30
}

void NiTimeController::SetTarget(NiObjectNET* newTarget) {   // orig: 0x0013EC20
    if (targetPtr == newTarget) return;
    NiPointer<NiTimeController> keep(this);
    if (targetPtr && targetPtr->controller) targetPtr->RemoveController(this);
    targetPtr = newTarget;
    if (!newTarget) return;
    for (NiTimeController* c = newTarget->controller.Get(); c; c = c->nextControllerPtr.Get())
        if (c == this) return;
    newTarget->PrependController(this);
}

// ---- NiSequence -----------------------------------------------------------------------------------------------------
NiSequence::NiSequence(const std::string& sequenceName, u32 count) : name(sequenceName), entries(count) {}

NiTimeController* NiSequence::Controller(u32 i) const {
    return i < entries.size() ? dynamic_cast<NiTimeController*>(entries[i].controllerObj.Get()) : nullptr;
}

NiTransformRecord& NiSequence::CacheSlot(u32& cursor) {
    if (cursor >= cache.size()) cache.emplace_back();
    return cache[cursor++];
}

bool NiSequence::SetOwner(NiKeyframeManager* manager) {   // orig: 0x0012A630
    if (manager && owner) return false;
    owner = manager;
    return true;
}

bool NiSequence::LinkToTargets(NiAVObject* node, u32& index) {   // orig: 0x0012BCF0
    if (!node) return false;
    const u32 n = ControllerCount();
    if (targets.size() < n) targets.resize(n, nullptr);
    while (index < n && node->name == entries[index].name) {
        targets[index++] = node;
        if (index == n) return true;
    }
    if (auto* parent = dynamic_cast<NiNode*>(node))
        for (const auto& child : parent->Children())
            if (child && LinkToTargets(child.Get(), index)) return true;
    return false;
}

void NiSequence::AttachControllers(s8 prio) {         // orig: 0x0012A710
    NiObjectNET* root = owner ? owner->targetPtr : nullptr;
    for (u32 i = 0; i < targets.size(); ++i) {
        NiPointer<NiTimeController> c = Controller(i);
        if (!c) continue;
        c->SetTarget(targets[i]);
        if (IsKeyframeKind(c.Get())) {
            c->priority = prio;
            c->Stop();
        }
        if (targets[i] && targets[i] == root) {         // run right after the manager in the root's chain
            targets[i]->RemoveController(c.Get());
            c->nextControllerPtr = owner->nextControllerPtr.Get();
            owner->nextControllerPtr = c.Get();
        }
    }
}

void NiSequence::DetachControllers() {                // orig: 0x0012A870
    for (u32 i = 0; i < targets.size(); ++i) {
        NiTimeController* c = Controller(i);
        if (targets[i]) targets[i]->RemoveController(c);
        if (c) c->SetTarget(nullptr);
    }
}

bool NiSequence::Activate(bool initTempTransforms, s8 prio) {   // orig: 0x0012BB80
    (void)initTempTransforms;                         // only feeds root accumulation (not ported)
    if (state != 0) return false;
    AttachControllers(prio);
    cumulative = false;                               // orig: owner +0x4C && reference target == owner target
    return true;
}

bool NiSequence::Deactivate(bool transition) {        // orig: 0x0012BC10
    (void)transition;                                 // only used by root accumulation
    if (state == 0) return false;
    if (NiTimeController* c = ReferenceController(); c && c->lastTime != -kMaxFloat)
        offset = F((double(c->accumulatedTime) / c->frequency + offset) - c->lastTime);
    DetachControllers();
    return true;
}

bool NiSequence::UpdateController(f32 time, u32 i) {   // orig: 0x0012C0A0
    NiTimeController* c = Controller(i);
    if (!c) return false;
    // A visibility, UV or keyframe controller with data runs (forced, active only for this call) unless a keyframe
    // controller of higher priority drives the same target.
    bool hasData = false;
    if (auto* vis = dynamic_cast<NiVisController*>(c)) {
        hasData = vis->dataPtr.Get() != nullptr;
    } else if (auto* uv = dynamic_cast<NiUVController*>(c)) {
        hasData = uv->dataPtr.Get() != nullptr;
    } else if (auto* kf = dynamic_cast<NiKeyframeController*>(c)) {
        hasData = kf->dataPtr.Get() != nullptr;
    } else {
        return false;                                 // (NiUDSKFController: no shipped data uses it)
    }
    if (!hasData) return false;
    s32 maxPriority = INT_MIN;
    if (c->targetPtr)
        for (NiTimeController* x = c->targetPtr->controller.Get(); x; x = x->nextControllerPtr.Get())
            if (IsKeyframeKind(x) && x->priority > maxPriority) maxPriority = x->priority;
    if (c->priority < maxPriority) return false;
    c->SetActive(true);
    c->forceUpdate = true;
    c->Update(time);
    c->SetActive(false);
    return true;
}

void NiSequence::UpdateAll(f32 time) {                // orig: 0x0012C890
    for (u32 i = 0; i < ControllerCount(); ++i) UpdateController(time, i);
}

void NiSequence::UpdateAndBlend(f32 t1, f32 t2, f32 w) {   // orig: 0x0012C8F0
    for (u32 i = 0; i < ControllerCount(); ++i) {
        NiAVObject* target = nullptr;
        NiPoint3 ta, tb;
        NiQuaternion qa, qb;
        f32 sa = 0, sb = 0;
        const bool ok1 = UpdateController(t1, i);
        if (ok1) {
            target = AsAV(Controller(i)->targetPtr);
            ta = target->local.translate;
            qa = QuatFromRotation(target->local.rotate);
            sa = target->local.scale;
        }
        // The original reads the partner's result from the same target object (stale when ok1 failed; unused).
        const bool ok2 = partner->UpdateController(t2, i);
        if (ok1 && ok2) {
            tb = target->local.translate;
            qb = QuatFromRotation(target->local.rotate);
            sb = target->local.scale;
            BlendInto(target, w, ta, qa, sa, tb, qb, sb);
        }
    }
}

void NiSequence::UpdateSumAndCache(f32 t1, f32 t2, f32 w) {   // orig: 0x0012D4B0
    u32 cursor = 0;
    for (u32 i = 0; i < ControllerCount(); ++i) {
        NiAVObject* target = AsAV(Controller(i)->targetPtr);
        UpdateController(t1, i);
        const NiPoint3 ta = target->local.translate;
        const NiQuaternion qa = QuatFromRotation(target->local.rotate);
        const f32 sa = target->local.scale;
        tempPartner->UpdateController(t2, i);
        const NiPoint3 tb = target->local.translate;
        const NiQuaternion qb = QuatFromRotation(target->local.rotate);
        const f32 sb = target->local.scale;
        BlendInto(target, w, ta, qa, sa, tb, qb, sb);
        Record(CacheSlot(cursor), target);
    }
}

void NiSequence::UpdateAndCache(f32 time) {           // orig: 0x0012D830
    u32 cursor = 0;
    for (u32 i = 0; i < ControllerCount(); ++i) {
        UpdateController(time, i);
        Record(CacheSlot(cursor), AsAV(Controller(i)->targetPtr));
    }
}

void NiSequence::SyncUpdateAndCache(f32 time, NiSequence* other, f32 otherTime) {   // orig: 0x0012D9E0
    // Walks both controller lists so that the two caches line up by target: a target only one sequence animates
    // gets the other sequence's record from its current (pre-update) transform.
    std::vector<NiAVObject*> mine, theirs;            // orig: two NiTPointerMap<NiAVObject*, bool> sets
    for (u32 i = 0; i < ControllerCount(); ++i) mine.push_back(AsAV(Controller(i)->targetPtr));
    for (u32 j = 0; j < other->ControllerCount(); ++j) theirs.push_back(AsAV(other->Controller(j)->targetPtr));
    auto contains = [](const std::vector<NiAVObject*>& v, NiAVObject* o) {
        for (NiAVObject* x : v)
            if (x == o) return true;
        return false;
    };
    u32 i = 0, j = 0, myCursor = 0, otherCursor = 0;
    while (i < ControllerCount() || j < other->ControllerCount()) {
        NiTimeController* c1 = i < ControllerCount() ? Controller(i) : nullptr;
        NiTimeController* c2 = j < other->ControllerCount() ? other->Controller(j) : nullptr;
        NiAVObject* t1 = c1 ? AsAV(c1->targetPtr) : nullptr;
        NiAVObject* t2 = c2 ? AsAV(c2->targetPtr) : nullptr;
        bool updateMine = c1 != nullptr, updateOther = c2 != nullptr;
        if (c1 && c2 && t1 && t2 && t1 != t2) {
            if (contains(theirs, t1)) {
                Record(CacheSlot(myCursor), t2);      // the other sequence's target comes first
                updateMine = false;
            } else {
                Record(other->CacheSlot(otherCursor), t1);   // t2 is ours, or neither: ours first
                updateOther = false;
            }
        } else if (c1 && !c2) {
            Record(other->CacheSlot(otherCursor), t1);
        } else if (!c1 && c2) {
            Record(CacheSlot(myCursor), t2);
        }
        if (updateMine) {
            UpdateController(time, i);
            Record(CacheSlot(myCursor), t1);
            ++i;
        }
        if (updateOther) {
            other->UpdateController(otherTime, j);
            Record(other->CacheSlot(otherCursor), t2);
            ++j;
        }
    }
}

void NiSequence::CacheCurrentTransforms() {           // orig: 0x0012E260
    u32 cursor = 0;
    for (u32 i = 0; i < ControllerCount(); ++i) Record(CacheSlot(cursor), AsAV(Controller(i)->targetPtr));
}

void NiSequence::BlendCachedTransforms(f32 t) {       // orig: 0x0012AB50
    const f32 omt = F(1.0 - double(t));
    const size_t n = std::min(partner->cache.size(), cache.size());   // the whole lists, not the controller count
    for (size_t k = 0; k < n; ++k) {
        const NiTransformRecord& rp = partner->cache[k];
        const NiTransformRecord& rq = cache[k];
        NiQuaternion qp = QuatFromRotation(rp.rotate);
        const NiQuaternion qq = QuatFromRotation(rq.rotate);
        const double dot = ((double(qq.z) * qp.z + double(qq.y) * qp.y) + double(qq.x) * qp.x) + double(qq.w) * qp.w;
        if (dot < 0.0) qp = {-qp.w, -qp.x, -qp.y, -qp.z};
        const NiQuaternion r = QuatSlerp(t, qp, qq);
        const f32 x = F(double(omt) * rp.translate.x + double(t) * rq.translate.x);
        const f32 y = F(double(F(double(omt) * rp.translate.y)) + double(t) * rq.translate.y);
        const f32 z = F(double(F(double(omt) * rp.translate.z)) + double(F(double(t) * rq.translate.z)));
        const double s = double(t) * rq.scale + double(omt) * rp.scale;
        NiAVObject* obj = rq.object;
        obj->local.rotate = QuatToRotation(r);
        obj->local.translate = {x, y, z};
        obj->local.scale = F(std::fabs(s));
    }
}

double NiSequence::ComputeMorphTime(NiSequence* other, f32 otherTime) {   // orig: 0x0012A910
    NiTimeController* rcB = other->ReferenceController();
    const f32 sB = F(rcB->ComputeScaledTime(otherTime));
    using Key = NiTextKeyExtraData::TextKey;
    const Key *first = nullptr, *last = nullptr, *prev = nullptr, *next = nullptr;
    if (NiTextKeyExtraData* keys = TextKeys(other))
        for (const Key& k : keys->keys) {
            if (k.text.compare(0, 6, "morph:") != 0) continue;
            if (!first) first = &k;
            last = &k;
            if (k.time < sB) {
                prev = &k;
            } else if (!next) {
                next = &k;
            }
        }
    // The original reads through a null key when the other sequence has no "morph:" keys (a fault); the game only
    // morphs sequences that have them. The port then maps to time 0.
    if (!next && !prev) return 0.0;
    double pT, nT;
    if (next) {
        if (prev) {
            pT = prev->time;
        } else {
            prev = last;
            pT = (double(rcB->loKeyTime) - rcB->hiKeyTime) + last->time;   // wrap backwards
        }
        nT = next->time;
    } else {
        pT = prev->time;
        next = first;
        nT = (double(rcB->hiKeyTime) - rcB->loKeyTime) + first->time;     // wrap forwards
    }
    const double frac = (double(sB) - pT) / (nT - pT);
    const Key *mp = nullptr, *mn = nullptr;
    if (NiTextKeyExtraData* keys = TextKeys(this))
        for (const Key& k : keys->keys) {
            if (!mp && k.text == prev->text) mp = &k;
            if (!mn && k.text == next->text) mn = &k;
        }
    const f32 vP = mp ? mp->time : 0.0f;
    double vN = mn ? double(mn->time) : 0.0;
    if (vP > vN) {
        NiTimeController* rcA = ReferenceController();
        vN = vN + (double(rcA->hiKeyTime) - rcA->loKeyTime);
    }
    return (vN - vP) * frac + vP;
}

void NiSequence::UpdateActive(f32 time) { UpdateAll(F(double(offset) + time)); }   // orig: 0x0012D330

void NiSequence::UpdateMorphSource(f32 t) {           // orig: 0x0012D350
    NiSequence* p = partner;
    const f32 a = endPoint, b = p->endPoint;
    if (t < a) {
        UpdateAll(F(double(offset) + t));
        return;                                       // (the partner's temp transforms: accumulation only)
    }
    const f32 pt = F(double(p->offset) + t);
    if (!(t <= b)) {
        p->UpdateAll(pt);
        return;
    }
    const double m = ComputeMorphTime(p, pt);
    const f32 w = F((double(t) - a) / (double(b) - a));
    NiTimeController* rc = ReferenceController();
    const f32 t1 = F((m - rc->phase) / rc->frequency);
    UpdateAndBlend(t1, pt, w);
}

void NiSequence::UpdateBlend(f32 t) {                 // orig: 0x0012EB10
    const f32 s = partner ? partner->endPoint : t;
    const f32 e = endPoint;
    if (t < s) {                                      // the source is still playing
        partner->UpdateAll(F(double(partner->offset) + t));
        return;
    }
    if (!(t <= e)) {                                  // blend finished
        if (state == 2) {
            owner->RemoveSequence(owner->GetSequence("TempBlendSource"));
            state = 1;
        } else if (state == 3) {
            NiKeyframeManager* mgr = owner;
            mgr->RemoveSequence(mgr->GetSequence("TempBlendSource"));
            Deactivate(true);
            mgr->RemoveSequence(this);
            return;
        }
        UpdateAll(F(double(offset) + t));
        return;
    }
    if (updateBlendTransforms) {                      // first blend frame: capture both poses
        if (!partner) {
            const u32 n = ControllerCount();
            NiPointer<NiSequence> tmp = new NiSequence("TempBlendSource", n);
            for (u32 i = 0; i < n; ++i) tmp->entries[i].controllerObj = entries[i].controllerObj;
            tmp->endPoint = t;
            tmp->state = 4;
            owner->AddSequence(tmp.Get(), false);
            partner = tmp.Get();
            tmp->CacheCurrentTransforms();            // the current pose is the blend source
            if (state == 3) {
                UpdateAndCache(e);
            } else {
                UpdateAndCache(F(double(offset) + e));
            }
        } else {
            const f32 pt = F(double(partner->offset) + s);
            if (syncBlendTransforms) {
                SyncUpdateAndCache(F(double(offset) + e), partner, pt);
                syncBlendTransforms = false;
            } else {
                partner->UpdateAndCache(pt);
                UpdateAndCache(F(double(offset) + e));
            }
        }
        updateBlendTransforms = false;
    }
    BlendCachedTransforms(F((double(t) - s) / (double(e) - s)));
}

void NiSequence::UpdateSum(f32 t, f32 weight) {       // orig: 0x0012EE20
    NiSequence* p = partner;
    NiSequence* m = tempPartner;
    const f32 t1 = F(double(offset) + t);
    const f32 t2 = F(double(p->offset) + t);
    if (m) {
        if (!(t <= endPoint)) {                       // transition over
            owner->RemoveSequence(owner->GetSequence("TempMixSource"));
            tempPartner = nullptr;
        } else {
            NiSequence* source;
            if (m == p) {                             // first frame after SumActivate with a transition
                const u32 n = ControllerCount();
                NiPointer<NiSequence> tm = new NiSequence("TempMixSource", n);
                for (u32 i = 0; i < n; ++i) tm->entries[i].controllerObj = entries[i].controllerObj;
                tm->endPoint = t;
                tm->state = 4;
                tm->partner = this;
                owner->AddSequence(tm.Get(), false);
                partner = tm.Get();
                tempPartner = p;
                tm->CacheCurrentTransforms();
                source = tm.Get();
                UpdateSumAndCache(t1, t2, weight);
                BlendCachedTransforms(F(0.0 / (double(endPoint) - t)));   // literally 0 / (E - t), as the original
            } else {                                  // m is the TempMixSource
                partner = m;
                tempPartner = p;
                source = m;
                const f32 s = m->endPoint;
                UpdateSumAndCache(t1, t2, weight);
                BlendCachedTransforms(F((double(t) - s) / (double(endPoint) - s)));
            }
            tempPartner = source;
            partner = p;
        }
    }
    if (!tempPartner) UpdateAndBlend(t1, t2, weight);
}

// ---- NiKeyframeManager ----------------------------------------------------------------------------------------------
NiKeyframeManager::NiKeyframeManager() {             // orig: 0x00130BD0
    // orig: the NiTimeController constructor 0x00140FD0 (flags 0x0C = active, clamp; key range FLT_MAX / -FLT_MAX).
    flags = 0x0C;
    loKeyTime = 3.4028234663852886e38f;
    hiKeyTime = -3.4028234663852886e38f;
}

NiKeyframeManager::NiKeyframeManager(NiAVObject* target) : NiKeyframeManager() {   // orig: 0x00130D20
    if (target->controller) target->RemoveController(this);
    target->PrependController(this);
    targetPtr = target;
}

NiKeyframeManager::~NiKeyframeManager() {
    for (Item*& head : buckets_)
        while (head) {
            Item* next = head->next;
            delete head;
            head = next;
        }
    for (Item* item : retired_) delete item;
}

u32 NiKeyframeManager::Hash(const std::string& key) {   // orig: 0x000C3700
    u32 h = 0;
    for (char c : key) h = h * 33 + static_cast<u32>(static_cast<s32>(static_cast<signed char>(c)));
    return h % kHashSize;
}

void NiKeyframeManager::SetAt(const std::string& key, NiSequence* seq) {   // orig: 0x00037B40
    Item*& head = buckets_[Hash(key)];
    for (Item* item = head; item; item = item->next)
        if (item->key == key) {
            item->value = seq;
            return;
        }
    head = new Item{key, seq, head};
    ++count_;
}

bool NiKeyframeManager::RemoveAt(const std::string& key) {   // orig: 0x0003B4B0
    Item** link = &buckets_[Hash(key)];
    for (Item* item = *link; item; link = &item->next, item = item->next)
        if (item->key == key) {
            *link = item->next;
            --count_;
            if (updating_) {
                retired_.push_back(item);
            } else {
                delete item;
            }
            return true;
        }
    return false;
}

NiKeyframeManager::Item* NiKeyframeManager::NextItem(const Item* item) const {   // orig: 0x0012FF20
    if (item->next) return item->next;
    for (u32 b = Hash(item->key) + 1; b < kHashSize; ++b)
        if (buckets_[b]) return buckets_[b];
    return nullptr;
}

NiSequence* NiKeyframeManager::GetSequence(const std::string& name) const {   // orig: 0x0001BFC0
    for (Item* item = buckets_[Hash(name)]; item; item = item->next)
        if (item->key == name) return item->value.Get();
    return nullptr;
}

void NiKeyframeManager::Update(f32 t) {               // orig: 0x00130030
    ++updating_;
    Item* pos = nullptr;
    for (Item* head : buckets_)
        if (head) {
            pos = head;
            break;
        }
    while (pos) {
        Item* item = pos;
        pos = NextItem(item);                         // the iterator moves on before the handler runs
        NiPointer<NiSequence> seq = item->value;
        if (!seq) continue;
        switch (seq->state) {
        case 1: seq->UpdateActive(t); break;
        case 2:
        case 3:
        case 5: seq->UpdateBlend(t); break;
        case 6: seq->UpdateMorphSource(t); break;
        case 8: seq->UpdateSum(t, sumWeight_); break;
        default: break;
        }
    }
    if (--updating_ == 0) {
        for (Item* item : retired_) delete item;
        retired_.clear();
    }
}

bool NiKeyframeManager::AddSequence(NiSequence* seq, bool link) {   // orig: 0x0012FFB0
    if (!seq->SetOwner(this)) return false;
    if (link) {
        u32 index = 0;                                // (the loop transform 0x0012CC00 only feeds accumulation)
        if (!seq->LinkToTargets(AsAV(targetPtr), index)) return false;   // the owner stays set
    }
    SetAt(seq->name, seq);
    return true;
}

bool NiKeyframeManager::RemoveSequence(NiSequence* seq) {   // orig: 0x00130FE0
    if (!seq) return false;
    NiPointer<NiSequence> keep(seq);
    seq->SetOwner(nullptr);
    return RemoveAt(seq->name);
}

void NiKeyframeManager::RemoveAllSequences() {        // orig: 0x00131010
    for (u32 b = 0; b < kHashSize; ++b)
        while (buckets_[b]) {
            NiPointer<NiSequence> seq = buckets_[b]->value;
            if (seq) seq->SetOwner(nullptr);
            RemoveAt(buckets_[b]->key);
        }
}

bool NiKeyframeManager::ActivateSequence(NiSequence* seq, s8 prio) {   // orig: 0x0012F3A0
    if (!seq->Activate(false, prio)) return false;
    seq->state = 1;
    return true;
}

bool NiKeyframeManager::DeactivateSequence(NiSequence* seq) {   // orig: 0x0012FAB0
    if (!seq->Deactivate(false)) return false;
    seq->state = 0;
    return true;
}

bool NiKeyframeManager::BlendFromSequence(NiSequence* src, NiSequence* dst, const char* destTextKey, f32 blendStart,
                                          f32 blendEnd, s8 prio) {   // orig: 0x0012F3D0
    if ((src && src->state != 0) || dst->state != 0) return false;
    if (!dst->Activate(true, prio)) return false;
    if (src && !src->Activate(true, prio)) return false;
    if (src) {
        bool mismatch = src->ControllerCount() != dst->ControllerCount();
        for (u32 i = 0; !mismatch && i < src->ControllerCount(); ++i)
            mismatch = AsAV(src->Controller(i)->targetPtr) != AsAV(dst->Controller(i)->targetPtr);
        if (mismatch) dst->syncBlendTransforms = true;
        src->partner = dst;
        dst->partner = src;
    } else {
        dst->partner = nullptr;
    }
    NiTimeController* rc = dst->ReferenceController();
    double k = 0.0;
    if (destTextKey)
        if (NiTextKeyExtraData* keys = TextKeys(dst))
            for (const auto& key : keys->keys)
                if (key.text == destTextKey) {
                    k = (double(key.time) - rc->phase) / rc->frequency;
                    break;
                }
    if (src) {
        src->endPoint = blendStart;
        src->state = 4;
    }
    dst->endPoint = blendEnd;
    dst->state = 5;
    dst->updateBlendTransforms = true;
    dst->offset = F(k / rc->frequency - blendEnd);    // (the key time is divided by the frequency twice)
    return true;
}

bool NiKeyframeManager::BlendDeactivate(NiSequence* src, NiSequence* dst) {   // orig: 0x001310D0
    if (dst->state != 5 || (src && src->state != 4)) return false;
    dst->Deactivate(false);
    if (!src) {
        RemoveSequence(GetSequence("TempBlendSource"));
    } else {
        src->Deactivate(true);
        src->state = 0;
        src->partner = nullptr;
    }
    dst->state = 0;
    dst->partner = nullptr;
    return true;
}

bool NiKeyframeManager::MorphActivate(NiSequence* src, NiSequence* dst, f32 morphStart, f32 morphEnd) {   // 0x0012F5D0
    if (src->state != 0 || dst->state != 0) return false;
    if (!dst->Activate(true, 0) || !src->Activate(true, 0)) return false;
    if (!SameTargets(src, dst)) {                     // "MorphActivate failed: ..." (formatted, not printed)
        dst->Deactivate(false);
        src->Deactivate(true);
        return false;
    }
    src->partner = dst;
    dst->partner = src;
    const double m = dst->ComputeMorphTime(src, F(double(src->offset) + morphStart));
    NiTimeController* rc = dst->ReferenceController();
    src->endPoint = morphStart;
    src->state = 6;
    dst->endPoint = morphEnd;
    dst->state = 7;
    dst->offset = F((m - rc->phase) / rc->frequency - morphStart);
    return true;
}

bool NiKeyframeManager::MorphDeactivate(NiSequence* src, NiSequence* dst) {   // orig: 0x0012FAE0
    if (src->state != 6 || dst->state != 7) return false;
    src->Deactivate(false);
    dst->Deactivate(true);
    src->state = dst->state = 0;
    src->partner = dst->partner = nullptr;
    return true;
}

bool NiKeyframeManager::SumActivate(NiSequence* s1, NiSequence* s2, f32 weight) {   // orig: 0x0012F8F0
    if (sumActive_) return false;
    if (s1->state != 0 || s2->state != 0) return false;
    if (!s2->Activate(true, 0) || !s1->Activate(true, 0)) return false;
    if (!SameTargets(s1, s2)) {                       // "SumActivate failed: ..." (formatted, not printed)
        s2->Deactivate(false);
        s1->Deactivate(true);
        return false;
    }
    sumWeight_ = weight;
    s1->tempPartner = s2->tempPartner = nullptr;
    s1->partner = s2;
    s2->partner = s1;
    s1->state = 8;
    s2->state = 9;
    s2->offset = s1->offset;
    sum1_ = s1;
    sum2_ = s2;
    sumActive_ = true;
    return true;
}

bool NiKeyframeManager::SumActivate(NiSequence* s1, NiSequence* s2, f32 weight, f32 transitionEnd) {   // 0x0012F790
    if (s1->state != 0 || s2->state != 0) return false;
    if (!s2->Activate(true, 0) || !s1->Activate(true, 0)) return false;
    if (!SameTargets(s1, s2)) return false;           // both stay activated, as in the original
    sumWeight_ = weight;
    s1->tempPartner = s2;
    s2->tempPartner = s1;
    s1->endPoint = transitionEnd;
    s1->partner = s2;
    s2->partner = s1;
    s1->state = 8;
    s2->state = 9;
    s2->offset = s1->offset;
    sum1_ = s1;
    sum2_ = s2;
    sumActive_ = true;
    return true;
}

bool NiKeyframeManager::SumDeactivate(NiSequence* s1, NiSequence* s2) {   // orig: 0x0012FB40
    if (s1->state != 8 || s2->state != 9) return false;
    s1->Deactivate(false);
    s2->Deactivate(true);
    s1->state = s2->state = 0;
    s1->partner = s2->partner = nullptr;
    sumActive_ = false;
    sum1_ = sum2_ = nullptr;
    return true;
}

void NiKeyframeManager::SetSumWeight(f32 weight) {    // orig: 0x0012FD60
    if (weight >= 0.0f && weight <= 1.0f) sumWeight_ = weight;   // (temp transforms: accumulation only)
}

bool NiKeyframeManager::DeactivateAll() {             // orig: 0x00131170
    RemoveSequence(GetSequence("TempBlendSource"));   // removed without deactivation; TempMixSource stays
    bool ok = true;
    for (u32 b = 0; b < kHashSize; ++b)
        for (Item* item = buckets_[b]; item; item = item->next) {
            NiSequence* seq = item->value.Get();
            if (!seq || seq->state == 0) continue;
            ok = seq->Deactivate(false) && ok;
            seq->state = 0;
            seq->partner = nullptr;
        }
    sumActive_ = false;
    sum1_ = sum2_ = nullptr;
    return ok;
}

}  // namespace fut::ni
