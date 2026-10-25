// Light manager; see LightManager.h.
#include "game/level/LightManager.h"

#include "core/CrtQsort.h"
#include "core/CrtRand.h"
#include "game/level/Camera.h"
#include "game/level/LevelData.h"
#include "game/object/NodeUpdate.h"

#include <bit>
#include <cmath>
#include <vector>

namespace fut::game {

using ni::detail::F;

LMTarget::LMTarget(ni::NiNode* node, u32 count, u32 targetKind, LightManager* mgr)   // orig: 0x000C0550
    : kind(targetKind), manager(mgr) {
    for (u32 i = 0; i < count; ++i) {
        ni::NiPointer<ni::NiPointLight> l(new ni::NiPointLight);   // attenuation 0 / 1 / 0
        l->switchOn = false;
        l->AttachAffectedNode(node);
        pointLights.push_back(l);
    }
    dirLight = new ni::NiDirectionalLight;
    dirLight->switchOn = false;
    dirLight->AttachAffectedNode(node);
    if (node) node->UpdateEffects();                  // 0x00102190 (the affected node; no null test in the original)
    fade = 0;
    if (manager) {
        next = manager->firstTarget;
        if (next) next->prev = this;
        manager->firstTarget = this;
    }
}

LMTarget::~LMTarget() {                               // orig: 0x000BFD00
    // the unlink 0x000BFC20 while the manager DAT_002EA820 exists (the port: `manager`, cleared by ~LightManager)
    if (manager) {
        if (next) next->prev = prev;
        if (prev) {
            prev->next = next;
        } else if (manager->firstTarget == this) {
            manager->firstTarget = next;
        }
    }
    // the point lights +0xC0 (a new[] of NiPointer, the eh vector destructor iterator 0x00166E30 with 0x0010DE50):
    // from the last index down to 0, then the directional light +0xC4 (0x000BFD73); the effect ids go back to the
    // LIFO free list in this order (docs/analysis/effect_release_order.md 3, rule A)
    for (size_t i = pointLights.size(); i-- > 0;) pointLights[i] = nullptr;
    pointLights.clear();
    dirLight = nullptr;
}

void LMTarget::UpdateWorldBound() {                   // orig: 0x000BFE00
    worldBound.center = world.translate;
    worldBound.radius = 0;
}

LMSource::LMSource(LightManager* mgr) : manager(mgr) {   // orig: 0x000C0790 (flags stay 0x80)
    if (manager) {
        if (manager->firstSource) manager->firstSource->prev = this;
        next = manager->firstSource;
        manager->firstSource = this;
    }
}

LMSource::~LMSource() {                               // orig: 0x000BFDC0 -> RemoveSource 0x000BFBC0
    if (!manager) return;
    if (next) next->prev = prev;
    if (prev) {
        prev->next = next;
    } else {
        manager->firstSource = next;
    }
}

void LMSource::UpdateWorldBound() {                   // orig: 0x000BFE00
    worldBound.center = world.translate;
    worldBound.radius = 0;
}

ni::NiPointer<LMSource> LightManager::AddLight(LevelData& level, const ni::NiPoint3& pos, const ni::NiColor& color,
                                               f32 radius, u32 type, bool attach) {   // orig: 0x000C08F0
    ni::NiPointer<LMSource> s(new LMSource(this));
    s->local.translate = pos;
    s->color = color;
    s->radius = radius;
    s->type = type;
    if (attach) {
        ni::NiRoom* room = level.roomGroup ? level.roomGroup->FindRoomContaining(pos) : nullptr;
        ni::NiNode* parent = room ? static_cast<ni::NiNode*>(room) : level.dynamicNode.Get();
        parent->AttachChild(s.Get(), true);
        s->UpdateDownwardPass(0.0f, true);            // orig: NiAVObject::Update(0, true)
    }
    return s;
}

LightManager::LightManager() {                        // orig: 0x000BFF20
    dirLight = new ni::NiDirectionalLight;
    dirLight->switchOn = false;
}

LightManager::~LightManager() {
    for (LMTarget* t = firstTarget; t; t = t->next) t->manager = nullptr;
    for (LMSource* s = firstSource; s; s = s->next) s->manager = nullptr;
}

namespace {

constexpr f32 kPiBss = std::bit_cast<f32>(0x40490FDBu);   // [0x002F9274] (FPATAN(1, 1) * 4, set at start-up)

// orig: 0x000BFE30 (thiscall source, camera): the source's score +0xE0: 0 for the global kinds 3 / 4, -1 without a
// radius, else its depth along the camera's direction (0 just behind the camera, within the radius); kind 8 x 5.
f32 Score(LMSource& s, const ni::NiCamera& cam) {
    if (s.type == 3 || s.type == 4) return s.score = 0.0f;
    if (!(s.radius > 0.0f)) return s.score = -1.0f;   // TEST AH,0x41; JNE (NaN too)
    const double dx = double(s.world.translate.x) - cam.world.translate.x;
    const double dy = double(s.world.translate.y) - cam.world.translate.y;
    const double dz = double(s.world.translate.z) - cam.world.translate.z;
    const double d = (dz * cam.worldDir.z + dy * cam.worldDir.y) + dx * cam.worldDir.x;
    s.score = F(d);                                   // FST (the comparisons on the register)
    if (d < 0.0 && d > -double(s.radius)) s.score = 0.0f;
    if (s.type == 8) s.score = F(double(s.score) * 5.0f);   // [0x00293C24]
    return s.score;
}

int CompareScore(const void* a, const void* b) {      // orig: 0x000BFB70 (larger first; NaN equal)
    const f32 x = (*static_cast<LMSource* const*>(a))->score, y = (*static_cast<LMSource* const*>(b))->score;
    if (x > y) return -1;
    if (x < y) return 1;
    return 0;
}

// orig: 0x000C0830 (thiscall source, (out, t)): the directional light's rotation by the source kind.
ni::NiMatrix3 SourceRotation(const LMSource& s, f32 t, const LevelData& level) {
    switch (s.type) {
    case 5: case 6: case 7: {                         // the liquids' swaying light: FSIN (the C library's, #35)
        const double a = std::sin(double(t) * 2.36f) * 0.36f - double(kPiBss) * 0.5f;   // [0x002A3234] / [0x002A3230]
        return ni::MakeZRotation(F(a));               // 0x001030F0
    }
    case 3: return ni::MakeZRotation(F(double(kPiBss) * 0.5f));
    case 4:                                           // the camera's local rotation ((S+0x8C)->+0x30 +0x30)
        if (level.camera && level.camera->sceneCamera) return level.camera->sceneCamera->local.rotate;
        return {};
    default: return {};                               // 0x00102FF0 (identity)
    }
}

}  // namespace

void LightManager::Update(const LevelData& level, f32 frameTime) {   // orig: 0x000C0B50
    if (!level.player) return;                        // S+0x94
    if (!playerTarget) playerTarget = new LMTarget(level.scene.Get(), 5, 2, this);   // new(0xE0), S+0x18 (0x00013E20)
    u32 n = 0;
    for (LMSource* s = firstSource; s; s = s->next) { // +0x04, next +0xE4
        if (s->type == 2) {
            // rand 0x000C0C70: ((r * [0x002955E8]) * 10.0f + 50.0f) * 0.2f + radius * 0.8f (x87, one FSTP)
            const double r = double(crt::Rand()) * std::bit_cast<f32>(0x38000100u);
            s->radius = F((r * 10.0f + 50.0f) * double(std::bit_cast<f32>(0x3E4CCCCDu)) +
                          double(s->radius) * double(std::bit_cast<f32>(0x3F4CCCCDu)));
        }
        UpdateNodeAt(s, 0.0f);                        // orig: NiAVObject::Update(0, 1) 0x001018D0
        ++n;
    }
    std::vector<LMSource*> sorted;                    // new(4n)
    sorted.reserve(n);
    const ni::NiCamera* cam = level.camera ? level.camera->sceneCamera.Get() : nullptr;   // (S+0x8C)->+0x30
    for (LMSource* s = firstSource; s; s = s->next)
        if (cam && !(Score(*s, *cam) < 0.0f)) sorted.push_back(s);   // FCOMP 0; TEST AH,5; JNP (NaN kept)
    if (!sorted.empty()) crt::Qsort(sorted.data(), sorted.size(), sizeof(LMSource*), CompareScore);   // 0x001679F0
    for (LMTarget* t = firstTarget; t; t = t->next) { // +0x08, next +0xD4
        UpdateNodeAt(t, 0.0f);                        // Update(0, 1)
        const ni::NiPoint3 p = t->world.translate;    // +0xA0
        const bool isPlayer = t == playerTarget.Get();
        u32 used = 0, directional = 0;
        for (LMSource* s : sorted) {
            const u32 ty = s->type;
            const bool pointKind = !(ty == 8 || ty == 9 || ty == 11 || ty == 2 || ty == 4 || ty == 3);
            if (isPlayer == pointKind) continue;
            const f32 r2 = F(double(s->radius) * s->radius);   // FLD; FLD ST0; FMUL; FSTP
            if (!isPlayer) {                          // (p - s) squared, compared unrounded: TEST AH,0x41; JE
                const double dx = double(p.x) - s->world.translate.x, dy = double(p.y) - s->world.translate.y,
                             dz = double(p.z) - s->world.translate.z;
                if ((dz * dz + dy * dy) + dx * dx > r2) continue;
            }
            if (ty == 3 || ty == 5 || ty == 6 || ty == 4 || ty == 7) {   // a directional light
                t->fade = F(double(frameTime) + t->fade);
                f32 dim = 1.0f;
                if (!isPlayer && t->fade < 0.5f) dim = F(double(t->fade) + t->fade);   // TEST AH,5; JP: the fade-in
                ++directional;
                ni::NiDirectionalLight& L = isPlayer ? *dirLight : *t->dirLight;
                L.local.translate = s->world.translate;   // +0x60 = source +0xA0
                L.diffuse = s->color;                 // +0xF0 = +0xC0
                ++L.revision;
                L.ambient = {0.0f, 0.0f, 0.0f};       // DAT_0030E940 (BSS: L, black)
                ++L.revision;
                L.specular = {0.0f, 0.0f, 0.0f};
                ++L.revision;
                L.local.rotate = SourceRotation(*s, t->fade, level);
                L.dimmer = dim;
                ++L.revision;
                L.switchOn = true;
                UpdateNodeAt(&L, 0.0f);
            } else if (used < t->pointLights.size()) {   // a point light
                ni::NiPointLight& L = *t->pointLights[used++];
                L.local.translate = s->world.translate;
                L.diffuse = s->color;
                ++L.revision;
                const f32 quadratic = F(10.0 / r2);   // FLD 10.0f; FDIV
                L.ambient = {0.0f, 0.0f, 0.0f};
                ++L.revision;
                L.specular = {0.0f, 0.0f, 0.0f};
                L.constantAtten = 1.0f;               // +0x110
                L.dimmer = 1.0f;
                L.linearAtten = 0.0f;                 // +0x114
                L.revision += 5;
                L.switchOn = true;
                L.quadraticAtten = quadratic;         // +0x118
                UpdateNodeAt(&L, 0.0f);
            }
        }
        if (!isPlayer && directional == 0 && t->dirLight->switchOn) {   // the fade-out
            const double d = double(t->dirLight->dimmer) - (double(frameTime) + frameTime);
            t->fade = 0.0f;
            f32 v = F(d);
            if (d < 0.0) {                            // FCOM 0; TEST AH,5; JP
                t->dirLight->switchOn = false;
                v = 0.0f;
            }
            t->dirLight->dimmer = v;
            ++t->dirLight->revision;
        }
        for (u32 i = used; i < t->pointLights.size(); ++i) t->pointLights[i]->switchOn = false;
    }
}

void LightManager::SetGlobalLight(LevelData& level, const ni::NiColor& color, const ni::NiPoint3& ambient, u32 type) {
    ni::NiPointer<LMSource> first = AddLight(level, {0, 0, 0}, color, 1.0f, type, false);   // vf04 (DAT_002F915C)
    if (first) globalSource = AddLight(level, {0, 0, 0}, color, 1.0f, type, false);   // (the cast's second call)
    else globalSource = nullptr;
    first = nullptr;
    level.globalAmbient = ambient;                    // DAT_002EA828
}

ni::NiPointer<LMTarget> LightManager::AddTarget(const ni::NiPoint3& offset, ni::NiNode* node) {   // orig: 0x000C0A50
    if (!node) return {};
    dirLight->AttachAffectedNode(node);
    ni::NiPointer<LMTarget> t(new LMTarget(node, 1, 1, this));
    t->local.translate = offset;
    node->AttachChild(t.Get(), true);
    t->UpdateDownwardPass(0.0f, true);                // orig: NiAVObject::Update(0.0, true)
    node->UpdateEffects();                            // 0x00102190
    return t;
}

}  // namespace fut::game
