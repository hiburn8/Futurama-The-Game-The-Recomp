// Script natives, decompiled from default.xbe; see ScriptNatives.h. x87 arithmetic is modelled as double with the
// float store at the end (F); comparisons follow the original's FPU status-word tests (unordered = NaN operands).
#include "game/script/ScriptNatives.h"

#include "core/CrtRand.h"
#include "core/memory/MemoryManager.h"
#include "engine/ni/NiMath.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace fut::script {
namespace {

using ni::detail::F;

u32 Get(VmAddr a) {
    u32 v;
    std::memcpy(&v, mem::FromVmAddr(a), 4);
    return v;
}
void Set(VmAddr a, u32 v) { std::memcpy(mem::FromVmAddr(a), &v, 4); }
f32 GetF(VmAddr a) {
    f32 v;
    std::memcpy(&v, mem::FromVmAddr(a), 4);
    return v;
}
void SetF(VmAddr a, f32 v) { std::memcpy(mem::FromVmAddr(a), &v, 4); }
s32 GetI(VmAddr a) { return static_cast<s32>(Get(a)); }

// x87 FCOM outcome of `a` against `b`.
enum class Cmp { Less, Equal, Greater, Unordered };
Cmp Compare(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return Cmp::Unordered;
    return a < b ? Cmp::Less : (a == b ? Cmp::Equal : Cmp::Greater);
}

// x87 NaN propagation for an operation on two floats (orig: the FPU's rule, as the emulated original shows): a NaN
// operand gives a quiet NaN - a signalling NaN loses to a quiet one, otherwise with two NaNs the one with the larger
// significand wins, on a tie the positive one.
bool X87NaN(f32 a, f32 b, u32& out) {
    const bool na = std::isnan(a), nb = std::isnan(b);
    if (!na && !nb) return false;
    u32 ua, ub;
    std::memcpy(&ua, &a, 4);
    std::memcpy(&ub, &b, 4);
    const bool qa = (ua & 0x400000) != 0, qb = (ub & 0x400000) != 0;
    ua |= 0x400000;                                    // quiet
    ub |= 0x400000;
    if (na && nb && qa != qb) {
        out = qa ? ua : ub;
    } else if (na && nb) {
        const u32 sa = ua & 0x7FFFFF, sb = ub & 0x7FFFFF;
        out = sa != sb ? (sa > sb ? ua : ub) : ((ua & 0x80000000) == 0 ? ua : ub);
    } else {
        out = na ? ua : ub;
    }
    return true;
}

// orig: 0x000B01A0: strstr (VM address of the match) when both are non-null, else 0.
VmAddr ScriptStrStr(VmAddr hay, VmAddr needle) {
    if (!hay || !needle) return 0;
    const char* h = static_cast<const char*>(mem::FromVmAddr(hay));
    const char* found = std::strstr(h, static_cast<const char*>(mem::FromVmAddr(needle)));
    return found ? hay + static_cast<VmAddr>(found - h) : 0;
}

}  // namespace

s32 ScriptStrCmp(VmAddr a, VmAddr b) {                 // orig: 0x000B0140
    if (a == b) return 0;
    if (!a || !b) return 1;
    const auto* p = static_cast<const u8*>(mem::FromVmAddr(a));
    const auto* q = static_cast<const u8*>(mem::FromVmAddr(b));
    for (;; ++p, ++q) {
        if (*p != *q) return *p < *q ? -1 : 1;
        if (*p == 0) return 0;
    }
}

s32 FloatToIntCrt(f32 value) {                         // orig: 0x00167110
    const double d = value;
    // FISTP to 64 bits: NaN and magnitudes from 2^63 store the integer indefinite 0x8000000000000000.
    if (std::isnan(d) || std::fabs(d) >= 9223372036854775808.0) return 0;
    return static_cast<s32>(static_cast<u32>(static_cast<u64>(static_cast<s64>(std::trunc(d)))));
}

bool CallCoreNative(u32 method, VmAddr& sp) {         // orig: 0x000B0CD0, class 0, methods 3-60
    // Binary operators: a = [sp-8], b = [sp-4]; the result replaces a. Unary: x = [sp-4], replaced in place.
    const VmAddr a = sp - 8, b = sp - 4, x = sp - 4;
    auto binary = [&](u32 result) {
        Set(a, result);
        sp -= 4;
    };
    auto binaryF = [&](double result) {
        SetF(a, F(result));
        sp -= 4;
    };
    auto flag = [](bool v) { return v ? 1u : 0u; };
    auto flagF = [](bool v) { return v ? 0x3F800000u : 0u; };   // FILD of 0 / 1, stored as float
    switch (method) {
        case 3: binary(flag(ScriptStrCmp(Get(a), Get(b)) == 0)); return true;   // string ==
        case 4: binary(flag(ScriptStrCmp(Get(a), Get(b)) != 0)); return true;   // string !=
        case 5: binary(ScriptStrStr(Get(a), Get(b))); return true;              // strstr
        case 6: Set(x, static_cast<u32>(FloatToIntCrt(GetF(x)))); return true;   // float -> int
        case 7: SetF(x, -GetF(x)); return true;                                  // float negate (FCHS)
        case 8: Set(x, flag(Compare(GetF(x), 0.0) == Cmp::Equal)); return true;  // float ! (== 0.0f)
        case 9:
        case 10:
        case 11:
        case 12: {                                     // float + - / * (FLD, FADD / FSUB / FDIV / FMUL, FSTP)
            const f32 fa = GetF(a), fb = GetF(b);
            u32 nan;
            if (X87NaN(fa, fb, nan)) return binary(nan), true;
            const double da = fa, db = fb;
            binaryF(method == 9 ? db + da : method == 10 ? da - db : method == 11 ? da / db : db * da);
            return true;
        }
        // Float comparisons with a float result (1.0f / 0.0f).
        case 13: binary(flagF(Compare(GetF(a), GetF(b)) == Cmp::Equal)); return true;
        case 14: binary(flagF(Compare(GetF(a), GetF(b)) != Cmp::Equal)); return true;
        case 15: {
            const Cmp c = Compare(GetF(a), GetF(b));
            binary(flagF(c == Cmp::Equal || c == Cmp::Greater));
            return true;
        }
        case 16: {
            const Cmp c = Compare(GetF(a), GetF(b));
            binary(flagF(c == Cmp::Less || c == Cmp::Equal));
            return true;
        }
        case 17: binary(flagF(Compare(GetF(a), GetF(b)) == Cmp::Greater)); return true;
        case 18: binary(flagF(Compare(GetF(a), GetF(b)) == Cmp::Less)); return true;
        case 19: SetF(x, static_cast<f32>(GetI(x))); return true;               // int -> float (FILD, FSTP)
        case 20: {                                                               // int -> bool: only the low byte
            const u8 v = GetI(x) != 0 ? 1 : 0;
            std::memcpy(mem::FromVmAddr(x), &v, 1);
            return true;
        }
        case 21: Set(x, static_cast<u32>(0u - Get(x))); return true;            // int negate
        case 22: Set(x, flag(Get(x) == 0)); return true;                         // int !
        case 23: binary(Get(b) + Get(a)); return true;
        case 24: binary(Get(a) - Get(b)); return true;
        case 25: {                                                               // int / (IDIV)
            // orig: a zero divisor (or INT_MIN / -1) raises a divide exception; the port pushes 0 (KNOWN_ISSUES #20).
            const s32 n = GetI(a), d = GetI(b);
            binary(d == 0 || (n == INT32_MIN && d == -1) ? 0u : static_cast<u32>(n / d));
            return true;
        }
        case 26: binary(static_cast<u32>(static_cast<s64>(GetI(b)) * GetI(a))); return true;
        case 27: binary(flag(Get(a) == Get(b))); return true;
        case 28: binary(flag(Get(a) != Get(b))); return true;
        case 29: binary(flag(GetI(a) >= GetI(b))); return true;
        case 30: binary(flag(GetI(a) <= GetI(b))); return true;
        case 31: binary(flag(GetI(a) > GetI(b))); return true;
        case 32: binary(flag(GetI(a) < GetI(b))); return true;
        // bool ==, != (low bytes), bool -> int (low byte zero-extended).
        case 33: binary(flag((Get(a) & 0xFF) == (Get(b) & 0xFF))); return true;
        case 34: binary(flag((Get(a) & 0xFF) != (Get(b) & 0xFF))); return true;
        case 35: Set(x, Get(x) & 0xFF); return true;
        // float a, int b: arithmetic with a float result.
        case 36: binaryF(static_cast<double>(GetI(b)) + GetF(a)); return true;
        case 37: binaryF(static_cast<double>(GetF(a)) - GetI(b)); return true;
        case 38: binaryF(static_cast<double>(GetF(a)) / GetI(b)); return true;
        case 39: binaryF(static_cast<double>(GetI(b)) * GetF(a)); return true;
        // float a, int b comparisons (int result); the original compares b against a.
        case 40: binary(flag(Compare(GetI(b), GetF(a)) == Cmp::Equal)); return true;
        case 41: binary(flag(Compare(GetI(b), GetF(a)) != Cmp::Equal)); return true;
        case 42: {
            const Cmp c = Compare(GetI(b), GetF(a));
            binary(flag(c == Cmp::Less || c == Cmp::Equal));                  // a >= b
            return true;
        }
        case 43: {
            const Cmp c = Compare(GetI(b), GetF(a));
            binary(flag(c == Cmp::Equal || c == Cmp::Greater));               // a <= b
            return true;
        }
        case 44: binary(flag(Compare(GetI(b), GetF(a)) == Cmp::Less)); return true;      // a > b
        case 45: binary(flag(Compare(GetI(b), GetF(a)) == Cmp::Greater)); return true;   // a < b
        // int a, float b: arithmetic with a float result.
        case 46: binaryF(static_cast<double>(GetI(a)) + GetF(b)); return true;
        case 47: binaryF(static_cast<double>(GetI(a)) - GetF(b)); return true;
        case 48: binaryF(static_cast<double>(GetI(a)) / GetF(b)); return true;
        case 49: binaryF(static_cast<double>(GetI(a)) * GetF(b)); return true;
        // int a, float b comparisons (int result).
        case 50: binary(flag(Compare(GetI(a), GetF(b)) == Cmp::Equal)); return true;
        case 51: binary(flag(Compare(GetI(a), GetF(b)) != Cmp::Equal)); return true;
        case 52: {
            const Cmp c = Compare(GetI(a), GetF(b));
            binary(flag(c == Cmp::Equal || c == Cmp::Greater));
            return true;
        }
        case 53: {
            const Cmp c = Compare(GetI(a), GetF(b));
            binary(flag(c == Cmp::Less || c == Cmp::Equal));
            return true;
        }
        case 54: binary(flag(Compare(GetI(a), GetF(b)) == Cmp::Greater)); return true;
        case 55: binary(flag(Compare(GetI(a), GetF(b)) == Cmp::Less)); return true;
        case 56: SetF(x, F(std::sqrt(static_cast<double>(GetF(x))))); return true;
        case 57:
        case 58: {                                     // orig: FSIN / FCOS
            const f32 v = GetF(x);
            // x87: an argument of magnitude 2^63 or more is left unchanged (C2 set; NaN stays NaN); an infinity is an
            // invalid operation (the default NaN). The emulator used for the oracle returns infinities unchanged.
            if (std::isinf(v)) return SetF(x, std::bit_cast<f32>(0xFFC00000u)), true;
            if (std::isnan(v) || std::fabs(v) >= 9223372036854775808.0f) return SetF(x, v), true;
            // The port uses the C library within range (see ScriptNativesTests.cpp).
            SetF(x, F(method == 57 ? std::sin(static_cast<double>(v)) : std::cos(static_cast<double>(v))));
            return true;
        }
        case 59: SetF(x, std::fabs(GetF(x))); return true;
        case 60: {                                     // orig: 0x000B6FB0 (lo, hi): lo + (hi - lo) * rand() / 32767
            const double r = static_cast<double>(crt::Rand()) * static_cast<double>(std::bit_cast<f32>(0x38000100u));
            const double lo = GetF(a), hi = GetF(b);
            binaryF((hi - lo) * r + lo);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace fut::script
