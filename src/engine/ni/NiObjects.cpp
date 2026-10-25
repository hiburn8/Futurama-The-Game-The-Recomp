// LoadBinary implementations decompiled from default.xbe (addresses in NiObjects.h and below).
// Version checks compare the file version with FUN_00107750("a.b.c.d"); the shipped files are 4.2.1.0 except
// FutNTSC02/images/english/leaveme.nif (4.1.0.13) and FutNTSC03/sounds/soundlookup.nif (4.0.0.2).
#include "engine/ni/NiObjects.h"

#include <cmath>
#include <functional>

namespace fut::ni {

void LoadPoint3(NiStream& s, NiPoint3& p) {           // orig: 0x001408B0
    p.x = s.ReadF32();
    p.y = s.ReadF32();
    p.z = s.ReadF32();
}

void LoadMatrix3(NiStream& s, NiMatrix3& m) {         // orig: 0x00104130
    // Nine floats = M row-major (p' = M p). The original stores them column-major in a padded 3x4 block.
    for (auto& row : m.m)
        for (f32& f : row) f = s.ReadF32();
}

void LoadBound(NiStream& s, NiBound& b) {             // orig: 0x00141E60
    LoadPoint3(s, b.center);
    b.radius = s.ReadF32();
}

static void LoadPlane(NiStream& s, NiPlane& p) {      // same reader as NiBound (0x00141E60)
    LoadPoint3(s, p.normal);
    p.constant = s.ReadF32();
}

static void LoadTransform(NiStream& s, NiTransform& t) {   // orig: 0x0014DA10
    LoadMatrix3(s, t.rotate);
    LoadPoint3(s, t.translate);
    t.scale = s.ReadF32();
}

static void LoadColor(NiStream& s, NiColor& c) {      // NiColor::LoadBinary = the NiPoint3 reader (0x001408B0)
    c.r = s.ReadF32();
    c.g = s.ReadF32();
    c.b = s.ReadF32();
}

static void LoadColorA(NiStream& s, NiColorA& c) {    // orig: 0x00151AC0
    c.r = s.ReadF32();
    c.g = s.ReadF32();
    c.b = s.ReadF32();
    c.a = s.ReadF32();
}

static void LoadTcb(NiStream& s, Tcb& t) {
    t.tension = s.ReadF32();
    t.continuity = s.ReadF32();
    t.bias = s.ReadF32();
}

// ---- keys -----------------------------------------------------------------------------------------------------------
// Every key starts with its time (orig: 0x00152C10). Type 0 (orig: 0x00013DB0) reads nothing and yields no array.
void LoadFloatKeys(NiStream& s, KeyType type, u32 count, FloatKeys& out) {
    out.type = type;
    out.keys.clear();
    switch (type) {
    case KeyType::Linear:      // orig: 0x001322F0 -> 0x001201A0
    case KeyType::Quadratic:   // orig: 0x00120AD0
    case KeyType::Tcb:         // orig: 0x0013BE80
        break;
    case KeyType::None:
        return;
    default:                   // the original indexes its table out of bounds (null entry -> crash)
        s.Fail(NiStreamError::Unsupported, "bad float key type");
        return;
    }
    out.keys.resize(count);
    for (FloatKey& k : out.keys) {
        k.time = s.ReadF32();
        k.value = s.ReadF32();
        if (type == KeyType::Quadratic) {
            k.inTangent = s.ReadF32();
            k.outTangent = s.ReadF32();
        } else if (type == KeyType::Tcb) {
            LoadTcb(s, k.tcb);
        }
    }
}

void LoadPosKeys(NiStream& s, KeyType type, u32 count, PosKeys& out) {
    out.type = type;
    out.keys.clear();
    switch (type) {
    case KeyType::Linear:      // orig: 0x00132740 -> 0x00120D00
    case KeyType::Quadratic:   // orig: 0x001219D0
    case KeyType::Tcb:         // orig: 0x0013B3C0
        break;
    case KeyType::None:
        return;
    default:
        s.Fail(NiStreamError::Unsupported, "bad position key type");
        return;
    }
    out.keys.resize(count);
    for (PosKey& k : out.keys) {
        k.time = s.ReadF32();
        LoadPoint3(s, k.value);
        if (type == KeyType::Quadratic) {
            LoadPoint3(s, k.inTangent);
            LoadPoint3(s, k.outTangent);
        } else if (type == KeyType::Tcb) {
            LoadTcb(s, k.tcb);
        }
    }
}

// orig: 0x00124930 - one Euler "key": u32 axis order, then per axis u32 count [+ u32 type + float keys].
static void LoadEulerKey(NiStream& s, EulerRotKey& k) {
    k.axisOrder = static_cast<u8>(s.ReadU32());
    for (FloatKeys& axis : k.axes) {
        const u32 count = s.ReadU32() & 0xFFFF;              // stored as u16 (this+4+i*2)
        axis = {};
        if (count) {
            // Stored as a byte (this+0x10+i), but the full u32 indexes the loader table.
            const auto type = static_cast<KeyType>(s.ReadU32());
            LoadFloatKeys(s, type, count, axis);
        }
    }
}

void LoadRotKeys(NiStream& s, KeyType type, u32 count, RotKeys& out) {
    out.type = type;
    out.keys.clear();
    out.euler.clear();
    switch (type) {
    case KeyType::Linear:      // orig: 0x00132D60 -> 0x00122130
    case KeyType::Quadratic:   // orig: 0x00122520
    case KeyType::Tcb:         // orig: 0x0013B870
        break;
    case KeyType::Euler:       // orig: 0x00124A50
        out.euler.resize(count);
        for (EulerRotKey& k : out.euler) LoadEulerKey(s, k);
        return;
    case KeyType::None:        // orig: table entries 0 and 5 are 0x00013DB0
        return;
    default:
        s.Fail(NiStreamError::Unsupported, "bad rotation key type");
        return;
    }
    out.keys.resize(count);
    for (RotKey& k : out.keys) {
        k.time = s.ReadF32();
        // HYPOTHESIS (docs/NOTES.md): 0x00122130 reads this quaternion into a stack temporary that is never stored,
        // so the shipped Xbox build appears to drop it. The port keeps the value until the animation code is ported.
        k.value.w = s.ReadF32();
        k.value.x = s.ReadF32();
        k.value.y = s.ReadF32();
        k.value.z = s.ReadF32();
        if (type == KeyType::Tcb) LoadTcb(s, k.tcb);
    }
}

// Common "u32 count; if count { u32 type; keys }" group used by the keyed data objects.
template <class Keys, class LoadFn>
static void LoadKeyGroup(NiStream& s, Keys& keys, LoadFn load) {
    const u32 count = s.ReadU32();
    keys = {};
    if (count) {
        const auto type = static_cast<KeyType>(s.ReadU32());
        load(s, type, count, keys);
    }
}

// =====================================================================================================================
namespace {
bool g_shareTextures = true;                          // orig: DAT_002F9AA0 (on from the boot, 0x000A283F)
bool g_sharePixelData = true;                         // orig: DAT_002F973C
std::vector<NiSourceTexture*>& SharedTextures() {     // orig: DAT_002D79B4 (count DAT_002D79BC; raw pointers)
    static std::vector<NiSourceTexture*> list;
    return list;
}
std::vector<NiPixelData*>& SharedPixelData() {        // orig: DAT_002D78F4 (count DAT_002D78FC)
    static std::vector<NiPixelData*> list;
    return list;
}
template <class T>
void RemoveShared(std::vector<T*>& list, const T* o) {
    for (auto& p : list)
        if (p == o) p = nullptr;                      // (the slot cleared, as 0x001116E0 does for pixel data)
}
NiSourceTexture* FindSharedTexture(const NiSourceTexture& t) {   // orig: 0x00118330
    if (!g_shareTextures || !t.unknown3C || !t.name.empty()) return nullptr;   // (+0x3C; GetName null)
    for (NiSourceTexture* p : SharedTextures())
        if (p && p->unknown3C && t.unknown3C == p->unknown3C) return p;
    return nullptr;
}
void AddSharedTexture(NiSourceTexture* t) {           // orig: 0x00118A80 (unless already in the list)
    auto& list = SharedTextures();
    for (NiSourceTexture* p : list)
        if (p == t) return;
    list.push_back(t);
}
}  // namespace

void NiObject::LoadBinary(NiStream& s) { s.RegisterObject(this); }   // orig: 0x000FE020

void NiObjectNET::LoadBinary(NiStream& s) {           // orig: 0x000FE360
    NiObject::LoadBinary(s);
    s.ReadString(name);
    extraDataLink = s.ReadLinkID();
    controllerLink = s.ReadLinkID();
}

void NiAVObject::LoadBinary(NiStream& s) {            // orig: 0x00102380
    NiObjectNET::LoadBinary(s);
    flags = s.ReadU16();
    const u32 v = s.FileVersion();
    if (v < NiVersion(4, 1, 0, 11)) flags = static_cast<u16>(((flags & 0xFFF8) << 1) | (flags & 7));
    if (v < NiVersion(4, 1, 0, 12)) flags = static_cast<u16>(((flags & 0xFFF0) << 4) | (flags & 0xF) | 0x70);
    LoadPoint3(s, local.translate);
    LoadMatrix3(s, local.rotate);
    local.scale = s.ReadF32();
    LoadPoint3(s, velocity);
    // orig: 0x00102446-0x00102502 (a game addition): a non-zero velocity ((z*z + y*y) + x*x != 0, x87; a NaN counts as
    // non-zero) becomes a 16-byte NiExtraData "#V#\0" + the three floats, prepended to the extra-data chain
    // (AddExtraData 0x000FE420), when the object's "#V#" vector (0x000CDB30) has zero length. At load time the chain
    // is still empty (LinkObject appends the file's extra data later), so that vector is the static zero one.
    const auto sq = [](const NiPoint3& p) {
        return (double(p.z) * p.z + double(p.y) * p.y) + double(p.x) * p.x;
    };
    if (!(sq(velocity) == 0.0)) {
        NiPoint3 existing;
        for (NiExtraData* e = extraData.Get(); e; e = e->nextData.Get())
            if (e->size == 0x10 && e->data.size() >= 0x10 && std::memcmp(e->data.data(), "#V#\0", 4) == 0) {
                std::memcpy(&existing, e->data.data() + 4, 12);
                break;
            }
        if (sq(existing) == 0.0) {                    // TEST AH,0x44; JP (a NaN length keeps the old one)
            NiPointer<NiExtraData> vx(new NiExtraData);
            vx->size = 0x10;
            vx->data.resize(0x10);
            std::memcpy(vx->data.data(), "#V#\0", 4);
            std::memcpy(vx->data.data() + 4, &velocity, 12);
            vx->nextData = extraData;
            extraData = vx;
        }
    }
    s.ReadMultipleLinkIDs(propertyLinks);
    bool hasBound;
    if (v < NiVersion(4, 1, 0, 0)) {
        hasBound = s.ReadU32() != 0;
    } else {
        hasBound = s.ReadBool();
    }
    if (hasBound) {
        // orig: 0x0014D8A0 reads a u32 type and calls a bounding-volume factory table (0x0030EF54) that this
        // build leaves empty, so the original would call a null pointer. No shipped file sets the flag.
        s.Fail(NiStreamError::Unsupported, "NiAVObject bounding volume");
    }
}

void NiNode::LoadBinary(NiStream& s) {                // orig: 0x00104B80
    NiAVObject::LoadBinary(s);
    s.ReadMultipleLinkIDs(childLinks);
    s.ReadMultipleLinkIDs(effectLinks);
}

void NiBillboardNode::LoadBinary(NiStream& s) {       // orig: 0x00110050
    NiNode::LoadBinary(s);
    if (s.FileVersion() < NiVersion(4, 2, 0, 3)) flags = static_cast<u16>(((flags & 0xF000) << 1) | (flags & 0xFFF));
}

void NiSwitchNode::LoadBinary(NiStream& s) {          // orig: 0x00113A90
    NiNode::LoadBinary(s);
    index = static_cast<s32>(s.ReadU32());
}

void NiFltAnimationNode::LoadBinary(NiStream& s) {    // orig: 0x00114010
    NiSwitchNode::LoadBinary(s);
    period = s.ReadF32();
}

void NiLODNode::LoadBinary(NiStream& s) {             // orig: 0x00114800
    NiSwitchNode::LoadBinary(s);
    LoadPoint3(s, lodCenter);
    ranges.assign(s.ReadU32(), Range{});
    for (Range& r : ranges) {
        r.nearDist = s.ReadF32();
        r.farDist = s.ReadF32();
    }
}

void NiBSPNode::LoadBinary(NiStream& s) {             // orig: 0x0011D360
    NiNode::LoadBinary(s);
    LoadPlane(s, plane);
}

void NiRoom::LoadBinary(NiStream& s) {                // orig: 0x0011D840
    NiNode::LoadBinary(s);
    for (auto& list : linkLists) s.ReadMultipleLinkIDs(list);
}

void NiRoomGroup::LoadBinary(NiStream& s) {           // orig: 0x0011F190
    NiNode::LoadBinary(s);
    shell = s.ReadLinkID();
    s.ReadMultipleLinkIDs(rooms);
}

NiOutline::NiOutline() = default;                     // orig: 0x0013D100
NiOutline::~NiOutline() = default;

void NiOutline::LoadBinary(NiStream& s) {             // orig: 0x0013C850
    NiNode::LoadBinary(s);
    outlineData = s.ReadLinkID();
}

void NiOutline::LinkObject(NiStream& s) {
    NiNode::LinkObject(s);
    data = dynamic_cast<NiOutlineData*>(s.ResolveLink(outlineData));
}

void NiGeometry::LoadBinary(NiStream& s) {            // orig: 0x00144760
    NiAVObject::LoadBinary(s);
    modelDataLink = s.ReadLinkID();
    skinInstanceLink = s.ReadLinkID();
}

void NiDynamicEffect::LoadBinary(NiStream& s) {       // orig: 0x00141B10
    NiAVObject::LoadBinary(s);
    if (s.FileVersion() < NiVersion(4, 1, 0, 0)) {
        const u32 count = s.ReadU32();                  // obsolete affected-node pointers, discarded
        for (u32 i = 0; i < count && !s.Truncated(); ++i) s.ReadU32();
    }
}

void NiLight::LoadBinary(NiStream& s) {               // orig: 0x001457B0
    NiDynamicEffect::LoadBinary(s);
    dimmer = s.ReadF32();
    LoadColor(s, ambient);
    LoadColor(s, diffuse);
    LoadColor(s, specular);
}

void NiPointLight::LoadBinary(NiStream& s) {          // orig: 0x00117490
    NiLight::LoadBinary(s);
    constantAtten = s.ReadF32();
    linearAtten = s.ReadF32();
    quadraticAtten = s.ReadF32();
}

void NiTextureEffect::LoadBinary(NiStream& s) {       // orig: 0x0011A5A0
    NiDynamicEffect::LoadBinary(s);
    LoadMatrix3(s, modelProjection);
    LoadPoint3(s, modelProjectionTranslate);
    textureFiltering = s.ReadU32();
    textureClamping = s.ReadU32();
    textureType = s.ReadU32();
    coordGenType = s.ReadU32();
    sourceTexture = s.ReadLinkID();
    clippingPlaneEnabled = s.ReadBool();
    LoadPlane(s, clippingPlane);
    ps2L = s.ReadU16();
    ps2K = s.ReadU16();
    if (s.FileVersion() < NiVersion(4, 1, 0, 16)) s.ReadU16();   // obsolete, discarded
}

void NiCamera::LoadBinary(NiStream& s) {              // orig: 0x00100680
    NiAVObject::LoadBinary(s);
    for (f32& f : frustum) f = s.ReadF32();
    for (f32& f : port) f = s.ReadF32();
    lodAdjust = s.ReadF32();
    scene = s.ReadLinkID();
    s.ReadMultipleLinkIDs(screenPolygons);
    if (s.FileVersion() >= NiVersion(4, 1, 0, 4)) s.ReadMultipleLinkIDs(screenTextures);
}

// orig: 0x000FD540 (recursive). Leaves (type 3) hold triangle indices (orig: 0x000FC070), inner nodes two children.
static void LoadBspTree(NiStream& s, NiBsp::TreeNode& node, int depth) {
    node.type = s.ReadU32();
    LoadPoint3(s, node.boundMin);
    LoadPoint3(s, node.boundMax);
    node.value = s.ReadU32();
    if (node.type == 3) {
        const s32 count = static_cast<s32>(node.value);
        if (count > 0) {
            node.indices.resize(static_cast<u32>(count));
            for (u32& i : node.indices) i = s.ReadU32();
        }
        return;
    }
    if (depth > 256 || s.Failed() || s.Truncated()) {        // port-only guard against malformed recursion
        s.Fail(NiStreamError::Unsupported, "NiBsp tree too deep");
        return;
    }
    node.children.resize(2);
    LoadBspTree(s, node.children[0], depth + 1);
    LoadBspTree(s, node.children[1], depth + 1);
}

NiBsp* g_bspList = nullptr;                          // orig: DAT_002F8ED4 (NiCollision.h)

NiBsp::~NiBsp() {                                     // orig: 0x000FD740
    for (NiBsp** p = &g_bspList; *p; p = &(*p)->nextInWorld)
        if (*p == this) {
            *p = nextInWorld;
            break;
        }
}

void NiBsp::LinkObject(NiStream& s) {                 // orig: 0x000FCF40
    NiAVObject::LinkObject(s);
    // each link that is a NiTriShape becomes record k's geometry (the others keep none)
    for (size_t k = 0; k < links.size(); ++k)
        if (auto* shape = dynamic_cast<NiTriShape*>(s.ResolveLink(links[k])))
            if (k < meshes.size() && meshes[k]) meshes[k]->geom = shape;   // (port guard: the original indexes)
    // (port: the default sort key, below every listed NiBsp's; collision_meshes_port.md 7.5)
    u32 lowest = 0x80000000u;
    for (const NiBsp* b = g_bspList; b; b = b->nextInWorld)
        if (b->origTriAddr && b->origTriAddr < lowest) lowest = b->origTriAddr;
    origTriAddr = lowest - 0x00400000u;
    nextInWorld = g_bspList;                          // head insert, also without groups
    g_bspList = this;
}

void NiBsp::LoadBinary(NiStream& s) {                 // orig: 0x000FD9A0
    NiAVObject::LoadBinary(s);
    LoadPoint3(s, boundMin);
    LoadPoint3(s, boundMax);
    const s32 triCount = static_cast<s32>(s.ReadU32());
    const s32 vertCount = static_cast<s32>(s.ReadU32());
    triangles.resize(triCount > 0 ? static_cast<u32>(triCount) : 0);
    for (Triangle& t : triangles) {
        t.a = s.ReadU32();
        t.v[0] = s.ReadU32();
        t.v[1] = s.ReadU32();
        t.v[2] = s.ReadU32();
        t.b = s.ReadU32();
    }
    vertices.resize(vertCount > 0 ? static_cast<u32>(vertCount) : 0);
    for (NiPoint3& p : vertices) LoadPoint3(s, p);
    LoadBspTree(s, tree, 0);
    const s32 groupCount = static_cast<s32>(s.ReadU32());
    groups.resize(groupCount > 0 ? static_cast<u32>(groupCount) : 0);
    for (Group& g : groups) {                         // orig: one `new MeshRecord(0)` per group, SetAt(g)
        const s32 n = static_cast<s32>(s.ReadU32());
        for (s32 i = 0; i < n; ++i) {
            g.a.push_back(s.ReadU16());
            g.b.push_back(s.ReadU16());
        }
        auto rec = std::make_unique<MeshRecord>();
        rec->triIndex = g.a;
        rec->triFlags = g.b;
        meshes.push_back(std::move(rec));
    }
    s.ReadMultipleLinkIDs(links);
}

// orig: 0x0014EA10 - sphere around a point set: centre of the axis-aligned box, radius to the farthest point.
NiBound BoundOfPoints(const std::vector<NiPoint3>& pts) {
    NiBound b;                                        // no points: ZERO centre, radius 0
    if (pts.empty()) return b;
    f32 minX = pts[0].x, minY = pts[0].y, minZ = pts[0].z, maxX = minX, maxY = minY, maxZ = minZ;
    for (const NiPoint3& p : pts) {
        if (p.x < minX) minX = p.x;
        if (p.y < minY) minY = p.y;
        if (p.z < minZ) minZ = p.z;
        if (maxX < p.x) maxX = p.x;
        if (maxY < p.y) maxY = p.y;
        if (maxZ < p.z) maxZ = p.z;
    }
    b.center.x = static_cast<f32>((double(maxX) + minX) * 0.5);
    b.center.y = static_cast<f32>((double(maxY) + minY) * 0.5);
    b.center.z = static_cast<f32>((double(maxZ) + minZ) * 0.5);
    // The running maximum is kept as a float (each new maximum is reloaded from its float store); the comparison
    // uses the unrounded squared distance.
    f32 best = 0.0f;
    for (const NiPoint3& p : pts) {
        const double dx = double(p.x) - b.center.x, dy = double(p.y) - b.center.y, dz = double(p.z) - b.center.z;
        const double d2 = (dz * dz + dy * dy) + dx * dx;
        if (d2 > best) best = static_cast<f32>(d2);
    }
    b.radius = static_cast<f32>(std::sqrt(double(best)));
    return b;
}

void NiPortal::LoadBinary(NiStream& s) {              // orig: 0x001435A0
    NiAVObject::LoadBinary(s);
    planeCount = s.ReadU16();
    vertices.resize(s.ReadU16());
    for (NiPoint3& p : vertices) LoadPoint3(s, p);
    worldVertices.assign(vertices.size(), NiPoint3{});
    modelBound = BoundOfPoints(vertices);
    adjoiner = s.ReadLinkID();
}

void NiUDSFileObject::LoadBinary(NiStream& s) {       // orig: 0x0011FD70
    NiAVObject::LoadBinary(s);
    data.resize(s.ReadU32());
    if (!data.empty()) s.Read(data.data(), data.size());
}

// ---- properties -----------------------------------------------------------------------------------------------------
void NiProperty::LoadBinary(NiStream& s) {            // orig: 0x000FDF40
    NiObjectNET::LoadBinary(s);
    flags = s.ReadU16();
}

void NiAlphaProperty::LoadBinary(NiStream& s) {       // orig: 0x00104560
    NiProperty::LoadBinary(s);
    alphaTestRef = s.ReadU8();
}

void NiMaterialProperty::LoadBinary(NiStream& s) {    // orig: 0x00114DF0
    NiProperty::LoadBinary(s);
    LoadColor(s, ambient);
    LoadColor(s, diffuse);
    LoadColor(s, specular);
    LoadColor(s, emissive);
    shininess = s.ReadF32();
    alpha = s.ReadF32();
}

void NiStencilProperty::LoadBinary(NiStream& s) {     // orig: 0x00119730
    NiProperty::LoadBinary(s);
    enabled = s.ReadBool();
    function = s.ReadU32();
    reference = s.ReadU32();
    mask = s.ReadU32();
    failAction = s.ReadU32();
    zFailAction = s.ReadU32();
    passAction = s.ReadU32();
    drawMode = s.ReadU32();
}

void NiVertexColorProperty::LoadBinary(NiStream& s) { // orig: 0x001016D0
    NiProperty::LoadBinary(s);
    sourceVertexMode = s.ReadU32();
    lightingMode = s.ReadU32();
}

void NiZBufferProperty::LoadBinary(NiStream& s) {     // orig: 0x0011D1A0
    NiProperty::LoadBinary(s);
    if (s.FileVersion() >= NiVersion(4, 1, 0, 5)) testFunction = s.ReadU32();
}

// orig: 0x0010A440 (Map) / 0x0010A4D0 (BumpMap)
static void LoadTextureMap(NiStream& s, NiTexturingProperty::Map& m, bool bump) {
    m.isBumpMap = bump;
    m.texture = s.ReadLinkID();
    m.clampMode = s.ReadU32();
    m.filterMode = s.ReadU32();
    m.textureIndex = s.ReadU32();
    m.ps2L = s.ReadU16();
    m.ps2K = s.ReadU16();
    if (s.FileVersion() < NiVersion(4, 1, 0, 16)) s.ReadU16();   // obsolete, discarded
    if (bump) {
        m.lumaScale = s.ReadF32();
        m.lumaOffset = s.ReadF32();
        for (f32& f : m.bumpMatrix) f = s.ReadF32();
    }
}

void NiTexturingProperty::LoadBinary(NiStream& s) {   // orig: 0x0010A540
    NiProperty::LoadBinary(s);
    applyMode = s.ReadU32();
    const u32 count = s.ReadU32();
    const u32 v = s.FileVersion();
    present.assign(count, false);
    maps.clear();
    for (u32 i = 0; i < count && !s.Truncated(); ++i) {
        bool has;
        if (v < NiVersion(4, 1, 0, 0)) {
            has = s.ReadU32() != 0;
        } else {
            has = s.ReadBool();
        }
        present[i] = has;
        u32 slot = i;
        bool bump = false;
        if (v < NiVersion(3, 3, 0, 13)) {
            if (i >= 5) slot = i + 1;                 // pre-3.3.0.13 files have no bump slot
        } else if (i == 5) {
            bump = true;
        }
        if (maps.size() <= slot) maps.resize(slot + 1);
        if (has) LoadTextureMap(s, maps[slot], bump);
    }
    hasMultiTexture = false;
    for (u32 i = 1; i < count && i < present.size(); ++i) {
        if (present[i]) {
            hasMultiTexture = true;
            break;
        }
    }
}

void NiSourceTexture::LoadBinary(NiStream& s) {       // orig: 0x00118AC0
    NiObjectNET::LoadBinary(s);
    external = s.ReadBool();
    if (external) {
        // The original also builds the platform path (0x001516A0) and shares one object per file name through the
        // stream's name map (0x00108A20 / 0x001089F0 / 0x00108500).
        s.ReadString(fileName);
    } else {
        hasPixelData = s.ReadBool();
        if (hasPixelData) pixelData = s.ReadLinkID();
    }
    pixelLayout = s.ReadU32();
    mipMapFormat = s.ReadU32();
    alphaFormat = s.ReadU32();
    if (s.FileVersion() < NiVersion(3, 3, 0, 12) && pixelLayout == 4) pixelLayout = 5;
    isStatic = s.ReadU8();                            // read into a local by the original and not kept
    if (s.FileVersion() >= NiVersion(4, 1, 0, 13)) unknown3C = s.ReadU32();
    // 0x00118BE1: an earlier texture of the same key (0x00118330) takes this one's place in the stream (it is linked
    // with this one's link IDs, as the original reads them from the stream's list); else this joins the list
    if (NiSourceTexture* shared = FindSharedTexture(*this)) {
        shared->extraDataLink = extraDataLink, shared->controllerLink = controllerLink;
        shared->pixelData = pixelData;                // (consumed only: its pixel data is set)
        s.ReplaceLastObject(shared);                  // 0x00108500 (this one released)
        return;
    }
    AddSharedTexture(this);                           // 0x00118A80
}

NiSourceTexture::~NiSourceTexture() { RemoveShared(SharedTextures(), this); }
NiPixelData::~NiPixelData() { RemoveShared(SharedPixelData(), this); }

void SetTextureSharing(bool on) {                     // orig: 0x00118930
    g_shareTextures = on;
    if (!on) SharedTextures().clear();                // 0x001183D0
}

void SetPixelDataSharing(bool on) {                   // orig: 0x001119D0
    g_sharePixelData = on;
    if (!on) SharedPixelData().clear();               // 0x00111730
}

// ---- extra data -----------------------------------------------------------------------------------------------------
void NiExtraData::LoadBinary(NiStream& s) {           // orig: 0x001098A0
    NiObject::LoadBinary(s);
    next = s.ReadLinkID();
    size = s.ReadU32();
    // Only an object whose RTTI is exactly NiExtraData reads the payload; subclasses read their own fields.
    if (size && ClassName() == NiExtraData::kClassName) {
        data.resize(size);
        s.Read(data.data(), size);
    }
}

void NiStringExtraData::LoadBinary(NiStream& s) {     // orig: 0x00119AE0
    NiExtraData::LoadBinary(s);
    s.ReadString(string);
}

void NiTextKeyExtraData::LoadBinary(NiStream& s) {    // orig: 0x0012A2C0
    NiExtraData::LoadBinary(s);
    keys.resize(s.ReadU32());
    for (TextKey& k : keys) {                         // orig: 0x00153820
        k.time = s.ReadF32();
        s.ReadString(k.text);
    }
}

// ---- controllers ----------------------------------------------------------------------------------------------------
void NiTimeController::LoadBinary(NiStream& s) {      // orig: 0x001414F0
    NiObject::LoadBinary(s);
    nextController = s.ReadLinkID();
    flags = s.ReadU16();
    frequency = s.ReadF32();
    phase = s.ReadF32();
    loKeyTime = s.ReadF32();
    hiKeyTime = s.ReadF32();
    target = s.ReadLinkID();
}

void NiSingleDataController::LoadBinary(NiStream& s) {   // orig: 0x00152C50
    NiTimeController::LoadBinary(s);
    data = s.ReadLinkID();
}

void NiFlipController::LoadBinary(NiStream& s) {      // orig: 0x00125320
    NiTimeController::LoadBinary(s);
    affectedMap = s.ReadU32();
    flipStartTime = s.ReadF32();
    flipDelta = s.ReadF32();
    s.ReadMultipleLinkIDs(sources);
}

void NiUVController::LoadBinary(NiStream& s) {        // orig: 0x00129000
    NiTimeController::LoadBinary(s);
    textureSet = s.ReadU16();
    data = s.ReadLinkID();
}

void NiGeomMorpherController::LoadBinary(NiStream& s) {   // orig: 0x00125CF0
    NiTimeController::LoadBinary(s);
    data = s.ReadLinkID();
    if (s.FileVersion() >= NiVersion(4, 0, 0, 2)) alwaysUpdate = s.ReadBool();
}

// ---- controller data ------------------------------------------------------------------------------------------------
void NiFloatData::LoadBinary(NiStream& s) {           // orig: 0x001203C0
    NiObject::LoadBinary(s);
    LoadKeyGroup(s, keys, LoadFloatKeys);
}

void NiPosData::LoadBinary(NiStream& s) {             // orig: 0x001318C0
    NiObject::LoadBinary(s);
    LoadKeyGroup(s, keys, LoadPosKeys);
}

void NiVisData::LoadBinary(NiStream& s) {             // orig: 0x001297F0 -> 0x001294A0 (no key type on disk)
    NiObject::LoadBinary(s);
    keys.resize(s.ReadU32());
    for (BoolKey& k : keys) {
        k.time = s.ReadF32();
        k.value = s.ReadBool();
    }
}

void NiUVData::LoadBinary(NiStream& s) {              // orig: 0x00128090
    NiObject::LoadBinary(s);
    for (FloatKeys& g : groups) LoadKeyGroup(s, g, LoadFloatKeys);
}

void NiKeyframeData::LoadBinary(NiStream& s) {        // orig: 0x00126BA0
    NiObject::LoadBinary(s);
    LoadKeyGroup(s, rotations, LoadRotKeys);
    LoadKeyGroup(s, positions, LoadPosKeys);
    LoadKeyGroup(s, scales, LoadFloatKeys);
}

void NiMorphData::LoadBinary(NiStream& s) {           // orig: 0x00125740
    NiObject::LoadBinary(s);
    const u32 morphCount = s.ReadU32();
    vertexCount = s.ReadU32();
    relativeTargets = s.ReadU8() == 1;
    morphs.clear();
    morphs.resize(morphCount);
    for (Morph& m : morphs) {
        const u32 keyCount = s.ReadU32();
        const auto type = static_cast<KeyType>(s.ReadU32());
        if (keyCount) LoadFloatKeys(s, type, keyCount, m.keys);
        m.vertices.resize(vertexCount);
        for (NiPoint3& p : m.vertices) LoadPoint3(s, p);
        if (s.Failed() || s.Truncated()) break;
    }
}

void NiSequence::LoadBinary(NiStream& s) {            // orig: 0x0012D130
    NiObject::LoadBinary(s);
    s.ReadString(name);
    textKeyReference = s.ReadU32();
    textKeys = s.ReadLinkID();
    entries.resize(s.ReadU32());
    for (Entry& e : entries) {
        s.ReadString(e.name);
        e.controller = s.ReadLinkID();
    }
}

void NiSequence::LinkObject(NiStream& s) {            // orig: 0x0012D220
    textKeysObj = s.ResolveLink(textKeys);
    for (Entry& e : entries) e.controllerObj = s.ResolveLink(e.controller);
}

// ---- geometry -------------------------------------------------------------------------------------------------------
// Arrays below 4.1.0.0 are flagged by a u32 (non-zero = present), from 4.1.0.0 on by a byte.
static bool ReadPresenceFlag(NiStream& s) {
    return s.FileVersion() < NiVersion(4, 1, 0, 0) ? s.ReadU32() != 0 : s.ReadBool();
}

void NiGeometryData::LoadBinary(NiStream& s) {        // orig: 0x00143DC0
    NiObject::LoadBinary(s);
    vertexCount = s.ReadU16();
    vertices.clear();
    normals.clear();
    colors.clear();
    if (ReadPresenceFlag(s)) {
        vertices.resize(vertexCount);
        for (NiPoint3& p : vertices) LoadPoint3(s, p);
    }
    if (ReadPresenceFlag(s)) {
        normals.resize(vertexCount);
        for (NiPoint3& p : normals) LoadPoint3(s, p);
    }
    LoadBound(s, bound);
    if (ReadPresenceFlag(s)) {
        colors.resize(vertexCount);
        for (NiColorA& c : colors) LoadColorA(s, c);
    }
    textureSets = s.ReadU16();
    if (s.FileVersion() < NiVersion(4, 1, 0, 0)) s.ReadU32();   // obsolete texture-coordinate flag
    uvs.resize(static_cast<size_t>(textureSets) * vertexCount);
    for (NiPoint2& uv : uvs) {                        // orig: 0x0014F0C0
        uv.u = s.ReadF32();
        uv.v = s.ReadF32();
    }
}

void NiTriBasedGeomData::LoadBinary(NiStream& s) {    // orig: 0x00151EE0
    NiGeometryData::LoadBinary(s);
    triangleCount = s.ReadU16();
}

void NiTriShapeData::LoadBinary(NiStream& s) {        // orig: 0x00116130
    NiTriBasedGeomData::LoadBinary(s);
    const u32 indexCount = s.ReadU32();
    triangleList.resize(indexCount);
    if (indexCount) s.Read(triangleList.data(), indexCount * sizeof(u16));
    sharedNormals.clear();
    sharedNormals.resize(s.ReadU16());
    for (auto& group : sharedNormals) {
        group.resize(s.ReadU16());
        if (!group.empty()) s.Read(group.data(), group.size() * sizeof(u16));
    }
}

void NiSkinInstance::LoadBinary(NiStream& s) {        // orig: 0x0010DD30
    NiObject::LoadBinary(s);
    skinDataLink = s.ReadLinkID();
    rootParentLink = s.ReadLinkID();
    s.ReadMultipleLinkIDs(boneLinks);
}

void NiSkinData::LoadBinary(NiStream& s) {            // orig: 0x0010DB30
    NiObject::LoadBinary(s);
    LoadTransform(s, rootParentToSkin);
    const u32 boneCount = s.ReadU32();
    const u32 v = s.FileVersion();
    if (v >= NiVersion(4, 0, 0, 1)) skinPartitionLink = s.ReadLinkID();
    hasVertexWeights = v > NiVersion(4, 1, 0, 13) ? s.ReadBool() : true;
    bones.clear();
    bones.resize(boneCount);
    for (Bone& b : bones) {
        LoadTransform(s, b.skinToBone);
        LoadBound(s, b.bound);
        b.vertexCount = s.ReadU16();
        if (hasVertexWeights) {
            b.weights.resize(b.vertexCount);
            for (Weight& w : b.weights) {
                w.vertex = s.ReadU16();
                w.weight = s.ReadF32();
            }
        }
        if (s.Failed() || s.Truncated()) break;
    }
    if (v == NiVersion(4, 1, 0, 9)) s.ReadU8();      // discarded
}

void NiSkinPartition::LoadBinary(NiStream& s) {       // orig: 0x0010D0F0
    NiObject::LoadBinary(s);
    partitions.clear();
    partitions.resize(s.ReadU32());
    for (Partition& p : partitions) {                 // orig: 0x0010C770
        p.vertexCount = s.ReadU16();
        p.triangleCount = s.ReadU16();
        p.boneCount = s.ReadU16();
        p.stripCount = s.ReadU16();
        p.bonesPerVertex = s.ReadU16();
        p.bones.resize(p.boneCount);
        if (p.boneCount) s.Read(p.bones.data(), p.bones.size() * sizeof(u16));
        p.vertexMap.resize(p.vertexCount);
        if (p.vertexCount) s.Read(p.vertexMap.data(), p.vertexMap.size() * sizeof(u16));
        const size_t weightCount = static_cast<size_t>(p.vertexCount) * p.bonesPerVertex;
        p.weights.resize(weightCount);
        if (weightCount) s.Read(p.weights.data(), weightCount * sizeof(f32));
        size_t indexCount;
        if (p.stripCount == 0) {
            indexCount = static_cast<size_t>(p.triangleCount) * 3;
        } else {
            p.stripLengths.resize(p.stripCount);
            s.Read(p.stripLengths.data(), p.stripLengths.size() * sizeof(u16));
            indexCount = 0;
            for (u16 len : p.stripLengths) indexCount += len;
        }
        p.indices.resize(indexCount);
        if (indexCount) s.Read(p.indices.data(), indexCount * sizeof(u16));
        p.hasBonePalette = s.ReadBool();
        if (p.hasBonePalette) {
            p.bonePalette.resize(weightCount);
            if (weightCount) s.Read(p.bonePalette.data(), weightCount);
        }
        if (s.Failed() || s.Truncated()) break;
    }
}

void NiPixelData::LoadBinary(NiStream& s) {           // orig: 0x00111C40
    NiObject::LoadBinary(s);
    format.format = s.ReadU32();                      // orig: 0x00140E70 (8 x u32)
    for (u32& m : format.channelMasks) m = s.ReadU32();
    format.bitsPerPixel = s.ReadU32();
    format.fastCompare[0] = s.ReadU32();
    format.fastCompare[1] = s.ReadU32();
    palette = s.ReadLinkID();
    const u32 mipCount = s.ReadU32();
    bytesPerPixel = s.ReadU32();
    mips.resize(mipCount);
    for (MipLevel& m : mips) {
        m.width = s.ReadU32();
        m.height = s.ReadU32();
        m.offset = s.ReadU32();
    }
    const u32 total = s.ReadU32();                    // stored as the offset entry past the last level
    pixels.resize(total);
    if (total) s.Read(pixels.data(), total);
    if (s.FileVersion() >= NiVersion(4, 2, 1, 1)) shareKey = s.ReadU32();   // +8
    // 0x00111D61: with the sharing on, an earlier pixel data of the same nonzero key takes this one's place in the
    // stream (with this one's palette link); else this joins the list unless it is in it
    auto& list = SharedPixelData();
    if (g_sharePixelData && shareKey)
        for (NiPixelData* p : list)
            if (p && p->shareKey && p->shareKey == shareKey) {
                p->palette = palette;
                s.ReplaceLastObject(p);               // 0x00108500
                return;
            }
    for (NiPixelData* p : list)
        if (p == this) return;
    list.push_back(this);                             // 0x000C5360
}

void NiOutlineData::LoadBinary(NiStream& s) {         // orig: 0x0013C280
    NiObject::LoadBinary(s);
    const s32 edgeCount = static_cast<s32>(s.ReadU32());
    const s32 faceCount = static_cast<s32>(s.ReadU32());
    const s32 listCount = static_cast<s32>(s.ReadU32());
    edges.clear();
    faces.clear();
    lists.clear();
    if (edgeCount == 0 || faceCount == 0 || listCount == 0) return;
    for (s32 i = 0; i < edgeCount; ++i) {
        Edge e;
        e.c = s.ReadU16();
        e.d = s.ReadU16();
        e.a = s.ReadU16();
        e.b = s.ReadU16();
        edges.push_back(e);
    }
    for (s32 i = 0; i < faceCount; ++i) {
        Face f;
        f.v[4] = s.ReadU16();
        f.v[5] = s.ReadU16();
        f.v[6] = s.ReadU16();
        f.v[0] = s.ReadU16();
        f.v[1] = s.ReadU16();
        f.v[2] = s.ReadU16();
        f.v[3] = s.ReadU16();
        faces.push_back(f);
    }
    std::vector<u32> counts;
    for (s32 i = 0; i < listCount; ++i) counts.push_back(s.ReadU32());
    for (u32 n : counts) {                            // one contiguous u16 array on disk, split by the counts
        std::vector<u16> list(n);
        if (n) s.Read(list.data(), n * sizeof(u16));
        lists.push_back(std::move(list));
    }
}

// =====================================================================================================================
void RegisterNiLoaders() {
#define REG(cls) NiStream::RegisterLoader(#cls, &cls::CreateObject)
    REG(NiNode); REG(NiBillboardNode); REG(NiSwitchNode); REG(NiFltAnimationNode); REG(NiLODNode);
    REG(NiBSPNode); REG(NiWall); REG(NiRoom); REG(NiRoomGroup); REG(NiOutline);
    REG(NiTriShape); REG(NiAmbientLight); REG(NiDirectionalLight); REG(NiPointLight); REG(NiTextureEffect);
    REG(NiCamera); REG(NiBsp); REG(NiPortal); REG(NiUDSNode); REG(NiUDSFileObject); REG(NiUDSObject);
    REG(NiAlphaProperty); REG(NiMaterialProperty); REG(NiShadeProperty); REG(NiSpecularProperty);
    REG(NiStencilProperty); REG(NiVertexColorProperty); REG(NiZBufferProperty); REG(NiTexturingProperty);
    REG(NiSourceTexture);
    REG(NiExtraData); REG(NiStringExtraData); REG(NiTextKeyExtraData);
    REG(NiKeyframeController); REG(NiAlphaController); REG(NiMaterialColorController); REG(NiVisController);
    REG(NiFlipController); REG(NiUVController); REG(NiGeomMorpherController);
    REG(NiFloatData); REG(NiPosData); REG(NiVisData); REG(NiUVData); REG(NiKeyframeData); REG(NiMorphData);
    REG(NiSequence);
    REG(NiTriShapeData); REG(NiSkinInstance); REG(NiSkinData); REG(NiSkinPartition); REG(NiPixelData);
    REG(NiOutlineData);
#undef REG
}

}  // namespace fut::ni
