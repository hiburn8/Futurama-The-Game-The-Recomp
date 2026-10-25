// "key value" configuration files embedded in character models (char.cfg), decompiled from default.xbe
// (0x00141F30-0x001429D0).
//
// One entry per token pair: a key (up to whitespace) and a value (up to whitespace, or a quoted string); '#' starts a
// comment line; a key alone on its line gets an empty value. Whitespace is exactly NUL, TAB, LF, CR and space.
// Verified against the original parser on every char.cfg of the disc (tests/game/ConfigFileTests.cpp).
#pragma once

#include "core/Types.h"

#include <span>
#include <string>
#include <vector>

namespace fut::game {

class ConfigFile {                                    // orig: 0x10 bytes, vtable 0x002AA2EC
public:
    struct Entry {                                    // orig: 0x0C bytes, vtable 0x002AA2F0
        std::string key, value;
        bool present = false;                         // orig: key/value pointers non-NULL
    };

    // orig: 0x001421A0. Parses `data` (the original works in place, writing NULs into the file object's buffer;
    // `data` receives the same modifications). Returns false only for an empty input pointer (the original's NULL).
    bool Parse(std::vector<u8>& data);

    // orig: 0x00141F30 / 0x001420E0: key / value of entry i (nullptr outside [0, Count()) or for an unused slot).
    const char* Key(s32 i) const;
    const char* Value(s32 i) const;
    // Value i, writable: callers that tokenise in place (strtok) modify it as the original modifies its buffer;
    // C-string reads of Value() then stop at the first written NUL, as in the original.
    char* MutableValue(s32 i);
    // orig: 0x00141F50 / 0x00141FD0: atoi / atof of the first entry whose key equals `key` (strcmp); 0 if none.
    s32 GetInt(const char* key) const;
    double GetFloat(const char* key) const;           // x87 result (callers round when they store)

    // orig: +0x0C. One more than the number of non-comment lines when there is at least one (an original quirk);
    // the last slot(s) stay empty.
    s32 Count() const { return static_cast<s32>(entries_.size()); }
    const std::vector<Entry>& Entries() const { return entries_; }
    bool ok = false;                                  // orig: +0x04, the parse result

private:
    std::vector<Entry> entries_;                      // orig: +0x08
};

}  // namespace fut::game
