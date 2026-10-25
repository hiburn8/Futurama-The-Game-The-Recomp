// Decompiled from default.xbe 0x000D9A20-0x000DA4F0. See LanguageDb.h for the format.
#include "game/text/LanguageDb.h"

#include <cstring>

namespace fut::text {
namespace {

u32 ReadU32(std::span<const u8> d, size_t at) {
    u32 v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
}

// Reads a NUL-terminated ASCII string at `at` (bounded by the data); advances `at` past the terminator.
std::string TakeCString(std::span<const u8> d, size_t& at) {
    std::string s;
    while (at < d.size() && d[at] != 0) s.push_back(static_cast<char>(d[at++]));
    ++at;
    return s;
}

}  // namespace

int StrICmp(const char* a, const char* b) {           // orig: 0x001684D1
    for (;; ++a, ++b) {
        int ca = static_cast<u8>(*a), cb = static_cast<u8>(*b);
        if (ca >= 'A' && ca <= 'Z') ca += 0x20;
        if (cb >= 'A' && cb <= 'Z') cb += 0x20;
        if (ca != cb || ca == 0) return ca - cb;
    }
}

// orig: 0x000D9FC0
bool LanguageDb::LoadIndex(std::span<const u8> file) {
    if (file.empty() || loaded_) return false;
    if (file.size() < kHeaderSize) return false;      // port-only bound (the original copies 0x1F0 bytes blindly)
    header_.assign(file.begin(), file.begin() + kHeaderSize);
    const u32 count = ReadU32(file, 4);
    const u32 namesOffset = ReadU32(file, 8);
    const u32 namesSize = ReadU32(file, 12);
    if (namesOffset > file.size() || namesSize > file.size() - namesOffset) return false;
    const std::span<const u8> names = file.subspan(namesOffset, namesSize);
    entries_.assign(count, Entry{});
    size_t at = 0;
    for (Entry& e : entries_) {                       // id, group (skipped), str3, str4
        e.id = TakeCString(names, at);
        TakeCString(names, at);
        e.str3 = TakeCString(names, at);
        e.str4 = TakeCString(names, at);
    }
    loaded_ = true;
    return true;
}

// orig: 0x000DA110
void LanguageDb::SelectLanguage(const std::string& language, std::span<const u8> file) {
    if (!currentLanguage_.empty() && StrICmp(currentLanguage_.c_str(), language.c_str()) == 0) return;
    if (!loaded_ || file.size() < kHeaderSize || std::memcmp(header_.data(), file.data(), kHeaderSize) != 0) return;
    // The last matching slot wins (the original scans all 15).
    int slot = -1;
    for (u32 i = 0; i < kLanguageSlots; ++i) {
        const char* name = reinterpret_cast<const char*>(header_.data() + 0x18 + i * 0x20);
        const size_t len = strnlen(name, 24);
        if (StrICmp(language.c_str(), std::string(name, len).c_str()) == 0) {
            currentLanguage_.assign(name, len);
            slot = static_cast<int>(i);
        }
    }
    if (slot < 0) return;
    const u32 offset = ReadU32(header_, 0x10 + slot * 0x20);
    const u32 size = ReadU32(header_, 0x14 + slot * 0x20);
    if (offset > file.size() || size > file.size() - offset) return;   // port-only bound
    const u8* p = file.data() + offset;
    const u8* end = p + size;
    for (Entry& e : entries_) {                       // orig: 0x000D9F50 per string
        std::u16string s;
        while (p + 1 < end && (p[0] | p[1]) != 0) {
            s.push_back(static_cast<char16_t>(p[0] | (p[1] << 8)));
            p += 2;
        }
        p += 2;
        e.text = std::move(s);
        e.hasText = true;
    }
}

// orig: 0x000DA400
void LanguageDb::SelectLanguage(const std::string& language, const FileLoader& loader) {
    if (!currentLanguage_.empty() && StrICmp(currentLanguage_.c_str(), language.c_str()) == 0) return;
    std::vector<u8> file;
    if (!loader || !loader(fileName_, file)) return;
    SelectLanguage(language, file);
}

// orig: 0x000DA220
const LanguageDb::Entry* LanguageDb::Find(const std::string& id) const {
    for (const Entry& e : entries_)
        if (StrICmp(e.id.c_str(), id.c_str()) == 0) return &e;
    return nullptr;
}

// ---- manager ----------------------------------------------------------------------------------------------------------
LanguageDb* TextManager::LoadDb(const std::string& fileName) {   // orig: 0x000D9E20
    for (auto& db : dbs_)                             // orig: 0x000D9D30
        if (StrICmp(db->FileName().c_str(), fileName.c_str()) == 0) return db.get();
    auto db = std::make_unique<LanguageDb>(fileName); // orig: 0x000DA460
    std::vector<u8> file;
    if (!loader_ || !loader_(fileName, file) || !db->LoadIndex(file)) return nullptr;   // orig: 0x000DA3A0
    db->SelectLanguage("English", loader_);           // orig: 0x000DA460 always loads English first
    db->SelectLanguage(language_, loader_);
    dbs_.push_back(std::move(db));
    return dbs_.back().get();
}

LanguageDb* TextManager::LoadDbFromMemory(std::span<const u8> file, const std::string& fileName) {   // 0x000D9E80
    for (auto& db : dbs_)
        if (StrICmp(db->FileName().c_str(), fileName.c_str()) == 0) return db.get();
    auto db = std::make_unique<LanguageDb>(fileName); // orig: 0x000DA2F0
    if (!db->LoadIndex(file)) return nullptr;
    db->SelectLanguage("English", file);
    db->SelectLanguage(language_, file);
    dbs_.push_back(std::move(db));
    return dbs_.back().get();
}

void TextManager::UnloadDb(LanguageDb* db) {          // orig: 0x000D9B40 (swap with the last entry and pop)
    for (size_t i = 0; i < dbs_.size(); ++i) {
        if (dbs_[i].get() != db) continue;
        std::swap(dbs_[i], dbs_.back());
        dbs_.pop_back();
        return;
    }
}

void TextManager::SetLanguage(const std::string& language) {   // orig: 0x000D9BB0
    if (StrICmp(language_.c_str(), language.c_str()) == 0) return;
    language_ = language;
    for (auto& db : dbs_) db->SelectLanguage(language_, loader_);
}

const LanguageDb::Entry* TextManager::Find(const std::string& id, const LanguageDb* db) const {   // orig: 0x000D9C30
    if (db) return db->Find(id);
    for (const auto& d : dbs_)
        if (const LanguageDb::Entry* e = d->Find(id)) return e;
    return nullptr;
}

const char16_t* TextManager::GetText(const std::string& id, const LanguageDb* db) const {   // orig: 0x000D9C80
    static const std::u16string kNotTranslated = u"(Not translated)";   // orig: widened once into DAT_002EC7D0
    const LanguageDb::Entry* e = Find(id, db);
    if (!e || !e->hasText || e->text.empty()) return kNotTranslated.c_str();
    return e->text.c_str();
}

const char* TextManager::GetStr3(const std::string& id, const LanguageDb* db) const {   // orig: 0x000D9CF0
    const LanguageDb::Entry* e = Find(id, db);
    return e ? e->str3.c_str() : nullptr;
}

const char* TextManager::GetStr4(const std::string& id, const LanguageDb* db) const {   // orig: 0x000D9D10
    const LanguageDb::Entry* e = Find(id, db);
    return e ? e->str4.c_str() : nullptr;
}

const char* TextManager::LanguageNameForXbox(u32 xboxLanguage) {   // orig: 0x000D9A20
    switch (xboxLanguage) {
    case 3: return "german";
    case 4: return "french";
    case 5: return "spanish";
    case 6: return "italian";
    default: return "english";
    }
}

}  // namespace fut::text
