// Decompiled from default.xbe NiStream TU (0x00108470-0x00109400). See NiStream.h for the file layout.
#include "engine/ni/NiStream.h"

#include "engine/ni/NiObjects.h"

#include <cstring>
#include <map>
#include <mutex>

namespace fut::ni {
namespace {

std::map<std::string, NiStream::CreateFn>& Loaders() {   // orig: g_NiStreamLoaderMap (0x002F9480)
    static std::map<std::string, NiStream::CreateFn> loaders;
    return loaders;
}

void EnsureLoaders() {
    static std::once_flag once;
    std::call_once(once, RegisterNiLoaders);
}

}  // namespace

void NiStream::RegisterLoader(const char* className, CreateFn create) { Loaders()[className] = create; }

NiStream::CreateFn NiStream::FindLoader(const std::string& className) {
    EnsureLoaders();
    auto it = Loaders().find(className);
    return it == Loaders().end() ? nullptr : it->second;
}

size_t NiStream::Read(void* dst, size_t n) {
    const size_t avail = pos_ < data_.size() ? data_.size() - pos_ : 0;
    const size_t k = n < avail ? n : avail;
    if (k) std::memcpy(dst, data_.data() + pos_, k);
    if (k < n) {
        // The original's destination keeps whatever it held (often uninitialized memory); the port zero-fills.
        std::memset(static_cast<u8*>(dst) + k, 0, n - k);
        truncated_ = true;
    }
    pos_ += k;
    return k;
}

// orig: 0x00108940. The original allocates len + 1 bytes and reads len characters.
void NiStream::ReadString(std::string& out) {
    const u32 len = ReadU32();
    const size_t avail = pos_ < data_.size() ? data_.size() - pos_ : 0;
    if (len > avail) {
        // A length past the end would make the original allocate and read garbage; nothing shipped does this.
        Fail(NiStreamError::Unsupported, "string length past end of stream");
        out.clear();
        pos_ = data_.size();
        truncated_ = true;
        return;
    }
    out.assign(reinterpret_cast<const char*>(data_.data() + pos_), len);
    pos_ += len;
}

// orig: 0x00108570. The original appends the ID to the link list at this+0x68 and LinkObject consumes it later.
u32 NiStream::ReadLinkID() { return ReadU32(); }

// orig: 0x001085C0. Count goes to the list at this+0x78, IDs to this+0x68.
void NiStream::ReadMultipleLinkIDs(std::vector<u32>& out) {
    const u32 count = ReadU32();
    out.clear();
    for (u32 i = 0; i < count && !truncated_; ++i) out.push_back(ReadU32());
}

void NiStream::ReleaseDynamicEffects() {
    for (NiObjectPtr& o : objects_)
        if (dynamic_cast<NiDynamicEffect*>(o.Get())) o = nullptr;
}

NiObject* NiStream::ResolveLink(u32 id) {
    if (id == kNullLink) return nullptr;
    if (id >= objects_.size()) {
        Fail(NiStreamError::Unsupported, "link ID out of range");
        return nullptr;
    }
    return objects_[id].Get();
}

// orig: 0x00108C00 (NiTArray<NiObjectPtr>::SetAtGrow at the current count)
void NiStream::RegisterObject(NiObject* object) { objects_.emplace_back(object); }
void NiStream::ReplaceLastObject(NiObject* object) {   // orig: 0x00108500 (SetAt(size - 1))
    if (!objects_.empty()) objects_.back() = object;
}

void NiStream::Fail(NiStreamError code, std::string message) {
    if (error_ == NiStreamError::None) {
        error_ = code;
        errorMessage_ = std::move(message);
    }
}

// orig: 0x00145430 NiBinaryStream::GetLine. Reads byte by byte up to '\n' or end of stream; '\r' is skipped and
// at most max-1 characters are stored (the loop stops before consuming more once the buffer is full).
size_t NiStream::GetLine(char* buf, size_t max) {
    size_t stored = 0, consumed = 0;
    for (size_t slot = 1; slot < max;) {
        u8 c;
        if (Read(&c, 1) != 1) break;
        ++consumed;
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[slot - 1] = static_cast<char>(c);
        ++stored;
        ++slot;
    }
    buf[stored] = 0;
    return consumed;
}

// orig: 0x001086B0
bool NiStream::LoadHeader() {
    char line[0x80];
    GetLine(line, sizeof(line));
    if (std::strstr(line, "NetImmerse File Format") != line) {
        Fail(NiStreamError::NotNif, "is not a NIF file");
        return false;
    }
    version_ = ReadU32();
    if (version_ < kMinVersion) {
        Fail(NiStreamError::TooOld, "NIF version is too old.");
        return false;
    }
    if (version_ > kMaxVersion) {
        Fail(NiStreamError::TooNew, "Unknown NIF version.");
        return false;
    }
    objectCount_ = ReadU32();
    return true;
}

// orig: 0x00108D50
bool NiStream::LoadObject() {
    const u32 len = ReadU32();
    // The original reads the name into a fixed stack buffer without a bound; the port rejects absurd lengths.
    if (len >= 0x100) {
        Fail(NiStreamError::Unsupported, "class name too long");
        return false;
    }
    char name[0x100];
    Read(name, len);
    name[len] = 0;
    lastClass_ = name;
    const CreateFn create = FindLoader(lastClass_);
    if (!create) {
        Fail(NiStreamError::UnknownClass, "cannot find create function " + lastClass_);
        return false;
    }
    NiObject* object = create();
    NiObjectPtr hold(object);          // freed if LoadBinary never registers it (only on a failed load)
    object->LoadBinary(*this);
    return !Failed();
}

// orig: 0x001087A0. Link IDs index the object array; 0xFFFFFFFF = null.
void NiStream::LoadTopLevelObjects() {
    const u32 count = ReadU32();
    topLevel_.clear();
    for (u32 i = 0; i < count && !truncated_; ++i) topLevel_.push_back(ReadU32());
}

// orig: 0x00108EA0 Load + 0x00108E10 LoadStream
bool NiStream::Load(std::span<const u8> data) {
    data_ = data;
    pos_ = 0;
    truncated_ = false;
    error_ = NiStreamError::None;
    errorMessage_.clear();
    objects_.clear();
    topLevel_.clear();
    if (!LoadHeader()) return false;
    for (u32 i = 0; i < objectCount_; ++i) {
        if (!LoadObject()) return false;
    }
    LoadTopLevelObjects();
    // orig: LoadStream then calls LinkObject (vtbl+0x10) on every object in load order, then PostLinkObject
    // (vtbl+0x28, 0x00108E78; only NiSourceTexture's does anything: the renderers' texture precache).
    for (auto& object : objects_) object->LinkObject(*this);
    for (auto& object : objects_) object->PostLinkObject(*this);
    // orig: 0x00107900. Files older than 4.1.0.12 get their selective-update flags recomputed on every root.
    if (version_ < NiVersion(4, 1, 0, 12)) {
        for (auto& object : objects_) {
            auto* av = dynamic_cast<NiAVObject*>(object.Get());
            if (!av || av->parent) continue;
            bool selective = false, rigid = false;
            av->SetSelectiveUpdateFlags(selective, false, rigid);
        }
    }
    // orig: then 0x000D2380 on the object array (+0x30): see ReleaseLoadReferences.
    return !Failed();
}

void NiStream::ReleaseLoadReferences() {
    for (NiObjectPtr& o : objects_) {
        if (!dynamic_cast<NiDynamicEffect*>(o.Get())) continue;
        bool top = false;
        for (u32 l : topLevel_) top |= l < objects_.size() && objects_[l].Get() == o.Get();
        if (!top) o = nullptr;
    }
}

}  // namespace fut::ni
