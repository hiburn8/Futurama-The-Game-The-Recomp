// Pickups: the objects the PickupDef family spawns (BenderRuby, Beer_Bottle, ammo, charges). Decompiled from default.xbe
// (Pickup vtbl 0x0029CDD0, ctor 0x00076AA0, 0x74 bytes; Powerup 0x0029CB30 / Charge 0x0029CC98 0x78 bytes, Ammo
// 0x0029CBD8 0x7C bytes); docs/analysis/pickup_port.md.
//
// Ported: construction, Init, Tick, PostUpdate (the camera-distance visibility, the fall), OnAttach / OnDetach,
// CanCollect, the registry key numbers, the definitions' Spawn, the drops (DropList::SpawnEntry sets the thrown state,
// Destructable.cpp) and the weapons' firing (inventory_batch1_port.md 5). NOT PORTED: the pickup flare (it needs the
// renderer's per-node "frames not drawn" counter, NiAVObject +0xB2), CreateFromString.
#pragma once

#include "game/object/GameObject.h"

namespace fut::game {

class PickupDef;
class Player;
class WeaponDef;
class FlareDef;
class SpriteEmitterDef;

class Pickup : public GameObject {
public:
    enum State : u32 { kIdle = 1, kFalling = 2, kHeld = 6 };

    Pickup(LevelData& level, PickupDef& def);         // orig: 0x00076AA0
    ~Pickup() override;                               // orig: 0x00076F50
    bool Init() override;                             // orig: 0x000748F0
    bool Tick() override;                             // orig: 0x00076820
    bool PostUpdate() override;                       // orig: 0x00076FD0 (from Stage::Render)
    void OnAttach() override;                         // orig: 0x00076720
    void OnDetach() override;                         // orig: 0x000748D0

    // orig: 0x00076B80 (slot 20) / 0x000749D0 (Powerup) / 0x000757B0 (Charge) / 0x00075040 (Ammo): the same body
    // instruction for instruction (inventory_batch1_port.md 2: no override needed).
    virtual void RemoveModel();
    void SetPickupable(u8 b) { pickupable = b; }      // orig: slot 21 0x00074890 (the argument's low byte)
    bool CanCollect() const;                          // orig: slot 22 0x000767D0
    bool IgnoresRange() const { return ignoreRange != 0; }   // orig: slot 23 0x000748A0
    void SetKey(s32 a, s32 b) { keyA = a, keyB = b; } // orig: 0x00076590
    // orig: 0x00076600 (thiscall(char* out), RET 4; FsQuickSave): the registry key, "%s" of +0x70 or "$%03d:%03d" of
    // +0x68 / +0x6C when neither is -1, into `out`; false (out unchanged) without one.
    bool GetKey(std::string& out) const;
    // orig: 0x00076650 (thiscall(char*), RET 4; FsQuickLoad): "$aaa:bbb" (exactly 7 characters after the '$': the
    // fourth one becomes a NUL, then atoi of both halves) sets +0x68 / +0x6C; any other string is copied to +0x70.
    void ApplyKey(const char* s);
    // orig: 0x000766D0 (thiscall(u8 v), RET 4): the nibbler map's entry of the key = v (0x000777F0 with +0x70, else
    // 0x00077810 with +0x68 / +0x6C, -1 included).
    void StoreKeyState(u8 v);

    u32 state = kIdle;                                // orig: +0x38
    ni::NiPointer<ni::NiAVObject> model;              // orig: +0x3C, the clone of the definition's model
    u8 pickupable = 1;                                // orig: +0x40
    u8 ignoreRange = 0;                               // orig: +0x41
    f32 flareTimer = 0;                               // orig: +0x44 (seconds since the last flare)
    f32 playerDist2 = 3.4028234663852886e38f;         // orig: +0x48 (written by Stage::Update)
    ni::NiPoint3 velocity;                            // orig: +0x4C (falling)
    f32 fallDt = 0;                                   // orig: +0x58
    f32 airTime = 0;                                  // orig: +0x5C
    s32 bounces = 0;                                  // orig: +0x60
    u8 rising = 0;                                    // orig: +0x64
    s32 keyA = -1, keyB = -1;                         // orig: +0x68 / +0x6C (the collected-pickup registry key)
    std::string keyName;                              // orig: +0x70 (strdup'd; empty = none)

private:
    void Fall();                                      // orig: 0x00076C10
};

class Powerup : public Pickup {                       // orig: 0x00074810 (0x78 bytes)
public:
    Powerup(LevelData& level, PickupDef& def, f32 amount) : Pickup(level, def), amount(amount) {}
    ~Powerup() override {}                            // orig: 0x00074840 (the vtable, jmp 0x00076F50)
    bool Tick() override;                             // orig: 0x00075650 (also Charge's)
    void OnAttach() override;                         // orig: 0x00074850
    f32 amount;                                       // orig: +0x74
};

class Charge : public Pickup {                        // orig: 0x00075610 (0x78 bytes)
public:
    Charge(LevelData& level, PickupDef& def, f32 amount) : Pickup(level, def), amount(amount) {}
    ~Charge() override {}                             // orig: 0x00075640 (the vtable, jmp 0x00076F50)
    bool Tick() override;                             // orig: 0x00075650
    void OnAttach() override;                         // orig: 0x00075690
    f32 amount;                                       // orig: +0x74
};

class Ammo : public Pickup {                          // orig: 0x00074F00 (0x7C bytes)
public:
    Ammo(LevelData& level, PickupDef& def, s32 count, u32 type) : Pickup(level, def), count(count), ammoType(type) {}
    ~Ammo() override {}                               // orig: 0x00074F30 (the vtable, jmp 0x00076F50)
    bool Tick() override;                             // orig: 0x00075000
    void OnAttach() override;                         // orig: 0x00074F40
    s32 count;                                        // orig: +0x74
    u32 ammoType;                                     // orig: +0x78
};

// orig: Weapon (ctor 0x0007D1F0, vtbl 0x0029D2A0, 0xBC bytes): a pickup with its own ground / held model clones and
// per-instance projectile definitions (docs/analysis/owl_weapon_port.md 8; the railgun's RailDef pair:
// natives_batch_b_port.md 4). Firing (slots 26-28, 30, 32, 33) since inventory_batch1_port.md 5.1 / 5.2. NOT PORTED:
// the take out / put away / fire / click sounds (game objects keep load handles only), the railgun's RailLaser object
// (RailDef::Spawn).
class Weapon : public Pickup {
public:
    Weapon(LevelData& level, WeaponDef& def);        // orig: 0x0007D1F0
    ~Weapon() override;                               // orig: 0x0007DAF0
    bool Init() override;                             // orig: slot 1 0x0007DDB0
    bool Tick() override;                             // orig: slot 2 0x0007CFB0
    void OnAttach() override;                         // orig: slot 15 0x0007D050
    void OnDetach() override;                         // orig: slot 16 0x0007DC50
    bool PutAway();                                   // orig: slot 24 0x0007CEB0 (L name)
    bool TakeOut();                                   // orig: slot 25 0x0007DD00 (L name; Stage::Update after collection)
    // orig: slot 26 vf68 0x0007D140 (thiscall(kind, unused), RET 8): only when ready (state 4): fire from the held
    // model's "tip" (else 5 above the holder) along the holder's rotation.
    bool FireRequest(u32 kind);
    // orig: slot 27 vf6C 0x0007ECC0 (thiscall(pos*, dir by value, a, b), RET 0x18): fire along a direction (no caller
    // found, inventory_batch1_port.md open question 3).
    bool FireDir(const ni::NiPoint3& pos, ni::NiPoint3 dir, u32 a, f32 b);
    // orig: slot 28 vf70 0x0007DF80 (thiscall(pos*, rot*, charged, charge), RET 0x10; always 0): the shot by type.
    bool Fire(const ni::NiPoint3& pos, const ni::NiMatrix3& rot, u32 charged, f32 charge);
    // orig: slot 30 vf78 0x0007CF40: WD+0x38 + 4 i for i = 0..6 (idle, attack 1, attack 2, moving attack 1, moving
    // attack 2, take out, put away), else NULL (an unknown weapon's empty names are NULL as well).
    const char* AnimName(u32 i) const;
    u32 WeaponType() const { return weaponType; }     // orig: slot 31 0x0007DAE0
    // orig: slot 32 vf80 0x0007D0D0 (thiscall(out*), RET 4): the tip's world position after tip->Update(+0x24, 1),
    // else ZERO.
    ni::NiPoint3 TipPosition();
    // orig: slot 33 vf84 0x0007DF20 (thiscall(out*), RET 4): the tip's world rotation (no update). Without a tip the
    // original copies nine leftover stack words (only the pads are written): the port returns a zero matrix (L).
    ni::NiMatrix3 TipRotation() const;
    // The holder: owner - 0x38, the Character::Inventory's character (no type check in the original; only Fry holds a
    // weapon, Player::OnPickup refuses the others).
    Player* Holder() const;

    s32 ammoSlot = -1;                                // orig: +0x74
    ni::NiPointer<ni::NiNode> groundModel;            // orig: +0x78
    ni::NiPointer<ni::NiNode> heldModel;              // orig: +0x7C
    f32 stateTimer = 0;                               // orig: +0x80
    u32 nextState = 1;                                // orig: +0x84
    u32 weaponType = 0;                               // orig: +0x98
    u32 voice = 0;                                    // orig: +0x9C
    Def* projectile = nullptr;                        // orig: +0xA0 (LaserDef; railgun: RailDef 0x0007BE80)
    Def* projectile2 = nullptr;                       // orig: +0xA4
    SpriteEmitterDef* particles = nullptr;            // orig: +0xA8 (shotgun only; uninitialised otherwise)
    SpriteEmitterDef* particles2 = nullptr;           // orig: +0xAC
    FlareDef* flare = nullptr;                        // orig: +0xB0
    f32 ammo = 0;                                     // orig: +0xB4
    u32 uB8 = 0;                                      // orig: +0xB8
};

class LaserDef;
class RailDef;
// orig: new(0xA8) + LaserDef::LaserDef 0x00079BF0 (RailDef: 0x0007BE80): a definition created at run time joins the
// level's list (Def::Def 0x000962E0), its "Something" spark emitter after it. Instantiated for LaserDef and RailDef
// (Pickup.cpp; also the level1-6 chicken walker).
template <class T = LaserDef>
T* NewLaser(LevelData& level, const char* name);

}  // namespace fut::game
