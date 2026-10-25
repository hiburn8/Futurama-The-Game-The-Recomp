// The XMV library's section of the user's default.xbe (section "XMV", VA 0x00264400..0x0028BF6C): the WMV2 decoder's
// tables (VLC trees, run / level tables, scan orders, IDCT and filter constants, 0x0026C690..) are read from it at
// run time and addressed by their original virtual addresses. Nothing of the image is embedded in the port.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xmv {

class XmvImage {
public:
    static constexpr uint32_t kBase = 0x00264400;  // section XMV
    static constexpr uint32_t kSize = 0x00027B6C;  // virtual size (the raw size 0x27B5C is zero-padded)

    // Copies the section out of default.xbe's file bytes; false if the file is not the expected XBE.
    bool LoadFromXbe(const std::vector<uint8_t>& xbe);
    bool LoadFromFile(const std::string& path);
    bool Loaded() const { return !bytes_.empty(); }

    // Host address of an original VA inside the section (the lifted kernels' K()).
    const uint8_t* At(uint32_t va) const { return bytes_.data() + (va - kBase); }
    bool Contains(uint32_t va, uint32_t n = 1) const { return va >= kBase && va - kBase + n <= bytes_.size(); }

private:
    std::vector<uint8_t> bytes_;   // + 64 zero bytes of slack (VLC peeks, scan overreads stay inside)
};

} // namespace xmv
