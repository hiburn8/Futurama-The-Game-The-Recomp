#include "core/XbeImage.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) return 3;
    std::vector<fut::u8> bytes((std::istreambuf_iterator<char>(f)), {});
    fut::xbe::XbeImage image;
    std::string error;
    if (!image.Load(bytes, &error) || !image.IsPalEu()) { std::cerr << error; return 4; }
    if (std::strcmp(image.StringAt(0x2D6228 + 0x66 * 4), "death_fall")) return 5;
    if (image.Byte(0x158864 + 6) != 2) return 6; // Xbox A8R8G8B8 is 32-bit.
    struct Range { fut::u32 address, bytes; };
    const Range ranges[] = {
        {0x158864, 0x66}, {0x2427C8, 32}, {0x2427E8, (0xA6 - 0x39) * 4},
        {0x2936A8 + 0x5C * 4, (0x88 - 0x5C) * 4}, {0x2D60D8, 29 * 4},
        {0x2D615C, 22 * 8}, {0x2D6228, 248 * 4}, {0x2D8358, 32 * 4}, {0x2D83D8, 166 * 4}};
    for (const auto& r : ranges) {
        if (!image.At(r.address, r.bytes) || !image.At(r.address + r.bytes - 1)) return 10;
        bool crossed = false;
        try { (void)image.At(r.address + r.bytes - 1, 2); }
        catch (const std::out_of_range&) { crossed = true; }
        if (!crossed) return 11;
    }
    // Every pointer used by these tables must identify a terminated in-image string.
    for (const auto& r : {Range{0x2D60D8, 29}, Range{0x2D6228, 248}, Range{0x2D615C, 22}}) {
        const fut::u32 stride = r.address == 0x2D615C ? 8 : 4;
        for (fut::u32 i = 0; i < r.bytes; ++i) {
            const auto address = r.address + i * stride;
            if (!image.Word(address) || !image.StringAt(address)[0]) return 12;
        }
    }
    bool rejected = false;
    try { (void)image.Word(0xDEADBEEF); } catch (const std::out_of_range&) { rejected = true; }
    if (!rejected) return 7;
    bytes.back() ^= 1;
    if (image.Load(std::move(bytes), &error) || image.Loaded() || image.IsPalEu()) return 8;
    if (error.find("hash mismatch") == std::string::npos) return 9;
    std::cout << "PASS: PAL identity, table pointers, format class, unsupported access and altered-image rejection\n";
}
