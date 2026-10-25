// Pickups (docs/analysis/pickup_port.md 1.3, 2). x87 conventions as in CharacterUpdate.cpp.
#include "game/object/Pickup.h"

#include "engine/ni/NiClone.h"
#include "engine/ni/NiCollision.h"
#include "game/GameTime.h"
#include "game/defs/CharacterDef.h"
#include "game/defs/Defs.h"
#include "game/defs/EffectDefs.h"
#include "game/level/Camera.h"
#include "game/level/Hud.h"
#include "game/level/LevelData.h"
#include "game/object/Character.h"
#include "game/object/Laser.h"
#include "game/object/NodeUpdate.h"
#include "game/GameRandom.h"
#include "game/sound/GameSound.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// The character whose inventory Owner (+0x38) `owner` is (owner - 0x38).
Character* InventoryHolder(Owner* owner) {
    auto* inv = dynamic_cast<Character::Inventory*>(owner);
    return inv ? &inv->holder : nullptr;
}

}  // namespace

// ---- construction ----------------------------------------------------------------------------------------------
Pickup::Pickup(LevelData& lvl, PickupDef& d) : GameObject(lvl, d) {   // orig: 0x00076AA0
    lvl.pickupFlareDef = lvl.defs ? lvl.defs->Find("effect_pickupflare") : nullptr;   // DAT_002E06AC (0x000C3F10)
}

Pickup::~Pickup() {                                   // orig: 0x00076F50
    keyName.clear();                                  // operator_delete(+0x70)
    RemoveModel();
}

void Pickup::RemoveModel() {                          // orig: slot 20 0x00076B80
    if (!model) return;
    ni::NiPointer<ni::NiAVObject> tmp = model;        // (the detach returns the child into a temporary)
    node->DetachChild(model.Get());                   // vfA4
    model = nullptr;
}

bool Pickup::Init() {                                 // orig: 0x000748F0
    if (model) return true;
    auto& d = static_cast<PickupDef&>(*def);
    ni::NiPointer<ni::NiObject> c = ni::Clone(d.model.Get());   // NiObject::Clone 0x000FE120
    model = dynamic_cast<ni::NiAVObject*>(c.Get());   // (RTTI 0x002F9190)
    node->AttachChild(model.Get(), true);             // vfA0
    model->local.translate = {};
    model->local.rotate = {};
    if (owner->IsOwnerType(4)) model->flags |= ni::kNiAVHidden;
    else model->flags &= ~ni::kNiAVHidden;
    node->UpdateProperties();                         // 0x000749A9
    node->UpdateDownwardPass(time, true);             // NiAVObject::Update(+0x24, 1) 0x001018D0
    return true;
}

// ---- per frame -------------------------------------------------------------------------------------------------
bool Pickup::Tick() {                                 // orig: 0x00076820
    if (model) {
        if (state == kIdle) {
            flareTimer = F(double(g_frameStep) + flareTimer);
        } else if (state == kFalling) {
            flareTimer = F(double(g_frameStep) + flareTimer);
            fallDt = F(double(g_frameStep) + fallDt);
        }
    }
    time = F(double(g_frameStep) + time);
    return true;
}

bool Powerup::Tick() {                                // orig: 0x00075650 (FCOMP 0.0; TEST AH,0x44; JP)
    if (amount == 0.0f) return false;                 // used up: deleted (NaN ticks on)
    return Pickup::Tick();
}

bool Charge::Tick() {                                 // orig: 0x00075650
    if (amount == 0.0f) return false;
    return Pickup::Tick();
}

bool Ammo::Tick() {                                   // orig: 0x00075000 (FILD)
    if (count == 0) return false;
    return Pickup::Tick();
}

bool Pickup::PostUpdate() {                           // orig: 0x00076FD0
    if (!model || (state != kIdle && state != kFalling)) return true;
    const NiPoint3 cam = level.camera ? level.camera->position : NiPoint3{};   // (S+0x8C)+0x48
    const NiPoint3 wp = GetWorldPosition();           // vf34
    const double dx = double(cam.x) - wp.x, dy = double(cam.y) - wp.y, dz = double(cam.z) - wp.z;
    const f32 dist2 = F((dz * dz + dy * dy) + dx * dx);
    if (dist2 < 40000.0f) {                           // [0x0029CE34]; TEST AH,5; JP
        if (owner->IsOwnerType(4)) node->flags |= ni::kNiAVHidden;
        else node->flags &= ~ni::kNiAVHidden;
    } else {
        node->flags |= ni::kNiAVHidden;               // farther than 200 units (or NaN)
    }
    if (node->framesNotDrawn < 10 &&                  // byte node+0xB2 (the cull pass, NiCull.h), unsigned (JAE)
        dist2 < 5625.0f &&                            // [0x0029CE30] (75 units); TEST AH,5; JP
        flareTimer > 1.1f) {                          // [0x00295644]; TEST AH,0x41; JNE
        if (Def* fx = level.pickupFlareDef)           // DAT_002E06AC
            (void)fx->SpawnEffect(level, node->worldBound.center, {0.0f, 1.0f, 0.0f});   // vf20 (result ignored)
        flareTimer = 0.0f;                            // (also without a flare definition)
    }
    if (state == kFalling) Fall();
    return true;
}

void Pickup::Fall() {                                 // orig: 0x00076C10
    const double t = fallDt;
    const f32 radius = node->worldBound.radius;       // node +0x2C
    const NiPoint3 old = node->local.translate;       // node +0x60
    NiPoint3 p;
    p.x = F(double(old.x) + t * velocity.x);          // product unrounded
    p.y = F(double(old.y) + F(t * velocity.y));
    p.z = F(double(old.z) + F(t * velocity.z));
    if (rising) {
        const double up = (1.0 * velocity.y + 0.0 * velocity.z) + 0.0 * velocity.x;   // (0,1,0) . v
        if (up <= 0.0) rising = 0;                    // FCOMP 0.0; TEST AH,0x41; JP (NaN keeps it)
    }
    if (!rising) {
        NiPoint3 dir{F(double(p.x) - old.x), F(double(p.y) - old.y), F(double(p.z) - old.z)};
        f32 dist = F(ni::Unitize(dir) + radius);      // 0x000116A0 (length in ST0); FADD radius
        if (const ni::RayHit hit = ni::RayCast(old, dir, dist, true, 3)) {
            const NiPoint3 n = hit.Normal();          // 0x000FA8C0
            const double d2 = ((double(n.z) * dir.z + double(n.y) * dir.y) + double(dir.x) * n.x) * 2.0;
            NiPoint3 r{F(double(dir.x) - n.x * d2), F(double(dir.y) - F(n.y * d2)), F(double(dir.z) - F(n.z * d2))};
            (void)ni::Unitize(r);
            const double s = double(dist) - radius;   // dist = the hit's t
            p.x = F(dir.x * s + old.x);
            p.y = F(double(F(dir.y * s)) + old.y);
            p.z = F(double(F(dir.z * s)) + old.z);
            const double speed = std::sqrt((double(velocity.x) * velocity.x + double(velocity.y) * velocity.y) +
                                           double(velocity.z) * velocity.z);
            velocity.x = F(r.x * speed * 0.5);        // [0x002D6630]
            velocity.y = F(double(F(r.y * speed)) * 0.5);
            velocity.z = F(double(F(r.z * speed)) * 0.5);
            ++bounces;
            const double v = std::sqrt((double(velocity.x) * velocity.x + double(velocity.y) * velocity.y) +
                                       double(velocity.z) * velocity.z);
            if (v <= 0.5) state = kIdle;              // [0x00293AA0]; TEST AH,0x41; JP
        }
    }
    const f32 angle = F(double(6.28318548f) * time * 2.0);   // NI_TWO_PI 0x002F9270; FMUL +0x24; FADD ST0,ST0
    node->local.rotate = ni::MakeYRotation(angle);    // 0x001030B0
    node->local.translate = p;                        // (no SetPosition: no UpdateRoom)
    // (node->vf30: no visible effect for NiNode)
    node->UpdateDownwardPass(0.0f, true);             // Update(0.0, 1)
    velocity.y = F(double(velocity.y) - 35.0 * fallDt);   // [0x002D662C]; FMUL +0x58; FSUBR +0x50
    airTime = F(double(airTime) + fallDt);
    fallDt = 0.0f;
}

// ---- owners ----------------------------------------------------------------------------------------------------
void Pickup::OnDetach() {                             // orig: 0x000748D0
    if (!owner) return;
    node->flags |= ni::kNiAVHidden;
    state = kIdle;
}

void Pickup::OnAttach() {                             // orig: 0x00076720
    if (!owner) return;
    if (owner->IsOwnerType(4)) {                      // collected into an inventory
        if (static_cast<s32>(id) <= 0 || !(level.GetObjFlags(id) & 2))   // signed (JLE)
            level.AddPickupValue(static_cast<PickupDef&>(*def).value);   // 0x000C3360(def+0x2C)
        level.SetObjFlags(id, level.GetObjFlags(id) | 2);
    }
    if (owner->IsOwnerType(4)) node->flags |= ni::kNiAVHidden;
    else node->flags &= ~ni::kNiAVHidden;
    state = owner->IsOwnerType(2) ? kIdle : kHeld;
    node->UpdateDownwardPass(time, true);
}

void Powerup::OnAttach() {                            // orig: 0x00074850
    if (!owner) return;
    if (owner->IsOwnerType(4)) {
        if (Character* c = InventoryHolder(owner)) c->Heal(amount);   // vf88 0x0005A770 -> 0x000357E0
        amount = 0.0f;
    }
    Pickup::OnAttach();
}

void Ammo::OnAttach() {                               // orig: 0x00074F40
    if (!owner) return;
    if (owner->IsOwnerType(4)) {
        if (auto* p = dynamic_cast<Player*>(InventoryHolder(owner))) {   // vf8C (players)
            if (ammoType == AmmoDef::kRocket) {
                p->AddAmmo(0, count);                 // (H: slot 0 for rockets)
            } else {
                for (s32 slot = 0; slot < 4; ++slot) p->AddAmmo(slot, count);
            }
        } else {
            NotPorted("Ammo::OnAttach: a non-player character's vf8C");
        }
        count = 0;
    }
    Pickup::OnAttach();
}

void Charge::OnAttach() {                             // orig: 0x00075690
    if (owner && owner->IsOwnerType(4)) {
        Character* c = InventoryHolder(owner);
        if (c && c->cdef.IsType(2)) {
            auto* p = static_cast<Player*>(c);
            p->AddCharge(amount);                     // vf15C 0x00050420
            amount = 0.0f;
            const f32 c = p->ChargeLevel();           // vf164 0x00050490 (FSTP esp+8)
            if (!level.quickLoading) {                // 0x00075702: DAT_002EAA48
                if (auto* d = level.defs ? dynamic_cast<ChargeUpEffectDef*>(level.defs->Find("ChargeUpEffect")) : nullptr) {
                    const NiPoint3 at = GetWorldPosition();   // vf34 (Session::FindDef 0x000C3F10, 0x0029CD14)
                    // def->0x00066290(P+0x38, &at, _ftol(c), "ChargeUp_eff" 0x0029CD04)
                    (void)d->SpawnAt(level, &p->inventory, at, static_cast<s32>(c), "ChargeUp_eff");
                }
                if (level.hud) level.hud->Command("chargetext", "show", -1.0f);   // 0x000E5B90 (0x0029CCF8 / 0x0029A420)
            } else if (level.hud) {
                level.hud->Command("charge", "charge", c);   // 0x0007577A: 0x000E5B90(0x0029A698 twice, c)
            }
        }
    }
    Pickup::OnAttach();
}

bool Pickup::GetKey(std::string& out) const {         // orig: 0x00076600
    char buf[0x40];
    if (!keyName.empty()) {                           // +0x70 (a NULL pointer: empty in the port)
        std::snprintf(buf, sizeof buf, "%s", keyName.c_str());   // sprintf(out, "%s") 0x00296C7C
        out = buf;
        return true;
    }
    if (keyA == -1 || keyB == -1) return false;       // +0x68 / +0x6C
    std::snprintf(buf, sizeof buf, "$%03d:%03d", static_cast<int>(keyA), static_cast<int>(keyB));   // 0x0029CDA8
    out = buf;
    return true;
}

void Pickup::ApplyKey(const char* s) {                // orig: 0x00076650
    if (!s) return;
    if (*s == '$') {
        ++s;
        if (std::strlen(s) != 7) return;
        char a[4] = {s[0], s[1], s[2], '\0'};         // [s + 3] = 0 (the line buffer), then atoi(s), atoi(s + 4)
        keyA = std::atoi(a);
        keyB = std::atoi(s + 4);
        return;
    }
    keyName = s;                                      // new(strlen + 1) copy into +0x70 (an old copy leaks)
}

void Pickup::StoreKeyState(u8 v) {                    // orig: 0x000766D0
    if (!keyName.empty()) {                           // 0x000777F0(v, +0x70) -> NiTStringMap::SetAt(key, v)
        level.SetNibblerState(keyName, v);
        return;
    }
    char key[0x100];                                  // 0x00077810(v, +0x68, +0x6C): sprintf "$%03d:%03d", SetAt
    std::snprintf(key, sizeof key, "$%03d:%03d", static_cast<int>(keyA), static_cast<int>(keyB));
    level.SetNibblerState(key, v);
}

bool Pickup::CanCollect() const {                     // orig: 0x000767D0
    if (!pickupable) return false;
    if (state != kFalling || bounces >= 1) return true;
    const double up = (1.0 * velocity.y + 0.0 * velocity.z) + 0.0 * velocity.x;
    return up <= 0.0;                                 // FCOMP 0.0; TEST AH,0x41; JP -> false (NaN: false)
}

// ---- the definitions' Spawn (pickup_port.md 1.3): no owner-type check, no ready check -----------------------------
namespace {
template <class T>
GameObject* SpawnPickup(T* o, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot, const char* label,
                        bool hasModel) {
    o->SetOwner(owner);                               // vf10
    o->SetName(label);                                // 0x000968C0
    o->SetPosition(pos);                              // vf1C (UpdateRoom)
    o->SetRotation(rot);                              // vf24
    if (hasModel) o->Init();                          // def+0x20
    return o;
}
}  // namespace

GameObject* PickupDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                             const char* label) {     // orig: 0x00077150 (new(0x74))
    return SpawnPickup(new Pickup(level, *this), owner, pos, rot, label, model.Get() != nullptr);
}

GameObject* PowerupDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                              const char* label) {    // orig: 0x00074B00 (new(0x78))
    return SpawnPickup(new Powerup(level, *this, amount), owner, pos, rot, label, model.Get() != nullptr);
}

GameObject* ChargeDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                             const char* label) {     // orig: 0x000758E0 (new(0x78))
    return SpawnPickup(new Charge(level, *this, amount), owner, pos, rot, label, model.Get() != nullptr);
}

GameObject* AmmoDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                           const char* label) {       // orig: 0x00075170 (new(0x7C))
    return SpawnPickup(new Ammo(level, *this, count, ammoType), owner, pos, rot, label, model.Get() != nullptr);
}

// ---- the weapon (owl_weapon_port.md 8) ------------------------------------------------------------------------
// A definition created at run time joins the level's list (orig: Def::Def 0x000962E0); a LaserDef's constructor appends
// its "Something" spark emitter after itself. (RailDef: the same constructor, then its own vtable 0x0007BE80.)
template <class T>
T* NewLaser(LevelData& level, const char* name) {     // orig: new(0xA8) + 0x00079BF0
    auto owned = std::make_unique<T>();
    T* d = owned.get();
    d->name = name;
    level.defs->Add(std::move(owned));
    std::vector<std::unique_ptr<Def>> extra;
    d->Construct(level, extra);
    for (auto& e : extra) level.defs->Add(std::move(e));
    return d;
}
template LaserDef* NewLaser<LaserDef>(LevelData&, const char*);
template RailDef* NewLaser<RailDef>(LevelData&, const char*);

namespace {
SpriteEmitterDef* NewParticles(LevelData& level, const char* texture, f32 size, f32 r30a, f32 r30b) {
    auto owned = std::make_unique<SpriteEmitterDef>();   // orig: new(0x114) + 0x0006E090("Particles")
    SpriteEmitterDef* d = owned.get();
    d->name = "Particles";
    level.defs->Add(std::move(owned));
    d->SetSpriteTexture(texture, level);              // vfA4
    d->SetRange40(0.0f, 0.0f);                        // vf44(0)
    d->SetRange28(20.0f, 150.0f);                     // vf28
    d->SetSize(size, size);                           // vfB4
    d->SetGravity(10.0f);                             // vf50
    d->SetValue4C(2.0f);                              // vf4C
    d->SetRange38(0.0f, 0.15f);                       // vf3C (0x3E19999A)
    d->SetRange30(r30a, r30b);                        // vf30
    d->SetCount(40);                                  // vf48
    d->SetFlagA8(1);                                  // vfAC
    d->SetFlagDC(1);                                  // vfC8
    return d;
}

FlareDef* NewFlare(LevelData& level, const char* name, const char* mesh) {   // orig: new(0x34) + 0x0006A000
    auto owned = std::make_unique<FlareDef>();
    FlareDef* d = owned.get();
    d->name = name;
    level.defs->Add(std::move(owned));
    d->SetValue(0.1f);                                // vf24 (0x3DCCCCCD)
    d->mesh = level.FindEffectObject(mesh);           // 0x000C2AF0 + DynamicCast NiAVObject (0x002F9190)
    return d;
}

void SetLaser(LaserDef& l, f32 v68, f32 v6C, f32 v70, f32 v78, f32 v7C, f32 v80) {
    l.value68[0] = v68, l.value68[1] = v6C, l.value68[2] = v70;   // +0x68 / +0x6C / +0x70
    l.value68[4] = v78, l.value68[5] = v7C;                     // +0x78 / +0x7C
    l.value80 = v80;
}
}  // namespace

Weapon::Weapon(LevelData& lvl, WeaponDef& d) : Pickup(lvl, d) {   // orig: 0x0007D1F0
    nextState = state;                                // +0x84 = +0x38
    weaponType = d.weaponType;                        // +0x98 = WD+0x30
    ammo = d.ammo;                                    // +0xB4 = WD+0x7C
    // (+0x88..+0x94 are not written by the original; +0xA8 / +0xAC only for the shotgun)
    if (!lvl.defs) return;
    switch (weaponType) {
    case WeaponDef::kShotgun: {
        ammoSlot = 1;
        LaserDef* a = NewLaser(lvl, "ShotgunLaser");
        SetLaser(*a, 0.0f, 0.0f, 1.0f, 0.9f, 2.0f, 7.5f);   // (write order: +0x68 +0x6C +0x70 +0x80 +0x78 +0x7C)
        a->flag85 = 1;
        a->value98 = 200.0f;
        LaserDef* b = NewLaser(lvl, "ShotgunLaserCharge");
        SetLaser(*b, 0.0f, 0.0f, 1.0f, 4.0f, 3.0f, 15.0f);
        b->flag85 = 1;
        b->value98 = 200.0f;
        projectile = a, projectile2 = b;
        particles = NewParticles(lvl, "Fry_pistolshotgunparticle.tga", 0.4f, 0.2f, 0.6f);
        particles2 = NewParticles(lvl, "Trooper_weaponparticle.tga", 0.2f, 0.0f, 0.4f);
        break;
    }
    case WeaponDef::kRailgun: {                       // orig: 0x0007D5A6 (natives_batch_b_port.md 4.2)
        ammoSlot = 2;
        RailDef* a = NewLaser<RailDef>(lvl, "RailgunLaser");   // orig: new(0xA8) + 0x0007BE80 (0x0029D248)
        a->value68[0] = 0.0f, a->value68[1] = 0.0f, a->value68[2] = 1.0f;   // +0x68 / +0x6C / +0x70
        a->value80 = 300.0f;                          // +0x80 (0x43960000)
        a->value68[4] = 2.0f, a->value68[5] = 2.0f;   // +0x78 / +0x7C
        a->flag85 = 0;
        a->value98 = 200.0f;                          // (0x43480000)
        a->flag84 = 1;
        projectile = a;                               // +0xA0
        RailDef* b = NewLaser<RailDef>(lvl, "RailgunLaserCharge");   // (0x0029D234)
        b->value68[0] = 0.0f, b->value68[1] = 0.0f, b->value68[2] = 1.0f;
        b->value80 = 300.0f;
        b->value68[5] = 3.0f, b->value68[4] = 4.0f;   // (write order: +0x7C, +0x78)
        b->flag85 = 1;
        b->value98 = 200.0f;                          // (+0x84 is not written: 0)
        projectile2 = b;                              // +0xA4
        break;
    }
    case WeaponDef::kMachineGun: {
        ammoSlot = 3;
        LaserDef* a = NewLaser(lvl, "MachinegunLaser");
        a->value68[4] = 0.5f, a->value68[5] = 2.0f, a->value1C = 120.0f;
        a->value68[0] = 1.0f, a->value68[1] = 0.0f, a->value68[2] = 0.0f, a->value80 = 20.0f;
        LaserDef* b = NewLaser(lvl, "MachinegunLaser");
        b->value68[4] = 0.5f, b->value68[5] = 5.0f, b->value1C = 120.0f;
        b->value68[0] = 0.0f, b->value68[1] = 0.0f, b->value68[2] = 1.0f, b->value80 = 100.0f;
        projectile = a, projectile2 = b;
        flare = NewFlare(lvl, "TrooperFlare", "Trooper_muzzle");
        break;
    }
    case WeaponDef::kPistol: {                        // (level1-1 PE_Pistola)
        ammoSlot = 0;
        LaserDef* a = NewLaser(lvl, "PistolLaser");
        a->value68[4] = 1.1f, a->value68[5] = 7.0f, a->value1C = 70.0f;
        a->value68[0] = 0.0f, a->value68[1] = 0.0f, a->value68[2] = 1.0f, a->value80 = 20.0f;
        {
            ni::NiObjectNET* glow = lvl.FindTexture("bolt_glow.tga");   // (the glow is looked up first)
            ni::NiObjectNET* beam = lvl.FindTexture("bolt_beam.tga");
            a->SetTextures(beam, glow);               // vf24
        }
        a->flag84 = 1;
        LaserDef* b = NewLaser(lvl, "PistolLaser");
        b->value68[4] = 2.0f, b->value68[5] = 7.0f, b->value1C = 70.0f;
        b->value68[0] = 0.0f, b->value68[1] = 0.0f, b->value68[2] = 1.0f, b->value80 = 25.0f;
        {
            ni::NiObjectNET* glow = lvl.FindTexture("bolt_glow.tga");
            ni::NiObjectNET* beam = lvl.FindTexture("bolt_beam.tga");
            b->SetTextures(beam, glow);
        }
        b->flag84 = 1;
        auto ripple = std::make_unique<RippleDef>();  // orig: new(0x34) 0x00071710("RippleLaser", "Ripple_white_circle.tga", 0)
        ripple->name = "RippleLaser";
        ripple->texture = lvl.FindTexture("Ripple_white_circle.tga");
        ripple->value1C = 0.0f;
        ripple->value20 = 1.0f;                       // (written after the constructor's 0.3)
        b->ripple8C = lvl.defs->Add(std::move(ripple));   // +0x8C
        projectile = a, projectile2 = b;
        flare = NewFlare(lvl, "PistolFlare", "Fry_Pistol_Muzzlefire");
        break;
    }
    default:                                          // kHammer (level1-1 PE_Hammer): nothing
        break;
    }
}

Weapon::~Weapon() {                                   // orig: 0x0007DAF0 (then ~Pickup 0x00076F50)
    if (voice && level.Sound().VoiceExists(static_cast<s32>(voice))) {   // 0x0007DB2B (no WD+0x6C test here)
        level.Sound().StopVoice(static_cast<s32>(voice));   // Voice_Stop 0x000C9B10
        voice = 0;
    }
    if (groundModel) {
        ni::NiPointer<ni::NiNode> tmp = groundModel;
        node->DetachChild(groundModel.Get());
        groundModel = nullptr;
    }
    if (heldModel) {
        ni::NiPointer<ni::NiNode> tmp = heldModel;
        node->DetachChild(heldModel.Get());
        heldModel = nullptr;
    }
}

bool Weapon::Init() {                                 // orig: 0x0007DDB0
    if (heldModel) return true;
    auto& d = static_cast<WeaponDef&>(*def);
    if (owner->IsOwnerType(2)) {
        ni::NiPointer<ni::NiObject> c = ni::Clone(d.model.Get());   // 0x000FE120 (WD+0x20)
        groundModel = dynamic_cast<ni::NiNode*>(c.Get());          // RTTI 0x002F9230
        if (groundModel) {
            node->AttachChild(groundModel.Get(), true);   // vfA0
            groundModel->flags &= ~ni::kNiAVHidden;
            groundModel->local.translate = {};
            groundModel->local.rotate = {};           // identity 0x002D7820
            groundModel->flags = static_cast<u16>(groundModel->flags & 0xFFF1);   // 0x00101B80(0)
        }
    }
    ni::NiPointer<ni::NiObject> c2 = ni::Clone(d.model2.Get());    // WD+0x34
    heldModel = dynamic_cast<ni::NiNode*>(c2.Get());
    if (heldModel) {
        node->AttachChild(heldModel.Get(), true);
        if (owner->IsOwnerType(2)) heldModel->flags |= ni::kNiAVHidden;
        else heldModel->flags &= ~ni::kNiAVHidden;
        heldModel->flags = static_cast<u16>(heldModel->flags & 0xFFF1);
    }
    (void)WeaponType();                               // vf7C (result unused)
    node->UpdateProperties();                         // 0x0007DEFD
    node->UpdateDownwardPass(time, true);             // Update(+0x24, 1)
    return true;
}

bool Weapon::Tick() {                                 // orig: 0x0007CFB0
    if (stateTimer > 0.0f) {                          // TEST AH,0x41; JNE
        const double x = double(stateTimer) - g_frameStep;   // FLD; FSUB [step]; FST
        stateTimer = F(x);
        if (x <= 0.0) {                               // TEST AH,0x41; JP (NaN: no)
            const u32 s = nextState;
            stateTimer = 0;
            state = 0;
            if (s == 3) TakeOut();
            else if (s == 5) PutAway();
            else if (s == 7) (void)FireRequest(0);   // vf68(0, 0): state is 0 now, so it returns at once
            else if (s == 8) {                        // vf74 0x000AEDD0 (false)
            } else {
                state = s;
            }
        }
    }
    (void)GetObjectByName(heldModel.Get(), "tip");    // (+0x7C)->vf54("tip"): result unused
    return Pickup::Tick();                            // 0x00076820 (model +0x3C is null: time only)
}

void Weapon::OnAttach() {                             // orig: 0x0007D050
    if (!owner) return;
    if (groundModel) {
        if (owner->IsOwnerType(4)) groundModel->flags |= ni::kNiAVHidden;
        else groundModel->flags &= ~ni::kNiAVHidden;
    }
    if (heldModel) {
        if (owner->IsOwnerType(2)) heldModel->flags |= ni::kNiAVHidden;
        else heldModel->flags &= ~ni::kNiAVHidden;
    }
    state = owner->IsOwnerType(2) ? kIdle : kHeld;    // NEG; SBB; AND -5; ADD 6
    node->UpdateDownwardPass(time, true);
}

void Weapon::OnDetach() {                             // orig: 0x0007DC50
    if (!owner) return;
    if (groundModel) {
        ni::NiPointer<ni::NiNode> tmp = groundModel;
        node->DetachChild(groundModel.Get());         // vfA4
        groundModel = nullptr;
    }
    if (heldModel) heldModel->flags |= ni::kNiAVHidden;
    state = kIdle;
}

bool Weapon::TakeOut() {                              // orig: 0x0007DD00
    if (state == 3 || state == 4) return false;
    if (stateTimer == 0.0f) {                         // TEST AH,0x44; JP
        state = 3;
        stateTimer = 1.0f;
        nextState = 4;
        const u32 loop = static_cast<WeaponDef&>(*def).loopSound;   // WD+0x6C
        if (loop && (!voice || !level.Sound().VoiceExists(static_cast<s32>(voice))))   // Voice_Exists 0x0007DD5D
            voice = static_cast<u32>(level.Sound().PlayAttached(loop, node.Get(), true));   // 0x000CAB80 -> +0x9C
        return true;
    }
    nextState = 3;
    return true;
}

bool Weapon::PutAway() {                              // orig: 0x0007CEB0
    if (state == 5 || state == 6) return false;
    if (stateTimer == 0.0f) {
        state = 5;
        stateTimer = 1.0f;
        nextState = 6;
        if (static_cast<WeaponDef&>(*def).loopSound && voice &&
            level.Sound().VoiceExists(static_cast<s32>(voice))) {   // 0x0007CF03
            level.Sound().StopVoice(static_cast<s32>(voice));       // Voice_Stop 0x000C9B10
            voice = 0;
        }
        return true;
    }
    nextState = 5;
    return true;
}

// ---- inventory_batch1_port.md 5.1 / 5.2 (checked against the assembly of 0x0007D140, 0x0007ECC0, 0x0007DF80,
// 0x0007CF40, 0x0007D0D0 and 0x0007DF20) ----------------------------------------------------------------------------
namespace {

// orig: the CRT _ftol 0x00167110 (truncation toward zero; the low 32 bits of the 64-bit result).
s32 FtoL(double v) { return static_cast<s32>(static_cast<s64>(v)); }

// The pistol / machine gun / shotgun shot colour (LaserDef +0x68..+0x70, dword stores).
void SetColour(LaserDef& l, f32 r, f32 g, f32 b) { l.value68[0] = r, l.value68[1] = g, l.value68[2] = b; }

}  // namespace

Player* Weapon::Holder() const {                      // owner ? owner - 0x38 : 0 (no type check in the original)
    return dynamic_cast<Player*>(InventoryHolder(owner));
}

const char* Weapon::AnimName(u32 i) const {           // orig: vf78 0x0007CF40 (CMP 6; JA: unsigned)
    if (i > 6) return nullptr;
    const std::string& n = static_cast<const WeaponDef&>(*def).animations[i];   // WD+0x38 + 4 i
    return n.empty() ? nullptr : n.c_str();
}

NiPoint3 Weapon::TipPosition() {                      // orig: vf80 0x0007D0D0
    if (heldModel)                                    // +0x7C
        if (ni::NiAVObject* tip = GetObjectByName(heldModel.Get(), "tip")) {   // vf54(0x00295D98)
            UpdateNodeAt(tip, time);                  // NiAVObject::Update(+0x24, 1)
            return tip->world.translate;              // +0xA0
        }
    return {};                                        // ZERO 0x002F915C
}

ni::NiMatrix3 Weapon::TipRotation() const {           // orig: vf84 0x0007DF20
    if (heldModel)
        if (ni::NiAVObject* tip = GetObjectByName(heldModel.Get(), "tip")) return tip->world.rotate;   // +0x70
    return ni::NiMatrix3{{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}};   // (leftover stack words in the original, L)
}

bool Weapon::FireRequest(u32 kind) {                  // orig: vf68 0x0007D140 (the second argument is not read)
    if (state != 4) return false;                     // +0x38: only "ready"
    nextState = 4;                                    // +0x84
    state = 7;                                        // firing
    Player* h = Holder();                             // owner - 0x38 (no null check afterwards)
    if (!h) {
        NotPorted("Weapon::FireRequest: a weapon without a holder (the original dereferences address 0)");
        return false;
    }
    NiPoint3 p = h->GetWorldPosition();               // vf34
    p.y = F(double(p.y) + 5.0f);                      // FADD [0x00293C24]
    if (ni::NiAVObject* tip = GetObjectByName(heldModel.Get(), "tip"))   // +0x7C vf54("tip"): +0x7C is not tested
        p = tip->world.translate;                     // +0xA0 (dwords)
    const ni::NiMatrix3 R = h->GetWorldRotation();    // vf38
    return Fire(p, R, kind, 0.0f);                    // vf70(&p, &R, kind, 0)
}

bool Weapon::FireDir(const NiPoint3& pos, NiPoint3 dir, u32 a, f32 b) {   // orig: vf6C 0x0007ECC0
    if (state != 4) return false;
    (void)ni::Unitize(dir);                           // 0x000116A0 (FSTP ST0)
    const NiPoint3 up = dir.y > 0.9f ? NiPoint3{0, 0, 1} : NiPoint3{0, 1, 0};   // [0x00293C08]; TEST AH,0x41; JNE -> UNIT_Y
    NiPoint3 s{F(double(dir.z) * up.y - double(up.z) * dir.y), F(double(up.z) * dir.x - double(dir.z) * up.x),
               F(double(up.x) * dir.y - double(up.y) * dir.x)};   // up x dir
    (void)ni::Unitize(s);
    ni::NiMatrix3 R;                                  // columns s | s x dir | dir (each element FSTP)
    R.m[0][0] = s.x, R.m[1][0] = s.y, R.m[2][0] = s.z;
    R.m[0][1] = F(double(s.y) * dir.z - double(s.z) * dir.y);
    R.m[1][1] = F(double(s.z) * dir.x - double(dir.z) * s.x);
    R.m[2][1] = F(double(s.x) * dir.y - double(s.y) * dir.x);
    R.m[0][2] = dir.x, R.m[1][2] = dir.y, R.m[2][2] = dir.z;
    return Fire(pos, R, a, b);                        // vf70
}

bool Weapon::Fire(const NiPoint3& pos, const ni::NiMatrix3& rot, u32 charged, f32 charge) {   // orig: 0x0007DF80
    stateTimer = 0.1f;                                // +0x80 = 0x3DCCCCCD (every type, the hammer too)
    if (weaponType == WeaponDef::kHammer) return false;
    Player* h = Holder();                             // re-read from +4 at every use (no null check)
    if (!h) {
        NotPorted("Weapon::Fire: a weapon without a holder (the original dereferences address 0)");
        return false;
    }
    // the projectile's shooter (0x0007E04E / 0x0007E608): +0x40 = owner - 0x38 when the owner is an inventory
    auto shooterLink = [&](GameObject* o) {
        if (o && owner->IsOwnerType(4))
            if (auto* pr = dynamic_cast<Projectile*>(o)) pr->shooter = h;
    };
    // the sounds (D+0x54 fire, +0x58 secondary, +0x5C click): the holder's first-person flag (vf148 0x00050AD0,
    // +0xB88, re-read at each sound) picks Sound_PlaySFX 0x000C99A0 (2D) or Sound_Play3D 0x000C9AC0 at pos, no loop;
    // the machine gun then sets the voice's pitch 0.75 (Voice_SetPitch 0x000C9BD0, 0x3F400000)
    auto sound = [&](u32 s) -> s32 {
        return h->firstPerson ? level.Sound().PlaySfx(s, false) : level.Sound().Play3D(s, pos, false);
    };
    const auto& wd = static_cast<const WeaponDef&>(*def);
    auto muzzleFlare = [&] {                          // 0x0007E25C: the muzzle flare
        const NiPoint3 dx = ni::MulVector(rot, {1.0f, 0.0f, 0.0f});   // 0x00011770 with UNIT_X
        if (this->flare) (void)this->flare->SpawnEffect(level, pos, dx);   // +0xB0 vf20(pos, &dx)
    };
    auto damageOf = [](GameObject* o) -> f32* {       // Laser +0x148
        auto* l = dynamic_cast<Laser*>(o);
        return l ? &l->damage : nullptr;
    };
    auto shotCount = [&]() {                          // FLD charge; FADD ST0,ST0; FADD 1.0; _ftol; at most the ammo
        s32 n = FtoL(double(charge) + charge + 1.0f); // [0x00293A98]
        if (n > h->GetAmmo(ammoSlot)) n = h->GetAmmo(ammoSlot);   // JLE (signed)
        return n;
    };
    switch (weaponType) {
    case WeaponDef::kPistol:
        if (!charged) {
            if (h->GetAmmo(ammoSlot) < 1) {           // vf94: the click (0x0007E21A), the flare
                (void)sound(wd.clickSound);
                muzzleFlare();
                return false;
            }
            h->UseAmmo(ammoSlot, 1);                  // vf90
            GameObject* o = projectile->Spawn(level, &level, pos, rot, "Pistol-Laser");   // +0xA0 vf14(S, ..) 0x0029CFA0
            (void)sound(wd.fireSound);                // 0x0007E046 / 0x0007E080 (whether or not o)
            shooterLink(o);
        } else {
            if (h->GetAmmo(ammoSlot) < 1) {           // the click (0x0007E217)
                (void)sound(wd.clickSound);
                muzzleFlare();
                return false;
            }
            const s32 n = shotCount();
            h->UseAmmo(ammoSlot, n);
            auto& L = static_cast<LaserDef&>(*projectile2);   // +0xA4
            if (n == 5) SetColour(L, 1, 1, 1);
            else if (n > 3) SetColour(L, 1, 0, 0);
            else if (n > 1) SetColour(L, 1, 1, 0);
            else SetColour(L, 0, 0, 1);
            if (GameObject* o = L.Spawn(level, &level, pos, rot, "Pistol-LaserCharge")) {   // 0x0029D348
                if (f32* dmg = damageOf(o))           // FILD n; FMUL [0x00293C18]; FMUL +0x80 -> +0x148
                    *dmg = F(double(n) * 0.8f * L.value80);
                (void)sound(wd.secondarySound);       // 0x0007E203.. (only with a shot)
                shooterLink(o);
            }
        }
        muzzleFlare();
        return false;
    case WeaponDef::kMachineGun:
        if (!charged) {
            if (h->GetAmmo(ammoSlot) < 1) { muzzleFlare(); return false; }   // (no click sound)
            SetColour(static_cast<LaserDef&>(*projectile), 0, 0, 1);
            h->UseAmmo(ammoSlot, 1);
            GameObject* o = projectile->Spawn(level, &level, pos, rot, "Tommy-Laser");
            level.Sound().SetPitch(sound(wd.fireSound), 0.75f);   // 0x0007E350 / 0x0007E513, 0x000C9BD0
            shooterLink(o);
        } else {
            if (h->GetAmmo(ammoSlot) < 1) { muzzleFlare(); return false; }
            const s32 n = shotCount();
            h->UseAmmo(ammoSlot, n);
            auto& L = static_cast<LaserDef&>(*projectile2);
            if (n == 5) SetColour(L, 1, 1, 1);
            else if (n > 3) SetColour(L, 1, 0, 0);
            else if (n > 1) SetColour(L, 1, 1, 0);
            else SetColour(L, 0, 0, 1);
            GameObject* o = L.Spawn(level, &level, pos, rot, "Tommy-Laser2");
            if (!o) { muzzleFlare(); return false; }
            if (f32* dmg = damageOf(o)) *dmg = F(double(n) * 0.6f * L.value80);   // [0x002976C0]
            level.Sound().SetPitch(sound(wd.secondarySound), 0.75f);   // 0x0007E4F6 / 0x0007E513
            shooterLink(o);
        }
        muzzleFlare();
        return false;
    case WeaponDef::kRailgun:
        if (!charged) {
            if (h->GetAmmo(ammoSlot) < 1) {           // the click (0x0007E752)
                (void)sound(wd.clickSound);
                muzzleFlare();
                return false;
            }
            h->UseAmmo(ammoSlot, 1);
            GameObject* o = projectile->Spawn(level, &level, pos, rot, "RailLaser");   // RailDef vf20 0x0007CDB0
            (void)sound(wd.fireSound);                // 0x0007E600 / 0x0007E63D
            shooterLink(o);
        } else {
            if (h->GetAmmo(ammoSlot) < 1) {           // the click (0x0007E74F)
                (void)sound(wd.clickSound);
                muzzleFlare();
                return false;
            }
            const s32 n = shotCount();
            h->UseAmmo(ammoSlot, n);
            GameObject* o = projectile2->Spawn(level, &level, pos, rot, "RailLaserCharge");   // (no colour)
            if (!o) { muzzleFlare(); return false; }
            if (f32* dmg = damageOf(o)) *dmg = F(double(n) * static_cast<LaserDef&>(*projectile2).value80);
            (void)sound(wd.secondarySound);           // 0x0007E72F / 0x0007E742
            shooterLink(o);
        }
        muzzleFlare();
        return false;
    case WeaponDef::kShotgun: {
        const NiPoint3 dx = ni::MulVector(rot, {1.0f, 0.0f, 0.0f});   // esp+0x2C (UNIT_X)
        stateTimer = charged == 1 ? 1.0f : 0.5f;      // +0x80 (overwrites the 0.1)
        const NiPoint3 dz = ni::MulVector(rot, {0.0f, 0.0f, 1.0f});   // esp+0x20 (UNIT_Z)
        s32 n = 0;                                    // esp+0x18
        if (!charged) {
            if (h->GetAmmo(ammoSlot) < 5) return false;   // (no click, no flare)
            h->UseAmmo(ammoSlot, 5);
            (void)sound(wd.fireSound);                // 0x0007E855 / 0x0007E958
        } else {
            if (h->GetAmmo(ammoSlot) < 5) {           // the click (0x0007EC62; no flare)
                (void)sound(wd.clickSound);
                return false;
            }
            const s32 k = static_cast<s8>(static_cast<s8>(FtoL(double(charge) * 5.0f)) % 6);   // MOVSX AL; CDQ;
                                                      //  IDIV 6; MOVSX DL
            n = FtoL((double(k) + 1.0f) * 5.0f);      // FILD; FADD 1.0; FMUL 5.0; _ftol
            if (n > h->GetAmmo(ammoSlot)) n = h->GetAmmo(ammoSlot);
            h->UseAmmo(ammoSlot, n);
            (void)sound(wd.secondarySound);           // 0x0007E946 / 0x0007E958
        }
        const NiPoint3 off{F(double(dz.x) * 0.15f), F(double(dz.y) * 0.15f), F(double(dz.z) * 0.15f)};   // [0x00293AA8]
        for (int i = 0; i < 9; ++i) {                 // esp+0x1C = 9 (DEC; JNE)
            const NiPoint3 p{F(double(off.x) + pos.x), F(double(off.y) + pos.y), F(double(off.z) + pos.z)};
            const f32 a = F((RandUnit() * 2.0 - 1.0f) * 0.09f);   // FADD ST0,ST0; FSUB [0x00293A98]; FMUL [0x002976E8]
            const ni::NiMatrix3 Ry = ni::MakeYRotation(a);        // 0x001030B0
            const f32 b = F((RandUnit() * 2.0 - 1.0f) * 0.09f);
            const ni::NiMatrix3 Rz = ni::MakeZRotation(b);        // 0x001030F0
            const ni::NiMatrix3 R = ni::Multiply(ni::Multiply(Ry, rot), Rz);   // 0x00013B30 twice
            auto& L0 = static_cast<LaserDef&>(*projectile);
            L0.value1C = F((RandUnit() * 2.0 - 1.0f) * 20.0f + 80.0f);   // the speed of +0xA0 (both kinds):
                                                      //  [0x00295AB4] / [0x002976D4]
            GameObject* o;
            if (!charged) {
                SetColour(L0, 0, 0, 1);
                o = L0.Spawn(level, &level, p, R, "Shotgun-Laser");   // 0x002976D8
                if (f32* dmg = damageOf(o)) *dmg = 7.5f;   // +0x148 = 0x40F00000 (no null check)
            } else {
                auto& L1 = static_cast<LaserDef&>(*projectile2);
                if (n >= 15) SetColour(L1, 1, 1, 1);
                else if (n > 10) SetColour(L1, 1, 0, 0);
                else if (n > 5) SetColour(L1, 1, 1, 0);
                else SetColour(L1, 0, 0, 1);
                o = L1.Spawn(level, &level, p, R, "Shotgun-Laser");
                if (f32* dmg = damageOf(o)) *dmg = F(double(n) * 30.0f * 0.1f);   // FILD; FMUL [0x0029763C];
            }                                         //  FMUL [0x00293AB0]
            shooterLink(o);                           // (o is not null-checked in the original)
        }
        const NiPoint3 q{F(double(off.x) + pos.x), F(double(off.y) + pos.y), F(double(off.z) + pos.z)};
        if (particles) (void)particles->SpawnEffect(level, q, dx);   // +0xA8 vf20 (no null check in the original)
        if (Def* sm = level.defs ? level.defs->Find("effect_smoke") : nullptr)   // 0x002976C4 (no null check)
            (void)sm->SpawnEffect(level, q, dx);
        uB8 ^= 1;                                     // +0xB8
        return false;
    }
    default:
        return false;
    }
}

GameObject* WeaponDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                             const char* label) {     // orig: 0x0007EE80 (new(0xBC))
    auto* o = new Weapon(level, *this);
    o->SetOwner(owner);                               // vf10
    o->SetName(label);                                // 0x000968C0
    o->SetPosition(pos);                              // vf1C
    o->SetRotation(rot);                              // vf24
    if (model2) o->Init();                            // WD+0x34
    return o;
}

}  // namespace fut::game
