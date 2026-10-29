// Localised text databases (.ldb), decompiled from default.xbe (0x000D9A20-0x000DA4F0).
//
// File layout (little endian):
//   0x000  char magic[4] "0BDL", u32 stringCount, u32 namesOffset, u32 namesSize
//   0x010  15 x { u32 offset, u32 size, char name[24] }   languages ("English", "French", "Italian", "Spanish",
//          "German", "Undefined0".."Undefined9"); the unused name bytes hold tool garbage
//   0x1F0  names blob: per string four NUL-terminated ASCII strings: id, group, str3, str4
//          (str3/str4 are voice-clip paths in the dialogue databases)
//   then per language: stringCount NUL-terminated UTF-16LE strings
// A database keeps the 0x1F0-byte header, the names blob and the current language's strings; switching language
// reloads the file and only succeeds if its header is byte-identical to the cached one.
//
// Verified against the original on every .ldb of the disc in all five languages (tests/game/LanguageDbTests.cpp).
#pragma once

#include "core/Types.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fut::text {

using FileLoader = std::function<bool(const std::string& name, std::vector<u8>& out)>;

class LanguageDb {                                    // orig: 0x18-byte object (ctor 0x000D9EF0, dtor 0x000DA280)
public:
    static constexpr size_t kHeaderSize = 0x1F0;
    static constexpr u32 kLanguageSlots = 15;

    struct Entry {                                    // orig: 16 bytes
        std::string id;                               // orig: +0 (pointer into the names blob)
        std::u16string text;                          // orig: +4 (copy of the current language's string)
        bool hasText = false;
        std::string str3, str4;                       // orig: +8 / +C
    };

    explicit LanguageDb(std::string fileName) : fileName_(std::move(fileName)) {}

    // orig: 0x000D9FC0. Parses the header and names blob; false if already loaded or the data is unusable.
    bool LoadIndex(std::span<const u8> file);
    // orig: 0x000DA110. Loads `language`'s strings from `file` if that language differs from the current one
    // (case-insensitive) and the file's header matches the cached header.
    void SelectLanguage(const std::string& language, std::span<const u8> file);
    // orig: 0x000DA400. Same, reading the file through `loader` (skipped when the language is already current).
    void SelectLanguage(const std::string& language, const FileLoader& loader);
    // orig: 0x000DA220. Case-insensitive id lookup.
    const Entry* Find(const std::string& id) const;

    const std::string& FileName() const { return fileName_; }
    const std::string& CurrentLanguage() const { return currentLanguage_; }   // as spelled in the file header
    const std::vector<Entry>& Entries() const { return entries_; }

private:
    std::string fileName_;                            // orig: +4
    std::vector<u8> header_;                          // orig: +0
    std::vector<Entry> entries_;                      // orig: +8
    bool loaded_ = false;
    std::string currentLanguage_;                     // orig: +0x14 (points into the header)
};

class TextManager {                                   // orig: singleton DAT_002EC6E0 (0x1C bytes, ctor 0x000D9AC0)
public:
    explicit TextManager(FileLoader loader) : loader_(std::move(loader)) {}

    // orig: 0x000D9E20. Returns the already loaded database of that file name, or loads it (English first, then the
    // current language, exactly as the original); nullptr if the file is missing or invalid.
    LanguageDb* LoadDb(const std::string& fileName);
    // orig: 0x000D9E80. Same from an in-memory copy of the file.
    LanguageDb* LoadDbFromMemory(std::span<const u8> file, const std::string& fileName);
    // orig: 0x000D9B40
    void UnloadDb(LanguageDb* db);
    // orig: 0x000D9BB0. Reloads every database in the new language.
    void SetLanguage(const std::string& language);
    const std::string& Language() const { return language_; }   // orig: +0x18 (default "English")

    // orig: 0x000D9C80. db = nullptr searches all databases in load order. Missing or empty strings return
    // u"(Not translated)".
    const char16_t* GetText(const std::string& id, const LanguageDb* db) const;
    // orig: 0x000D9CF0 / 0x000D9D10. nullptr when the id is unknown.
    const char* GetStr3(const std::string& id, const LanguageDb* db) const;
    const char* GetStr4(const std::string& id, const LanguageDb* db) const;

    // orig: 0x000D9A20 - XGetLanguage() mapped to the names the game passes to SetLanguage.
    static const char* LanguageNameForXbox(u32 xboxLanguage);
    // Host option validation: canonical disc language name, or nullptr if unsupported.
    static const char* ParseLanguage(const std::string& name);

    // orig: 0x000D9C30. db = nullptr searches all databases in load order.
    const LanguageDb::Entry* Find(const std::string& id, const LanguageDb* db) const;

private:

    FileLoader loader_;
    std::vector<std::unique_ptr<LanguageDb>> dbs_;    // orig: NiTArray at +4 (size +0xC)
    std::string language_ = "English";
};

// orig: _stricmp (ASCII case folding, "C" locale).
int StrICmp(const char* a, const char* b);

}  // namespace fut::text
