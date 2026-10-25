// Object definitions ("defs"), decompiled from default.xbe.
//
// Every level has a Levels\<level>\defs.nif whose top-level NiNodes are named "<Class>::<Name>" and carry their
// parameters as a comma-separated NiStringExtraData (e.g. "StaticDef::INST_Bus" with
// "Off,0.2,10,60,10,0.2,5,0.5,10,Box,0,cx,cy,cz,ex,ey,ez"). The level loader (Session stream callback 0x000C5BD0,
// kind 9) hands each node to the creator of its class prefix; the creator parses the string, builds the definition,
// takes the node's first child as the definition's model (detaching it) and prepares that model's materials.
//
// Verified against the original creators under emulation on every defs.nif of the disc (tests/game/DefsTests.cpp):
// every parsed field, sound request, model reference and the resulting scene state.
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fut::ni {
class NiStream;
}

namespace fut::game {

class LevelData;
class ExplosionDef;
class GameObject;
class Owner;
class SMarkFactoryDef;
class CharacterDef;
class RippleDef;

// Services the creators use (orig: the sound system at App +0xF0, vtable +0x10 "load sound" -> handle).
struct DefContext {
    std::function<u32(const std::string& path, u32 flags)> loadSound;
    std::function<void(u32 sample)> releaseSound;      // orig: Sound_Release 0x000C9880 (the definitions' dtors)
    // orig: 0x000CAFF0, the sound event registered under "<character>:<event>" (from Sounds\SoundLookup.nif).
    std::function<s32(const std::string& name)> lookupSoundEvent;
    std::function<s32()> rand;                         // orig: the CRT rand() 0x0016695C (NPC definitions)
    std::string language = "English";                  // orig: TextManager +0x18 (localised sound paths)
    // The level being loaded (its effects data: the NPC definitions look meshes / textures up there) and its name
    // (orig: g_level +0x0C).
    const LevelData* level = nullptr;
    std::string levelName;
};

class Def {                                            // orig: base ctor 0x000962E0 (vtable 0x002A11B4), 0x18 bytes
public:
    // the objects still of this definition deleted (`while (+0x0C) +0x0C->vf00(1)`: each unlinks itself); the
    // definition list (+4 / +8, head S+0x34) and the name (+0x10) are the registry's and the member's
    virtual ~Def();                                   // orig: 0x00096370
    virtual std::string_view ClassName() const = 0;
    // orig: vtable slot 3: `(t & mask) == t` for the classes that have a type mask (IsType(0) is then true), false
    // for the rest (docs/analysis/game_objects.md 2.2). Moveable / Debris keep this one (their masks 0x80 / 0x200 sit
    // in a slot nothing reached here calls).
    virtual bool IsType(u32 t) const { (void)t; return false; }   // orig: 0x00160680
    // orig: slot 6: the first part of an object's node name "%s::%s(%s)".
    virtual const char* TypeName() const { return "Definition"; }   // orig: 0x0004EBD0
    // orig: slot 8 of the effect definitions (thiscall(pos, dir)): the effect object spawned at `pos` emitting along
    // `dir`, or nullptr (0x0014B130 and every class without one).
    virtual GameObject* SpawnEffect(LevelData& level, const ni::NiPoint3& pos, const ni::NiPoint3& dir) {
        (void)level, (void)pos, (void)dir;
        return nullptr;
    }
    // orig: slot 1: ready to spawn (the ObjectDef family requests its model stream; others are always ready).
    virtual bool Ready() { return true; }             // orig: 0x000A32A0
    // orig: slot 5 (thiscall(owner, pos, rot, label)): the object spawned for `owner`, or nullptr (the base and the
    // classes not ported yet). `level` is the port's stand-in for the global level object.
    virtual GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                              const char* label) {
        (void)level, (void)owner, (void)pos, (void)rot, (void)label;
        return nullptr;                               // orig: 0x00155890
    }

    GameObject* firstInstance = nullptr;               // orig: +0x0C, the objects of this definition (GameObject.h)
    std::string name;                                  // orig: +0x10, the node name after "<Class>::"
    bool vertexColorsScaled = false;                   // orig: +0x14, set when a model's vertex colours were scaled
};

class StaticDef : public Def {                         // orig: vtable 0x002A1054, 0x80 bytes, ctor 0x00095C30
public:
    std::string_view ClassName() const override { return "StaticDef"; }
    // orig: 0x00095780 (Static.cpp): only into a stage owner.
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
    bool IsType(u32 t) const override { return (t & 0x1000) == t; }   // orig: 0x00095A40
    const char* TypeName() const override { return "StaticDef"; }

    enum Shape : u32 { kBox = 0, kPolygon = 1 };
    u32 shape = kBox;                                  // orig: +0x18
    // Box shape only; the original leaves both triples as stack garbage for "Polygon" (unused then).
    std::array<f32, 3> boxExtents{};                   // orig: +0x1C
    std::array<f32, 3> boxCenter{};                    // orig: +0x28
    bool animated = false;                             // orig: +0x34, the model has controllers / a NiFltAnimationNode
    u8 shapeFlag = 0;                                  // orig: +0x35, the integer after "Box," / "Polygon,"
    ni::NiPointer<ni::NiNode> model;                   // orig: +0x38
    RippleDef* ripple = nullptr;                       // orig: +0x3C (native 303)
    u32 idleSound = 0;                                 // orig: +0x40 (box idle loop, always requested)
    std::vector<u32> jumpSounds;                       // orig: NiTArray at +0x44 (5 box jump sounds)
    // "On," / "Off," prefix with 8 physics values. Without that prefix the original copies 8 uninitialised stack
    // floats over these defaults; the port keeps the defaults (see KNOWN_ISSUES).
    bool physicsOn = false;                            // orig: +0x5C
    std::array<f32, 8> physics{0.2f, 10.0f, 60.0f, 10.0f, 0.2f, 5.0f, 0.5f, 10.0f};   // orig: +0x60..+0x7C
    bool physicsFromString = false;                    // port-only: whether `physics` came from the string
};

// Definitions of objects with a model and an inventory icon (orig: base ctor 0x00097690, vtable 0x002A12D0 with a
// stream-callback interface at +0x18).
class ObjectDef : public Def {
public:
    u8 unknown1C = 1;                                  // orig: +0x1C
    ni::NiPointer<ni::NiNode> model;                   // orig: +0x20, the "Mesh" node
    // orig: +0x24. vtable slot 1 (0x000977E0, called by the loader after creation) queues a stream request for the
    // model when there is none; the port records that it would.
    bool streamRequested = false;
    void Register();                                   // orig: 0x000977E0
};

class PickupDef : public ObjectDef {                   // orig: vtable 0x0029CE40, 0x30 bytes, ctor 0x000771F0
public:
    std::string_view ClassName() const override { return "PickupDef"; }
    bool IsType(u32 t) const override { return (t & 0x4) == t; }   // orig: 0x00077220
    const char* TypeName() const override { return "PickupDef"; }
    ni::NiPointer<ni::NiObject> icon;                  // orig: +0x28, base texture of the "Icon" node
    s32 value = 0;                                     // orig: +0x2C
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;   // orig: 0x00077150 (Pickup.cpp)
};

class PowerupDef : public PickupDef {                  // orig: vtable 0x0029CB98, 0x34 bytes, ctor 0x00074A60
public:
    std::string_view ClassName() const override { return "PowerupDef"; }
    bool IsType(u32 t) const override { return (t & 0x404) == t; }   // orig: 0x00074AB0
    const char* TypeName() const override { return "PowerupDef"; }
    f32 amount = 100.0f;                               // orig: +0x30
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;   // orig: 0x00074B00 (Pickup.cpp)
};

class ChargeDef : public PickupDef {                   // orig: vtable 0x0029CD2C, 0x34 bytes, ctor 0x00075840
public:
    std::string_view ClassName() const override { return "ChargeDef"; }
    bool IsType(u32 t) const override { return (t & 0x100004) == t; }   // orig: 0x00075890
    const char* TypeName() const override { return "ChargeDef"; }
    f32 amount = 100.0f;                               // orig: +0x30
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;   // orig: 0x000758E0 (Pickup.cpp)
};

class AmmoDef : public PickupDef {                     // orig: vtable 0x0029CC40, 0x38 bytes, ctor 0x000750D0
public:
    std::string_view ClassName() const override { return "AmmoDef"; }
    bool IsType(u32 t) const override { return (t & 0x804) == t; }   // orig: 0x00075120
    const char* TypeName() const override { return "AmmoDef"; }
    enum AmmoType : u32 { kPistol, kShotgun, kRailgun, kMachinegun, kRocket };
    s32 count = 25;                                    // orig: +0x30
    u32 ammoType = kPistol;                            // orig: +0x34
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;   // orig: 0x00075170 (Pickup.cpp)
};

class WeaponDef : public PickupDef {                   // orig: vtable 0x0029D364, 0x80 bytes, ctor 0x0007EF20
public:
    std::string_view ClassName() const override { return "WeaponDef"; }
    bool IsType(u32 t) const override { return (t & 0xC) == t; }   // orig: 0x0007EF80
    const char* TypeName() const override { return "WeaponDef"; }
    enum WeaponType : u32 { kHammer, kPistol, kShotgun, kRailgun, kMachineGun };
    u32 weaponType = 0;                                // orig: +0x30 (not set by the ctor; every shipped weapon is known)
    ni::NiPointer<ni::NiNode> model2;                  // orig: +0x34, "Mesh" again
    // orig: +0x38..+0x50: idle, attack 1, attack 2, moving attack 1, moving attack 2, take out, put away.
    std::array<std::string, 7> animations;
    u32 fireSound = 0, secondarySound = 0, clickSound = 0, reloadSound = 0;     // orig: +0x54 +0x58 +0x5C +0x60
    u32 chargingSound = 0, chargedLoopSound = 0, loopSound = 0;                 // orig: +0x64 +0x68 +0x6C
    std::array<u32, 3> unknown70{};                    // orig: +0x70..+0x78
    f32 ammo = 0.0f;                                   // orig: +0x7C
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;   // orig: 0x0007EE80; NOT PORTED (Pickup.cpp)
};

class MoveableDef : public Def {                       // orig: vtable 0x0029CAF4, ctor 0x00074300, 0x30 bytes
public:
    std::string_view ClassName() const override { return "MoveableDef"; }
    const char* TypeName() const override { return "MoveableDef"; }
    enum Shape : u32 { kNone = 0, kBox = 1, kSphere = 2 };
    u32 shape = kNone;                                 // orig: +0x18
    // Box: x, y, z; sphere: radius only (the original copies two uninitialised floats into y and z).
    std::array<f32, 3> size{};                         // orig: +0x1C
    f32 mass = 0.0f;                                   // orig: +0x28
    ni::NiPointer<ni::NiNode> model;                   // orig: +0x2C
    // orig: 0x00074230 (Moveable.cpp): a Moveable for a stage owner.
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
};

class DebrisDef : public MoveableDef {                 // orig: vtable 0x0029D808 (ctor 0x0007FC30)
public:
    std::string_view ClassName() const override { return "DebrisDef"; }
    const char* TypeName() const override { return "DebrisDef"; }
    // orig: 0x0007FCB0 (Moveable.cpp): a Debris for a stage owner.
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
};

class DestructableDef : public Def {                   // orig: vtable 0x0029BB44, 0x98 bytes, ctor 0x000652E0
public:
    std::string_view ClassName() const override { return "DestructableDef"; }
    bool IsType(u32 t) const override { return (t & 0x40) == t; }   // orig: 0x000653C0
    const char* TypeName() const override { return "Destructable"; }
    // orig: +0x18, set only when the string names a sound (otherwise uninitialised in the original).
    u32 breakSound = 0;
    bool hasBreakSound = false;                        // port-only
    ni::NiPointer<ni::NiSwitchNode> model;             // orig: +0x1C, first NiSwitchNode under the node's first child
    std::array<f32, 3> extents{};                      // orig: +0x24 (values 4-6 of the string)
    std::array<f32, 3> center{};                       // orig: +0x30 (values 1-3)
    f32 health = 0.0f;                                 // orig: +0x3C (value 7)
    std::string effect;                                // orig: +0x40 (field 8; NULL when empty)
    std::string breakSoundPath;                        // orig: +0x44 (field 11)
    std::vector<std::string> effects1;                 // orig: +0x48 / count +0x4C (field 9, ';' separated)
    std::vector<std::string> effects2;                 // orig: +0x50 / count +0x54 (field 10)
    bool hasValue12 = false;                           // orig: +0x58
    s32 value12 = 0;                                   // orig: +0x5C (field 12)
    s32 value13 = 0;                                   // orig: +0x60 (field 13, then 9 floats)
    std::array<f32, 9> params{45.0f, 1.0f, 0.0f, 20.0f, 1.3f, 0.0f, 1.0f, 0.0f, 0.0f};   // orig: +0x64..+0x84
    bool explosive = false;                            // orig: +0x88 (by name: Sewerbarrel, Suncity_steelbarrel_explosive,
    f32 explosionA = 0.0f, explosionB = 0.0f;          //  Explosive_Barrel -> 20, 80)  orig: +0x8C / +0x90
    u32 unknown94 = 0;                                 // orig: +0x94 (the mine activation sound)
    // orig: 0x00065200 (Destructable.cpp)
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
};

// orig: StaticDef creator 0x00095E50. nullptr if the node has no usable parameter string.
std::unique_ptr<StaticDef> CreateStaticDef(ni::NiNode& node, DefContext& ctx);
std::unique_ptr<PickupDef> CreatePickupDef(ni::NiNode& node);      // orig: 0x000772C0
std::unique_ptr<PowerupDef> CreatePowerupDef(ni::NiNode& node);    // orig: 0x00074BA0
std::unique_ptr<ChargeDef> CreateChargeDef(ni::NiNode& node);      // orig: 0x00075980
std::unique_ptr<AmmoDef> CreateAmmoDef(ni::NiNode& node);          // orig: 0x00075210
std::unique_ptr<WeaponDef> CreateWeaponDef(ni::NiNode& node, DefContext& ctx);   // orig: 0x0007F090
std::unique_ptr<DebrisDef> CreateDebrisDef(ni::NiNode& node);      // orig: 0x0007FD80
std::unique_ptr<DestructableDef> CreateDestructableDef(ni::NiNode& node, DefContext& ctx);   // orig: 0x000656C0

// orig: NiAVObject vtbl+0x54 (0x00101970; NiNode 0x00105830): this object if its name equals `name` (strcmp),
// else the first match among the children, depth first. Unnamed objects never match.
ni::NiAVObject* GetObjectByName(ni::NiAVObject* object, const char* name);

// Line/field tokenizer used by the definition parsers (orig: 0x00097BA0-0x00097CD0, 0x84 bytes).
class DefTokenizer {
public:
    explicit DefTokenizer(const char* text);           // orig: 0x00097BA0 (works on a copy)
    bool NextLine();                                   // orig: 0x00097C00: first token up to '=' or ',', then ','
    const char* NextToken();                           // orig: 0x00097CB0: "<end of line>" when exhausted
    f32 NextFloat();                                   // orig: 0x00097CD0: atof, 0 when exhausted (not consumed)
private:
    std::string buffer_;
    bool hasBuffer_ = false;
    size_t cursor_ = 0;
    std::array<const char*, 29> tokens_{};
    u32 index_ = 0;                                    // orig: +0x80
};

// orig: 0x00096420 (thiscall on the definition): prepares a definition model's lighting.
//   nodes: recurse (except a node named "QDOutline"), vertex-colour property -> lighting mode 1;
//   geometry: material ambient = diffuse, emissive = black (two revisions); vertex-colour property -> lighting
//   mode 1 and, when the data has colours, RGB x 0.2, data flag 4, source mode 1 (else source mode 0).
void PrepareModelLighting(Def& def, ni::NiAVObject* object);
// orig: 0x00095850: true if the object or a descendant has controllers or is a NiFltAnimationNode.
bool HasAnimation(const ni::NiAVObject* object);
// orig: 0x00101DA0 (NiAVObject::GetProperty by NiProperty type number).
ni::NiProperty* GetProperty(const ni::NiAVObject& object, u32 type);
// orig: 0x00102DE0: detaches the first property of that type (types 0-11 only).
void RemoveProperty(ni::NiAVObject& object, u32 type);
// orig: 0x00013A50 (property list AddHead): the new property goes first.
void AttachProperty(ni::NiAVObject& object, ni::NiProperty* property);
// orig: 0x00013E40: the shared fog property (0x002D96D8: flags 0 after its bit 1 is cleared once, depth 1, black) at
// the head of the object's property list (0x00013C40), then UpdateProperties.
void AttachNoFog(ni::NiAVObject& object);

// The definitions of one level, in creation order (orig: list at level object +0x34; every definition constructor
// appends itself through Def::Def 0x000962E0).
class DefRegistry {
public:
    // orig: the kind-9 branch of the Session stream callback 0x000C5BD0: every top-level object of defs.nif whose
    // name starts with a known class prefix. Returns the definition created (nullptr if none / not ported yet); a
    // character also appends the weapon definitions it creates.
    Def* CreateFromNode(ni::NiObject* object, DefContext& ctx);
    // orig: the kind-3 branch of 0x000C5BD0 (Effects\<level>.nif): the level's ExplosionDef when it has none yet,
    // the effects data and every effect EffectList.txt names (0x000BF980), then the set-up 0x000C59E0: the shared
    // "newshade.tga" environment-map texture effect (once, under the dynamic node), the scorch-mark factory and its
    // instance (once) and the "RespawnEffect" definition. `afterExplosion` runs between the explosion definition and
    // the effects data (the handler's memory-card sound reload 0x000E4C90 and pause-panel reset 0x000A81F0).
    void LoadEffects(LevelData& level, ni::NiStream& effects, DefContext& ctx,
                     const std::function<void()>& afterExplosion = {});
    // orig: 0x000C3F10: the first definition with that name (_stricmp), or nullptr.
    Def* Find(const char* name) const;
    // orig: Session::FindCharacterDef 0x000C3FB0: the first character definition with that name (list S+0x3C,
    // _stricmp), or nullptr.
    CharacterDef* FindCharacterDef(const char* name) const;
    // A definition created outside defs.nif (orig: every definition constructor appends itself, 0x000962E0).
    Def* Add(std::unique_ptr<Def> def);
    const std::vector<std::unique_ptr<Def>>& Defs() const { return defs_; }
    // The level object's destructor 0x000C83F0 (docs/analysis/effect_release_order.md 3): `def` deleted (its deleting
    // dtor vf00(1) unlinks it from the lists; a null or unknown `def` is ignored). The level's own pointers to it are
    // cleared (S+0x40 / +0x44 / +0x48).
    void Delete(Def* def);
    // orig: 0x000C8630 `while (S+0x3C) S+0x3C->vf00(1)` (the character definitions, head first = creation order),
    // 0x000C8647 the list S+0x38 (L: no port definition is known to be on it), then 0x000C8660 `while (S+0x34)
    // S+0x34->vf00(1)` (every other definition, head first = creation order, 0x000962E0 appends).
    void DeleteAll();

    ExplosionDef* explosion = nullptr;                 // orig: level +0x44
    SMarkFactoryDef* scorchFactory = nullptr;          // orig: level +0x40
    Def* respawnEffect = nullptr;                      // orig: level +0x48

private:
    std::vector<std::unique_ptr<Def>> defs_;
};

}  // namespace fut::game
