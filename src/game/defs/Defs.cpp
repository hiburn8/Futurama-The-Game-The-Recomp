// Decompiled from default.xbe; see Defs.h. Parameter parsing follows the original's CRT calls exactly:
// strncmp for the keywords, strchr(p, ',') + 1 to step to the next value, atof / atoi for the values.
#include "game/defs/Defs.h"

#include "core/CrtString.h"
#include "engine/ni/NiCull.h"
#include "game/object/MarkInstance.h"

#include "game/defs/CharacterDef.h"
#include "game/defs/EffectDefs.h"
#include "game/defs/ExplosionDef.h"
#include "game/level/LevelData.h"
#include "game/text/LanguageDb.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace fut::game {
namespace {

using ni::detail::F;

// orig: "q = strchr(p, ','); p = q + (q != NULL)". Port-only: a NULL cursor stays NULL.
const char* AfterComma(const char* p) {
    if (!p) return nullptr;
    const char* q = std::strchr(p, ',');
    return q ? q + 1 : nullptr;
}
// orig: atof (0x00167609) stored with fstp dword. Port-only: NULL reads as 0 (the original would fault).
f32 Atof(const char* p) { return p ? F(std::strtod(p, nullptr)) : 0.0f; }
// orig: atoi (0x001674D2), truncated to a byte by the callers.
u8 AtoiByte(const char* p) { return p ? static_cast<u8>(std::atoi(p)) : 0; }
bool StartsWith(const char* p, const char* keyword, size_t n) { return p && crt::StrNICmp(p, keyword, n) == 0; }   // _strnicmp

// orig: the definition's parameter string: the node's first extra data as NiStringExtraData (FUN_0013EB60 + RTTI
// cast + string at +0x14).
const char* ParameterString(const ni::NiObjectNET& node) {
    const auto* sx = dynamic_cast<const ni::NiStringExtraData*>(node.extraData.Get());
    if (!sx || sx->string.empty()) return nullptr;
    return sx->string.c_str();
}

// orig: the model of a definition: the node's first child slot if it is a NiNode (0x0001D550 + RTTI cast).
ni::NiNode* FirstChildNode(ni::NiNode& node) {
    const auto& kids = node.Children();
    if (kids.empty() || !kids[0]) return nullptr;
    return dynamic_cast<ni::NiNode*>(kids[0].Get());
}

u32 PropertyType(const ni::NiProperty& p) {           // orig: NiProperty vtbl+0x2C, checked against the original
    static constexpr std::string_view kTypes[] = {
        "NiAlphaProperty", "NiDitherProperty", "NiFogProperty", "NiMaterialProperty", "NiRendererSpecificProperty",
        "NiShadeProperty", "NiSpecularProperty", "NiStencilProperty", "NiTexturingProperty", "NiVertexColorProperty",
        "NiWireframeProperty", "NiZBufferProperty"};
    const std::string_view name = p.ClassName();
    for (u32 i = 0; i < std::size(kTypes); ++i)
        if (kTypes[i] == name) return i;
    return 0xFFFFFFFFu;
}

}  // namespace

ni::NiProperty* GetProperty(const ni::NiAVObject& object, u32 type) {   // orig: 0x00101DA0
    for (const auto& p : object.properties)
        if (p && PropertyType(*p) == type) return p.Get();
    return nullptr;
}

void RemoveProperty(ni::NiAVObject& object, u32 type) {   // orig: 0x00102DE0
    if (type >= 12) return;
    for (auto it = object.properties.begin(); it != object.properties.end(); ++it)
        if (*it && PropertyType(**it) == type) {
            object.properties.erase(it);
            return;
        }
}

void AttachProperty(ni::NiAVObject& object, ni::NiProperty* property) {   // orig: 0x00013A50
    if (property) object.properties.insert(object.properties.begin(), ni::NiPointer<ni::NiProperty>(property));
}

void AttachNoFog(ni::NiAVObject& o) {                 // orig: 0x00013E40
    static ni::NiPointer<ni::NiFogProperty> fog = [] {
        ni::NiPointer<ni::NiFogProperty> f(new ni::NiFogProperty);
        f->flags = 0;
        return f;
    }();
    o.properties.insert(o.properties.begin(), ni::NiPointer<ni::NiProperty>(fog.Get()));   // AddHead 0x00013C40
    o.UpdateProperties();                             // (tail jump 0x00101EA0)
}

bool HasAnimation(const ni::NiAVObject* object) {    // orig: 0x00095850
    if (!object) return false;
    if (object->controller || dynamic_cast<const ni::NiFltAnimationNode*>(object)) return true;
    if (const auto* node = dynamic_cast<const ni::NiNode*>(object))
        for (const auto& child : node->Children())
            if (HasAnimation(child.Get())) return true;
    return false;
}

void PrepareModelLighting(Def& def, ni::NiAVObject* object) {   // orig: 0x00096420
    if (!object) return;
    if (auto* node = dynamic_cast<ni::NiNode*>(object)) {
        if (node->name == "QDOutline") return;
        for (const auto& child : node->Children()) PrepareModelLighting(def, child.Get());
        if (auto* vc = static_cast<ni::NiVertexColorProperty*>(GetProperty(*node, 9))) vc->lightingMode = 1;
    }
    auto* geom = dynamic_cast<ni::NiGeometry*>(object);
    if (!geom) return;
    if (auto* mat = static_cast<ni::NiMaterialProperty*>(GetProperty(*geom, 3))) {
        mat->ambient = mat->diffuse;
        ++mat->revision;
        mat->emissive = {0.0f, 0.0f, 0.0f};          // orig: a global black NiColor (0x0030E940)
        ++mat->revision;
        // orig: then clamps (diffuse + constant) into a temporary that is never used.
    }
    auto* vc = static_cast<ni::NiVertexColorProperty*>(GetProperty(*geom, 9));
    if (!vc) return;
    vc->lightingMode = 1;
    ni::NiGeometryData* data = geom->modelData.Get();
    if (data && !data->colors.empty()) {
        for (u32 i = 0; i < data->vertexCount && i < data->colors.size(); ++i) {
            ni::NiColorA& c = data->colors[i];
            c.r = F(double(c.r) * 0.2f);
            c.g = F(double(c.g) * 0.2f);
            c.b = F(double(c.b) * 0.2f);
        }
        data->revisionFlags |= 4;
        def.vertexColorsScaled = true;
        vc->sourceVertexMode = 1;
    } else {
        vc->sourceVertexMode = 0;
    }
}

std::unique_ptr<StaticDef> CreateStaticDef(ni::NiNode& node, DefContext& ctx) {   // orig: 0x00095E50
    const char* p = ParameterString(node);
    if (!p) return nullptr;
    bool physicsOn = false, havePhysics = false;
    if (StartsWith(p, "On,", 3)) {
        p = AfterComma(p);
        physicsOn = havePhysics = true;
    } else if (StartsWith(p, "Off,", 4)) {
        p = AfterComma(p);
        havePhysics = true;
    }
    std::array<f32, 8> physics{};
    if (havePhysics)
        for (f32& v : physics) {
            v = Atof(p);
            p = AfterComma(p);
        }
    u32 shape;
    u8 flag = 0;
    std::array<f32, 6> box{};
    if (StartsWith(p, "Box,", 4)) {
        shape = StaticDef::kBox;
        p = AfterComma(p);
        if (p) {
            flag = AtoiByte(p);
            p = AfterComma(p);                        // orig: strchr(p, ',') + 1 without the NULL check
        }
        for (f32& v : box) {
            v = Atof(p);
            p = AfterComma(p);
        }
    } else if (StartsWith(p, "Polygon", 7)) {
        shape = StaticDef::kPolygon;
        p = AfterComma(p);
        if (p) flag = AtoiByte(p);
    } else {
        return nullptr;
    }

    auto def = std::make_unique<StaticDef>();         // orig: ctor 0x00095C30 (name after "StaticDef::")
    def->name = node.name.size() > 11 ? node.name.substr(11) : std::string();
    def->idleSound = ctx.loadSound("Sounds/Level1/AMBIENCE-PLANETEXP/Box-Idle_loop.wav", 0);
    for (const char* jump : {"Sounds/Level1/AMBIENCE-PLANETEXP/Box-Jump.wav", "Sounds/Level1/AMBIENCE-PLANETEXP/Box-Jump-2.wav",
                             "Sounds/Level1/AMBIENCE-PLANETEXP/Box-Jump-3.wav", "Sounds/Level1/AMBIENCE-PLANETEXP/Box-Jump-4.wav",
                             "Sounds/Level1/AMBIENCE-PLANETEXP/Box-Jump-5.wav"})
        def->jumpSounds.push_back(ctx.loadSound(jump, 0));
    if (shape == StaticDef::kBox) {
        def->boxCenter = {box[0], box[1], box[2]};
        def->boxExtents = {box[3], box[4], box[5]};
    }
    if (havePhysics) {
        def->physics = physics;
        def->physicsFromString = true;
    }
    def->shape = shape;
    def->physicsOn = physicsOn;
    def->shapeFlag = flag;

    def->model = FirstChildNode(node);
    if (def->model) node.DetachChild(def->model.Get());
    PrepareModelLighting(*def, def->model.Get());
    def->animated = HasAnimation(def->model.Get());
    return def;
}

// ---- generic helpers ------------------------------------------------------------------------------------------------
ni::NiAVObject* GetObjectByName(ni::NiAVObject* object, const char* name) {   // orig: 0x00101970 / 0x00105830
    if (!object || !name) return nullptr;
    if (!object->name.empty() && std::strcmp(object->name.c_str(), name) == 0) return object;
    if (auto* node = dynamic_cast<ni::NiNode*>(object))
        for (const auto& child : node->Children())
            if (child)
                if (ni::NiAVObject* found = GetObjectByName(child.Get(), name)) return found;
    return nullptr;
}

namespace {

ni::NiNode* FindNode(ni::NiNode& root, const char* name) {   // GetObjectByName + RTTI cast to NiNode
    return dynamic_cast<ni::NiNode*>(GetObjectByName(&root, name));
}

// orig (in every ObjectDef creator): the "Icon" node's base texture becomes the definition's icon (texturing
// property maps[0] -> texture) and the icon node is detached from the definition node.
void TakeIcon(ni::NiNode& node, PickupDef& def) {
    ni::NiNode* icon = FindNode(node, "Icon");
    if (!icon) return;
    if (auto* tp = static_cast<ni::NiTexturingProperty*>(GetProperty(*icon, 8)))
        if (tp->HasMap(0)) def.icon = tp->maps[0].textureObj;
    node.DetachChild(icon);
}

// The C string the original modifies in place ("*strchr(p, ',') = 0"): the port truncates the extra data string.
ni::NiStringExtraData* ParameterData(ni::NiObjectNET& node) {
    auto* sx = dynamic_cast<ni::NiStringExtraData*>(node.extraData.Get());
    return sx && !sx->string.empty() ? sx : nullptr;
}

std::string NameAfter(const ni::NiObjectNET& node, size_t prefixLength) {
    return node.name.size() > prefixLength ? node.name.substr(prefixLength) : std::string();
}

// orig: _stricmp
bool EqualsNoCase(const char* a, const char* b) {
    for (;; ++a, ++b) {
        int ca = static_cast<u8>(*a), cb = static_cast<u8>(*b);
        if (ca >= 'A' && ca <= 'Z') ca += 0x20;
        if (cb >= 'A' && cb <= 'Z') cb += 0x20;
        if (ca != cb) return false;
        if (!ca) return true;
    }
}

}  // namespace

// ---- tokenizer --------------------------------------------------------------------------------------------------------
DefTokenizer::DefTokenizer(const char* text) : hasBuffer_(text != nullptr) {   // orig: 0x00097BA0
    if (text) buffer_ = text;
    buffer_.push_back('\0');                          // the working copy is a C string
}

bool DefTokenizer::NextLine() {                       // orig: 0x00097C00
    index_ = 0;
    if (!hasBuffer_) return false;
    char* line = buffer_.data() + cursor_;
    size_t end = cursor_;
    while (buffer_[end] != '\0' && buffer_[end] != '\n') ++end;
    cursor_ = end;
    if (buffer_[end] == '\n') {
        buffer_[end] = '\0';
        ++cursor_;
    }
    if (*line == '\0') return false;
    // CRT strtok: first with "=,", then ","; at most 29 tokens; a NULL terminates the list.
    char* state = line;
    auto strtok = [&](const char* delims) -> const char* {
        while (*state && std::strchr(delims, *state)) ++state;
        if (!*state) return nullptr;
        char* token = state;
        while (*state && !std::strchr(delims, *state)) ++state;
        if (*state) *state++ = '\0';
        return token;
    };
    tokens_[0] = strtok("=,");
    for (u32 i = 1; i < 29; ++i)
        if (!(tokens_[i] = strtok(","))) break;
    return true;
}

const char* DefTokenizer::NextToken() {               // orig: 0x00097CB0
    if (index_ < tokens_.size() && tokens_[index_]) return tokens_[index_++];
    return "<end of line>";
}

f32 DefTokenizer::NextFloat() {                       // orig: 0x00097CD0
    if (index_ >= tokens_.size() || !tokens_[index_]) return 0.0f;
    return F(std::strtod(tokens_[index_++], nullptr));
}

// ---- ObjectDef family -------------------------------------------------------------------------------------------------
void ObjectDef::Register() {                          // orig: 0x000977E0
    if (!model && !streamRequested) streamRequested = true;   // orig: +0x24 = new stream request (0x000D2160)
}

std::unique_ptr<PickupDef> CreatePickupDef(ni::NiNode& node) {   // orig: 0x000772C0
    ni::NiStringExtraData* sx = ParameterData(node);
    if (!sx) return nullptr;
    const size_t comma = sx->string.find(',');
    if (comma != std::string::npos) sx->string.resize(comma);   // orig: *strchr(p, ',') = 0 (faults without one)
    auto def = std::make_unique<PickupDef>();
    def->name = NameAfter(node, 11);
    def->model = FindNode(node, "Mesh");
    if (def->model) def->model->local.scale = F(std::fabs(double(1.4f)));   // orig: fabs of the double 0x0029CE88
    TakeIcon(node, *def);
    if (!sx->string.empty()) def->value = std::atoi(sx->string.c_str());
    // orig: needs the level object's name (+0x0C), always set while a level loads.
    if (!def->name.empty() && EqualsNoCase(def->name.c_str(), "BenderRuby") && def->model)
        if (ni::NiAVObject* shape = GetObjectByName(def->model.Get(), "ammoShape1"))
            if (ni::NiProperty* alpha = GetProperty(*shape, 0)) alpha->flags &= ~1u;   // alpha blending off
    return def;
}

namespace {
template <class T>
std::unique_ptr<T> CreateAmountDef(ni::NiNode& node, size_t prefixLength) {   // Powerup / Charge
    ni::NiStringExtraData* sx = ParameterData(node);
    if (!sx) return nullptr;
    const f32 amount = Atof(sx->string.c_str());
    auto def = std::make_unique<T>();
    def->name = NameAfter(node, prefixLength);
    def->model = FindNode(node, "Mesh");
    def->amount = amount;
    TakeIcon(node, *def);
    return def;
}
}  // namespace

std::unique_ptr<PowerupDef> CreatePowerupDef(ni::NiNode& node) {   // orig: 0x00074BA0
    return CreateAmountDef<PowerupDef>(node, 12);
}

std::unique_ptr<ChargeDef> CreateChargeDef(ni::NiNode& node) {      // orig: 0x00075980
    return CreateAmountDef<ChargeDef>(node, 11);
}

std::unique_ptr<AmmoDef> CreateAmmoDef(ni::NiNode& node) {          // orig: 0x00075210
    ni::NiStringExtraData* sx = ParameterData(node);
    if (!sx) return nullptr;
    const size_t comma = sx->string.find(',');
    if (comma == std::string::npos) return nullptr;
    const std::string countText = sx->string.substr(comma + 1);
    sx->string.resize(comma);                         // orig: *comma = 0
    const char* type = sx->string.c_str();
    u32 ammoType = AmmoDef::kPistol;
    if (EqualsNoCase(type, "Pistol")) ammoType = AmmoDef::kPistol;
    else if (EqualsNoCase(type, "Shotgun")) ammoType = AmmoDef::kShotgun;
    else if (EqualsNoCase(type, "Railgun")) ammoType = AmmoDef::kRailgun;
    else if (EqualsNoCase(type, "Machinegun")) ammoType = AmmoDef::kMachinegun;
    else if (EqualsNoCase(type, "Rocket")) ammoType = AmmoDef::kRocket;
    const s32 count = std::atoi(countText.c_str());
    auto def = std::make_unique<AmmoDef>();
    def->name = NameAfter(node, 9);
    def->model = FindNode(node, "Mesh");
    def->count = count;
    def->ammoType = ammoType;
    TakeIcon(node, *def);
    return def;
}

std::unique_ptr<WeaponDef> CreateWeaponDef(ni::NiNode& node, DefContext& ctx) {   // orig: 0x0007F090
    ni::NiStringExtraData* sx = ParameterData(node);
    if (!sx) return nullptr;
    DefTokenizer tokens(sx->string.c_str());
    tokens.NextLine();
    const std::string weapon = tokens.NextToken();
    auto def = std::make_unique<WeaponDef>();
    def->name = NameAfter(node, 11);
    struct Spec {
        const char* name;
        u32 type;
        std::array<const char*, 7> anims;
        // sound requests in the original's order: (path, field)
        std::vector<std::pair<const char*, u32 WeaponDef::*>> sounds;
    };
    static const Spec kSpecs[] = {
        {"Hammer", WeaponDef::kHammer,
         {"hammer_idle", "hammer_attack1", "hammer_attack2", "move_hammer_attack1", "hammer_attack2", "hammer_takeout",
          "hammer_putaway"},
         {{"sounds/weapons/hammer swing 1.wav", &WeaponDef::fireSound},
          {"sounds/weapons/hammer swing 2.wav", &WeaponDef::secondarySound}}},
        {"Pistol", WeaponDef::kPistol,
         {"pistol_idle", "pistol_fire1", "pistol_fire2", "move_pistol_attack1", "pistol_fire2", "pistol_takeout",
          "pistol_putaway"},
         {{"sounds/weapons/laser pistol fire 1.wav", &WeaponDef::fireSound},
          {"sounds/weapons/laser pistol reload.wav", &WeaponDef::reloadSound},
          {"sounds/weapons/click.wav", &WeaponDef::clickSound},
          {"sounds/weapons/laser pistol charged - charging.wav", &WeaponDef::chargingSound},
          {"sounds/weapons/laser pistol charged - loop.wav", &WeaponDef::chargedLoopSound},
          {"sounds/weapons/laser pistol charged - fire.wav", &WeaponDef::secondarySound}}},
        {"MachineGun", WeaponDef::kMachineGun,
         {"shotgun_idle", "machine_gun_attack1", "machinegun_fire2", "machine_gun_attack1", "machinegun_fire2",
          "shotgun_takeout", "shotgun_putaway"},
         {{"sounds/weapons/laser pistol fire 1.wav", &WeaponDef::fireSound},
          {"sounds/weapons/laser pistol fire 1.wav", &WeaponDef::secondarySound},
          {"sounds/weapons/machinegun reload.wav", &WeaponDef::reloadSound},
          {"sounds/weapons/click.wav", &WeaponDef::clickSound},
          {"sounds/weapons/machinegun charged - charging.wav", &WeaponDef::chargingSound},
          {"sounds/weapons/machinegun charged - loop.wav", &WeaponDef::chargedLoopSound}}},
        {"Shotgun", WeaponDef::kShotgun,
         {"shotgun_idle", "shotgun_fire1", "shotgun_fire2", "shotgun_fire1", "shotgun_fire2", "shotgun_takeout",
          "shotgun_putaway"},
         {{"sounds/weapons/laser shotgun fire 1.wav", &WeaponDef::fireSound},
          {"sounds/weapons/laser shotgun reload.wav", &WeaponDef::reloadSound},
          {"sounds/weapons/click.wav", &WeaponDef::clickSound},
          {"sounds/weapons/laser shotgun charged - charging.wav", &WeaponDef::chargingSound},
          {"sounds/weapons/laser shotgun charged - loop.wav", &WeaponDef::chargedLoopSound},
          {"sounds/weapons/laser shotgun charged - fire.wav", &WeaponDef::secondarySound}}},
        {"Railgun", WeaponDef::kRailgun,
         {"railgun_idle", "railgun_attack1", "railgun_attack2", "railgun_attack1", "railgun_attack2",
          "shotgun_takeout", "shotgun_putaway"},
         {{"sounds/Level5/Railgun.wav", &WeaponDef::fireSound},
          {"sounds/weapons/click.wav", &WeaponDef::clickSound},
          {"sounds/Level5/Railgun charged - charge.wav", &WeaponDef::chargingSound},
          {"sounds/Level5/Railgun charged - loop.wav", &WeaponDef::chargedLoopSound},
          {"sounds/Level5/Railgun charged - fire.wav", &WeaponDef::secondarySound},
          {"sounds/Level5/Railgun-loop.wav", &WeaponDef::loopSound}}},
    };
    for (const Spec& spec : kSpecs) {                 // orig: exact compares (repe cmpsb including the NUL)
        if (weapon != spec.name) continue;
        def->weaponType = spec.type;
        for (u32 i = 0; i < 7; ++i) def->animations[i] = spec.anims[i];
        for (const auto& [path, field] : spec.sounds) (*def).*field = ctx.loadSound(path, 0);
        break;
    }
    def->ammo = tokens.NextFloat();
    def->model = FindNode(node, "Mesh");
    def->model2 = FindNode(node, "Mesh");
    if (def->model) node.DetachChild(def->model.Get());
    if (def->model2) node.DetachChild(def->model2.Get());
    TakeIcon(node, *def);
    return def;
}

// ---- moveable / debris ------------------------------------------------------------------------------------------------
std::unique_ptr<DebrisDef> CreateDebrisDef(ni::NiNode& node) {      // orig: 0x0007FD80
    const char* p = ParameterString(node);
    if (!p) return nullptr;
    u32 shape;
    std::array<f32, 3> size{};
    const char* q;
    if (StartsWith(p, "Box,", 4)) {
        shape = MoveableDef::kBox;
        size[0] = Atof(p + 4);
        q = std::strchr(p + 4, ',');                  // orig: strchr(p, ',') + 1 without NULL checks
        size[1] = Atof(q ? q + 1 : nullptr);
        q = q ? std::strchr(q + 1, ',') : nullptr;
        size[2] = Atof(q ? q + 1 : nullptr);
        q = q ? std::strchr(q + 1, ',') : nullptr;
    } else if (StartsWith(p, "Sphere,", 7)) {
        shape = MoveableDef::kSphere;
        size[0] = Atof(p + 7);
        q = std::strchr(p + 7, ',');
    } else {
        return nullptr;
    }
    const f32 mass = Atof(q ? q + 1 : nullptr);
    auto def = std::make_unique<DebrisDef>();
    def->name = NameAfter(node, 11);
    def->shape = shape;
    def->size = size;
    def->mass = mass;
    def->model = FirstChildNode(node);
    if (def->model) node.DetachChild(def->model.Get());
    return def;
}

// ---- destructable -----------------------------------------------------------------------------------------------------
namespace {
// orig: 0x00065180: the object itself if it is a NiSwitchNode, else the first one among its children, depth first.
ni::NiSwitchNode* FindSwitchNode(ni::NiAVObject* object) {
    if (!object) return nullptr;
    if (auto* sw = dynamic_cast<ni::NiSwitchNode*>(object)) return sw;
    if (auto* node = dynamic_cast<ni::NiNode*>(object))
        for (const auto& child : node->Children())
            if (ni::NiSwitchNode* found = FindSwitchNode(child.Get())) return found;
    return nullptr;
}

// orig: the ';'-separated effect lists: count = 1 + number of ';', each entry copied (stack buffer of 18 entries).
std::vector<std::string> SplitList(const char* field) {
    std::vector<std::string> out;
    if (!*field) return out;
    const char* start = field;
    for (const char* s = std::strchr(field, ';'); s; s = std::strchr(s + 1, ';')) {
        out.emplace_back(start, s);
        start = s + 1;
    }
    out.emplace_back(start);
    return out;
}
}  // namespace

std::unique_ptr<DestructableDef> CreateDestructableDef(ni::NiNode& node, DefContext& ctx) {   // orig: 0x000656C0
    ni::NiStringExtraData* sx = ParameterData(node);
    if (!sx) return nullptr;
    auto def = std::make_unique<DestructableDef>();
    def->name = NameAfter(node, 17);                  // orig: ctor 0x000652E0 (name after "DestructableDef::")
    for (const char* barrel : {"Sewerbarrel", "Suncity_steelbarrel_explosive", "Explosive_Barrel"})
        if (def->name == barrel) {                    // orig: exact compares
            def->explosive = true;
            def->explosionA = 20.0f;
            def->explosionB = 80.0f;
        }
    // The original parses the extra data string in place, writing NULs over the field separators.
    std::string text = sx->string;
    char* p = text.data();
    auto next = [&](char* at) {                       // orig: strchr(at, ',') + 1 (no NULL check)
        char* c = std::strchr(at, ',');
        return c ? c + 1 : at + std::strlen(at);
    };
    f32* const slots[7] = {&def->center[0], &def->center[1], &def->center[2], &def->extents[0], &def->extents[1],
                           &def->extents[2], &def->health};
    for (u32 i = 0; i < 7; ++i) {
        *slots[i] = Atof(p);
        p = next(p);
    }
    char* fields[6];                                  // effect, list 1, list 2, sound, value 12, rest
    fields[0] = p;
    for (u32 i = 1; i < 6; ++i) {
        char* c = std::strchr(fields[i - 1], ',');
        if (c) *c = '\0';
        fields[i] = c ? c + 1 : fields[i - 1] + std::strlen(fields[i - 1]);
    }
    if (*fields[0]) def->effect = fields[0];
    def->effects1 = SplitList(fields[1]);
    def->effects2 = SplitList(fields[2]);
    if (*fields[3]) {
        def->breakSoundPath = fields[3];
        def->breakSound = ctx.loadSound(def->breakSoundPath, 0);
        def->hasBreakSound = true;
    }
    if (*fields[4]) {
        def->hasValue12 = true;
        def->value12 = std::atoi(fields[4]);
    }
    if (*fields[5]) {                                 // orig: strtok(rest, ","), then 9 x strtok(NULL, ",")
        char* state = fields[5];
        auto strtok = [&]() -> const char* {
            while (*state == ',') ++state;
            if (!*state) return nullptr;
            char* token = state;
            while (*state && *state != ',') ++state;
            if (*state) *state++ = '\0';
            return token;
        };
        const char* t = strtok();
        def->value13 = t ? std::atoi(t) : 0;
        for (f32& v : def->params) v = Atof(strtok());
    }
    sx->string.resize(std::strlen(text.c_str()));     // orig: the string now ends at the first written NUL

    def->model = FindSwitchNode(node.Children().empty() ? nullptr : node.Children()[0].Get());
    if (def->model && def->model->parent) def->model->parent->DetachChild(def->model.Get());   // orig: faults without
    PrepareModelLighting(*def, def->model.Get());
    return def;
}

Def* DefRegistry::CreateFromNode(ni::NiObject* object, DefContext& ctx) {   // orig: 0x000C5BD0, kind 9
    auto* net = dynamic_cast<ni::NiObjectNET*>(object);
    auto* node = dynamic_cast<ni::NiNode*>(object);
    if (!net || net->name.empty()) return nullptr;
    const std::string& n = net->name;
    auto is = [&](const char* prefix) { return crt::StrNICmp(n.c_str(), prefix, std::strlen(prefix)) == 0; };   // _strnicmp
    std::unique_ptr<Def> def;
    // In the original's order; the creators return NULL for a node that is not a NiNode.
    if (is("StaticDef::")) {
        if (node) def = CreateStaticDef(*node, ctx);
    } else if (is("MoveableDef::")) {
        // orig: 0x000745D0 (no MoveableDef in the shipped data; not ported yet)
    } else if (is("DestructableDef::")) {
        if (node) def = CreateDestructableDef(*node, ctx);
    } else if (is("DebrisDef::")) {
        if (node) def = CreateDebrisDef(*node);
    } else if (is("WeaponDef::")) {
        if (node) def = CreateWeaponDef(*node, ctx);
    } else if (is("PickupDef::")) {
        if (node) def = CreatePickupDef(*node);
    } else if (is("ObjectDef::")) {
        // orig: 0x000974A0 (not ported yet)
    } else if (is("CharacterDef::")) {
        // orig: 0x000C62EF: the class from the name after the prefix, the constructor (a GenericNPCDef / SimpleNPCDef
        // also builds its own part, appending its weapon definitions after itself), then slot 1 (RequestStream).
        std::unique_ptr<CharacterDef> character = CreateCharacterDef(n.substr(14));
        CharacterDef* c = character.get();
        defs_.push_back(std::move(character));
        if (auto* g = dynamic_cast<GenericNPCDef*>(c)) {
            if (ctx.level) g->Construct(ctx, *ctx.level, ctx.levelName, defs_);
        } else if (auto* simple = dynamic_cast<SimpleNPCDef*>(c)) {
            simple->Construct(ctx);
        }
        c->streamContext = &ctx;
        c->RequestStream();
        return c;
    } else if (is("PowerupDef::")) {
        if (node) def = CreatePowerupDef(*node);
    } else if (is("ChargeDef::")) {
        if (node) def = CreateChargeDef(*node);
    } else if (is("AmmoDef::")) {
        if (node) def = CreateAmmoDef(*node);
    }
    if (!def) return nullptr;
    // orig: the loader then calls vtable slot 1 (StaticDef: returns true; ObjectDef family: 0x000977E0).
    if (auto* od = dynamic_cast<ObjectDef*>(def.get())) od->Register();
    defs_.push_back(std::move(def));
    return defs_.back().get();
}

Def::~Def() {                                         // orig: 0x00096370
    while (firstInstance) delete firstInstance;       // vf00(1) (GameObject's dtor moves +0x0C on)
}

Def* DefRegistry::Find(const char* name) const {      // orig: 0x000C3F10
    for (const auto& d : defs_)
        if (!d->name.empty() && text::StrICmp(d->name.c_str(), name) == 0) return d.get();
    return nullptr;
}

CharacterDef* DefRegistry::FindCharacterDef(const char* name) const {   // orig: 0x000C3FB0
    for (const auto& d : defs_)
        if (auto* c = dynamic_cast<CharacterDef*>(d.get()))
            if (text::StrICmp(c->name.c_str(), name) == 0) return c;
    return nullptr;
}

Def* DefRegistry::Add(std::unique_ptr<Def> def) {
    defs_.push_back(std::move(def));
    return defs_.back().get();
}

void DefRegistry::Delete(Def* def) {
    if (!def) return;
    if (explosion == def) explosion = nullptr;
    if (scorchFactory == def) scorchFactory = nullptr;
    if (respawnEffect == def) respawnEffect = nullptr;
    auto it = std::find_if(defs_.begin(), defs_.end(), [def](const auto& d) { return d.get() == def; });
    if (it == defs_.end()) return;
    std::unique_ptr<Def> keep = std::move(*it);       // (unlinked first, as the deleting dtor does)
    defs_.erase(it);
    keep.reset();
}

void DefRegistry::DeleteAll() {
    // the character definitions (S+0x3C), head first
    for (;;) {
        auto it = std::find_if(defs_.begin(), defs_.end(), [](const auto& d) { return dynamic_cast<CharacterDef*>(d.get()); });
        if (it == defs_.end()) break;
        Delete(it->get());
    }
    while (!defs_.empty()) Delete(defs_.front().get());   // the rest of S+0x34, head first
}

void DefRegistry::LoadEffects(LevelData& level, ni::NiStream& effects, DefContext& ctx,
                              const std::function<void()>& afterExplosion) {   // orig: 0x000C5BD0, kind 3
    if (!explosion) {                                  // orig: level +0x44; ctor 0x00067570, then slot 1
        auto e = std::make_unique<ExplosionDef>(ctx);
        explosion = e.get();
        defs_.push_back(std::move(e));
    }
    if (afterExplosion) afterExplosion();              // orig: 0x000C5D14..0x000C5D2D
    // orig: 0x000BF980 with the registry 0x000BF870.
    for (const std::string& effectName : level.LoadEffectsStream(effects))
        EffectLibrary::Create(effectName.c_str(), level, defs_);
    // orig: 0x000C59E0.
    if (!level.envTextureEffect)
        if (ni::NiObjectNET* tex = level.FindTexture("newshade.tga")) {
            ni::NiPointer<ni::NiTextureEffect> e(new ni::NiTextureEffect);   // orig: 0x00119F00
            level.envTextureEffect = e;
            e->sourceTextureObj = tex;
            e->textureType = 2;                        // environment map (stored in this order: +0x168, +0x160,
            e->textureFiltering = 0;                   //  +0x164, +0x16C)
            e->textureClamping = 3;
            e->coordGenType = 2;                       // sphere map
            e->UpdateProjection();
            e->switchOn = true;
            level.dynamicNode->AttachChild(e.Get(), true);
            level.dynamicNode->UpdateProperties();     // 0x000C5B2A
            level.dynamicNode->UpdateEffects();
            level.dynamicNode->UpdateDownwardPass(0.0f, true);
        }
    if (!scorchFactory) {                              // orig: ctor 0x00072C70 (with ScorchMarkDef ctor 0x000721D0)
        auto factory = std::make_unique<SMarkFactoryDef>();
        factory->name = "SMarkFactory";
        scorchFactory = factory.get();
        defs_.push_back(std::move(factory));
        auto mark = std::make_unique<ScorchMarkDef>();
        mark->name = "Scorch_Mark";
        mark->texture = level.FindTexture("Scorch.TGA");
        scorchFactory->mark = mark.get();
        defs_.push_back(std::move(mark));
        level.markInstance = SpawnMarkInstance(level, *scorchFactory, &level, 0x40, "MarkEffectInstance");   // vf20
    }
    respawnEffect = Find("RespawnEffect");
    // 0x000C5D64 (after 0x000C59E0): the level's outline texture into the outline polygons' properties
    if (ni::NiObjectNET* tex = level.FindTexture("outline.tga")) ni::SetOutlineTexture(tex);   // 0x0013CEF0
}

}  // namespace fut::game
