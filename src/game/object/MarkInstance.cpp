// Scorch-mark factory instance and the scorch marks; see MarkInstance.h and docs/analysis/fireball_port.md 9.2.
#include "game/object/MarkInstance.h"

#include "core/CrtMath.h"
#include "game/GameRandom.h"
#include "game/GameTime.h"
#include "game/defs/EffectDefs.h"
#include "game/level/LevelData.h"
#include "game/object/NodeUpdate.h"

#include <bit>
#include <cmath>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

// ---- ScorchMark -----------------------------------------------------------------------------------------------------
ScorchMark::ScorchMark(LevelData& lvl, ScorchMarkDef& d) : GameObject(lvl, d) {}   // orig: 0x000725F3

ScorchMark::~ScorchMark() {                           // orig: 0x000722F0
    if (shape) node->DetachChild(shape.Get());        // vfA4 (the returned reference is released)
    shape = nullptr;
    material = nullptr;
    alpha = nullptr;
}

bool ScorchMark::Build(u32 markType) {                // orig: vf50 0x00072740
    (void)markType;
    const auto& d = static_cast<ScorchMarkDef&>(*def);
    value4C = 0.4f;                                   // orig: 0x3ECCCCCD
    lifetime = F(RandRange(4.0f, 10.0f));             // orig: RandRange 0x000B6FB0 (return 0x00072774)
    const double h = RandRange(0.65f, 1.0f) * 0.5f;   // orig: (return 0x000727A0); FMUL [0x00293AA0]
    const f32 lo = F(-h), hi = F(h);
    ni::NiPointer<ni::NiTriShapeData> data(new ni::NiTriShapeData);   // orig: new(0x30) / new(0x20) / new(0xC)
    data->vertexCount = 4;
    data->vertices = {{lo, lo, 0.01f}, {lo, hi, 0.01f}, {hi, hi, 0.01f}, {hi, lo, 0.01f}};   // 0x3C23D70A
    data->bound = ni::BoundOfPoints(data->vertices);
    data->textureSets = 1;
    data->uvs = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
    data->triangleCount = 2;
    data->triangleList = {0, 1, 2, 0, 2, 3};
    shape = new ni::NiTriShape;                       // orig: new(0xE0) NiTriShape(4, v, 0, 0, uv, 2, tris) 0x001164E0
    shape->modelData = data.Get();
    material = new ni::NiMaterialProperty;            // orig: new(0x5C) 0x00114B10
    material->emissive = {1.0f, 1.0f, 1.0f};          // orig: +0x40..+0x48 (L: emissive)
    ++material->revision;
    material->alpha = 1.0f;                           // orig: +0x50
    ++material->revision;
    AttachProperty(*shape, material.Get());           // orig: AddHead 0x00013A50
    alpha = new ni::NiAlphaProperty;                  // orig: new(0x1C), flags 0xEC -> 0x20ED
    alpha->flags = 0x20ED;
    AttachProperty(*shape, alpha.Get());
    if (d.texture) {                                  // orig: new(0x3C) 0x00109DC0, base texture def+0x20 (0x0001D610)
        ni::NiPointer<ni::NiTexturingProperty> tex = MakeTexturing(d.texture.Get());
        AttachProperty(*shape, tex.Get());
    }
    auto* z = new ni::NiZBufferProperty;              // orig: new(0x1C): flags 1, function 3
    z->flags = 1;
    z->testFunction = 3;
    AttachProperty(*shape, z);
    // orig: new(0x38) NiStencilProperty (vtbl 0x002949E4; the inline constructor 0x00072B2E), then +0x18 = 0, +0x34 = 3
    {                                                 // (the constructor's defaults: NiObjects.h)
        auto* st = new ni::NiStencilProperty;
        st->enabled = false;                          // byte +0x18 = 0
        st->drawMode = 3;                             // +0x34 = 3: both faces (D3DCULL_NONE)
        AttachProperty(*shape, st);
    }
    auto* vc = new ni::NiVertexColorProperty;         // orig: new(0x20) (vtbl 0x00293AD0): +0x18 = 0, +0x1C = 0
    vc->sourceVertexMode = 0;
    vc->lightingMode = 0;
    AttachProperty(*shape, vc);
    node->AttachChild(shape.Get(), true);             // vfA0
    shape->flags &= ~ni::kNiAVHidden;
    shape->local.translate = {};
    node->UpdateProperties();                         // 0x00072C26
    node->UpdateEffects();
    UpdateNodeAt(node.Get(), 0.0f);                   // orig: NiAVObject::Update(0, 1) 0x001018D0
    data->revisionFlags |= 1;                         // orig: (+0x40)->data(+0xC8)->+0x2E |= 1
    UpdateRoom();                                     // orig: 0x00096F20
    return true;
}

bool ScorchMark::Tick() {                             // orig: vf08 0x00072400
    if (dying) return false;                          // -> deleted by Session::Update step 7
    if (time > lifetime) {                            // FCOMP +0x50; TEST AH,0x41; JNE skip (NaN skips)
        node->DetachChild(shape.Get());               // vfA4 (also with a NULL shape, every frame after)
        shape = nullptr;
    }
    time = F(double(g_frameStep) + time);
    return true;
}

bool ScorchMark::PostUpdate() {                       // orig: vf0C 0x000724C0 (result ignored by the Session)
    if (dying) return false;
    const double half = double(lifetime) * 0.5f;      // FMUL [0x00293AA0]
    if (half < time) {                                // FCOM +0x24; TEST AH,5; JP skip (NaN skips)
        ++material->revision;
        material->alpha = F((double(lifetime) - time) / half);
    } else if (time > 0.0f) {                         // TEST AH,0x41; JNE skip (NaN skips)
        const double k = (half - time) / half;        // FDIVRP
        material->emissive = {F(k), F(k), F(k)};      // orig: +0x44, +0x48, then +0x40
        ++material->revision;
        material->alpha = 1.0f;
        ++material->revision;
    }
    if (shape) UpdateNodeAt(shape.Get(), time);       // orig: shape->Update(time, 1)
    return true;
}

ScorchMark* SpawnScorchMark(LevelData& level, ScorchMarkDef& d, u32 type, Owner* owner, const NiPoint3& pos,
                            const ni::NiMatrix3& rot, const char* label, const NiPoint3& n) {   // orig: 0x00072590
    if (!d.texture) return nullptr;                   // orig: def+0x20
    auto* m = new ScorchMark(level, d);               // orig: new(0x54); GameObject ctor; vtbl 0x0029C850
    m->SetOwner(owner);                               // vf10
    m->SetName(label);                                // 0x000968C0
    m->SetPosition(pos);                              // vf1C
    m->SetRotation(rot);                              // vf24
    m->type = type;                                   // orig: +0x38
    m->Build(type);                                   // vf50 0x00072740 (2 RandRange)
    const ni::NiMatrix3 R = m->node->local.rotate;    // vf30 +0x30
    const f32 spin = F(RandRange(0.0f, 3.0f));        // orig: 0x000B6FB0 (the 3rd draw, return 0x00072695)
    m->node->local.rotate = ni::Multiply(ni::AxisAngle(spin, n.x, n.y, n.z), R);   // 0x00103130, 0x00013B30
    UpdateNodeAt(m->node.Get(), 0.0f);                // orig: NiAVObject::Update(0, 1) 0x001018D0
    return m;
}

// ---- MarkInstance ---------------------------------------------------------------------------------------------------
MarkInstance::~MarkInstance() {                       // orig: 0x00072E00
    // (port guard, UB in the original: a slot can name a mark its stage already deleted; only the marks still in
    // the definition's instance list are written)
    auto alive = [&](const GameObject* m) {
        for (const GameObject* o = markDef ? markDef->firstInstance : nullptr; o; o = o->defNext)
            if (o == m) return true;
        return false;
    };
    for (u32 i = 0; i < capacity && i < marks.size(); ++i)   // +0x40 (JBE / JB: unsigned)
        if (marks[i] && alive(marks[i])) marks[i]->dying = 1;   // (+0x48)[i] +0x3C
    marks.clear();                                    // operator delete(+0x48), +0x48 = 0 (then 0x00065D40)
}

MarkInstance::MarkInstance(LevelData& lvl, SMarkFactoryDef& d, u32 n)   // orig: 0x00072D60
    : GameObject(lvl, d), capacity(n ? n : 0x10), marks(n ? n : 0x10, nullptr) {}

void MarkInstance::OnAttach() {                       // orig: 0x00072F70
    if (owner) node->UpdateDownwardPass(time, true);
}

bool MarkInstance::AddScorch(u32 type, const NiPoint3& pos, NiPoint3 n) {   // orig: 0x000730C0
    ni::Unitize(n);                                   // 0x000116A0 (in place, the length dropped)
    f32 angle;
    if (!(-1.0f < n.z)) {                             // FLD [0x293BF0]; FCOMP n.z; TEST AH,5; JP -> PI
        angle = std::bit_cast<f32>(0x40490FDBu);      // orig: [0x002F9274]
    } else if (!(n.z < 1.0f)) {                       // FCOMP [0x293A98]; TEST AH,5; JP -> 0
        angle = 0.0f;
    } else {
        angle = F(crt::Acos(double(n.z)));            // CRT acos 0x001673E4
    }
    const double z0 = double(n.z) * 0.0f;             // FMUL [0x293AB8] (kept)
    const f32 ax = F(double(n.y) - z0);
    const f32 ay = F(z0 - n.x);
    const f32 az = F(double(n.x) * 0.0f - double(n.y) * 0.0f);
    return AddMark(type, pos, ni::AxisAngle(angle, ax, ay, az), n);   // 0x00103130, then 0x00072F90
}

bool MarkInstance::AddMark(u32 type, const NiPoint3& pos, const ni::NiMatrix3& rot, const NiPoint3& n) {
    for (u32 k = 0; k < capacity; ++k) {              // orig: 0x00072F90
        ScorchMark* m = marks[k];
        if (!m) break;                                // the first empty slot ends the search
        const f32 r = F(double(markDef->Size(m->type)) + markDef->Size(type));
        const NiPoint3 w = m->GetWorldPosition();     // vf34
        const double dx = double(w.x) - pos.x, dy = double(w.y) - pos.y, dz = double(w.z) - pos.z;
        if (std::sqrt((dz * dz + dy * dy) + dx * dx) < r && m->shape) {   // TEST AH,5; JP skip (NaN skips)
            m->time = 0.0f;                           // a mark close by is refreshed: no new mark
            m->dying = 0;
            return false;
        }
    }
    if (ScorchMark* old = marks[next]) {
        old->dying = 1;                               // deleted by its next Tick
        marks[next] = nullptr;
    }
    marks[next] = SpawnScorchMark(level, *markDef, type, owner, pos, rot, label ? label->c_str() : nullptr, n);
    if (++next >= capacity) next = 0;
    return true;
}

MarkInstance* SpawnMarkInstance(LevelData& level, SMarkFactoryDef& def, Owner* owner, u32 capacity,
                                const char* label) {  // orig: 0x00072ED0
    auto* o = new MarkInstance(level, def, capacity);
    o->SetName(label);                                // (id 0: no trigger lookup)
    o->SetOwner(owner);
    o->markDef = def.mark;
    return o;
}

}  // namespace fut::game
