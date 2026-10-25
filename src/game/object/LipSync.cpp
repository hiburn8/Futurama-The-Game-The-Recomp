// The lip-sync face and player; see LipSync.h.
#include "game/object/LipSync.h"

#include "core/CrtString.h"
#include "game/level/Dialogue.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace fut::game {

using ni::detail::F;

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// orig: ByteReader (0x0009DA50 / 0x0009DAE0) over the face file's private copy: raw little-endian words, no size
// check in the original (the port reads zeros past the end).
u32 Word(const std::vector<u8>& b, size_t& at) {
    u32 v = 0;
    for (int i = 0; i < 4; ++i, ++at)
        if (at < b.size()) v |= u32(b[at]) << (8 * i);
    return v;
}
u16 Half(const std::vector<u8>& b, size_t& at) {
    u16 v = 0;
    for (int i = 0; i < 2; ++i, ++at)
        if (at < b.size()) v = static_cast<u16>(v | (b[at] << (8 * i)));
    return v;
}

// The subtree walk of 0x0009F490 / 0x0009FBB0 / 0x000A0780 / 0x000A0A20 / 0x000A0D00: a node's slots in order (the
// slot count +0xCC read once, each slot tested against the current size), a NiLODNode's children skipped (the
// "QDOutline" shells).
template <class Fn>
void ForEachChild(ni::NiAVObject* o, Fn&& fn) {
    auto* n = dynamic_cast<ni::NiNode*>(o);           // DynamicCast 0x002F9230
    if (!n || dynamic_cast<ni::NiLODNode*>(o)) return;   // DynamicCast 0x002F98A8
    const size_t count = n->Children().size();
    for (size_t i = 0; i < count; ++i) {
        if (i >= n->Children().size() || !n->Children()[i]) continue;
        if (!fn(n->Children()[i].Get())) return;
    }
}

// orig: 0x0009E4B0 (object, name): a string extra data (exactly RTTI 0x002F9B18) of the object whose string equals
// the name (_stricmp).
bool HasStringExtraData(const ni::NiAVObject* o, const char* name) {
    for (const ni::NiExtraData* e = o ? o->extraData.Get() : nullptr; e; e = e->nextData.Get())
        if (auto* s = dynamic_cast<const ni::NiStringExtraData*>(e); s && crt::StrICmp(s->string.c_str(), name) == 0)
            return true;
    return false;
}

// orig: 0x0009E260 (out, object, a, b): null when b > a; a geometry's parent (+0x14); a node's first child (slot
// order, the size +0xCC sampled once, each slot tested against the current size) with a result, the same a / b
// passed down (H: not incremented).
ni::NiAVObject* FindGeometryParent(ni::NiAVObject* o, s32 a, s32 b) {
    if (b > a || !o) return nullptr;
    if (dynamic_cast<ni::NiGeometry*>(o)) return o->parent;
    auto* n = dynamic_cast<ni::NiNode*>(o);          // IsKindOf(NiNode)
    if (!n) return nullptr;
    const size_t count = n->Children().size();
    for (size_t i = 0; i < count; ++i) {
        if (i >= n->Children().size() || !n->Children()[i]) continue;
        if (ni::NiAVObject* r = FindGeometryParent(n->Children()[i].Get(), a, b)) return r;
    }
    return nullptr;
}

// orig: the shape table DAT_002D6654 (count DAT_002D6780 = 38; 8-byte records {kind, code, ...}): entry k is the
// face's target k.
constexpr char kShapes[38][2] = {
    {'N', 'R'}, {'N', 'A'}, {'N', 'C'}, {'N', 'F'}, {'N', 'L'}, {'N', 'M'}, {'N', 'O'}, {'N', 'U'},
    {'A', 'R'}, {'A', 'A'}, {'A', 'C'}, {'A', 'F'}, {'A', 'L'}, {'A', 'M'}, {'A', 'O'}, {'A', 'U'},
    {'H', 'R'}, {'H', 'A'}, {'H', 'C'}, {'H', 'F'}, {'H', 'L'}, {'H', 'M'}, {'H', 'O'}, {'H', 'U'},
    {'E', 'Q'}, {'E', 'W'}, {'E', 'E'}, {'E', 'R'}, {'E', 'T'}, {'E', 'Y'}, {'E', 'U'}, {'E', 'I'},
    {'E', 'Z'}, {'E', 'X'}, {'E', 'C'}, {'E', 'V'}, {'E', 'B'}, {'E', 'N'},
};

// The table of a kind (the jump tables 0x0009DD28 / 0x0009DDE0 over kind - 'A'): 'N' 0, 'A' 1, 'H' 2, 'E' 3.
int TableOf(char kind) {
    switch (kind) {
    case 'N': return 0;
    case 'A': return 1;
    case 'H': return 2;
    case 'E': return 3;
    default: return -1;
    }
}

}  // namespace

// ---- the morph ----------------------------------------------------------------------------------------------------
void LipSyncMorph::Read(const std::vector<u8>& b, size_t& at) {   // orig: 0x0009F940
    positions.clear();                                // (+4 / +8 deleted, the count and the scale 0, the uvs cleared)
    deltas.clear();
    count = 0;
    scale = 0.0f;
    uvs.clear();                                      // 0x0009F200
    const u32 kind = Word(b, at);
    count = static_cast<s32>(Word(b, at));
    if (count > 0) {
        if (kind == 1) {                              // positions and uv sets
            positions.resize(static_cast<size_t>(count));
            for (ni::NiPoint3& p : positions) {
                p.x = std::bit_cast<f32>(Word(b, at));
                p.y = std::bit_cast<f32>(Word(b, at));
                p.z = std::bit_cast<f32>(Word(b, at));
            }
            const s32 sets = static_cast<s32>(Word(b, at));
            for (s32 s = 0; s < sets; ++s) {          // new(0xC) TextureCoords; key; count; the pairs; SetAt
                const s32 key = static_cast<s32>(Word(b, at));
                const s32 n = static_cast<s32>(Word(b, at));
                std::vector<ni::NiPoint2> uv(n > 0 ? static_cast<size_t>(n) : 0);
                for (ni::NiPoint2& q : uv) {
                    q.u = std::bit_cast<f32>(Word(b, at));
                    q.v = std::bit_cast<f32>(Word(b, at));
                }
                uvs[key] = std::move(uv);             // NiTStringMap::SetAt 0x0010BD70 (an older entry leaks)
            }
        } else {                                      // compressed offsets
            deltas.resize(static_cast<size_t>(count));
            for (Delta& d : deltas) {
                d.vertex = Half(b, at);
                d.x = static_cast<s16>(Half(b, at));
                d.y = static_cast<s16>(Half(b, at));
                d.z = static_cast<s16>(Half(b, at));
            }
        }
    }
    scale = std::bit_cast<f32>(Word(b, at));          // +0xC (always read)
}

s32 LipSyncMorph::CountVertices(ni::NiAVObject* o) {  // orig: 0x0009F490
    if (!o) return 0;
    s32 n = 0;
    if (auto* g = dynamic_cast<ni::NiGeometry*>(o))   // RTTI 0x0030EC0C (a geometry's count, not added)
        n = g->modelData ? g->modelData->vertexCount : 0;   // (data +8; no null test in the original)
    ForEachChild(o, [&](ni::NiAVObject* c) {
        n += CountVertices(c);
        return true;
    });
    return n;
}

bool LipSyncMorph::GatherPositions(ni::NiAVObject* o, u32& counter) {   // orig: 0x0009FBB0 (no second object)
    if (!o) return true;
    if (auto* g = dynamic_cast<ni::NiGeometry*>(o)) {
        if (ni::NiGeometryData* d = g->modelData.Get())
            for (u16 i = 0; i < d->vertexCount; ++i) {   // (the identity transform without a second object)
                if (counter < positions.size() && i < d->vertices.size()) positions[counter] = d->vertices[i];
                ++counter;
            }
    }
    bool ok = true;
    ForEachChild(o, [&](ni::NiAVObject* c) { return ok = GatherPositions(c, counter); });
    return ok;
}

bool LipSyncMorph::GatherUVs(ni::NiAVObject* o, u32& counter, const std::vector<u32>& first) {   // orig: 0x000A0A20
    if (!o) return true;
    if (auto* g = dynamic_cast<ni::NiGeometry*>(o)) {
        ni::NiGeometryData* d = g->modelData.Get();
        const u16 n = d ? d->vertexCount : 0;
        bool take = false;
        if (!first.empty()) take = std::find(first.begin(), first.end(), counter) != first.end();
        else take = HasStringExtraData(o, "NiData_EYEHERE=");   // 0x002A17D0
        if (take && d) {                              // new(0xC) 0x0009ED80(n, data +0x28): texture set 0
            std::vector<ni::NiPoint2> uv(n);
            for (u16 i = 0; i < n && i < d->uvs.size(); ++i) uv[i] = d->uvs[i];
            uvs[static_cast<s32>(counter)] = std::move(uv);
        }
        counter += n;
    }
    bool ok = true;
    ForEachChild(o, [&](ni::NiAVObject* c) { return ok = GatherUVs(c, counter, first); });
    return ok;
}

void LipSyncMorph::CopyPositions(const LipSyncMorph& base) {   // orig: 0x0009ED10
    for (s32 i = 0; i < count; ++i)
        if (static_cast<size_t>(i) < positions.size() && static_cast<size_t>(i) < base.positions.size())
            positions[static_cast<size_t>(i)] = base.positions[static_cast<size_t>(i)];
}

void LipSyncMorph::CopyUVs(const LipSyncMorph& base) {   // orig: 0x0009F620
    uvs.clear();                                      // 0x0009F200
    if (base.uvs.empty() || count <= 0) return;       // (base +0x14, this +0)
    for (const auto& [key, uv] : base.uvs) uvs[key] = uv;   // new(0xC) + 0x0009EDE0 copy; SetAt
}

void LipSyncMorph::AddTarget(const LipSyncMorph& t, f32 w) {   // orig: 0x0009EEA0
    if (w == 0.0f) return;                            // FCOMP [0x00293AB8]; TEST AH,0x44; JNP
    // (unrolled by four in the original, the same order and the same stop)
    for (s32 i = 0; i < t.count && static_cast<size_t>(i) < t.deltas.size(); ++i) {
        const Delta& d = t.deltas[static_cast<size_t>(i)];
        const double f = 1.0 / double(t.scale);       // FLD +0xC; FDIVR [0x00293A98] (each delta)
        const double x = double(d.x) * f;             // FILD; FMUL ST(1) (kept)
        const f32 y = F(double(d.y) * f);             // FSTP [esp+0x14]
        const f32 z = F(double(d.z) * f);             // FSTP [esp+0x18]
        const double xw = x * w;                      // FMUL [w] (kept)
        const f32 yw = F(double(y) * w);
        const f32 zw = F(double(z) * w);
        if (d.vertex >= count) return;                // CMP EDX, [ECX]; JGE: the rest is not added
        ni::NiPoint3& p = positions[d.vertex];
        p.x = F(xw + p.x);                            // FADD; FSTP
        p.y = F(double(yw) + p.y);
        p.z = F(double(zw) + p.z);
    }
}

void LipSyncMorph::ApplyPositions(ni::NiAVObject* o, u32& counter) const {   // orig: 0x000A0780
    if (!o) return;
    if (auto* g = dynamic_cast<ni::NiGeometry*>(o); g && g->modelData) {
        ni::NiGeometryData& d = *g->modelData;
        const u16 n = d.vertexCount;
        u16 i = 0;
        // the equal prefix only advances the counter (FCOMP; TEST AH,0x44; JP: a difference or a NaN)
        for (; i < n; ++i, ++counter) {
            if (counter >= positions.size() || i >= d.vertices.size()) return;   // (port guard)
            const ni::NiPoint3& v = d.vertices[i];
            const ni::NiPoint3& p = positions[counter];
            if (!(v.x == p.x) || !(v.y == p.y) || !(v.z == p.z)) break;
        }
        if (i < n) {                                  // from the first difference on, every vertex is written
            for (; i < n; ++i, ++counter)
                if (counter < positions.size() && i < d.vertices.size()) d.vertices[i] = positions[counter];
            d.revisionFlags |= 1;                     // data +0x2E |= 1 (positions changed)
        }
    }
    ForEachChild(o, [&](ni::NiAVObject* c) {
        ApplyPositions(c, counter);
        return true;
    });
}

void LipSyncMorph::ApplyUVs(ni::NiAVObject* o, u32& counter) const {   // orig: 0x000A0D00
    if (!o) return;
    if (auto* g = dynamic_cast<ni::NiGeometry*>(o); g && g->modelData) {
        ni::NiGeometryData& d = *g->modelData;
        const u16 n = d.vertexCount;
        if (!uvs.empty()) {                           // the map's count +0x14
            const auto it = uvs.find(static_cast<s32>(counter));   // 0x001077C0
            if (it != uvs.end()) {
                const std::vector<ni::NiPoint2>& tc = it->second;
                for (u16 i = 0; i < n; ++i) {         // (unrolled by four in the original)
                    if (i >= tc.size() || i >= d.uvs.size()) break;   // (port guard)
                    d.uvs[i].u = F(double(tc[i].u) + uvOffset.u);   // FADD +0x20
                    d.uvs[i].v = F(double(tc[i].v) + uvOffset.v);   // FADD +0x24
                }
                d.revisionFlags |= 8;                 // data +0x2E |= 8 (uvs changed)
            }
        }
        counter += n;
    }
    ForEachChild(o, [&](ni::NiAVObject* c) {
        ApplyUVs(c, counter);
        return true;
    });
}

// ---- the face -----------------------------------------------------------------------------------------------------
LipSyncFace::LipSyncFace() {                          // orig: 0x0009DC60
    std::memset(tables, -1, sizeof tables);           // REP STOSD 0xFFFFFFFF
    for (s32 k = 0; k < 38; ++k) {
        const int t = TableOf(kShapes[k][0]);
        if (t >= 0) tables[t][static_cast<u8>(kShapes[k][1])] = static_cast<s8>(k);
    }
}

ni::NiAVObject* LipSyncFace::FindMeshNode(ni::NiAVObject* model) {   // orig: 0x0009E640
    // the model's first NiNode child (by its RTTI chain) whose name (5 characters or more) ends in "_Mesh"
    // (_stricmp, 0x002A17B0), else 0x0009E260(model, 2, 0): the parent of the first geometry depth first (Fry's
    // HUD_Head: the root "NR" of eight NiTriShapes)
    if (auto* n = dynamic_cast<ni::NiNode*>(model)) {
        const size_t count = n->Children().size();
        for (size_t i = 0; i < count; ++i) {
            ni::NiAVObject* c = i < n->Children().size() ? n->Children()[i].Get() : nullptr;
            if (!c || c->name.empty() || !dynamic_cast<ni::NiNode*>(c)) continue;
            const std::string& s = c->name;
            if (s.size() >= 5 && crt::StrICmp("_Mesh", s.c_str() + s.size() - 5) == 0) return c;
        }
    }
    return FindGeometryParent(model, 2, 0);
}

std::unique_ptr<LipSyncFace> LipSyncFace::Create(const ni::NiUDSFileObject* file, ni::NiAVObject* model) {
    // orig: 0x0009DE30 -> Load 0x0009E810
    if (!file) return nullptr;
    const std::vector<u8>& b = file->data;            // (ByteReader::Attach: a private copy)
    auto face = std::make_unique<LipSyncFace>();
    ni::NiAVObject* mesh = FindMeshNode(model);
    char header[33] = {};                             // the 32-byte magic
    if (b.size() >= 32) std::memcpy(header, b.data(), 32);
    size_t at = 32;
    if (std::strcmp(header, "LipSyncFaceFile v1.11") == 0) face->version = 4;        // 0x002A1790
    else if (std::strcmp(header, "LipSyncFaceFile v1.1") == 0) face->version = 3;    // 0x002A1770
    else if (std::strcmp(header, "LipSyncFaceFile v1.02") == 0) face->version = 1;   // 0x002A1730
    else if (std::strcmp(header, "LipSyncFaceFile v1.03") == 0) face->version = 2;   // 0x002A1750
    else return nullptr;
    if (face->version >= 2) face->mode = Word(b, at);   // data +0x10
    bool indices = face->version == 3;
    if (face->version >= 4) {
        const u32 stored = Word(b, at);
        indices = stored == 0;                        // SETE [esp+0x13]
        if (stored) {                                 // the base morph stored in the file
            face->morph = std::make_unique<LipSyncMorph>();   // new(0x28) 0x0009F2F0
            face->morph->Read(b, at);                 // 0x0009F940
        }
    }
    if (indices) {
        // orig: n, then n vertex numbers; LipSync_BuildMorphFromIndices 0x000A15A0(mesh as NiAVObject, list, n)
        const u32 n = Word(b, at);
        std::vector<u32> first(n);
        for (u32& v : first) v = Word(b, at);
        if (mesh) {
            const s32 total = LipSyncMorph::CountVertices(mesh);   // 0x0009F490
            auto m = std::make_unique<LipSyncMorph>();   // new(0x28) 0x0009F350(total)
            m->count = total;
            if (total > 0) m->positions.resize(static_cast<size_t>(total));
            mesh->local.translate = {0.0f, 0.0f, 0.0f};   // DAT_002F915C (the zero vector)
            mesh->UpdateDownwardPass(0.0f, true);         // NiAVObject::Update(0, 1) 0x001018D0
            u32 counter = 0;
            if (m->GatherPositions(mesh, counter) && static_cast<s32>(counter) == m->count) {   // 0x0009FBB0
                counter = 0;
                m->GatherUVs(mesh, counter, first);      // 0x000A0A20
                m->deltas.clear();                       // +8 = 0
                face->morph = std::move(m);
            }
        }
    }
    s32 targets = static_cast<s32>(Word(b, at)) + 1;   // data +0xC = the file's count + 1
    if (!face->morph) {
        // orig: LipSync_BuildMorph 0x000A1110(mesh, 0, 0) (the v1.02 / v1.03 faces, a failed index build)
        NotPorted("LipSyncFace::Load: LipSync_BuildMorph 0x000A1110 (a v1.02 / v1.03 face, or no morph from the "
                  "vertex indices)");
        return nullptr;                               // (its failure: the face data deleted, Load fails)
    }
    if (targets < 1) targets = 1;                     // (port guard: new(count * 4) of a negative count)
    face->targets.resize(static_cast<size_t>(targets));
    face->targets[0] = std::make_unique<LipSyncMorph>(*face->morph);   // new(0x28) 0x000A1080 (0x0009F720 copy)
    for (s32 i = 1; i < targets; ++i) {
        face->targets[static_cast<size_t>(i)] = std::make_unique<LipSyncMorph>();   // new(0x28) 0x0009F2F0
        face->targets[static_cast<size_t>(i)]->Read(b, at);                          // 0x0009F940
    }
    face->mesh = mesh;                                // data +0
    return face;
}

s32 LipSyncFace::Lookup(char kind, char code) const {   // orig: 0x0009DD80
    const int t = TableOf(kind);
    return t < 0 ? -1 : tables[t][static_cast<u8>(code)];   // MOVSX
}

void LipSyncFace::Begin() {                           // orig: 0x0009E140
    if (!morph || targets.empty() || !targets[0]) return;
    morph->CopyPositions(*targets[0]);                // 0x0009ED10
    morph->CopyUVs(*targets[0]);                      // 0x0009F620
}

void LipSyncFace::SetUVOffset(const ni::NiPoint2& o) {   // orig: 0x0009F1E0
    if (morph) morph->uvOffset = o;
}

void LipSyncFace::End() {                             // orig: 0x0009E470
    if (!morph) return;
    u32 counter = 0;
    morph->ApplyPositions(mesh.Get(), counter);       // 0x000A1700 -> 0x000A0780
    counter = 0;
    morph->ApplyUVs(mesh.Get(), counter);             // 0x000A1780 -> 0x000A0D00
}

void LipSyncFace::AddTarget(s32 index, f32 weight) {   // orig: 0x0009E170
    if (index < 0 || static_cast<size_t>(index) >= targets.size()) return;   // (no bounds check in the original)
    if (const LipSyncMorph* t = targets[static_cast<size_t>(index)].get(); t && morph) morph->AddTarget(*t, weight);
}

void LipSyncFace::Expression(char code, f32 weight) {   // orig: 0x0009E120
    const s32 i = Lookup('E', code);
    if (i > 0) AddTarget(i, weight);
}

void LipSyncFace::Mouth(f32 turn, char a, f32 wa, char b, f32 wb) {   // orig: 0x0009DFE0
    const char side = turn < 0.0f ? 'A' : 'H';        // FCOMP [0.0]; TEST AH,5; JNP ('H' for 0 and a NaN)
    const f32 at = turn > 0.0f ? turn : -turn;        // TEST AH,0x41; JNE: FCHS (0 becomes -0)
    if (const s32 i = Lookup(side, 'R'); i > 0)       // the turned rest shape
        AddTarget(i, F(((1.0 - double(wa)) + (1.0 - double(wb))) * at * 0.5f));   // [0x00293AA0]
    if (const s32 i = Lookup(side, a); i > 0) AddTarget(i, F(double(at) * wa * 0.5f));
    if (const s32 i = Lookup(side, b); i > 0) AddTarget(i, F(double(at) * wb * 0.5f));
    if (const s32 i = Lookup('N', a); i > 0) AddTarget(i, F((1.0 - double(at)) * wa));
    if (const s32 i = Lookup('N', b); i > 0) AddTarget(i, F((1.0 - double(at)) * wb));
}

// ---- the key file's evaluation --------------------------------------------------------------------------------------
void EvaluateKeyFile(const LipSyncKeyFile& key, LipSyncFace& face, f32 t) {   // orig: 0x0009D2D0
    face.Begin();                                     // 0x0009DD50
    char codes[12] = {};
    f32 values[10] = {};
    const s32 n = key.Channels(codes, values, 10, t);   // 0x0009CFD0
    f32 turn = 0.0f;                                  // (the face argument's stack slot, zeroed)
    if (n > 0) {
        for (s32 i = 0; i < n; ++i) {
            const char c = codes[i];
            if (c == 'A') {
                turn = F(double(turn) - values[i]);   // FSUB
            } else if (c == 'H') {
                turn = F(double(turn) + values[i]);   // FADD
            } else {
                f32 v = values[i];
                if (v < 0.0f) v = 0.0f;               // TEST AH,5; JP
                else if (v > 1.0f) v = 1.0f;          // TEST AH,0x41; JNE
                face.Expression(c, v);                // 0x0009E120
            }
        }
        if (turn < -1.0f) turn = -1.0f;               // [0x00293BF0]; TEST AH,5; JP
        else if (turn > 1.0f) turn = 1.0f;            // TEST AH,0x41; JNE
    }
    char a = 'R', b = 'R';                            // 0x52
    f32 wa = 0.0f, wb = 0.0f;
    const s32 i = key.FindPhoneme(t);                 // 0x0009CAB0
    const s32 count = static_cast<s32>(key.records1.size());
    auto rec = [&](s32 k) -> const LipSyncKeyFile::Record1& { return key.records1[static_cast<size_t>(k)]; };
    if (i == -1) {
        // the rest shape, no weight
    } else if (face.mode == 1) {                      // (+0)+0x10: the phoneme alone
        a = b = static_cast<char>(rec(i).a);
        wa = 1.0f;
    } else if (i == count - 1) {
        a = b = static_cast<char>(rec(i).a);
        wa = wb = std::bit_cast<f32>(rec(i).c);
    } else {
        const LipSyncKeyFile::Record1& p = rec(i);
        const LipSyncKeyFile::Record1& q = rec(i + 1);
        const f32 tp = std::bit_cast<f32>(p.b), tq = std::bit_cast<f32>(q.b);
        const double u = std::fabs(double(t) - tq) / std::fabs(double(tq) - tp);   // FABS; FDIVP
        double s;                                     // the smooth curve inline, on the unrounded fraction
        if (u < 0.5 || u == 0.5) {                    // FCOM [0x00293AA0]; TEST AH,0x41; JP
            s = u * u;
            s = s + s;
        } else {
            const double r = 1.0 - u;
            s = r * r;
            s = 1.0 - (s + s);
        }
        a = static_cast<char>(p.a);
        wa = F(s * std::bit_cast<f32>(p.c));
        b = static_cast<char>(q.a);
        wb = F((1.0 - s) * std::bit_cast<f32>(q.c));
    }
    face.SetUVOffset(key.EyeOffset(t));               // 0x0009D0C0, 0x0009DD60
    face.Mouth(turn, a, wa, b, wb);                   // 0x0009DFE0
    face.End();                                       // 0x0009DD70
}

// ---- the player ---------------------------------------------------------------------------------------------------
LipSyncPlayer::~LipSyncPlayer() {                     // orig: 0x0009D7D0
    for (LipTrack* t : tracks) t->player = nullptr;   // (port: see `tracks`)
    tracks.clear();
    Stop();
    keyFile = nullptr;
    face.reset();                                     // 0x0009DE10, delete
}

bool LipSyncPlayer::SetUp(const ni::NiUDSFileObject* file, ni::NiAVObject* model) {   // orig: 0x0009D880
    face = LipSyncFace::Create(file, model);          // +8
    return face != nullptr;
}

void LipSyncPlayer::SetKeyFile(LipSyncKeyFile* k) {   // orig: 0x0009D9A0
    if (keyFile.Get() != k) keyFile = k;
}

void LipSyncPlayer::Stop() {                          // orig: 0x0009D710
    if (face && state != 1) {
        face->Begin();                                // 0x0009DD50
        face->End();                                  // 0x0009DD70
    }
    state = 1;
    time = 0.0f;
    flag14 = 0;
}

void LipSyncPlayer::Update(f32 dt) {                  // orig: 0x0009D960
    if (!keyFile || state == 1) return;
    if (face) EvaluateKeyFile(*keyFile, *face, time);   // 0x0009D2D0(face, +0xC)
    if (state != 2) time = F(double(dt) + time);      // FLD dt; FADD +0xC; FSTP
}

}  // namespace fut::game
