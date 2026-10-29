#include "XmvImage.h"
#include "core/XbeImage.h"

#include <cstring>
#include <fstream>

namespace xmv {

bool XmvImage::LoadFromXbe(const std::vector<uint8_t>& xbe) {
    auto u32At = [&](size_t o) -> uint32_t {
        uint32_t v = 0;
        if (o + 4 <= xbe.size()) std::memcpy(&v, &xbe[o], 4);
        return v;
    };
    bytes_.clear();
    physicalBase_ = kBase;
    fut::xbe::XbeImage identity;
    if (!identity.Load(xbe)) return false;
    physicalBase_ = identity.IsPalEu() ? 0x00264360 : kBase;
    const uint32_t base = u32At(0x104), count = u32At(0x11C), headers = u32At(0x120);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t h = size_t(headers - base) + i * 0x38;
        const uint32_t sva = u32At(h + 4), vsize = u32At(h + 8), raw = u32At(h + 12), rsize = u32At(h + 16);
        if (sva != physicalBase_ || vsize != kSize || rsize > vsize || size_t(raw) + rsize > xbe.size()) continue;
        bytes_.assign(size_t(vsize) + 64, 0);
        std::memcpy(bytes_.data(), &xbe[raw], rsize);
        // Sanity: the first instructions of XMVDecoder_CreateDecoderForPackets (push ebp; mov ebp, esp; sub esp, 0x24).
        static const uint8_t kProlog[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24};
        if (std::memcmp(bytes_.data(), kProlog, sizeof kProlog) != 0) {
            bytes_.clear();
            return false;
        }
        return true;
    }
    return false;
}

bool XmvImage::LoadFromFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return LoadFromXbe(data);
}

} // namespace xmv
