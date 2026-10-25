// The user's default.xbe, read by the original's virtual addresses. The data the decompiled code reads from the
// original's data sections (the game's, the XDK libraries' and NetImmerse's .data / .rdata, e.g. name and code
// tables, render-state defaults) is not embedded in the port: it is read from here at run time.
//
// One process-wide image, loaded once at start (fut_game: the disc image's / extracted folder's default.xbe, before
// anything reads it; fut_tests: FUT_EXTRACTED_DIR's). Only the Futurama (Xbox, NTSC-U) build is accepted: the
// certificate's title id and region and the header's time stamp are checked.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace fut::xbe {

class XbeImage {
public:
    // Takes default.xbe's file bytes; false (with a reason) if they are not the expected build.
    bool Load(std::vector<u8> bytes, std::string* error = nullptr);
    bool Loaded() const { return !bytes_.empty(); }
    const std::vector<u8>& Bytes() const { return bytes_; }   // the whole file (the sound and movie libraries')

    // Host address of the n bytes at `va`, all inside one section's file data; nullptr otherwise.
    const u8* At(u32 va, u32 n = 1) const;
    u8 Byte(u32 va) const;                            // 0 outside the sections' data (and without an image)
    u32 Word(u32 va) const;                           // (little-endian, as the Xbox)
    // The NUL-terminated string at `va` (inside one section's data), else "".
    const char* String(u32 va) const;
    // The string a pointer table's entry at `va` points to (String(Word(va))).
    const char* StringAt(u32 va) const { return String(Word(va)); }

private:
    struct Section {
        u32 va, size, raw;                            // (size: the bytes present in the file)
    };
    std::vector<u8> bytes_;
    std::vector<Section> sections_;
};

// Loads the process's image (once; a second call fails). The error names what is missing or wrong.
bool LoadGameImage(std::vector<u8> bytes, std::string* error = nullptr);
// The process's image (empty until LoadGameImage succeeded).
const XbeImage& GameImage();

}  // namespace fut::xbe
