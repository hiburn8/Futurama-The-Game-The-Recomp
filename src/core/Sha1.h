// SHA-1 (FIPS 180-1). The save games' signature stand-in (game/app/SaveStorage.h: the console's XCalculateSignature*
// is an HMAC-SHA1 with a console / title key, which the port does not have; KNOWN_ISSUES #66) and the tests' digests.
#pragma once

#include "core/Types.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace fut::crypto {

class Sha1 {
public:
    void Update(const u8* data, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            block_[used_++] = data[i];
            if (used_ == 64) Block();
        }
        bits_ += u64(n) * 8;
    }
    std::array<u8, 20> Final() {
        const u64 bits = bits_;
        const u8 pad = 0x80;
        Update(&pad, 1);
        const u8 zero = 0;
        while (used_ != 56) Update(&zero, 1);
        u8 len[8];
        for (int i = 0; i < 8; ++i) len[i] = u8(bits >> (56 - 8 * i));
        Update(len, 8);
        std::array<u8, 20> out{};
        for (int i = 0; i < 5; ++i)
            for (int k = 0; k < 4; ++k) out[size_t(4 * i + k)] = u8(h_[i] >> (24 - 8 * k));
        return out;
    }
    static std::array<u8, 20> Of(const u8* data, size_t n) {
        Sha1 s;
        s.Update(data, n);
        return s.Final();
    }
    static std::string Hex(const std::array<u8, 20>& d) {
        static const char* kDigits = "0123456789abcdef";
        std::string s;
        for (u8 b : d) s += kDigits[b >> 4], s += kDigits[b & 15];
        return s;
    }

private:
    static u32 Rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
    void Block() {
        u32 w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (u32(block_[4 * i]) << 24) | (u32(block_[4 * i + 1]) << 16) | (u32(block_[4 * i + 2]) << 8) |
                   u32(block_[4 * i + 3]);
        for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
            else f = b ^ c ^ d, k = 0xCA62C1D6u;
            const u32 t = Rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = Rol(b, 30), b = a, a = t;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
        used_ = 0;
    }
    u32 h_[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    u8 block_[64] = {};
    size_t used_ = 0;
    u64 bits_ = 0;
};

}  // namespace fut::crypto
