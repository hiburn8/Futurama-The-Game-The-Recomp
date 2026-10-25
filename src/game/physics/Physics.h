// The physics world stepped once per fixed step in play (orig: Physic 0x000AE9D0 / vf01 0x000AE860 / vf02 0x000AE870,
// the workspace 0x000ADDB0 and its solver 0x000AE160). docs/analysis/physics.md.
//
// Every physics object is in a global list (newest first) and integrates its velocity and position (characters:
// 0x000AF200); then the solver resets the contacts, pushes overlapping characters apart (0x000A9E30), pushes
// characters out of boxes (0x000AA8A0), and slides every character's box out of the stage triangles a sphere query
// gathered (0x000FB410, 0x000AA510, the 13-axis box-triangle test 0x000A9240). Characters are axis-aligned boxes;
// collisions move positions only, the next integrate reads the contact flags.
//
// x87 order of the original kept: "dbl" intermediates stay double, F() = an FSTP to float; each comparison follows its
// FNSTSW idiom (NaN included).
//
// The sphere query also gathers the collision mesh records' triangles (flags bit 1; the scratch and the records live in
// engine/ni/NiCollision.h; docs/analysis/collision_meshes_port.md). The dynamic rigid boxes (the debris of a breaking
// destructable: their integrate 0x000AFAC0, the time bisection Step 0x000AB430, the box-triangle test 0x000AB6D0, the
// impulse 0x000AE400, box-box 0x000ACCD0 and the dynamic branches of char-box) follow
// docs/analysis/simplenpc_moveable_port.md 4. NOT PORTED: the objects' vf20 query. The ray test (vf18, Physic vf03
// RayCastObjects) and the sphere test (vf1C, Physic vf05 QueryObjects) are ported for characters and boxes.
//
// The original's 0x30-byte matrices (RigidBox +0x90) are three 16-byte chunks that are the port's NiMatrix3 columns:
// chunk k element j = m[j][k] (as MakeYRotation 0x001030B0 writes them).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"

#include <array>
#include <functional>
#include <vector>

namespace fut::game {
class GameObject;
}

namespace fut::phys {

class PhysWorld;

// A gathered triangle (orig: a pointer to a 0x14-byte triangle): a NiBsp's stage triangle, or (`mesh`, no bsp) slot
// `index` of the sphere query's mesh scratch (ni::g_scratch: a later query overwrites it, as in the original).
struct TriRef {
    ni::NiBsp* bsp = nullptr;
    u32 index = 0;
    bool mesh = false;
    bool Empty() const { return !bsp && !mesh; }
    u32& Flags() const;                               // +0x10 (byte 0 flags, byte 1 surface, byte 2 edges, byte 3 slide)
    const ni::NiPoint3& V(int k) const;               // [+4k]
    ni::NiPoint3 Normal() const;                      // orig: 0x000FA8C0 (with the stored 1 / |cross| +0xC)
    // The original's address of the triangle (the RadixSortPtrs key 0x000FA020; collision_meshes_port.md 7.5).
    u64 OrigAddr() const;
};

struct CharShape {                                    // orig: 0xD0 bytes, ctor 0x000A91E0, vtbl 0x002A1F08
    u32 kind = 1;                                     // +0x10 (1 = character box)
    ni::NiPoint3 pos;                                 // +0x14 (box centre, world)
    std::array<u8, 8> contactFlags{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // +0x54 (0xFF = empty)
    std::array<TriRef, 8> contacts{};                 // +0x5C
    CharShape* next = nullptr;                        // +0x7C (the world's shape list)
    PhysWorld* world = nullptr;                       // +0x80 (set while listed)
    ni::NiPoint3 ext;                                 // +0x90 half extents
    u32 flags = 0;                                    // +0x9C contact flags (physics.md 3.9)
    u8 descending = 0;                                // +0xA0
    f32 stepHeight = 1.8f;                            // +0xA8
    f32 stepLeft = 0;                                 // +0xAC
    u8 noPush = 0;                                    // +0xB0
    u8 b1 = 0;                                        // +0xB1 (= body +0x1A)
    u8 partnerIsBox = 0;                              // +0xB2
    void* partner = nullptr;                          // +0xB4 (CharShape* or RigidBox*)
    u8 pinned = 0;                                    // +0xB8
    u8 b9 = 0;                                        // +0xB9 (= body +0x1C)
    ni::NiPoint3 slideStart;                          // +0xBC
    ni::NiAVObject* excludeNode = nullptr;            // +0xC8
    u8 cc = 0;                                        // +0xCC
    void ResetContacts();                             // orig: 0x000A9190
    void CharChar(CharShape& b);                      // orig: 0x000A9E30
    void CharBox(struct RigidBox& x);                 // orig: 0x000AA8A0
    void Slide(std::vector<TriRef>& tris);            // orig: 0x000AA510
    bool TriBox(const TriRef& t, ni::NiPoint3& outDir, f32& outDepth);   // orig: 0x000A9240
};

// orig: 0x170 bytes, ctor 0x000AB290 (base 0x000AD980; docs/analysis/simplenpc_moveable_port.md 4). The 0x30-byte
// matrices' pad lanes (+0x9C / +0xAC / +0xBC ...) never reach a matrix element (MatrixAdd and the Gram-Schmidt pass
// treat them as separate lanes), so the port keeps none.
struct RigidBox {
    f32 mass = 0, invMass = 0;                        // +0x10 / +0x14
    ni::NiMatrix3 inertia{{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}};      // +0x20 (body frame, diagonal)
    ni::NiMatrix3 invInertia{{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}};   // +0x50
    ni::NiPoint3 pos;                                 // +0x80
    ni::NiMatrix3 rot;                                // +0x90 (3 rows x 4 floats in the original)
    ni::NiPoint3 P, L;                                // +0xC0 momentum / +0xCC angular momentum
    ni::NiMatrix3 invInertiaWorld{{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}};   // +0xE0
    ni::NiPoint3 v, w;                                // +0x110 velocity / +0x11C angular velocity
    std::array<f32, 3> material{0.85f, 0.75f, 0.57f}; // +0x128 -> new(0xC) MaterialInit(0) 0x000AB180 (not read, L)
    ni::NiPoint3 force, torque;                       // +0x138 / +0x144 (the accumulators)
    u8 isStatic = 0;                                  // +0x150
    u8 noRotation = 0;                                // +0x151
    u32 flags = 0;                                    // +0x154 contact flags (4.9)
    u32 kind = 4;                                     // +0x158
    ni::NiPoint3 ext;                                 // +0x160 half extents
    CharShape* standing = nullptr;                    // +0x16C
    RigidBox* next = nullptr;                         // +0x130 (the world's box list)
    PhysWorld* world = nullptr;                       // +0x12C

    void InitInertia();                               // orig: vf0C 0x000AB330
    void UpdateDerived();                             // orig: 0x000ADA00
    void IntegrateBy(f32 dt);                         // orig: 0x000ADB20
    void Advance(f32 dt) {                            // orig: 0x000ADD90
        IntegrateBy(dt);
        UpdateDerived();
    }
    void Step(const std::vector<TriRef>& tris);       // orig: vf10 0x000AB430 (the time bisection)
    // orig: vf14 0x000AB6D0: the 13-axis box-triangle test; `respond` = the contact response (impulse, flags).
    bool TriangleTest(const TriRef& t, ni::NiPoint3& outN, f32& outDepth, bool respond);
    ni::NiPoint3 PointVelocity(const ni::NiPoint3& r) const;            // orig: 0x000AD600
    void ApplyImpulse(const ni::NiPoint3& r, const ni::NiPoint3& J);    // orig: 0x000AD720
    void AddForceAtPoint(const ni::NiPoint3& f, const ni::NiPoint3& r); // orig: 0x000AD680
    void ClearAccumulators() { force = {}, torque = {}; }               // orig: 0x000AD5A0
    void SetVelocity(const ni::NiPoint3& vel);        // orig: 0x000AD8A0
    void BoxBox(RigidBox& y);                         // orig: 0x000ACCD0
    double BoundingRadius() const;                    // orig: vf04 0x000AB400 (ST0)
};

class PhysObject {                                    // orig: base ctor 0x000AF020, vtbl 0x002A1F8C
public:
    PhysObject();                                     // orig: head insert into the global list DAT_002E9C40
    virtual ~PhysObject();                            // orig: 0x000AF080 (unlink)
    PhysObject(const PhysObject&) = delete;
    PhysObject& operator=(const PhysObject&) = delete;
    virtual void SetPosition(const ni::NiPoint3& p) = 0;   // vf04
    virtual ni::NiPoint3 GetPosition() const = 0;          // vf08
    void SetForce(const ni::NiPoint3& f) { force = f; }    // vf10 0x000AEC70
    virtual void Integrate(f32 dt) = 0;                    // vf14
    // orig: vf1C (thiscall(const NiPoint3* c, const f32* r), RET 8): does the sphere reach the object? The base
    // (0x000AEFF0) says no.
    virtual bool OverlapsSphere(const ni::NiPoint3& c, const f32& r) const { (void)c, (void)r; return false; }
    // orig: vf18 (thiscall(const NiPoint3* o, const NiPoint3* d, f32* len, bool frontOnly), RET 0x10): does the ray
    // o + t*d (0 <= t <= *len) reach the object? A hit writes its t into `len`. The base (0x000AEFE0) says no.
    virtual bool RayTest(const ni::NiPoint3& o, const ni::NiPoint3& d, f32& len, bool frontOnly) const {
        (void)o, (void)d, (void)len, (void)frontOnly;
        return false;
    }
    virtual void SetEnabled(bool on) { enabled = on; }     // vf34
    PhysObject* next = nullptr;                       // +0x04
    PhysObject* prev = nullptr;                       // +0x08
    ni::NiPoint3 force{0, -100, 0};                   // +0x0C (orig: copy of 0x002D682C)
    u8 halveXZ = 1;                                   // +0x18
    u8 b19 = 0, b1A = 0, integratePinned = 0, b1C = 0, noRay = 0;   // +0x19..+0x1D
    game::GameObject* owner = nullptr;                // +0x20
    u8 enabled = 1;                                   // +0x24
};

class CharBody : public PhysObject {                  // orig: 0x48 bytes, ctor 0x000AF0D0, vtbl 0x002A2058
public:
    CharBody(f32 ex, f32 ey, f32 ez, PhysWorld* world);   // orig: (ex, ey, ez, mass); mass is not read
    ~CharBody() override;                             // orig: 0x000B0090
    void SetPosition(const ni::NiPoint3& p) override { lastPos = p; shape.pos = p; }   // orig: 0x000AEC90
    ni::NiPoint3 GetPosition() const override { return shape.pos; }                  // orig: 0x000AECD0
    void Integrate(f32 dt) override;                  // orig: 0x000AF200
    bool OverlapsSphere(const ni::NiPoint3& c, const f32& r) const override;   // orig: vf1C 0x000AF5F0
    // orig: vf18 0x000AF400: the slab test against the shape's axis-aligned box.
    bool RayTest(const ni::NiPoint3& o, const ni::NiPoint3& d, f32& len, bool frontOnly) const override;
    void SetEnabled(bool on) override;                // orig: 0x000AED40
    void SetStepHeight(f32 h) { shape.stepHeight = h; }   // orig: vf38 0x000AED20
    void AddVelocity(const ni::NiPoint3& v) {         // orig: vf50 0x000AF7D0 (each F(+0x28.. + v))
        velocity = {static_cast<f32>(double(velocity.x) + v.x), static_cast<f32>(double(v.y) + velocity.y),
                    static_cast<f32>(double(v.z) + velocity.z)};
    }
    ni::NiPoint3 velocity;                            // +0x28
    ni::NiPoint3 lastPos;                             // +0x34 (the position at the start of the last integrate)
    CharShape shape;                                  // +0x40 (a separate 0xD0 object in the original)
    u8 supported = 0;                                 // +0x45
    PhysWorld* world = nullptr;

private:
    // The NiAVObject the shape excludes from mesh queries (orig: owner->vf30(), the character's node).
    ni::NiAVObject* OwnerNode() const;
};

class CollisionBox : public PhysObject {              // orig: 0x30 bytes, ctor 0x000AF9B0, vtbl 0x002A1FC8
public:
    // orig: 0x000AF9B0 (thiscall(ex, ey, ez, mass), RET 0x10): the body new(0x170) 0x000AB290, its material
    // new(0xC) 0x000AB180(0), the extents vf18, the mass vf08, InitInertia vf0C, then the world's AddBox.
    CollisionBox(f32 ex, f32 ey, f32 ez, f32 mass, PhysWorld* world);
    ~CollisionBox() override;
    void SetPosition(const ni::NiPoint3& p) override { box.pos = p; }   // orig: 0x000AEDE0
    ni::NiPoint3 GetPosition() const override { return box.pos; }      // orig: 0x000AEE00
    ni::NiPoint3 GetVelocity() const { return box.v; }                 // orig: vf0C 0x000AEE70
    void SetRotation(const ni::NiMatrix3& r) { box.rot = r; }          // orig: vf38 0x000AEE30
    ni::NiMatrix3 GetRotation() const { return box.rot; }              // orig: vf3C 0x000AEE50
    // orig: vf40 0x000AF880 (thiscall(point*, impulse*), RET 8): the impulse at a world point.
    void ApplyImpulseAt(const ni::NiPoint3& point, const ni::NiPoint3& impulse);
    bool IsAtRest() const;                            // orig: vf44 0x000AF910
    // orig: 0x000AFAC0 (simplenpc_moveable_port.md 4.2): a static box does nothing; a dynamic box gathers the stage
    // triangles around it, steps (Step 0x000AB430), damps its momenta and adds its gravity for the next step.
    void Integrate(f32 dt) override;
    void Gravity(f32 dt);                             // orig: 0x000AF800
    bool OverlapsSphere(const ni::NiPoint3& c, const f32& r) const override;   // orig: vf1C 0x000AFF20
    // orig: vf18 0x000AFC90: the ray in the box's frame, then the slab test against the half extents.
    bool RayTest(const ni::NiPoint3& o, const ni::NiPoint3& d, f32& len, bool frontOnly) const override;
    RigidBox box;                                    // +0x28 (a separate 0x170 object in the original)
    PhysWorld* world = nullptr;
};

class PhysWorld {                                     // orig: the workspace, 0x16040 bytes, ctor 0x000ADDB0
public:
    PhysWorld() = default;
    PhysWorld(const PhysWorld&) = delete;
    PhysWorld& operator=(const PhysWorld&) = delete;
    ~PhysWorld();                                     // (detaches the shapes and boxes still listed)
    void AddShape(CharShape& s);                      // orig: 0x000ADFE0 (tail)
    void RemoveShape(CharShape& s);                   // orig: 0x000AE020
    void AddBox(RigidBox& b);                         // orig: 0x000ADF30 (tail)
    void RemoveBox(RigidBox& b);                      // orig: 0x000ADF80
    void Solve();                                     // orig: 0x000AE160
    RigidBox* boxes = nullptr;                        // +0x00
    CharShape* shapes = nullptr;                      // +0x04
    bool enabled = true;                              // +0x10 (set by 0x000ADF20)
    s32 count = 0;                                    // +0x14
    // orig: the Bender-player proximity check of the slide pass (S+0x94 with type 3): writes the player's
    // GetWorldPosition (its feet) and returns true when the level's player is Bender. Asked only for shapes with +0xB1.
    std::function<bool(ni::NiPoint3&)> benderFeet;
};

class Physic {                                        // orig: 0x28 bytes, ctor 0x000AE9D0, Session +0x30, DAT_002E9A7C
public:
    void Accumulate(f32 dt);                          // orig: vf01 0x000AE860
    void Step();                                      // orig: vf02 0x000AE870
    // orig: vf05 0x000AEB10 (thiscall(PhysObject* exclude, const NiPoint3* c, const f32* r)): every object of the
    // global list (head first) but `exclude` whose vf1C accepts the sphere, in the result array +0x08 (cleared first;
    // it grows by its +0x1C step with 0x000BA340). The reference stays valid until the next query.
    const std::vector<PhysObject*>& QueryObjects(const PhysObject* exclude, const ni::NiPoint3& c, const f32& r);
    // orig: vf03 0x000AE920 (thiscall(ignore, o, d, f32* len, frontOnly), RET 0x14): every object of the global list
    // (head first) but `ignore` is ray-tested (vf18; each hit shortens `len`); the last object that accepted.
    PhysObject* RayCastObjects(const PhysObject* ignore, const ni::NiPoint3& o, const ni::NiPoint3& d, f32& len,
                               bool frontOnly) const;
    PhysWorld world;                                 // +0x20
    f32 acc = 0;                                      // +0x24
    std::vector<PhysObject*> queryResult;             // +0x08 (NiTPrimitiveArray: data +0x0C, count +0x14)
};

// The global physics object list (orig: DAT_002E9C40, head insert).
extern PhysObject* g_objects;

// orig: QuerySphere 0x000FB410: flags 1 = every triangle of every kd-tree leaf whose box the query cube reaches,
// 2 = the collision mesh records' triangles that reach it (4: not the camera-transparent ones), over all NiBsps
// (newest first), sorted by the original's address order (TriRef::OrigAddr) and without duplicates.
const std::vector<TriRef>& QuerySphere(const ni::NiPoint3& c, f32 r, u32 flags);

}  // namespace fut::phys
