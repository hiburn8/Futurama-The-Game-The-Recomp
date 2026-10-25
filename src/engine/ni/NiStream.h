// NetImmerse 4.2 NIF streaming (NiStream), decompiled from default.xbe (NiStream TU 0x00108470-0x00109400).
//   orig: 0x00109230 NiStream::NiStream, 0x00108EA0 Load(NiBinaryStream*), 0x00108E10 LoadStream,
//         0x001086B0 LoadHeader, 0x00108D50 LoadObject, 0x001087A0 LoadTopLevelObjects,
//         0x00108570 ReadLinkID, 0x001085C0 ReadMultipleLinkIDs, 0x00108940 string read, 0x00108860 RegisterLoader
//
// File layout (version 4.2.1.0 on the Xbox disc):
//   text line "NetImmerse File Format, Version x.x.x.x\n"   (NiBinaryStream::GetLine, max 127 chars)
//   u32 version, u32 objectCount
//   objectCount x { u32 len, char className[len], <class LoadBinary payload> }
//   u32 topLevelCount, topLevelCount x u32 linkID       (linkID = load-order index, 0xFFFFFFFF = null)
//
// Objects are indexed in the order NiObject::LoadBinary registers them (= file order). Link IDs are read during
// LoadBinary and resolved in the LinkObject pass (vtbl+0x10), which runs over all objects in load order. The port
// keeps the raw IDs in each object and resolves them into pointers there (only for the classes ported so far).
// Verified per block against the original under emulation (tools/difftest/nif_oracle.py).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiRefObject.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace fut::ni {

class NiObject;
using NiObjectPtr = NiPointer<NiObject>;

// orig: FUN_00107750 parses "a.b.c.d" into a<<24 | b<<16 | c<<8 | d.
constexpr u32 NiVersion(u32 a, u32 b, u32 c, u32 d) { return a << 24 | b << 16 | c << 8 | d; }

constexpr u32 kNullLink = 0xFFFFFFFFu;

// orig: g_NiStreamError (0x002F9278) codes; Unsupported is port-only (see Fail()).
enum class NiStreamError : u32 {
    None = 0,
    NoStream = 1,
    NotNif = 2,          // "is not a NIF file"
    TooOld = 3,          // "NIF version is too old."
    TooNew = 4,          // "Unknown NIF version."
    UnknownClass = 5,    // "cannot find create function <class>"
    Unsupported = 100,   // port-only: input the original could not have loaded either (it would crash)
};

class NiStream {
public:
    // orig: g_NiStreamMinVersion / g_NiStreamMaxVersion, set by static initializers in default.xbe.
    static constexpr u32 kMinVersion = NiVersion(3, 3, 0, 11);
    static constexpr u32 kMaxVersion = NiVersion(4, 2, 1, 0);

    using CreateFn = NiObject* (*)();
    // orig: 0x00108860 NiStream::RegisterLoader. The port registers the classes it implements on first use.
    static void RegisterLoader(const char* className, CreateFn create);
    static CreateFn FindLoader(const std::string& className);

    // orig: 0x00108EA0 Load(NiBinaryStream*) + 0x00108E10 LoadStream (load part; link passes not ported yet).
    bool Load(std::span<const u8> data);

    u32 FileVersion() const { return version_; }       // orig: this+0x04
    u32 ObjectCount() const { return objectCount_; }   // orig: this+0x08 (header value)
    NiStreamError Error() const { return error_; }
    const std::string& ErrorMessage() const { return errorMessage_; }
    const std::string& LastClass() const { return lastClass_; }   // orig: g_NiStreamLastClass
    const std::vector<NiObjectPtr>& Objects() const { return objects_; }
    // The references to the file's dynamic effects dropped, in the object order (the original's ClearObjects releases
    // every object; a file the port keeps for the raw pointers into it must not keep its lights alive: the effect
    // ids, renderer_plan.md 18).
    void ReleaseDynamicEffects();
    // orig: the end of LoadStream 0x00108E10 / of the incremental link step 0x00108F40: 0x000D2380 on the object array
    // (+0x30) releases every object's reference in load order, so what only the file held dies before any caller
    // sees the stream (the top-level array keeps its objects). The port keeps its object array (the link ids index
    // it, the tests inspect it, code points into kept files) and releases here only the dynamic effects that are not
    // top-level objects: their ids (and their leaving the effect lists) are what the order makes visible
    // (renderer_plan.md 19; the stream manager calls it after each load). L: the other objects only the file held
    // (unused textures, ...) die there in the original too.
    void ReleaseLoadReferences();
    const std::vector<u32>& TopLevelLinks() const { return topLevel_; }
    size_t Position() const { return pos_; }

    // ---- used by LoadBinary implementations ------------------------------------------------------------------
    // orig: istr->Read(buf, n) (NiBinaryStream vtbl+8). Short reads zero-fill the rest and set Truncated().
    size_t Read(void* dst, size_t n);
    template <class T>
    T Read() {
        T v{};
        Read(&v, sizeof(T));
        return v;
    }
    u8 ReadU8() { return Read<u8>(); }
    u16 ReadU16() { return Read<u16>(); }
    u32 ReadU32() { return Read<u32>(); }
    f32 ReadF32() { return Read<f32>(); }
    bool ReadBool() { return ReadU8() != 0; }           // NiBool stored as one byte (version >= 4.1.0.0)
    void ReadString(std::string& out);                   // orig: 0x00108940 (u32 length + chars)
    u32 ReadLinkID();                                    // orig: 0x00108570
    void ReadMultipleLinkIDs(std::vector<u32>& out);     // orig: 0x001085C0 (u32 count + count x u32)
    void RegisterObject(NiObject* object);               // orig: 0x00108C00 (called by NiObject::LoadBinary)
    // orig: 0x00108500: the last registered object replaced by `object` (a shared texture / pixel data).
    void ReplaceLastObject(NiObject* object);
    // orig: 0x00107890 GetObjectFromLinkID (used by LinkObject): 0xFFFFFFFF -> null. The original indexes the object
    // array without a bounds check; the port fails the load instead.
    NiObject* ResolveLink(u32 id);
    void Fail(NiStreamError code, std::string message);
    bool Failed() const { return error_ != NiStreamError::None; }
    bool Truncated() const { return truncated_; }

private:
    bool LoadHeader();            // orig: 0x001086B0
    bool LoadObject();            // orig: 0x00108D50
    void LoadTopLevelObjects();   // orig: 0x001087A0
    size_t GetLine(char* buf, size_t max);   // orig: 0x00145430 NiBinaryStream::GetLine

    std::span<const u8> data_;
    size_t pos_ = 0;
    bool truncated_ = false;
    u32 version_ = 0;
    u32 objectCount_ = 0;
    NiStreamError error_ = NiStreamError::None;
    std::string errorMessage_;
    std::string lastClass_;
    std::vector<NiObjectPtr> objects_;   // orig: NiTArray<NiObjectPtr> at this+0x34
    std::vector<u32> topLevel_;
};

}  // namespace fut::ni
