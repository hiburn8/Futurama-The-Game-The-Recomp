// NetImmerse 4.2 object classes: stream loading (LoadBinary, vtbl+0x0C) for every class the shipped Xbox NIF/UCF
// files use (52 concrete classes, decomp/nif_classes.tsv), plus their base classes. Each LoadBinary is decompiled
// from default.xbe and cites its original address; the class-match names in Ghidra are `<Class>::vf03`.
//
// Members keep the engine's data but not its memory layout. Link IDs (u32, kNullLink = none) are kept raw until the
// LinkObject pass is ported. The original's .text also registers loaders for 50 more classes that no shipped file
// uses (particles, LOD bones, ...); they are not ported yet and fail with NiStreamError::UnknownClass.
#pragma once

#include "engine/ni/NiMath.h"
#include "engine/ni/NiStream.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fut::ni {

struct NiPoint2 { f32 u = 0, v = 0; };
struct NiColor { f32 r = 0, g = 0, b = 0; };
struct NiColorA { f32 r = 0, g = 0, b = 0, a = 0; };
struct NiQuaternion { f32 w = 1, x = 0, y = 0, z = 0; };

void LoadPoint3(NiStream& s, NiPoint3& p);         // orig: 0x001408B0
void LoadMatrix3(NiStream& s, NiMatrix3& m);       // orig: 0x00104130 (9 floats = M row-major; see NiMath.h)
void LoadBound(NiStream& s, NiBound& b);           // orig: 0x00141E60 (also used for planes)

// ---- animation keys (orig: per-type loader tables at 0x0030F0C0 float / 0x0030F0D4 pos / 0x0030F0E8 rot,
//      filled by static initializers; index = key type) ----------------------------------------------------------
enum class KeyType : u32 { None = 0, Linear = 1, Quadratic = 2, Tcb = 3, Euler = 4 };
struct Tcb { f32 tension = 0, continuity = 0, bias = 0; };
// Quadratic (Bezier) keys store the incoming tangent first (+0x08 / +0x10) and the outgoing one second.
struct FloatKey { f32 time = 0, value = 0, inTangent = 0, outTangent = 0; Tcb tcb; };
struct PosKey { f32 time = 0; NiPoint3 value, inTangent, outTangent; Tcb tcb; };
struct RotKey { f32 time = 0; NiQuaternion value; Tcb tcb; };
struct FloatKeys { KeyType type = KeyType::None; std::vector<FloatKey> keys; };
struct PosKeys { KeyType type = KeyType::None; std::vector<PosKey> keys; };
struct EulerRotKey {                                // orig: 0x00124930 (in-memory 0x20 bytes)
    u8 axisOrder = 0;                                // low byte of the first u32 (index into 0x002D7A38)
    std::array<FloatKeys, 3> axes;
    mutable std::array<u32, 3> lastIndex{};          // orig: u16 at +0x0A per axis, cached by the interpolator
};
struct RotKeys { KeyType type = KeyType::None; std::vector<RotKey> keys; std::vector<EulerRotKey> euler; };
struct BoolKey { f32 time = 0; bool value = false; };

void LoadFloatKeys(NiStream& s, KeyType type, u32 count, FloatKeys& out);
void LoadPosKeys(NiStream& s, KeyType type, u32 count, PosKeys& out);
void LoadRotKeys(NiStream& s, KeyType type, u32 count, RotKeys& out);

// =====================================================================================================================
class NiNode;
class NiExtraData;
class NiTimeController;
class NiProperty;
class NiGeometryData;
class NiSkinInstance;
class NiSkinData;
class NiSkinPartition;
class NiDynamicEffect;
class NiPixelData;
class NiScreenTexture;
class NiScreenPolygon;
struct NiGeometryRendererData;

class NiObject : public NiRefObject {
public:
    static constexpr std::string_view kClassName = "NiObject";
    virtual std::string_view ClassName() const = 0;
    virtual void LoadBinary(NiStream& s);            // orig: 0x000FE020 (registers the object with the stream)
    // orig: vtbl+0x10, called for every object in load order after all objects are loaded. NiObject's is empty
    // (0x000AEDC0). Classes whose links are not resolved yet keep the raw IDs read by LoadBinary.
    virtual void LinkObject(NiStream& s) { (void)s; }
    // orig: vtbl+0x28 (PostLinkObject), called for every object in load order after the link pass (0x00108E78);
    // only NiSourceTexture's does anything (0x00118200).
    virtual void PostLinkObject(NiStream& s) { (void)s; }
};

#define FUT_NI_CLASS(name)                                                                  \
    static constexpr std::string_view kClassName = #name;                                   \
    std::string_view ClassName() const override { return kClassName; }                      \
    static NiObject* CreateObject() { return new name; }

class NiObjectNET : public NiObject {
public:
    FUT_NI_CLASS(NiObjectNET)
    void LoadBinary(NiStream& s) override;           // orig: 0x000FE360
    void LinkObject(NiStream& s) override;           // orig: 0x000FE5F0
    std::string name;
    u32 extraDataLink = kNullLink;
    u32 controllerLink = kNullLink;
    NiPointer<NiExtraData> extraData;                // orig: this+0x0C (head of the extra-data chain)
    NiPointer<NiTimeController> controller;          // orig: this+0x10
    // orig: 0x000FE480 PrependController (c->next = head; head = c) and 0x000FE4C0 RemoveController (unlinks c from
    // the chain and clears its next; no-op when absent).
    void PrependController(NiTimeController* c);
    void RemoveController(NiTimeController* c);
};

// Scene-graph flag bits (NiAVObject flags, this+0xB0).
struct NiCullPass;                                   // NiCull.h
class NiRenderer;                                    // NiRenderer.h
class NiCamera;

enum : u16 {
    kNiAVHidden = 0x0001,
    kNiAVVisualBound = 0x0100,                       // set by the update passes when the world bound is meaningful
    kNiAVSwitchUpdateOnlyActive = 0x0400,            // NiSwitchNode: update only the active child
    kNiPortalActive = 0x0100,                        // NiPortal: active (cleared by FsPortalDisable)
    kNiPortalInUse = 0x0200,                         // NiPortal: being traversed by the cull pass
    kNiFltPingPong = 0x0800,                         // NiFltAnimationNode: ping-pong cycle
};

class NiLight;
class NiTextureEffect;

// orig: NiPropertyState (0x38 bytes, vtable 0x002AAC64, ctor 0x0014D800): one property per type (NiProperty vf2C):
// +8 + 4 * type. A geometry's +0xC0, the renderer's +0xC while it is drawn (renderer_lighting_port.md 1.1).
class NiPropertyState : public NiRefObject {
public:
    enum Type : u32 {
        kAlpha = 0, kDither = 1, kFog = 2, kMaterial = 3, kRendererSpecific = 4, kShade = 5, kSpecular = 6,
        kStencil = 7, kTexturing = 8, kVertexColor = 9, kWireframe = 10, kZBuffer = 11, kCount = 12
    };
    NiPropertyState();                               // orig: 0x0014D800 -> 0x0014D630 (the class defaults)
    explicit NiPropertyState(const NiPropertyState& o);   // orig: 0x0014D520
    ~NiPropertyState() override;
    // The class default instances (static initialisers 0x001043D0 / 0x00112D40 / 0x00104710 / 0x00114CE0 /
    // 0x00117830 / 0x00117940 / 0x00118C80 / 0x00119510 / 0x0010A330 / 0x001014E0 / 0x0011CDA0 / 0x0011D020:
    // DAT_002F91E8, 002F97F8, 002F9214, 002F98C4, 002F9A44, 002F9A60, 002F9AD4, 002F9B08, 002F94CC, 002F9154,
    // 002F9C84, 002F9CA8).
    static NiProperty* Default(u32 type);
    NiPointer<NiProperty> slot[kCount];              // +0x08..
};

// orig: NiDynamicEffectState (0x20 bytes, vtable 0x002A6840): the effects acting on a geometry (+0xC4).
class NiDynamicEffectState : public NiRefObject {
public:
    NiDynamicEffectState();
    explicit NiDynamicEffectState(const NiDynamicEffectState& o);   // orig: 0x0014D2F0
    ~NiDynamicEffectState() override;
    // orig: 0x0014D480: a light (vfA0 < 4) into +0xC, a texture effect by its type +0x168 (0 / 1 into the lists
    // +0x10 / +0x14, 2 / 3 replace +0x18 / +0x1C), +8 = 1 for texture effects. The lists are sorted by the effect id
    // (+0xC4, ascending; an id already present is not added again: 0x0014D3F0).
    void Add(NiDynamicEffect* effect);
    // orig: 0x0014D380: the same env / fog map pointers and element-wise equal lists +0xC / +0x10 / +0x14.
    bool Equals(const NiDynamicEffectState* o) const;
    bool anyTextureEffect = false;                   // +0x08
    // (the original's lists and pointers hold no references: 0x0014D3F0 / 0x0014D480)
    std::vector<NiRawPointer<NiLight>> lights;       // +0x0C
    std::vector<NiRawPointer<NiTextureEffect>> projectedLights;    // +0x10
    std::vector<NiRawPointer<NiTextureEffect>> projectedShadows;   // +0x14
    NiRawPointer<NiTextureEffect> environmentMap;    // +0x18
    NiRawPointer<NiTextureEffect> fogMap;            // +0x1C
};

class NiAVObject : public NiObjectNET {
public:
    FUT_NI_CLASS(NiAVObject)
    void LoadBinary(NiStream& s) override;           // orig: 0x00102380
    void LinkObject(NiStream& s) override;           // orig: 0x00102580
    // The render data pushed down the scene graph (renderer_lighting_port.md 1; NiRenderState.cpp).
    // orig: 0x00101EA0: the ancestors' merged properties (0x00105050; a default state at the root), then vf74.
    void UpdateProperties();
    // orig: 0x00102190: the ancestors' merged effects (0x00106220; none at the root), then vf78; the geometry
    // classes' sharing cache DAT_0030EF10 is released before and after.
    void UpdateEffects();
    // vtbl+0x74 (slot 29) / +0x78 (slot 30): the push-downs (NiAVObject's: nothing, L).
    virtual void PushProperties(NiPropertyState* state) { (void)state; }
    virtual void PushEffects(NiDynamicEffectState* state) { (void)state; }
    // orig: 0x00101FE0 (out, parentState, copy): parentState when there are no own properties, else parentState
    // itself (copy false) or a copy (0x0014D520) with each own property in its type's slot (list order).
    NiPointer<NiPropertyState> MergeProperties(NiPropertyState* parentState, bool copy) const;
    u16 flags = 0x80;                                // orig: ctor 0x00102990 (selective-update bits 0x70 off, 0x80 on)
    NiTransform local;                               // orig: this+0x30
    NiPoint3 velocity;
    std::vector<u32> propertyLinks;
    std::vector<NiPointer<NiProperty>> properties;   // orig: list at this+0x18 (order as linked, see LinkObject)
    NiNode* parent = nullptr;                        // orig: this+0x14 (not reference counted)
    NiTransform world;                               // orig: this+0x70
    NiBound worldBound;                              // orig: this+0x20

    // orig: NiAVObject::Update(time) = UpdateDownwardPass then the upward bound pass.
    virtual void UpdateDownwardPass(f32 time, bool updateControllers);   // vtbl+0x64 (slot 25), orig: 0x00101E10
    virtual bool IsVisualObject() const { return true; }                 // vtbl+0x70 (slot 28), orig: 0x000A32A0
    virtual void UpdateWorldBound() {}                                   // vtbl+0x98 (slot 38), orig: 0x000B0420
    virtual void UpdateWorldData();                                      // vtbl+0x9C (slot 39), orig: 0x00143770
    void UpdateControllers(f32 time, bool properties);                   // orig: 0x00101C60 (NiAnimation.cpp)
    // vtbl+0x58 (slot 22), orig: 0x001029C0. Recomputes flags 0x10 selective update, 0x20 selective transforms,
    // 0x40 property controllers, 0x80 rigid from the controllers present.
    virtual void SetSelectiveUpdateFlags(bool& selectiveUpdate, bool selectiveTransforms, bool& rigid);
    bool HasTransformController() const;                                 // orig: 0x00101930
    bool HasPropertyController() const;                                  // orig: 0x00101F80
    // The selective passes (what the flags above select): vtbl+0x68 (slot 26) / +0x6C (slot 27); the leaves share
    // 0x00101E40: UpdateControllers(t, flags & 0x40), then with flags & 0x20 UpdateWorldData and UpdateWorldBound.
    virtual void UpdateSelectedDownwardPass(f32 time);
    virtual void UpdateRigidDownwardPass(f32 time) { NiAVObject::UpdateSelectedDownwardPass(time); }
    // orig: 0x00101900: vf68(t), then the parent's vtbl+0xB0 (0x000B0420 for every node class: nothing).
    void UpdateSelected(f32 time) { UpdateSelectedDownwardPass(time); }

    // The cull pass (NiCull.h; docs/analysis/cull_counter_port.md): frames since the object was last drawn, written
    // only by the pass (no constructor writes it: a test hook gives new game nodes the original's leftover byte).
    u8 framesNotDrawn = 0;                           // orig: this+0xB2
    virtual void OnNotDrawn();                       // orig: vtbl+0x50 (slot 20) 0x00101A00: ++ saturating
    virtual void OnVisible(NiCullPass& pass) { (void)pass; }   // orig: vtbl+0x84 (slot 33): display (draw)
};

class NiNode : public NiAVObject {
public:
    FUT_NI_CLASS(NiNode)
    NiNode() { flags |= kNiAVVisualBound; }          // orig: 0x00105C10
    ~NiNode() override;                              // orig: 0x00106740 (detaches its effects and its children)
    void LoadBinary(NiStream& s) override;           // orig: 0x00104B80
    void LinkObject(NiStream& s) override;           // orig: 0x001064B0
    void UpdateDownwardPass(f32 time, bool updateControllers) override;  // orig: 0x00104D40
    bool IsVisualObject() const override { return (flags & kNiAVVisualBound) != 0; }   // orig: 0x00067E50
    void UpdateWorldBound() override;                // orig: 0x00104FD0
    void SetSelectiveUpdateFlags(bool& selectiveUpdate, bool selectiveTransforms, bool& rigid) override;   // 0x001053C0
    void UpdateSelectedDownwardPass(f32 time) override;   // orig: 0x00104E00
    void UpdateRigidDownwardPass(f32 time) override;      // orig: 0x00104F00

    // orig: vtbl+0xAC (0x00105F50) SetAt and vtbl+0xA4 (0x00105E60) DetachChild. The array keeps empty slots;
    // `size` is its high-water mark (NiTArray m_usSize), `childCount` the number of non-null entries.
    void SetAt(u32 index, NiAVObject* child);
    NiPointer<NiAVObject> DetachChild(NiAVObject* child);
    // orig: vtbl+0xA0 (0x00105C90): detaches the child from its parent, then puts it into the first empty slot
    // (firstAvailable, 0x00105B60) or appends it.
    void AttachChild(NiAVObject* child, bool firstAvailable);
    // orig: 0x001055B0: each effect leaves the list and loses this node (0x001419F0).
    void DetachAllEffects();
    // orig: 0x00105530: an effect not in the list yet is prepended and gets this node as affected node.
    void AttachEffect(NiDynamicEffect* effect);
    // orig: 0x00105570: an effect in the list leaves it and loses this node (NiDynamicEffect::DetachAffectedNode).
    void DetachEffect(NiDynamicEffect* effect);
    void PushProperties(NiPropertyState* state) override;   // orig: 0x00105280
    void PushEffects(NiDynamicEffectState* state) override;  // orig: 0x00105320
    // orig: 0x00105150 (out, parentState, copy): parentState without own effects, else parentState itself (copy
    // false), a copy (0x0014D2F0) or a new empty state (no parent state), each effect added by 0x0014D480.
    NiPointer<NiDynamicEffectState> MergeEffects(NiDynamicEffectState* parentState, bool copy) const;
    const std::vector<NiPointer<NiAVObject>>& Children() const { return children_; }
    u32 ChildCount() const { return childCount_; }
    void OnNotDrawn() override;                      // orig: 0x00105B10 (the children once, while the own count is 0)
    void OnVisible(NiCullPass& pass) override;       // orig: 0x001054E0

    std::vector<u32> childLinks;
    std::vector<u32> effectLinks;
    // orig: list at this+0xD8 (NiTList: no references; an effect's destructor takes itself out, 0x00141A30)
    std::vector<NiRawPointer<NiDynamicEffect>> effects;

protected:
    void MergeChildBounds();                         // shared tail of 0x00104D40 / 0x00104FD0
    std::vector<NiPointer<NiAVObject>> children_;    // orig: NiTArray at this+0xC0 (data +0xC4, size +0xCC)
    u32 childCount_ = 0;                             // orig: this+0xD0
};

class NiBillboardNode : public NiNode {
public:
    FUT_NI_CLASS(NiBillboardNode)
    void LoadBinary(NiStream& s) override;           // orig: 0x00110050
    void UpdateDownwardPass(f32 time, bool updateControllers) override;  // orig: 0x0010FE90
    void UpdateWorldBound() override;                // orig: 0x0010FFF0
    void UpdateSelectedDownwardPass(f32 time) override;   // orig: 0x0010FED0
    void UpdateRigidDownwardPass(f32 time) override;      // orig: 0x0010FF00
    void OnVisible(NiCullPass& pass) override;       // orig: 0x001109E0
    void FaceCamera(const NiCamera& cam);            // orig: 0x001103F0
    f32 lastUpdateTime = 0;                          // orig: this+0xE0
    bool lastUpdateControllers = true;               // orig: this+0xE4 (1 in CreateObject 0x00110350 / clone 0x001102B0)
};

class NiSwitchNode : public NiNode {
public:
    FUT_NI_CLASS(NiSwitchNode)
    void LoadBinary(NiStream& s) override;           // orig: 0x00113A90
    void UpdateDownwardPass(f32 time, bool updateControllers) override;  // orig: 0x00113270
    void UpdateWorldBound() override;                // orig: 0x00113640
    void UpdateSelectedDownwardPass(f32 time) override;   // orig: 0x00113360 (also NiLODNode / NiFltAnimationNode)
    void UpdateRigidDownwardPass(f32 time) override;      // orig: 0x00113470
    s32 index = 0;                                   // orig: this+0xE0 (active child, < 0 = none)
    f32 lastUpdateTime = 0;                          // orig: this+0xE4
    u32 revision = 0;                                // orig: this+0xE8
    std::vector<u32> childRevision;                  // orig: this+0xEC (per child: the revision of its last update)
    bool lastUpdateControllers = false;              // orig: this+0x104
    void OnVisible(NiCullPass& pass) override;       // orig: 0x00113570 (only the active child)
};

class NiFltAnimationNode : public NiSwitchNode {
public:
    FUT_NI_CLASS(NiFltAnimationNode)
    void LoadBinary(NiStream& s) override;           // orig: 0x00114010
    void OnVisible(NiCullPass& pass) override;       // orig: 0x00113E50 (0x00113D90 picks the frame)
    f32 period = 0;                                  // orig: this+0x110
};

class NiLODNode : public NiSwitchNode {
public:
    FUT_NI_CLASS(NiLODNode)
    void LoadBinary(NiStream& s) override;           // orig: 0x00114800
    void UpdateWorldData() override;                 // orig: 0x001141D0
    void OnVisible(NiCullPass& pass) override;       // orig: 0x001142E0 (the range by camera distance)
    struct Range { f32 nearDist = 0, farDist = 0, worldNearSq = 0, worldFarSq = 0; };   // orig: 16 bytes each
    NiPoint3 lodCenter;                              // orig: this+0x110
    NiPoint3 worldLodCenter;                         // orig: this+0x11C
    std::vector<Range> ranges;                       // orig: this+0x128
};

class NiBSPNode : public NiNode {
public:
    FUT_NI_CLASS(NiBSPNode)
    void LoadBinary(NiStream& s) override;           // orig: 0x0011D360
    void OnVisible(NiCullPass& pass) override;       // orig: 0x00112390 (NOT PORTED: the camera-side order)
    NiPlane plane;                                   // orig: this+0xE0
};

// Game class (no PS2 symbol); its loader is NiBSPNode's (0x0011D360).
class NiWall : public NiBSPNode {
public:
    FUT_NI_CLASS(NiWall)
    void UpdateWorldData() override;                 // orig: 0x0011D460
    void OnVisible(NiCullPass& pass) override;       // orig: 0x001054E0 (NiNode's, not NiBSPNode's)
    NiPlane worldPlane;                              // orig: this+0xF0
};

class NiRoom : public NiNode {
public:
    FUT_NI_CLASS(NiRoom)
    void LoadBinary(NiStream& s) override;           // orig: 0x0011D840
    // orig: 0x0011EC60: the four link lists into the lists +0xE0, +0xF8, +0x128 and +0x140 (tail inserts, nulls
    // included).
    void LinkObject(NiStream& s) override;
    // orig: 0x0011EB90: a fixture not yet in the list is appended, then the child array is rebuilt.
    void AttachFixture(NiAVObject* obj);
    // orig: 0x0011DCC0: every child detached, then re-attached in list order +0x128, +0xE0, fixtures.
    void UpdateChildArray();
    // orig: 0x0011EEA0: the first occurrence of obj leaves the fixture list (order kept) and the children are rebuilt;
    // nothing when obj is not a fixture.
    void DetachFixture(NiAVObject* obj);
    std::array<std::vector<u32>, 4> linkLists;
    // orig: 0x0011DE40: false when the point is behind any wall of list +0xE0 (world plane: n.p - c < 0), true
    // otherwise (also for an empty list or a NaN distance).
    bool ContainsPoint(const NiPoint3& p) const;
    void OnVisible(NiCullPass& pass) override;       // orig: 0x0011DB40 (in-portals off while displayed)
    std::vector<NiPointer<NiAVObject>> list0;        // orig: +0xE0, the walls (ContainsPoint)
    std::vector<NiPointer<NiAVObject>> list1;        // orig: +0xF8 (not part of the child array)
    std::vector<NiPointer<NiAVObject>> list2;        // orig: +0x128
    std::vector<NiPointer<NiAVObject>> fixtures;     // orig: +0x140
};

class NiRoomGroup : public NiNode {
public:
    FUT_NI_CLASS(NiRoomGroup)
    void LoadBinary(NiStream& s) override;           // orig: 0x0011F190
    // orig: 0x0011F7C0: the shell (+0xE0) and the room list (+0xE4). Files of version 4.1.0.8 and later (all shipped
    // ones) insert each room at the head (0x0011F4D0), older ones at the tail (0x0011F590); null links are skipped.
    void LinkObject(NiStream& s) override;
    // orig: 0x0011F1D0: the first room of the list that contains the point, or nullptr.
    NiRoom* FindRoomContaining(const NiPoint3& p) const;
    void OnVisible(NiCullPass& pass) override;       // orig: 0x0011F6D0 (only the camera's room)
    NiPointer<NiRoom> lastRoom;                      // orig: +0xEC (the room hint of the cull pass)
    u32 shell = kNullLink;
    std::vector<u32> rooms;
    NiPointer<NiObject> shellObject;                 // orig: +0xE0
    std::vector<NiPointer<NiRoom>> roomList;         // orig: +0xE4
};

class NiOutlineData;
class NiGeometry;

// orig: NiOutline (0x110 bytes, ctor 0x0013D100: NiNode, +0xE0 / +0xE8 = 0, the geometry array +0xEC (0x000FD3B0(1,
// 1)), +0x104 = 0, +0xE4 = 1.0): a node whose tri-based geometry children get a silhouette outline (screen polygons
// of the camera, renderer_lighting_port.md 8.2; NiOutline.cpp).
class NiOutline : public NiNode {
public:
    FUT_NI_CLASS(NiOutline)
    NiOutline();
    ~NiOutline() override;
    void LoadBinary(NiStream& s) override;           // orig: 0x0013C850
    void LinkObject(NiStream& s) override;           // orig: 0x0013CF90 (NiNode's, then the data link)
    // orig: vf33 0x0013E8D0: with edges in the data, the outline mode DAT_002D7B58 on and the width +0xE4 >= 1e-4f:
    // the geometry list (0x0013D8D0), the extra data 0x0013CA40, the other children culled first, then each listed
    // geometry with the vertex scratch (the display writes its world vertices there), then the silhouette
    // 0x00153F20. Otherwise NiNode's display.
    void OnVisible(NiCullPass& pass) override;
    // orig: 0x0013D8D0 (once, while the list is empty): the tri-based geometry children (the first childCount slots)
    // that are unblended or carry the alpha flag 0x2000 (set here from a "NiData_OutlineNoSort" string extra data
    // of the geometry or an ancestor) into the list.
    void CollectGeometry();
    // orig: 0x0013DAA0 (the outline conversion 0x0013E5E0): the list, a new outline data +0xE8 from the listed
    // geometry's world vertices (welded within 1e-3, sorted by y with the CRT qsort), triangles, edges and the
    // vertex-edge lists.
    void BuildData();
    // orig: 0x0013CA40 (once per data, while +0x28 is set): the per-geometry colours +0x18, the z priorities +0x104
    // and the interior modes from the geometry's "NiData_Outline*" / "NiData_Zprio" string extra data, then the edge
    // flags 0x10 / 0x20.
    void ApplyExtraData();
    // orig: 0x0013C8A0: the edge (b, a) whose second face is free and whose first face lacks the third vertex
    // DAT_002FA4D4, else a new edge (a, b).
    s32 FindOrAddEdge(s32 a, s32 b);
    // orig: 0x00153F20 (thiscall, the camera): the facing test per triangle, the silhouette edges (table
    // DAT_002FA47C), their vertices projected (0x00153DC0), the edge chains (0x00153EA0) and their quads into the
    // outline polygons (0x0013D440).
    void DrawSilhouette(NiCamera& camera);
    // orig: 0x0013D440 (thiscall, 7 arguments): `count` vertices of the current outline polygon DAT_002FA4E8 (a new
    // or pooled polygon when it is full), coloured (r, g, b, 1) but for the last two (0); the vertex / texture
    // coordinate (and colour) slots of the run.
    void AddPolygonRun(s32 count, u32 r, u32 g, u32 b, NiPoint3** vertices, NiPoint2** uvs, NiColorA** colors);
    u32 outlineData = kNullLink;
    NiPointer<NiOutlineData> data;                   // +0xE8 (from the file; or made by 0x0013DAA0)
    f32 width = 1.0f;                                // +0xE4
    NiCamera* camera = nullptr;                      // +0xE0 (the camera of the last display; refcounted there)
    std::vector<NiPointer<NiGeometry>> geometry;     // +0xF0 (count +0xF8)
    std::vector<u32> zPriority;                      // +0x104 (one per listed geometry; "NiData_Zprio" reads a %f)
};

class NiGeometry : public NiAVObject {
public:
    FUT_NI_CLASS(NiGeometry)
    void LoadBinary(NiStream& s) override;           // orig: 0x00144760
    void LinkObject(NiStream& s) override;           // orig: 0x00144E80
    void UpdateWorldData() override;                 // orig: 0x00144740
    void UpdateWorldBound() override;                // orig: 0x00144BA0
    void SetSelectiveUpdateFlags(bool& selectiveUpdate, bool selectiveTransforms, bool& rigid) override;   // 0x00144D80
    void PushProperties(NiPropertyState* state) override;   // orig: 0x00144BF0 (+0xC0 = the merged state)
    void PushEffects(NiDynamicEffectState* state) override;  // orig: 0x00144CB0 (the sharing cache DAT_0030EF10)
    NiPointer<NiPropertyState> propertyState;        // orig: this+0xC0
    NiPointer<NiDynamicEffectState> effectState;     // orig: this+0xC4 (null: no effect)
    bool effectsPushed = false;                      // (the port's: PushEffects ran once)
    u32 modelDataLink = kNullLink;
    u32 skinInstanceLink = kNullLink;
    NiPointer<NiGeometryData> modelData;             // orig: this+0xC8
    NiPointer<NiSkinInstance> skinInstance;          // orig: this+0xCC
    bool worldDataChanged = false;                   // orig: this+0xD8 / +0xD9, set on every world-data update
    bool worldDataChanged2 = false;
};

class NiTriShape : public NiGeometry {
public:
    FUT_NI_CLASS(NiTriShape)
    void OnVisible(NiCullPass& pass) override;       // orig: 0x00116760 (display: the renderer's draw)
};

// orig: NiTriStrips (vtbl 0x002A89B8, ctor 0x0011C1F0 = new(0x44) NiTriStripsData + 0x001454D0, 0xE0 bytes; clone
// 0x0011C480 = NiTriShape's 0x001166F0 with the strips' vtable). Built by code only (the laser beam, Laser.cpp); the
// disc's files hold none (the railgun's RailLaser builds them too). Its display 0x0011C4F0 is NiTriShape's 0x00116760
// instruction for instruction but the renderer slot (vfCC instead of vfC0: both 0x0014C580) and the frame statistics'
// counts (the data's +0x34 / +0x08 instead of vfB4 / vf30: not modelled).
class NiTriStrips : public NiGeometry {
public:
    FUT_NI_CLASS(NiTriStrips)
    void OnVisible(NiCullPass& pass) override;       // orig: 0x0011C4F0 (display: the renderer's draw)
};

class NiDynamicEffect : public NiAVObject {
public:
    FUT_NI_CLASS(NiDynamicEffect)
    void LoadBinary(NiStream& s) override;           // orig: 0x00141B10
    bool IsVisualObject() const override { return false; }   // orig: 0x000AEDD0
    // orig: 0x001419B0: a node not in the list yet is prepended and gets this effect (NiNode::AttachEffect).
    void AttachAffectedNode(NiNode* node);
    // orig: 0x001419F0: a node in the list leaves it and loses this effect (NiNode::DetachEffect).
    void DetachAffectedNode(NiNode* node);
    void ForgetAffectedNode(const NiNode* node);     // (a node being destroyed)
    void DetachAllAffectedNodes();                   // orig: 0x00141A30 (each node's effect list loses it)
    NiDynamicEffect();                               // orig: 0x00141910 (the id +0xC4 from the allocator)
    ~NiDynamicEffect() override;                     // orig: 0x00141B80 (the id back to the free list)
    // orig: vtbl+0xA0: 0 ambient, 1 directional, 2 point, 3 spot light; a texture effect above 3 (L: 4).
    virtual u32 EffectType() const { return 4; }
    bool switchOn = true;                            // orig: this+0xC0 (ctor 0x00141910)
    u32 effectId = 0;                                // orig: this+0xC4 (the D3D light index; EffectIds below)
    u32 revision = 1;                                // orig: this+0xCC, bumped when the effect moves
    std::vector<NiNode*> affectedNodes;              // orig: list at this+0xD0 (not referenced)
};

// The renderer's data of a light (orig: light +0x108; NiXBoxLightManager's D3DLIGHT8 copy), owned by the light.
struct NiLightRendererData {
    virtual ~NiLightRendererData() = default;
};

class NiLight : public NiDynamicEffect {
public:
    FUT_NI_CLASS(NiLight)
    void LoadBinary(NiStream& s) override;           // orig: 0x001457B0
    f32 dimmer = 1;                                  // orig: +0xE0 (ctor 0x00145660: dimmer and colours 1.0)
    NiColor ambient{1, 1, 1}, diffuse{1, 1, 1}, specular{1, 1, 1};   // orig: +0xE4 / +0xF0 / +0xFC
    std::unique_ptr<NiLightRendererData> rendererData;   // orig: +0x108
};

class NiAmbientLight : public NiLight {
public:
    FUT_NI_CLASS(NiAmbientLight)
    u32 EffectType() const override { return 0; }
};

class NiDirectionalLight : public NiLight {
public:
    FUT_NI_CLASS(NiDirectionalLight)
    u32 EffectType() const override { return 1; }
    void UpdateWorldData() override;                 // orig: 0x00112C80
    NiPoint3 worldDirection{1, 0, 0};                // orig: this+0x110 (first column of the world rotation; ctor 0x00112B40)
};

class NiPointLight : public NiLight {
public:
    FUT_NI_CLASS(NiPointLight)
    u32 EffectType() const override { return 2; }
    void LoadBinary(NiStream& s) override;           // orig: 0x00117490
    void UpdateWorldData() override;                 // orig: 0x00117340
    f32 constantAtten = 0, linearAtten = 1, quadraticAtten = 0;   // orig: +0x110.. (defaults: LMTarget 0x000C0550)
};

class NiTextureEffect : public NiDynamicEffect {
public:
    FUT_NI_CLASS(NiTextureEffect)
    NiTextureEffect() { clippingPlane.normal = {1, 0, 0}; }   // orig: ctor 0x00119F00
    void LoadBinary(NiStream& s) override;           // orig: 0x0011A5A0
    // orig: 0x0011A1F0: NiAVObject's (0x0011FCC0 -> 0x00102580), then +0x15C = the source texture's link (the
    // character files' environment maps: the effect holds the file's NiSourceTexture until it is released).
    void LinkObject(NiStream& s) override;
    void UpdateWorldData() override;                 // orig: 0x0011A520: NiAVObject's, then UpdateProjection
    // orig: 0x0011A290: the world projection (sphere maps; the other coordinate types: the model projection times the
    // transposed world rotation over the world scale, and its translate) and the world clipping plane; revision + 1.
    void UpdateProjection();
    // orig: 0x000344C0 (the game's SetModelPlane): the model clipping plane +0x174, then UpdateProjection.
    void SetModelPlane(const NiPlane& p) {
        clippingPlane = p;
        UpdateProjection();
    }
    NiMatrix3 modelProjection;                       // orig: +0xE0
    NiPoint3 modelProjectionTranslate;               // orig: +0x110
    NiMatrix3 worldProjection;                       // orig: +0x120
    NiPoint3 worldProjectionTranslate;               // orig: +0x150
    u32 textureFiltering = 0, textureClamping = 3, textureType = 0, coordGenType = 0;   // orig: +0x160..+0x16C
    u32 sourceTexture = kNullLink;
    NiPointer<NiObject> sourceTextureObj;            // orig: +0x15C (the file's link, or the game's set-up)
    bool clippingPlaneEnabled = false;               // orig: +0x170
    NiPlane clippingPlane;                           // orig: +0x174
    NiPlane worldClippingPlane{{1, 0, 0}, 0};        // orig: +0x184
    u16 ps2L = 0, ps2K = 0xFFB5;                     // orig: +0x194 / +0x196
};

class NiCamera : public NiAVObject {
public:
    FUT_NI_CLASS(NiCamera)
    // orig: 0x001009D0 ends with UpdateWorldData (the axes and planes of the identity transform) and the bound's
    // centre = the world translate.
    NiCamera() {
        UpdateWorldData();
        worldBound.center = world.translate;
    }
    void LoadBinary(NiStream& s) override;           // orig: 0x00100680
    bool IsVisualObject() const override { return false; }   // orig: 0x000AEDD0
    void UpdateWorldBound() override;                // orig: 0x000FF410 (centre = world position)
    // orig: 0x000FFE30: the world transform, the world axes and the six frustum planes, then (tail jump) the
    // projection matrix 0x000FF430 (NiCull.cpp).
    void UpdateWorldData() override;
    void UpdateProjection();                         // orig: 0x000FF430
    // orig: 0x00011960 (the game's SetFrustum: the near clamp, then the six values).
    void SetFrustum(const f32 f[6]);
    NiPoint3 worldDir, worldUp, worldRight;          // orig: +0x110 / +0x11C / +0x128 (world rotate columns 0 / 1 / 2)
    NiPlane frustumPlanes[6];                        // orig: the ctor's planes (near, far, left, right, top, bottom)
    f32 frustum[6] = {-0.5f, 0.5f, 0.5f, -0.5f, 1.0f, 2.0f};   // left, right, top, bottom, near, far (ctor 0x001009D0)
    f32 port[4] = {0.0f, 1.0f, 1.0f, 0.0f};          // left, right, top, bottom (ctor 0x001009D0)
    // orig: +0xC4..+0x100 (0x000FF430): the world-to-clip rows (x, y, z, w; 4 floats each: the right / up / direction
    // axes with the frustum's scales and offsets, w = the direction and its offset), +0x108 / +0x10C = 2 / (r - l),
    // 2 / (t - b). The outline's projection 0x00153DC0 reads it.
    f32 projection[16] = {};
    f32 projectionScale[2] = {};
    f32 lodAdjust = 1;
    u32 scene = kNullLink;
    // orig: +0x15C, the scene a Click (0x00100CE0) culls, where the game sets it at run time. The port's Click sites
    // pass their scene to VisitForCulling directly; this copy (not owning: the scene holds the camera) is read only
    // by TextObject::Click 0x00098DB0, whose scene-mode text objects cull their camera's scene, not the text's node
    // (null: the text's node, its own camera's scene).
    NiAVObject* sceneRoot = nullptr;
    std::vector<u32> screenPolygons;
    std::vector<u32> screenTextures;                 // version >= 4.1.0.4
    // orig: +0x190 the screen textures (NiTPrimitiveArray: +0x194 data, +0x19C size; null slots are refilled), drawn
    // by Click after the scene (renderer vfDC). The files' links above are not resolved into it (none in the game's).
    std::vector<NiPointer<NiScreenTexture>> screenTextureList;
    // orig: 0x0002BE40 (the game's AddScreenTexture): 0x00100230 (the screen polygons +0x160 compacted), then the
    // texture into the first empty slot of +0x190 (0x0002BD90).
    void AddScreenTexture(NiScreenTexture* texture);
    // orig: 0x001002D0: the texture's first slot of +0x190 cleared (SetAt 0x0012B7F0: the size stays).
    void RemoveScreenTexture(NiScreenTexture* texture);
    // orig: +0x160 the screen polygons (NiTArray: +0x164 data, +0x16C the size = one past the highest slot ever set,
    // +0x170 the non-null count; null slots stay inside the size). Click draws them after the scene (renderer vfD8).
    std::vector<NiPointer<NiScreenPolygon>> screenPolygonList;
    // orig: the game's AttachScreenPoly 0x00019460 (0x000193B0: the first null slot, else appended; then the
    // polygon's +0x1C |= 1). Returns the slot.
    u32 AttachScreenPolygon(NiScreenPolygon* polygon);
    // orig: 0x000FEB10 (thiscall(const NiPoint3*, f32* x, f32* y), RET 0xC): the world point projected with the rows
    // +0xC4 (x), +0xD4 (y) and +0xF4 (w) into the port; false (x / y partly written) behind the camera (w at most
    // 1e-5) or outside [-1, 1].
    bool WorldPtToScreenPt(const NiPoint3& p, f32& x, f32& y) const;
    // orig: 0x00100230: the non-null polygons moved down in order (SetAt: the size is not reduced).
    void CompactScreenPolygons();
    // orig: 0x00100FC0: the polygon's first slot cleared, then CompactScreenPolygons.
    void DetachScreenPolygon(NiScreenPolygon* polygon);
    // orig: +0x17C the objects displayed once after the scene (count +0x184; Click 0x00100E6A: vf84 with the camera,
    // then removed): no writer is ported (L).
    // orig: +0xC0 one more polygon Click draws last (0x00100F74): no writer is ported (L).
    NiRenderer* renderer = nullptr;                  // orig: +0x1A8 (App->vf14(); null: the cull pass alone)
};

// Game class: precomputed BSP collision mesh.
// The stage collision geometry (docs/analysis/collision.md): a world-space triangle soup with a kd-tree, plus the
// collision mesh groups. Every linked NiBsp is in the collision world's list (NiCollision.h).
class NiBsp : public NiAVObject {
public:
    FUT_NI_CLASS(NiBsp)
    ~NiBsp() override;                               // orig: 0x000FD740 (leaves the collision list)
    void LoadBinary(NiStream& s) override;           // orig: 0x000FD9A0
    // orig: vf04 0x000FCF40: each group's NiTriShape, then the head of the collision list.
    void LinkObject(NiStream& s) override;
    // On disk: a, v0, v1, v2, b (orig fields +0xC, +0, +4, +8, +0x10). `a` = 1 / |(v1-v0) x (v2-v0)| as float bits,
    // `b` = flags (byte 0 against the exclusion mask, byte 1 = the hit surface).
    struct Triangle { u32 v[3] = {}; u32 a = 0, b = 0; };
    struct MeshRecord;
    struct TreeNode {                                // orig: 0x000FD540 (recursive)
        u32 type = 0;                                // 3 = leaf, else the split axis 0..2
        NiPoint3 boundMin, boundMax;
        u32 value = 0;                               // inner node: the split (float bits, +0x24); leaf: count
        std::vector<u32> indices;                    // leaf only (orig: 0x000FC070): byte offsets (0x14 * triangle)
        std::vector<TreeNode> children;              // inner node: 2
        MeshRecord* records = nullptr;               // orig: +0x1C, the records inserted here (head; link `next`)
    };
    // A collision mesh record (docs/analysis/collision_meshes_port.md 1.1): a NiTriShape's triangles that the
    // per-frame refresh (NiCollision.h RefreshAllBsps) keeps in the kd-tree by their world sphere.
    struct MeshRecord {                              // orig: 0x50 bytes, ctor 0x000FBDB0, dtor 0x000FBE80
        MeshRecord() = default;
        MeshRecord(const MeshRecord&) = delete;
        MeshRecord& operator=(const MeshRecord&) = delete;
        ~MeshRecord();                               // orig: 0x000FBE80 (leaves its node's list first)
        NiPointer<NiTriShape> geom;                  // +0x00 (file groups: set by LinkObject)
        MeshRecord* next = nullptr;                  // +0x04 (the node's list)
        TreeNode* node = nullptr;                    // +0x08 (null: not in the tree)
        NiPoint3 center;                             // +0x0C the cached world sphere (ZERO)
        f32 radius = 0;                              // +0x18
        u32 type = 0;                                // +0x1C 1 solid, 2 camera-transparent (skipped by flags bit 2)
        std::vector<u16> triIndex;                   // +0x20 (data +0x24, size +0x2C): triangles of the shape
        std::vector<u16> triFlags;                   // +0x38 (data +0x3C, size +0x44): per-triangle flags
    };
    struct Group { std::vector<u16> a, b; };
    NiPoint3 boundMin, boundMax;
    std::vector<Triangle> triangles;
    std::vector<NiPoint3> vertices;
    TreeNode tree;
    std::vector<Group> groups;                       // (as loaded) a = triangle indices into the group's shape, b = flags
    std::vector<u32> links;
    NiBsp* nextInWorld = nullptr;                    // orig: +0xF0
    // orig: +0x10C NiTPrimitiveArray<MeshRecord*> (data +0x110, size +0x118): one record per file group (LoadBinary;
    // geometry from LinkObject), then AddCollisionMeshes appends. Its order is the order of every later step.
    // (Declared after `tree`: the records are destroyed first and leave their nodes, as ~NiBsp 0x000FD7CF does.)
    std::vector<std::unique_ptr<MeshRecord>> meshes;
    bool typesDirty = true;                          // orig: +0x124 (the ctor's 1; cleared by RecomputeMeshTypes)
    // The original's heap address of the triangle array (+0xC4 -> [0]): the sphere query's sort key
    // (collision_meshes_port.md 7.5). LinkObject gives a default below every listed NiBsp (the high region grows
    // downward, L); the play test injects the oracle's value (COLLBSP).
    u32 origTriAddr = 0;
};

class NiPortal : public NiAVObject {
public:
    FUT_NI_CLASS(NiPortal)
    void LoadBinary(NiStream& s) override;           // orig: 0x001435A0
    void UpdateWorldData() override;                 // orig: 0x00142C60
    void UpdateWorldBound() override;                // orig: 0x00142B60
    u16 planeCount = 0;
    std::vector<NiPoint3> vertices;                  // orig: this+0xC4 (count u16 at this+0xC0)
    std::vector<NiPoint3> worldVertices;             // orig: this+0xE0
    NiBound modelBound;                              // orig: this+0xD0, computed from the vertices (0x0014EA10)
    u32 adjoiner = kNullLink;
    void LinkObject(NiStream& s) override;           // (the adjoiner link)
    void OnVisible(NiCullPass& pass) override;       // orig: 0x001433C0 (planes pushed, the adjoiner displayed)
    NiAVObject* adjoinerObject = nullptr;            // orig: +0xC8 (the room behind the portal; not reference counted)
    std::vector<NiPlane> worldPlanes;                // orig: +0xE4 (planeCount entries, rebuilt by every traversal)
};

// Game classes from the level format ("UDS").
class NiUDSNode : public NiAVObject {                // loader: NiAVObject's (0x00102380 via 0x00145F10)
public:
    FUT_NI_CLASS(NiUDSNode)
};

class NiUDSFileObject : public NiAVObject {
public:
    FUT_NI_CLASS(NiUDSFileObject)
    void LoadBinary(NiStream& s) override;           // orig: 0x0011FD70
    std::vector<u8> data;
};

class NiUDSObject : public NiObjectNET {             // loader: NiObjectNET's (0x000FE360 via 0x00151FD0)
public:
    FUT_NI_CLASS(NiUDSObject)
};

// ---- properties -----------------------------------------------------------------------------------------------------
// A renderer's data of a property (orig: NiMaterialProperty +0x58), owned by the property.
struct NiPropertyRendererData {
    virtual ~NiPropertyRendererData() = default;
};

class NiProperty : public NiObjectNET {
public:
    FUT_NI_CLASS(NiProperty)
    void LoadBinary(NiStream& s) override;           // orig: 0x000FDF40
    u16 flags = 0;
};

class NiAlphaProperty : public NiProperty {
public:
    FUT_NI_CLASS(NiAlphaProperty)
    NiAlphaProperty() { flags = 0xEC; }              // orig: ctor 0x00013D80 (flags 0xEC: SRCALPHA / INVSRCALPHA, off)
    void LoadBinary(NiStream& s) override;           // orig: 0x00104560
    u8 alphaTestRef = 0;                             // orig: +0x18
};

class NiMaterialProperty : public NiProperty {
public:
    FUT_NI_CLASS(NiMaterialProperty)
    void LoadBinary(NiStream& s) override;           // orig: 0x00114DF0
    // (ctor 0x00114B10: ambient = diffuse = 0.5, specular = emissive = 0, shininess 4, alpha 1)
    NiColor ambient{0.5f, 0.5f, 0.5f}, diffuse{0.5f, 0.5f, 0.5f}, specular, emissive;
    f32 shininess = 4.0f, alpha = 1;
    u32 revision = 1;                                // orig: this+0x54, bumped by the alpha / colour controllers
    std::unique_ptr<NiPropertyRendererData> rendererData;   // orig: this+0x58 (the renderer's D3DMATERIAL8 copy)
};

class NiShadeProperty : public NiProperty { public: FUT_NI_CLASS(NiShadeProperty) };
// (Not read from files by the port yet; the game's shared fog-off property, 0x002D96D8, is one.)
class NiFogProperty : public NiProperty {
public:
    FUT_NI_CLASS(NiFogProperty)
    f32 depth = 1.0f;                                // orig: +0x18
    NiColor color{0, 0, 0};                          // orig: +0x1C
};
class NiSpecularProperty : public NiProperty { public: FUT_NI_CLASS(NiSpecularProperty) };

class NiStencilProperty : public NiProperty {
public:
    FUT_NI_CLASS(NiStencilProperty)
    void LoadBinary(NiStream& s) override;           // orig: 0x00119730
    // orig: the constructor 0x00026B60 (the stream factory 0x00119540 and the game's inline copies alike): +0x18 = 0,
    // +0x1C = 4, +0x20 = 0, +0x24 = -1, +0x28 = 0, +0x2C = 0, +0x30 = 3, +0x34 = 0
    bool enabled = false;                            // +0x18
    u32 function = 4, reference = 0, mask = 0xFFFFFFFFu, failAction = 0, zFailAction = 0, passAction = 3;   // +0x1C..
    u32 drawMode = 0;                                // +0x34 (3: both faces, CULLMODE none)
};

class NiVertexColorProperty : public NiProperty {
public:
    FUT_NI_CLASS(NiVertexColorProperty)
    void LoadBinary(NiStream& s) override;           // orig: 0x001016D0
    u32 sourceVertexMode = 0, lightingMode = 0;
};

class NiZBufferProperty : public NiProperty {
public:
    FUT_NI_CLASS(NiZBufferProperty)
    void LoadBinary(NiStream& s) override;           // orig: 0x0011D1A0
    u32 testFunction = 0;                            // version >= 4.1.0.5
};

class NiTexturingProperty : public NiProperty {
public:
    FUT_NI_CLASS(NiTexturingProperty)
    void LoadBinary(NiStream& s) override;           // orig: 0x0010A540
    void LinkObject(NiStream& s) override;           // orig: 0x0010A240
    struct Map {                                     // orig: 0x0010A440 (vtable 0x00293F38 slot 1)
        u32 texture = kNullLink;
        u32 clampMode = 0, filterMode = 0, textureIndex = 0;
        u16 ps2L = 0, ps2K = 0;
        bool isBumpMap = false;                      // slot 5 (orig: 0x0010A4D0, vtable 0x002A6C60)
        NiPointer<NiObject> textureObj;              // resolved by LinkObject (a NiSourceTexture)
        f32 lumaScale = 1, lumaOffset = 0;
        f32 bumpMatrix[4] = {0.5f, 0, 0, 0.5f};
    };
    u32 applyMode = 0;
    std::vector<bool> present;                       // one per slot read from the file
    std::vector<Map> maps;                           // index = slot; meaningful where present[slot]
    bool hasMultiTexture = false;                    // orig: this+0x1C, any slot > 0 in use
    bool HasMap(u32 slot) const { return slot < present.size() && present[slot]; }
    // orig: 0x0010A040 SetMap (the replaced map object is released).
    void SetMap(u32 slot, const Map& map);
    // orig: 0x0001D610: the base map's texture (a new map first when there is none: Map::Map 0x0001D570, clamp mode 3,
    // filter 0, set 0, ps2 0 / 0xFFB5).
    void SetBaseTexture(NiObject* texture);
};

class NiTexture : public NiObjectNET {
public:
    FUT_NI_CLASS(NiTexture)
    ~NiTexture() override;                           // orig: 0x00118490 (every renderer's PurgeTexture)
};

// orig: NiRenderedTexture (NiRenderedTexture::Create 0x00140070: a render target, never loaded from files; the
// renderer's data made by its vf108, NiXBoxRenderer::CreateRenderedTextureData).
class NiRenderedTexture : public NiTexture {
public:
    FUT_NI_CLASS(NiRenderedTexture)
    u32 width = 128, height = 128;
};
// orig: NiRenderedTexture::Create 0x00140070 (width, height, renderer, format preferences): sizes that are powers of
// two and a renderer that makes the texture's data (slot 66); null otherwise (also under the play oracle's fake
// renderer, whose slot fails). The preferences are not used by the renderer (NiXBoxRenderer 0x00147410).
NiPointer<NiRenderedTexture> CreateRenderedTexture(u32 width, u32 height, NiRenderer* renderer);
class NiSourceTexture : public NiTexture {
public:
    FUT_NI_CLASS(NiSourceTexture)
    void LoadBinary(NiStream& s) override;           // orig: 0x00118AC0
    void LinkObject(NiStream& s) override;           // orig: 0x001181A0 (vtbl+0x10)
    // orig: 0x00118200: with DAT_002D79A0 (static 1) the renderers' precache 0x00118050 (NiRenderer.h).
    void PostLinkObject(NiStream& s) override;
    bool external = false;
    std::string fileName;                            // external textures (the original also de-duplicates by name)
    bool hasPixelData = false;
    u32 pixelData = kNullLink;
    NiPointer<NiPixelData> pixelDataObj;             // orig: this+0x34, resolved by LinkObject
    u32 pixelLayout = 0, mipMapFormat = 0, alphaFormat = 0;
    u8 isStatic = 0;
    u32 unknown3C = 0;                               // orig: +0x3C, version >= 4.1.0.13 (the sharing key)
    ~NiSourceTexture() override;                     // (leaves the shared list, L: a UB guard for its raw pointers)
};
// orig: 0x00118930 (DAT_002F9AA0; set at boot 0x000A283F and again at the
// level's end 0x000C8A20): while on, NiSourceTexture::LoadBinary 0x00118AC0 shares an unnamed texture with an
// earlier one of the same key +0x3C (0x00118330: the list DAT_002D79B4; the stream's object replaced, 0x00108500);
// turning it off clears the list (0x001183D0). Every texture not shared joins the list (0x00118A80).
void SetTextureSharing(bool on);
// orig: 0x001119D0 (DAT_002F973C, the same two places): NiPixelData::LoadBinary 0x00111C40 shares pixel data with an
// earlier one of the same key +8 (the list DAT_002D78F4); off clears the list (0x00111730).
void SetPixelDataSharing(bool on);

// orig: NiScreenTexture (0x20 bytes, ctor 0x0010B2E0, vtable 0x002A6E00): screen-space rectangles of one texture,
// drawn by the renderer's vfDC (NiXBoxRenderer 0x0014C600) in pixels of the current target. Built by code (the
// loading screen, the level title); its stream loader (0x0010B850) is not ported (no file holds one).
class NiScreenTexture : public NiObject {
public:
    FUT_NI_CLASS(NiScreenTexture)
    // orig: 0x0010B2E0(texture): the rect array (growth 1, 0x0010B150), a new NiTexturingProperty (+0x14: apply mode
    // 0, SetBaseTexture(texture) 0x0001D610, the base map's clamp mode 0 0x000506A0), +0x18 = 0, +0x1C = 0.
    explicit NiScreenTexture(NiObject* texture = nullptr);
    ~NiScreenTexture() override;                     // orig: 0x0010B030 (0x00140280: every renderer's purge)
    struct Rect {                                    // 0x1C bytes
        s16 top = 0, left = 0;                       // +0x00 / +0x02 (pixels; wrapped into the target by the draw)
        u16 width = 0, height = 0;                   // +0x04 / +0x06
        u16 texTop = 0, texLeft = 0;                 // +0x08 / +0x0A (texels)
        NiColorA color;                              // +0x0C (r, g, b, a)
    };
    // orig: 0x0010B6E0 (top, left, width, height, texTop, texLeft, colour) -> 0x0010B660 (appended, capacity doubled):
    // the new rect's index.
    u32 AddRect(s16 top, s16 left, u16 width, u16 height, u16 texTop, u16 texLeft, const NiColorA& color);
    std::vector<Rect> rects;                         // +0x08 (data, capacity, size: +0x10 read as the u16 count)
    NiPointer<NiTexturingProperty> texturing;        // +0x14
    u16 dirty = 0;                                   // +0x18 (bit 1 the texture, bit 2 the colours; 0xF all)
    std::unique_ptr<NiGeometryRendererData> rendererData;   // +0x1C (the renderer's: its unshared group)
};

// orig: NiScreenPolygon (0x24 bytes, ctor 0x00106DE0 (count, vertices, texture coordinates, colours), vtable
// 0x002A6924): a polygon in screen space (x, y in [0, 1] of the target, y up; z a depth fraction), drawn by the
// renderer's vfD8 (NiXBoxRenderer 0x0014B680) with its own property state. Built by code (the HUD, the camera fade);
// no file holds one (its stream loader is not ported).
class NiScreenPolygon : public NiObject {
public:
    FUT_NI_CLASS(NiScreenPolygon)
    NiScreenPolygon() = default;
    // orig: 0x00106DE0: the arrays copied (new: 12 / 8 / 16 bytes per vertex; null: none), a new NiPropertyState
    // (0x0014D800) at +8, +0x1C = 0, +0x1E = +0x1F = 0, +0x20 = 0.
    NiScreenPolygon(u16 count, const NiPoint3* vertices, const NiPoint2* uvs, const NiColorA* colors);
    // orig: AttachProperty (the property state's slot of the property's type, vf2C): the port keeps a list, one per
    // type (the last attached wins, as the slot).
    void AttachProperty(NiProperty* property);
    template <class P>
    P* FindProperty() const {
        for (const auto& p : properties)
            if (auto* q = dynamic_cast<P*>(p.Get())) return q;
        return nullptr;
    }
    std::vector<NiPointer<NiProperty>> properties;   // +0x08 (the NiPropertyState's slots)
    // +0x08 as the renderer reads it: a default state (0x0014D800) with the attached properties in their slots.
    NiPropertyState* State();
    NiPointer<NiPropertyState> state;
    u16 vertexCount = 0;                             // +0x0C
    std::vector<NiPoint3> vertices;                  // +0x10
    std::vector<NiPoint2> uvs;                       // +0x14 (empty: none)
    std::vector<NiColorA> colors;                    // +0x18 (empty: none)
    u16 flags = 0;                                   // +0x1C (bit 0: attached to a camera, 0x00019460)
    u8 strip = 0;                                    // +0x1E (0x00106AA0: drawn as a strip, prim 6; else a fan, 7)
    u8 drawOnce = 0;                                 // +0x1F (0x00106AC0: with +0x1E, Click's first loop draws it
                                                     //  and removes it from the camera)
    u32 field20 = 0;                                 // +0x20
};

// ---- extra data -----------------------------------------------------------------------------------------------------
class NiExtraData : public NiObject {
public:
    FUT_NI_CLASS(NiExtraData)
    void LoadBinary(NiStream& s) override;           // orig: 0x001098A0
    void LinkObject(NiStream& s) override;           // orig: 0x00109770
    u32 next = kNullLink;
    NiPointer<NiExtraData> nextData;                 // orig: this+0x10
    u32 size = 0;
    std::vector<u8> data;                            // only for exact NiExtraData objects
};

class NiStringExtraData : public NiExtraData {
public:
    FUT_NI_CLASS(NiStringExtraData)
    void LoadBinary(NiStream& s) override;           // orig: 0x00119AE0
    std::string string;
};

class NiTextKeyExtraData : public NiExtraData {
public:
    FUT_NI_CLASS(NiTextKeyExtraData)
    void LoadBinary(NiStream& s) override;           // orig: 0x0012A2C0
    struct TextKey { f32 time = 0; std::string text; };   // orig: 0x00153820
    std::vector<TextKey> keys;
};

// ---- controllers ----------------------------------------------------------------------------------------------------
class NiTimeController : public NiObject {
public:
    FUT_NI_CLASS(NiTimeController)
    // flags (this+0x08): bit 0 app-time (first update counts from 0), bits 1-2 cycle type (0 loop, 1 reverse,
    // 2 clamp), bit 3 active.
    bool IsActive() const { return (flags & 8) != 0; }                       // orig: 0x0013EBF0
    void SetActive(bool on) { flags = on ? (flags | 8) : (flags & ~8); }   // orig: 0x0013EBD0
    // orig: 0x00141060 (vtbl+0x30): inactive, last time reset, start time reset for app-init controllers (bit 0).
    void Stop();
    // orig: 0x00141030 (vtbl+0x2C, time): active, last time reset, the start time = time for app-init controllers.
    void Start(f32 time);
    // orig: 0x0013EC20 (vtbl+0x3C): moves this controller from the old target's chain to the front of the new one's
    // (unless it is in that chain already).
    void SetTarget(NiObjectNET* target);
    u32 CycleType() const { return (flags >> 1) & 3; }                      // orig: 0x0013EBC0
    // orig: vtbl+0x48 (0x00141260). Returns the x87 value unrounded (callers round or compare it).
    double ComputeScaledTime(f32 time);
    // orig: vtbl+0x38. Controllers not ported yet do nothing.
    virtual void Update(f32 time) { (void)time; }
    // orig: vtbl+0x34. True for the keyframe, visibility and UV controllers.
    virtual bool IsTransformController() const { return false; }
    // orig: vtbl+0x40, called for each controller of a geometry by its display (0x0013EC90 from NiTriShape's
    // 0x00116760): 0x000B0420 (nothing) except NiUVController 0x00128740 and NiGeomMorpherController 0x00125E10.
    virtual void OnPreDisplay() {}
    // orig: 0x00034140. True when this update can be skipped (inactive, same time, or same scaled time); otherwise
    // refreshes lastScaledTime when computeScaledFirst is set.
    bool SkipUpdate(f32 time);
    f32 startTime = -3.4028234663852886e38f;         // orig: this+0x24 (-FLT_MAX = not started)
    f32 lastTime = -3.4028234663852886e38f;          // orig: this+0x28
    f32 accumulatedTime = 0;                         // orig: this+0x2C
    f32 lastScaledTime = -3.4028234663852886e38f;    // orig: this+0x30
    bool computeScaledFirst = true;                  // orig: this+0x34 (1 for most classes, 0 for keyframe)
    bool forceUpdate = false;                        // orig: this+0x3F of the keyframe / visibility / UV controllers
    s8 priority = -1;                                // orig: this+0x3E of the same classes (their ctors set -1)
    void LoadBinary(NiStream& s) override;           // orig: 0x001414F0
    // orig: 0x00141560. Subclasses' data links (keyframe data, UV data, flip sources, ...) are not resolved yet.
    void LinkObject(NiStream& s) override;
    u32 nextController = kNullLink;
    NiPointer<NiTimeController> nextControllerPtr;   // orig: this+0x10
    NiObjectNET* targetPtr = nullptr;                // orig: this+0x0C (not reference counted)
    u16 flags = 0;
    f32 frequency = 1, phase = 0, loKeyTime = 0, hiKeyTime = 0;
    u32 target = kNullLink;
};

// TimeController + one data link (orig: 0x00152C50): keyframe, alpha, material colour and visibility controllers.
class NiSingleDataController : public NiTimeController {
public:
    FUT_NI_CLASS(NiSingleDataController)
    void LoadBinary(NiStream& s) override;           // orig: 0x00152C50
    void LinkObject(NiStream& s) override;           // data link of the keyframe/alpha/colour/vis controllers
    u32 data = kNullLink;
    NiPointer<NiObject> dataPtr;
};

class NiKeyframeData;
class NiVisData;

class NiKeyframeController : public NiSingleDataController {
public:
    FUT_NI_CLASS(NiKeyframeController)
    NiKeyframeController() { computeScaledFirst = false; }   // orig: CreateObject 0x00127850 clears this+0x34
    void Update(f32 time) override;                  // orig: 0x001270B0
    bool IsTransformController() const override { return true; }
    NiKeyframeData* Data() const;
    u32 lastRotIndex = 0, lastPosIndex = 0, lastScaleIndex = 0;   // orig: u16 at this+0x38 / 0x3A / 0x3C
};
class NiAlphaController : public NiSingleDataController {
public:
    FUT_NI_CLASS(NiAlphaController)
    void Update(f32 time) override;                  // orig: 0x001206E0 (material property alpha)
    u32 lastIndex = 0;                               // orig: this+0x38
};

class NiMaterialColorController : public NiSingleDataController {
public:
    FUT_NI_CLASS(NiMaterialColorController)
    void Update(f32 time) override;                  // orig: 0x00133730 (colour slot from flags bits 4-6)
    u32 lastIndex = 0;                               // orig: this+0x38
};

class NiVisController : public NiSingleDataController {
public:
    FUT_NI_CLASS(NiVisController)
    void Update(f32 time) override;                  // orig: 0x00129A60 (hidden flag of the target)
    bool IsTransformController() const override { return true; }
    u32 lastIndex = 0;                               // orig: this+0x48
};

class NiFlipController : public NiTimeController {
public:
    FUT_NI_CLASS(NiFlipController)
    void LoadBinary(NiStream& s) override;           // orig: 0x00125320
    void LinkObject(NiStream& s) override;           // orig: 0x00125370
    void Update(f32 time) override;                  // orig: 0x00124C60
    u32 affectedMap = 0;                             // orig: this+0x54 (texturing property slot)
    f32 flipStartTime = 0, flipDelta = 0;            // orig: this+0x58 / +0x60
    f32 span = 0;                                    // orig: this+0x5C = source count * delta
    u32 index = 0;                                   // orig: this+0x50
    std::vector<u32> sources;
    std::vector<NiPointer<NiObject>> sourceTextures; // orig: NiTArray data at this+0x3C, size this+0x44
};

class NiUVController : public NiTimeController {
public:
    FUT_NI_CLASS(NiUVController)
    void LoadBinary(NiStream& s) override;           // orig: 0x00129000
    void LinkObject(NiStream& s) override;           // orig: 0x00129030
    void Update(f32 time) override;                  // orig: 0x001286F0 (marks the UVs dirty)
    bool IsTransformController() const override { return true; }
    u16 textureSet = 0;
    u32 data = kNullLink;
    NiPointer<NiObject> dataPtr;
    bool uvDirty = false;                            // orig: this+0x6C (consumed when the geometry is drawn)
    // orig: 0x00128740: with uvDirty, the target's data's texture set `textureSet` moved from the last applied offset
    // / tiling to the keys' values at lastScaledTime (each about the texture centre), the data's +0x2E |= 8.
    void OnPreDisplay() override;
    u32 lastIndex[4] = {};                           // orig: +0x44 / +0x4C / +0x48 / +0x50 (the groups' key caches)
    f32 appliedOffset[2] = {0, 0};                   // orig: +0x5C / +0x60 (ctor 0x00128F50: 0)
    f32 appliedTiling[2] = {1, 1};                   // orig: +0x64 / +0x68 (ctor: 1)
};

class NiGeomMorpherController : public NiTimeController {
public:
    FUT_NI_CLASS(NiGeomMorpherController)
    void LoadBinary(NiStream& s) override;           // orig: 0x00125CF0
    void LinkObject(NiStream& s) override;           // orig: 0x00125D00
    void Update(f32 time) override;                  // orig: 0x00152FD0 (marks the morph dirty)
    u32 data = kNullLink;
    NiPointer<NiObject> dataPtr;                     // orig: this+0x38
    bool alwaysUpdate = false;                       // orig: this+0x3E (version >= 4.0.0.2)
    bool morphDirty = false;                         // orig: this+0x3C
};

// ---- controller data ------------------------------------------------------------------------------------------------
class NiFloatData : public NiObject {
public:
    FUT_NI_CLASS(NiFloatData)
    void LoadBinary(NiStream& s) override;           // orig: 0x001203C0
    FloatKeys keys;
};

class NiPosData : public NiObject {
public:
    FUT_NI_CLASS(NiPosData)
    void LoadBinary(NiStream& s) override;           // orig: 0x001318C0
    PosKeys keys;
};

class NiVisData : public NiObject {
public:
    FUT_NI_CLASS(NiVisData)
    void LoadBinary(NiStream& s) override;           // orig: 0x001297F0
    std::vector<BoolKey> keys;                       // orig: 0x001294A0
};

class NiUVData : public NiObject {
public:
    FUT_NI_CLASS(NiUVData)
    void LoadBinary(NiStream& s) override;           // orig: 0x00128090
    std::array<FloatKeys, 4> groups;                 // u offset, v offset, u tiling, v tiling
};

class NiKeyframeData : public NiObject {
public:
    FUT_NI_CLASS(NiKeyframeData)
    void LoadBinary(NiStream& s) override;           // orig: 0x00126BA0
    RotKeys rotations;
    PosKeys positions;
    FloatKeys scales;
};

class NiMorphData : public NiObject {
public:
    FUT_NI_CLASS(NiMorphData)
    void LoadBinary(NiStream& s) override;           // orig: 0x00125740
    struct Morph { FloatKeys keys; std::vector<NiPoint3> vertices; };
    u32 vertexCount = 0;
    bool relativeTargets = false;
    std::vector<Morph> morphs;
};

class NiKeyframeManager;

// One cached transform of a sequence's blend cache (orig: 0x50-byte records in the list at this+0x74).
struct NiTransformRecord {
    NiAVObject* object = nullptr;
    NiMatrix3 rotate;
    NiPoint3 translate;
    f32 scale = 0;
};

class NiSequence : public NiObject {
public:
    FUT_NI_CLASS(NiSequence)
    NiSequence() = default;
    // orig: 0x0012E3A0 NiSequence(name, arraySize, growBy) as used for the temporary blend / mix sources: `count`
    // unnamed entries.
    NiSequence(const std::string& sequenceName, u32 count);
    void LoadBinary(NiStream& s) override;           // orig: 0x0012D130
    void LinkObject(NiStream& s) override;           // orig: 0x0012D220
    struct Entry {
        std::string name;                            // orig: name array at this+0x14 (data +0x18)
        u32 controller = kNullLink;
        NiPointer<NiObject> controllerObj;           // orig: controller array at this+0x44 (data +0x48)
    };
    std::string name;                                // orig: this+0x08
    u32 textKeyReference = 0;                        // orig: this+0x90, index of the reference (time) controller
    u32 textKeys = kNullLink;
    NiPointer<NiObject> textKeysObj;                 // orig: this+0x8C (a NiTextKeyExtraData)
    std::vector<Entry> entries;

    // ---- runtime (NiKeyframeManager.cpp) ----------------------------------------------------------------------
    NiTimeController* Controller(u32 i) const;
    NiTimeController* ReferenceController() const { return Controller(textKeyReference); }
    u32 ControllerCount() const { return static_cast<u32>(entries.size()); }   // orig: this+0x50
    bool SetOwner(NiKeyframeManager* manager);       // orig: 0x0012A630
    bool LinkToTargets(NiAVObject* node, u32& index);   // orig: 0x0012BCF0
    bool Activate(bool initTempTransforms, s8 priority);   // orig: 0x0012BB80
    bool Deactivate(bool transition);                // orig: 0x0012BC10
    void AttachControllers(s8 priority);             // orig: 0x0012A710
    void DetachControllers();                        // orig: 0x0012A870
    bool UpdateController(f32 time, u32 i);          // orig: 0x0012C0A0
    void UpdateAll(f32 time);                        // orig: 0x0012C890
    void UpdateAndBlend(f32 t1, f32 t2, f32 weight);   // orig: 0x0012C8F0
    void UpdateSumAndCache(f32 t1, f32 t2, f32 weight);   // orig: 0x0012D4B0
    void UpdateAndCache(f32 time);                   // orig: 0x0012D830
    void SyncUpdateAndCache(f32 time, NiSequence* other, f32 otherTime);   // orig: 0x0012D9E0
    void CacheCurrentTransforms();                   // orig: 0x0012E260
    void BlendCachedTransforms(f32 t);               // orig: 0x0012AB50
    double ComputeMorphTime(NiSequence* other, f32 otherTime);   // orig: 0x0012A910
    void UpdateActive(f32 time);                     // orig: 0x0012D330 (state 1)
    void UpdateMorphSource(f32 time);                // orig: 0x0012D350 (state 6)
    void UpdateBlend(f32 time);                      // orig: 0x0012EB10 (states 2, 3, 5)
    void UpdateSum(f32 time, f32 weight);            // orig: 0x0012EE20 (state 8)

    NiKeyframeManager* owner = nullptr;              // orig: this+0x94 (not reference counted)
    s32 state = 0;                                   // orig: this+0x98 (see NiKeyframeManager.h)
    f32 endPoint = 0;                                // orig: this+0x9C, blend / morph / sum boundary time
    NiSequence* partner = nullptr;                   // orig: this+0xA0
    NiSequence* tempPartner = nullptr;               // orig: this+0xA4
    f32 offset = 0;                                  // orig: this+0xA8, sequence time = offset + manager time
    bool cumulative = false;                         // orig: this+0xAC (root accumulation; see NiKeyframeManager.h)
    bool updateBlendTransforms = false;              // orig: this+0xB4
    bool syncBlendTransforms = false;                // orig: this+0xB5
    std::vector<NiAVObject*> targets;                // orig: this+0x5C, filled by LinkToTargets
    // The blend cache never shrinks: each fill overwrites from the front and appends past the end; blends run over
    // the whole list (see BlendCachedTransforms).
    std::vector<NiTransformRecord> cache;            // orig: this+0x74

private:
    NiTransformRecord& CacheSlot(u32& cursor);
};

// ---- geometry -------------------------------------------------------------------------------------------------------
// orig: NiBound::ComputeFromPoints 0x0014EA10 - centre of the points' axis-aligned box, radius to the farthest point.
NiBound BoundOfPoints(const std::vector<NiPoint3>& pts);

// The renderer's data of a geometry data (NiGeometryData +0x30; NiXBoxRenderer's NiGeometryBufferData: the vertex /
// index buffers, render/nixbox/NiXBoxGeometryBuffers.h). Owned by the data: destroying it is the renderer's purge
// (the data's dtor 0x001437C0 -> 0x00140220: every renderer's vfE8 0x00146F30 -> the group manager's vf24).
struct NiGeometryRendererData {
    virtual ~NiGeometryRendererData() = default;
};

class NiGeometryData : public NiObject {
public:
    FUT_NI_CLASS(NiGeometryData)
    void LoadBinary(NiStream& s) override;           // orig: 0x00143DC0
    std::unique_ptr<NiGeometryRendererData> rendererData;   // orig: this+0x30
    u16 vertexCount = 0;
    std::vector<NiPoint3> vertices, normals;
    NiBound bound;
    std::vector<NiColorA> colors;
    u16 textureSets = 0;
    std::vector<NiPoint2> uvs;                       // textureSets * vertexCount
    u8 revisionFlags = 0;                            // orig: this+0x2E (consistency bits; 4 = colours changed)
};

class NiTriBasedGeomData : public NiGeometryData {
public:
    FUT_NI_CLASS(NiTriBasedGeomData)
    void LoadBinary(NiStream& s) override;           // orig: 0x00151EE0
    u16 triangleCount = 0;
};

class NiTriShapeData : public NiTriBasedGeomData {
public:
    FUT_NI_CLASS(NiTriShapeData)
    void LoadBinary(NiStream& s) override;           // orig: 0x00116130
    std::vector<u16> triangleList;                   // u32 count of u16 indices on disk
    std::vector<std::vector<u16>> sharedNormals;     // per-vertex match groups
};

// orig: NiTriStripsData (vtbl 0x002A8930, ctor 0x0011BB60 over NiTriBasedGeomData 0x00151DE0, 0x44 bytes).
class NiTriStripsData : public NiTriBasedGeomData {
public:
    FUT_NI_CLASS(NiTriStripsData)
    u16 stripCount = 0;                              // orig: +0x38
    std::vector<u16> stripLengths;                   // orig: +0x3C
    std::vector<u16> stripLists;                     // orig: +0x40 (the strips' indices, one after the other)
};

// orig: NiParticles (vtbl 0x002A71E0, ctor 0x0010F710, 0xF0 bytes) and NiParticlesData (vtbl 0x002A7168, ctor
// 0x0010F170, 0x40 bytes): created by the particle emitters (never loaded from the disc's files). The world bound is
// NiGeometry's (0x0010F9A0 adds the offset of an emitter node +0xE0, never set by the code ported). The display
// 0x0010F8B0 is ported (OnVisible -> NiRenderer::DisplayParticles). NOT PORTED: the streaming slots, the data id
// counter (0x002D7D20).
class NiParticlesData : public NiGeometryData {
public:
    FUT_NI_CLASS(NiParticlesData)
    f32 value34 = 1.0f;                              // orig: +0x34
    u16 activeCount = 0;                             // orig: +0x38 (SetActiveParticleCount 0x0010F3D0: min(n, count))
    std::vector<f32> sizes;                          // orig: +0x3C (ctor: count x 1.0)
};
class NiParticles : public NiGeometry {
public:
    FUT_NI_CLASS(NiParticles)
    void OnVisible(NiCullPass& pass) override;       // orig: 0x0010F8B0 (display: the renderer's slot 52)
    // orig: 0x0010E7F0: a state with a texture effect gives the particles a new state with its lights only.
    void PushEffects(NiDynamicEffectState* state) override;
    NiAVObject* emitterNode = nullptr;               // orig: +0xE0
};

class NiSkinInstance : public NiObject {
public:
    FUT_NI_CLASS(NiSkinInstance)
    void LoadBinary(NiStream& s) override;           // orig: 0x0010DD30
    void LinkObject(NiStream& s) override;           // orig: 0x0010DF20
    // orig: 0x0010E140. Model-space bound of the skinned mesh from the bones' current world transforms.
    NiBound ComputeModelBound() const;
    u32 skinDataLink = kNullLink, rootParentLink = kNullLink;
    std::vector<u32> boneLinks;
    NiPointer<NiSkinData> skinData;                  // orig: this+0x08
    NiAVObject* rootParent = nullptr;                // orig: this+0x0C (not reference counted)
    std::vector<NiAVObject*> bones;                  // orig: this+0x10 (not reference counted)
    // orig: this+0x14 (ctor 0x0010DEA0: 0xFFFFFFFF): the renderer frame of the last software deform (0x00157C90)
    u32 deformFrame = 0xFFFFFFFFu;
};

class NiSkinData : public NiObject {
public:
    FUT_NI_CLASS(NiSkinData)
    void LoadBinary(NiStream& s) override;           // orig: 0x0010DB30
    void LinkObject(NiStream& s) override;           // orig: 0x0010D9A0
    struct Weight { u16 vertex = 0; f32 weight = 0; };
    struct Bone { NiTransform skinToBone; NiBound bound; u16 vertexCount = 0; std::vector<Weight> weights; };   // orig: 0x60 bytes
    NiTransform rootParentToSkin;                    // orig: this+0x10
    u32 skinPartitionLink = kNullLink;               // version >= 4.0.0.1
    NiPointer<NiSkinPartition> skinPartition;        // orig: this+0x08
    bool hasVertexWeights = true;                    // read when version > 4.1.0.13
    std::vector<Bone> bones;                         // orig: this+0x54 (count this+0x50)
};

class NiSkinPartition : public NiObject {
public:
    FUT_NI_CLASS(NiSkinPartition)
    void LoadBinary(NiStream& s) override;           // orig: 0x0010D0F0
    struct Partition {                               // orig: 0x0010C770
        u16 vertexCount = 0, triangleCount = 0, boneCount = 0, stripCount = 0, bonesPerVertex = 0;
        std::vector<u16> bones, vertexMap;
        std::vector<f32> weights;                    // vertexCount * bonesPerVertex
        std::vector<u16> stripLengths;
        std::vector<u16> indices;                    // strips (sum of lengths) or triangle list (3 * triangleCount)
        bool hasBonePalette = false;
        std::vector<u8> bonePalette;                 // vertexCount * bonesPerVertex
    };
    std::vector<Partition> partitions;
};

class NiPixelData : public NiObject {
public:
    FUT_NI_CLASS(NiPixelData)
    void LoadBinary(NiStream& s) override;           // orig: 0x00111C40
    struct Format { u32 format = 0; u32 channelMasks[4] = {}; u32 bitsPerPixel = 0; u32 fastCompare[2] = {}; };  // orig: 0x00140E70
    struct MipLevel { u32 width = 0, height = 0, offset = 0; };
    Format format;
    u32 palette = kNullLink;
    u32 bytesPerPixel = 0;
    std::vector<MipLevel> mips;
    std::vector<u8> pixels;
    u32 shareKey = 0;                                // orig: +8 (version >= 4.2.1.1)
    ~NiPixelData() override;                         // (leaves the shared list, L)
};

// orig: NiOutlineData (0x2C bytes, ctor 0x0013C070: the arrays 0, +0x28 = 1): the edges +8 (16 bytes: the vertices
// +0 / +2, the faces +4 / +6 (-1: none), the flags +8: 0x10 interior, 0x20 never, 0x40, the facing counters
// (+4 front, +1 back) and 0x80 silhouette), the faces +0xC (16 bytes: the vertices +0..+4, the geometry +6, the edges
// +8..+C), the vertex-edge lists +0x10 (count, pointer into +0x14), the geometry colours +0x18; the counts +0x1C
// edges, +0x20 faces, +0x24 vertices; +0x28 the extra data still to apply (0x0013CA40).
class NiOutlineData : public NiObject {
public:
    FUT_NI_CLASS(NiOutlineData)
    void LoadBinary(NiStream& s) override;           // orig: 0x0013C280
    struct Edge { u16 a = 0, b = 0, c = 0, d = 0, flags = 0; };      // disk order: +4, +6, +0, +2 (+8 = 0)
    struct Face { u16 v[7] = {}; };                                  // disk order: +8, +A, +C, +0, +2, +4, +6
    std::vector<Edge> edges;
    std::vector<Face> faces;
    std::vector<std::vector<u16>> lists;             // per vertex: its edges (the count +0x24 = lists.size())
    std::vector<NiColorA> colors;                    // +0x18 (0x0013CA40: one per listed geometry)
    bool extraDataPending = true;                    // +0x28
};

#undef FUT_NI_CLASS

// Registers the CreateObject functions of all classes above (orig: ~100 static-init RegisterLoader calls).
void RegisterNiLoaders();

}  // namespace fut::ni
