// Decompiled from default.xbe 0x00141F30-0x001429D0; see ConfigFile.h. The parser follows the original's two
// passes over the buffer index by index (0x001421A0).
#include "game/data/ConfigFile.h"

#include <cstdlib>
#include <cstring>

namespace fut::game {
namespace {
bool IsSpace(u8 c) { return c == 0 || c == '\t' || c == '\n' || c == '\r' || c == ' '; }
}  // namespace

bool ConfigFile::Parse(std::vector<u8>& buffer) {     // orig: 0x001421A0
    entries_.clear();
    const s32 size = static_cast<s32>(buffer.size());
    u8* d = buffer.data();
    // Bytes past the end read as NUL (the original may read one byte past its buffer; see the value branch).
    auto at = [&](s32 i) -> u8 { return i < size ? d[i] : 0; };

    // Pass 1: count lines that start with a token (comments '#' and blank lines excluded).
    s32 count = 0;
    for (s32 i = 0; i < size;) {
        const u8 c = d[i];
        if (IsSpace(c)) {
            ++i;
            continue;
        }
        if (c != '#') {
            ++count;
            while (i < size && !IsSpace(d[i])) ++i;   // the token
        }
        while (i < size && d[i] != '\n') ++i;         // rest of the line (stops on the newline itself)
    }
    if (count > 0) ++count;                           // orig: one spare slot
    entries_.assign(static_cast<size_t>(count), Entry{});

    // Pass 2: split into key / value, writing NULs into the buffer like the original.
    s32 slot = 0;
    auto store = [&](const char* key, const char* value) {
        if (slot < count) entries_[slot] = {key, value, true};   // port-only bound (the original would overflow)
        ++slot;
    };
    for (s32 i = 0; i < size;) {
        while (i < size && IsSpace(d[i])) ++i;
        if (i >= size) break;
        const s32 keyStart = i;
        if (d[i] == '#') {
            while (i < size && d[i] != '\n') ++i;
            continue;
        }
        const bool quoted = d[i] == '"';
        // orig: a quoted key ends at its first non-quote character (so it is just "\"").
        for (;;) {
            if (quoted ? d[i] != '"' : IsSpace(d[i])) break;
            if (++i >= size) return ok = true;
        }
        d[i] = 0;                                     // end of key
        while (true) {                                // skip blanks up to (not past) a newline
            const u8 c = at(i);
            if (!IsSpace(c) || c == '\n') break;
            if (++i >= size) break;
        }
        const char* key = reinterpret_cast<const char*>(d + keyStart);
        if (at(i) == '\n') {
            store(key, "");
            continue;
        }
        if (i >= size) break;
        s32 valueStart = i;
        if (d[i] == '"') {
            if (++i >= size) break;
            valueStart = i;
            while (d[i] != '"')
                if (++i >= size) return ok = true;
        } else {
            while (!IsSpace(d[i]))
                if (++i >= size) return ok = true;
        }
        d[i] = 0;                                     // end of value
        store(key, reinterpret_cast<const char*>(d + valueStart));
    }
    return ok = true;
}

const char* ConfigFile::Key(s32 i) const {            // orig: 0x00141F30
    if (i < 0 || i >= Count() || !entries_[i].present) return nullptr;
    return entries_[i].key.c_str();
}

const char* ConfigFile::Value(s32 i) const {          // orig: 0x001420E0
    if (i < 0 || i >= Count() || !entries_[i].present) return nullptr;
    return entries_[i].value.c_str();
}

char* ConfigFile::MutableValue(s32 i) {
    if (i < 0 || i >= Count() || !entries_[i].present) return nullptr;
    return entries_[i].value.data();
}

s32 ConfigFile::GetInt(const char* key) const {       // orig: 0x00141F50
    for (const Entry& e : entries_)
        if (e.present && std::strcmp(key, e.key.c_str()) == 0) return std::atoi(e.value.c_str());
    return 0;
}

double ConfigFile::GetFloat(const char* key) const {  // orig: 0x00141FD0
    for (const Entry& e : entries_)
        if (e.present && std::strcmp(key, e.key.c_str()) == 0) return std::strtod(e.value.c_str(), nullptr);
    return 0.0;
}

}  // namespace fut::game
