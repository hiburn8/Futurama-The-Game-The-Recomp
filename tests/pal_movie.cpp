#include "core/XbeImage.h"
#include "video/xmv/XmvImage.h"
#include "video/xmv/Wmv2Decoder.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

static std::vector<uint8_t> Read(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open owned input");
    return {(std::istreambuf_iterator<char>(f)), {}};
}
static uint32_t Word(const uint8_t* p) {
    uint32_t value;
    std::memcpy(&value, p, sizeof value);
    return value;
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    auto bytes = Read(argv[1]);
    fut::xbe::XbeImage identity;
    if (!identity.Load(bytes) || !identity.IsPalEu()) return 3;
    xmv::XmvImage image;
    if (!image.LoadFromXbe(bytes)) return 4;
    // Every run/level descriptor pointer must resolve physically, not be translated twice.
    for (auto table : {0x2747B0u, 0x274820u}) {
        for (unsigned i = 0; i < 3; ++i) {
            const auto* descriptor = image.At(table + i * 36);
            for (unsigned field : {0u, 3u, 4u, 5u, 6u, 7u, 8u})
                (void)image.AtPhysical(Word(descriptor + field * 4));
        }
    }
    bool refused = false;
    try { (void)image.AtPhysical(0xFFFFFFFF); } catch (const std::out_of_range&) { refused = true; }
    if (!refused || image.Contains(0xFFFFFFFF, 2)) return 5;

    auto movie = Read(argv[2]);
    if (movie.size() < 36) return 6;
    const auto width = Word(movie.data() + 20), height = Word(movie.data() + 24);
    const unsigned tracks = movie[32] | (movie[33] << 8);
    const size_t header = 36 + size_t(tracks) * 12;
    const size_t packetEnd = Word(movie.data() + 4);
    if (width == 0 || height == 0 || width > 1920 || height > 1080 ||
        packetEnd > movie.size() || header + 12 + size_t(tracks) * 4 > packetEnd) return 7;
    xmv::Wmv2Decoder decoder;
    const unsigned tag = tracks ? movie[36] | (movie[37] << 8) : 0;
    if (!decoder.Create(image, width, height, tag)) return 8;
    const size_t videoHeader = header + 4;
    const uint32_t flags = Word(movie.data() + videoHeader);
    size_t cursor = videoHeader + 8 + size_t(tracks) * 4;
    if (flags & 0x80000000u) {
        if (cursor + 4 > packetEnd) return 9;
        decoder.SetExtension(Word(movie.data() + cursor));
        cursor += 4;
    }
    const unsigned frames = (flags >> 23) & 255;
    if (!frames) return 10;
    unsigned decoded = 0;
    for (; decoded < frames && decoded < 12; ++decoded) {
        if (cursor + 4 > packetEnd) return 11;
        const size_t size = (Word(movie.data() + cursor) & 0x1FFFF) * 4 + 4;
        cursor += 4;
        if (size > packetEnd - cursor) return 12;
        // Match the decoder's documented eight-byte lookahead without touching another packet.
        std::vector<uint8_t> frame(movie.begin() + cursor, movie.begin() + cursor + size);
        frame.resize(size + 8, 0);
        if (decoder.DecodeFrame(frame.data(), size) != xmv::Wmv2Decoder::Result::Ok) return 13;
        cursor += size;
    }
    bytes.back() ^= 1;
    if (image.LoadFromXbe(bytes) || image.Loaded()) return 14;
    std::cout << "PASS: PAL XMV pointers and " << decoded << " native decoded frames; no visual/oracle acceptance\n";
}
