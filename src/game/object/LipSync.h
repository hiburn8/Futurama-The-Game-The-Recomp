// The lip-sync face: a face morph built from a character's "LipSyncFaceFile" (the morph targets of the mouth shapes,
// the head-turn variants, the expressions and the eyes' uv offset) and the player that plays a key file on it.
// Decompiled from default.xbe 0x0009D2D0-0x000A1780 (renderer_plan.md 19).
//
// A face (LipSyncFace::Create 0x0009DE30) is made for a character's model when its definition has a "lipsync*" file
// (Character::Init: LipSyncPlayer::SetUp 0x0009D880 on the character's player at +0x40) and for the HUD head
// (HUDHead's model, Hud.cpp). Its morph object holds the base positions of every geometry vertex below the mesh node
// (subtree pre-order, NiLODNodes skipped), and the uvs of the geometries the file names (the eyes); each morph target
// is a list of compressed vertex offsets. A frame's evaluation (0x0009D2D0) starts from the base (Begin), adds the
// targets the key file's channels and phonemes weight at the time, sets the eyes' uv offset and writes the result
// into the geometry data (End: the changed positions, the uvs, the data's revision bits 1 / 8).
//
// NOT PORTED: LipSync_BuildMorph 0x000A1110 (the v1.02 / v1.03 faces and the fallback when the vertex indices do not
// build a morph; no shipped file needs it: every face file is a v1.11 with vertex indices), the second-object
// branch of 0x0009FBB0 (a transform between two meshes, only used by 0x000A1110).
#pragma once

#include "engine/ni/NiObjects.h"
#include "game/defs/CharacterDef.h"

#include <map>
#include <memory>
#include <vector>

namespace fut::game {

class LipTrack;

// orig: the morph object and the morph targets share one class (0x28 bytes; ctors 0x0009F2F0 (empty), 0x0009F350
// (n vertices), 0x000A1080 (a copy, 0x0009F720); dtor 0x0009F410).
struct LipSyncMorph {
    struct Delta {                                    // orig: 8 bytes: the vertex number and its offset in steps
        u16 vertex = 0;
        s16 x = 0, y = 0, z = 0;
    };
    s32 count = 0;                                    // orig: +0x00 (vertices or deltas)
    std::vector<ni::NiPoint3> positions;              // orig: +0x04 (absolute positions: the morph, the base)
    std::vector<Delta> deltas;                        // orig: +0x08 (the targets)
    f32 scale = 0.0f;                                 // orig: +0x0C (a delta step is 1 / scale)
    // orig: +0x10, a NiTMap<int, TextureCoords*> (37 buckets): the uvs of a geometry by its first vertex number.
    std::map<s32, std::vector<ni::NiPoint2>> uvs;
    ni::NiPoint2 uvOffset;                            // orig: +0x20 / +0x24 (the morph's; set before every End)

    // orig: 0x0009F940: kind, count, then the positions and uv sets (kind 1) or the deltas, then the scale word.
    void Read(const std::vector<u8>& file, size_t& at);
    // orig: 0x0009F490: the vertex count of the geometries below `o` (a geometry's own; NiLODNodes skipped).
    static s32 CountVertices(ni::NiAVObject* o);
    // orig: 0x0009FBB0 (o, nullptr, &counter): the geometries' vertex positions in pre-order from `counter` on.
    bool GatherPositions(ni::NiAVObject* o, u32& counter);
    // orig: 0x000A0A20: the uvs of the geometries whose first vertex number is listed (or, without a list, that carry
    // the string extra data "NiData_EYEHERE=").
    bool GatherUVs(ni::NiAVObject* o, u32& counter, const std::vector<u32>& first);
    void CopyPositions(const LipSyncMorph& base);     // orig: 0x0009ED10
    void CopyUVs(const LipSyncMorph& base);           // orig: 0x0009F620
    void AddTarget(const LipSyncMorph& target, f32 weight);   // orig: 0x0009EEA0
    void ApplyPositions(ni::NiAVObject* o, u32& counter) const;   // orig: 0x000A0780
    void ApplyUVs(ni::NiAVObject* o, u32& counter) const;         // orig: 0x000A0D00
};

// orig: LipSyncFace (0x404 bytes, ctor 0x0009DC60): +0 the face data (0x14 bytes, LipSyncFace::Load 0x0009E810),
// then four 256-entry tables of target numbers by shape code ('N' +4, 'A' +0x104, 'H' +0x204, 'E' +0x304) filled
// from the shape table DAT_002D6654 (38 entries {kind, code}; entry k is target k, 0 = 'N' 'R' the base).
struct LipSyncFace {
    // ---- the face data (+0)
    ni::NiPointer<ni::NiAVObject> mesh;               // +0x00 (the "*_Mesh" node; the HUD head re-finds it)
    std::unique_ptr<LipSyncMorph> morph;              // +0x04
    std::vector<std::unique_ptr<LipSyncMorph>> targets;   // +0x08 / +0x0C (count: the file's + 1; 0 the base copy)
    u32 mode = 0;                                     // +0x10 (the file word: 1 = no blend between the phonemes)
    u32 version = 0;                                  // (port: 4 v1.11, 3 v1.1, 1 v1.02, 2 v1.03)
    s8 tables[4][256];                                // N, A, H, E

    LipSyncFace();                                    // orig: 0x0009DC60
    // orig: LipSyncFace::Create 0x0009DE30 (file, model): the file's private copy (ByteReader::Attach 0x0009DBB0),
    // new(0x404), Load 0x0009E810; null when Load fails.
    static std::unique_ptr<LipSyncFace> Create(const ni::NiUDSFileObject* file, ni::NiAVObject* model);
    // orig: LipSync_FindMeshNode 0x0009E640 (model).
    static ni::NiAVObject* FindMeshNode(ni::NiAVObject* model);

    s32 Lookup(char kind, char code) const;           // orig: 0x0009DD80 (-1: none)
    void Begin();                                     // orig: 0x0009DD50 -> 0x0009E140
    void SetUVOffset(const ni::NiPoint2& o);          // orig: 0x0009DD60 -> 0x0009E190 -> 0x0009F1E0
    void End();                                       // orig: 0x0009DD70 -> 0x0009E470
    void AddTarget(s32 index, f32 weight);            // orig: 0x0009E170
    void Expression(char code, f32 weight);           // orig: 0x0009E120 ('E' shapes)
    // orig: 0x0009DFE0 (turn, phoneme a, weight a, phoneme b, weight b): the mouth, split between the plain shapes
    // ('N') and the turned ones ('A' for a turn below 0, 'H' else) by |turn|.
    void Mouth(f32 turn, char a, f32 wa, char b, f32 wb);
};

// orig: 0x0009D2D0 (thiscall key file; face, t): the key file's state at t applied to the face.
void EvaluateKeyFile(const LipSyncKeyFile& key, LipSyncFace& face, f32 t);

// orig: the lip-sync player of a character (Character +0x40, 0x18 bytes, vtable 0x002A15EC, ctor 0x0009D760, dtor
// 0x0009D7D0). Its state (+0x10) is what FsIsCharacterSpeaking (148) tests: 0 while a key file plays.
class LipSyncPlayer {
public:
    LipSyncPlayer() = default;
    LipSyncPlayer(const LipSyncPlayer&) = delete;
    LipSyncPlayer& operator=(const LipSyncPlayer&) = delete;
    ~LipSyncPlayer();                                 // orig: 0x0009D7D0 (Stop, the key file and the face released)

    // orig: 0x0009D880 (file, model): the face of the model; false (and no face) when the file builds none.
    bool SetUp(const ni::NiUDSFileObject* file, ni::NiAVObject* model);
    void SetKeyFile(LipSyncKeyFile* k);               // orig: 0x0009D9A0
    void Play() { state = 0; }                        // orig: 0x0009DA20
    void Pause() { state = 2; }                       // orig: 0x0009D750
    void Stop();                                      // orig: 0x0009D710 (the face back to its base unless stopped)
    void Update(f32 dt);                              // orig: 0x0009D960

    ni::NiPointer<LipSyncKeyFile> keyFile;            // orig: +0x04
    std::unique_ptr<LipSyncFace> face;                // orig: +0x08
    f32 time = 0.0f;                                  // orig: +0x0C
    u32 state = 1;                                    // orig: +0x10: 1 stopped, 0 playing, 2 paused
    u8 flag14 = 0;                                    // orig: +0x14 (cleared by Stop; no reader found)
    // (port: the dialogue tracks that point at this player; its destructor detaches them, a UB guard for a
    // dialogue that outlives its character)
    std::vector<LipTrack*> tracks;
};

}  // namespace fut::game
