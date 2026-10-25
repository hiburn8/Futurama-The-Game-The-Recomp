// DSP56300 interpreter for the GP program's instruction subset (see Dsp56300.h; encodings from the DSP56300
// Family Manual, appendix A; the census of the forms in docs/analysis/audio_dsp_port.md 11).
#include "audio/dsp/Dsp56300.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <utility>

namespace fut::audio::dsp {

namespace {

// Register codes (the manual's 6-bit DDDDDD encoding; the 5-bit fields use the same values).
constexpr u32 kX0 = 0x04, kX1 = 0x05, kY0 = 0x06, kY1 = 0x07, kA0 = 0x08, kB0 = 0x09, kA2 = 0x0A, kB2 = 0x0B,
              kA1 = 0x0C, kB1 = 0x0D, kA = 0x0E, kB = 0x0F, kR0 = 0x10, kN0 = 0x18, kM0 = 0x20, kSR = 0x39,
              kOMR = 0x3A, kLA = 0x3E, kLC = 0x3F;
constexpr u8 kSrcX48 = 0x40, kSrcY48 = 0x41;     // the 48-bit X (X1:X0) / Y (Y1:Y0) ALU sources
constexpr u8 kRegOperand = 0xFF;                 // MemOp::mode of a register bit-test operand

// the interpreter's small helpers are inlined into the instruction handlers
#if defined(_MSC_VER)
#define DSP_INLINE __forceinline
#else
#define DSP_INLINE inline __attribute__((always_inline))
#endif

s32 SextN(u32 v, u32 bits) { return static_cast<s32>(v << (32 - bits)) >> (32 - bits); }

// Multiplier operand pairs QQQ (ALU byte bits 6..4): x0,x0 y0,y0 x1,x0 y1,y0 x0,y1 y0,x0 x1,y0 y1,x1.
constexpr u8 kQQQ[8][2] = {{kX0, kX0}, {kY0, kY0}, {kX1, kX0}, {kY1, kY0},
                           {kX0, kY1}, {kY0, kX0}, {kX1, kY0}, {kY1, kX1}};
// The 24-bit data registers selected by JJJ 1xx of the ALU table (and Tcc / CMPU ggg): x0 y0 x1 y1.
constexpr u8 kJJJ[4] = {kX0, kY0, kX1, kY1};
// XY moves: (Rn), (Rn)+Nn, (Rn)-, (Rn)+ as MMM modes.
constexpr u8 kXYMode[4] = {4, 1, 2, 3};

}  // namespace

Dsp56300::Dsp56300(u32 xWords, u32 yWords, u32 pWords, Peripherals* periph)
    : x_(xWords, 0), y_(yWords, 0), p_(pWords, 0), decoded_(pWords), periph_(periph) {
    outside_.op = Op::Unsupported, outside_.exec = &ExecNonParallel;
    for (u32 code = 0; code < 64; ++code) plain_[code] = PlainReg(code);
    Reset();
}

void Dsp56300::Reset() {
    x0 = x1 = y0 = y1 = 0;
    a = b = 0;
    for (u32 i = 0; i < 8; ++i) r[i] = 0, n[i] = 0, m[i] = kMask24;   // M reset: linear addressing
    sr = 0xC00300;                               // reset SR: CP = 3, I = 3
    omr = la = lc = sp = 0;
    pc = 0;
}

void Dsp56300::Fault(const char* fmt, ...) {
    if (faulted_) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char where[48];
    std::snprintf(where, sizeof where, " (P:%04X, word %06X)", curPc_, ReadP(curPc_));
    fault_ = std::string(buf) + where;
    faulted_ = true;
}

// ---- memory ------------------------------------------------------------------------------------------------------
u32 Dsp56300::ReadSlow(Space s, u32 addr) {
    if (s == kX) {
        if (addr >= 0xFFFF80) return periph_ ? periph_->PeripheralRead(addr) & kMask24 : 0;
        if (addr - viewBase_ < viewSize_) return x_[viewTarget_ + (addr - viewBase_)];
    } else if (s == kP) {
        if (addr < p_.size()) return p_[addr];
    }
    Fault("%c:%06X read out of range", "XYP"[s], addr);
    return 0;
}

void Dsp56300::WriteSlow(Space s, u32 addr, u32 value) {
    if (s == kX) {
        if (addr >= 0xFFFF80) {
            if (periph_) periph_->PeripheralWrite(addr, value);
            return;
        }
        if (addr - viewBase_ < viewSize_) {
            x_[viewTarget_ + (addr - viewBase_)] = value;
            return;
        }
    } else if (s == kP) {
        if (addr < p_.size()) {
            WriteP(addr, value);
            return;
        }
    }
    Fault("%c:%06X write out of range", "XYP"[s], addr);
}

void Dsp56300::WriteP(u32 addr, u32 value) {
    if (addr >= p_.size()) return;
    p_[addr] = value & kMask24;
    // every instruction that covers this word decodes again (instructions are at most 2 words)
    decoded_[addr].exec = nullptr;
    if (addr >= 1) decoded_[addr - 1].exec = nullptr;
}

// ---- registers -----------------------------------------------------------------------------------------------------
bool Dsp56300::ValidReg(u32 code) const {
    return (code >= kX0 && code <= 0x27) || code == kSR || code == kOMR || code == kLA || code == kLC;
}

DSP_INLINE u32 Dsp56300::IntegerShift() const {
    switch ((sr >> 10) & 3) {                    // S1:S0
    case 0: return 47;                           // no scaling: integer part bits 55..47
    case 1: return 48;                           // scale down
    case 2: return 46;                           // scale up (effects 6 / 7: ori #$8,mr .. andi #$f7,mr)
    default: return 47;                          // reserved (the GP program never selects it)
    }
}

u32 Dsp56300::ReadReg(u32 code) {
    switch (code) {
    case kX0: return x0;
    case kX1: return x1;
    case kY0: return y0;
    case kY1: return y1;
    case kA0: return static_cast<u32>(a) & kMask24;
    case kB0: return static_cast<u32>(b) & kMask24;
    case kA2: return static_cast<u32>(static_cast<s32>(static_cast<s8>(a >> 48))) & kMask24;
    case kB2: return static_cast<u32>(static_cast<s32>(static_cast<s8>(b >> 48))) & kMask24;
    case kA1: return static_cast<u32>(a >> 24) & kMask24;
    case kB1: return static_cast<u32>(b >> 24) & kMask24;
    case kA:
    case kB: {
        // data shifter + limiter: bits k..k-23 when the integer part (bits 55..k) is a sign extension, else the
        // largest value of the accumulator's sign (and L is set)
        if ((sr >> 10 & 3) == 3) Fault("reserved scaling mode");
        const u64 v = code == kA ? a : b;
        const u32 k = IntegerShift();
        const u64 top = v >> k;
        if (top == 0 || top == (u64(1) << (56 - k)) - 1) return static_cast<u32>(v >> (k - 23)) & kMask24;
        sr |= kL;
        return (v >> 55) ? 0x800000u : 0x7FFFFFu;
    }
    case kSR: return sr;
    case kOMR: return omr;
    case kLA: return la;
    case kLC: return lc;
    default:
        if (code >= kR0 && code < kR0 + 8) return r[code - kR0];
        if (code >= kN0 && code < kN0 + 8) return n[code - kN0];
        if (code >= kM0 && code < kM0 + 8) return m[code - kM0];
        Fault("register %02X read", code);
        return 0;
    }
}

void Dsp56300::WriteReg(u32 code, u32 v) {
    v &= kMask24;
    switch (code) {
    case kX0: x0 = v; return;
    case kX1: x1 = v; return;
    case kY0: y0 = v; return;
    case kY1: y1 = v; return;
    case kA0: a = (a & ~u64(kMask24)) | v; return;
    case kB0: b = (b & ~u64(kMask24)) | v; return;
    case kA2: a = (a & kMask48) | (u64(v & 0xFF) << 48); return;
    case kB2: b = (b & kMask48) | (u64(v & 0xFF) << 48); return;
    case kA1: a = (a & ~(u64(kMask24) << 24)) | (u64(v) << 24); return;
    case kB1: b = (b & ~(u64(kMask24) << 24)) | (u64(v) << 24); return;
    case kA: a = Acc24(v); return;
    case kB: b = Acc24(v); return;
    case kSR: sr = v; return;
    case kOMR: omr = v; return;
    case kLA: la = v; return;
    case kLC: lc = v; return;
    default:
        if (code >= kR0 && code < kR0 + 8) r[code - kR0] = v;
        else if (code >= kN0 && code < kN0 + 8) n[code - kN0] = v;
        else if (code >= kM0 && code < kM0 + 8) m[code - kM0] = v;
        else Fault("register %02X write", code);
    }
}

// ---- address generation (linear only) ----------------------------------------------------------------------------
DSP_INLINE u32 Dsp56300::Offset(u32 rn, s32 delta) {
    if (m[rn] != kMask24) Fault("address modifier M%u = %06X (modulo / bit-reverse not modelled)", rn, m[rn]);
    return static_cast<u32>(static_cast<s32>(r[rn]) + delta) & kMask24;
}

DSP_INLINE u32 Dsp56300::EffectiveAddress(const MemOp& o) {
    const u32 i = o.ear;
    switch (o.mode) {
    case 0: { const u32 ad = r[i]; r[i] = Offset(i, -Sext24(n[i])); return ad; }   // (Rn)-Nn
    case 1: { const u32 ad = r[i]; r[i] = Offset(i, Sext24(n[i])); return ad; }    // (Rn)+Nn
    case 2: { const u32 ad = r[i]; r[i] = Offset(i, -1); return ad; }              // (Rn)-
    case 3: { const u32 ad = r[i]; r[i] = Offset(i, 1); return ad; }               // (Rn)+
    case 4: return r[i];                                                           // (Rn)
    case 5: return Offset(i, Sext24(n[i]));                                        // (Rn+Nn)
    case 7: r[i] = Offset(i, -1); return r[i];                                     // -(Rn)
    case kAbs:
    case kShort: return o.addr;
    default: Fault("effective address mode %u", o.mode); return 0;
    }
}

// ---- data ALU -----------------------------------------------------------------------------------------------------
DSP_INLINE u64 Dsp56300::Source56(u32 code) {
    switch (code) {
    case kA: return a;
    case kB: return b;
    case kSrcX48: return static_cast<u64>(static_cast<s64>((u64(x1) << 40) | (u64(x0) << 16)) >> 8) & kMask56;
    case kSrcY48: return static_cast<u64>(static_cast<s64>((u64(y1) << 40) | (u64(y0) << 16)) >> 8) & kMask56;
    default: return Acc24(ReadReg(code));
    }
}

DSP_INLINE void Dsp56300::Flags56(u64 res, bool v, bool setC, bool c) {
    const u32 k = IntegerShift();
    u32 f = 0;
    if (res >> 55) f |= kN;
    if (res == 0) f |= kZ;
    const u64 top = res >> k;
    if (top != 0 && top != (u64(1) << (56 - k)) - 1) f |= kE;      // extension in use
    if (((res >> k) & 1) == ((res >> (k - 1)) & 1)) f |= kU;        // unnormalized
    if (v) f |= kV | kL;                                            // L: sticky overflow
    u32 mask = kN | kZ | kE | kU | kV;
    if (setC) {
        mask |= kC;
        if (c) f |= kC;
    }
    sr = (sr & ~mask) | f;
}

DSP_INLINE u64 Dsp56300::Saturate(u64 res, bool& v) const {
    if (!(sr & kSM)) return res;
    const u64 top = res >> 47;
    if (top == 0 || top == 0x1FF) return res;
    v = true;
    return (res >> 55) ? 0xFF800000000000ull : 0x007FFFFFFFFFFFull;
}

DSP_INLINE u64 Dsp56300::Round(u64 acc) {
    if (sr & (kRM | (3u << 10))) Fault("rounding with RM / a scaling mode (not modelled)");
    return RoundConvergent(acc);
}

DSP_INLINE u64 Dsp56300::Add56(u64 d, u64 s, bool& c, bool& v) const {
    const u64 t = d + s;
    const u64 res = t & kMask56;
    c = (t >> 56) & 1;
    v = !(((d ^ s) >> 55) & 1) && (((d ^ res) >> 55) & 1);
    return res;
}

DSP_INLINE u64 Dsp56300::Sub56(u64 d, u64 s, bool& c, bool& v) const {
    const u64 res = (d - s) & kMask56;
    c = d < s;
    v = (((d ^ s) >> 55) & 1) && (((d ^ res) >> 55) & 1);
    return res;
}

DSP_INLINE bool Dsp56300::Condition(u32 cc) const {
    const bool C = sr & kC, V = sr & kV, Z = sr & kZ, N = sr & kN, U = sr & kU, E = sr & kE, L = sr & kL;
    switch (cc & 15) {
    case 0: return !C;                           // cc (hs)
    case 1: return N == V;                       // ge
    case 2: return !Z;                           // ne
    case 3: return !N;                           // pl
    case 4: return !(Z || (!U && !E));           // nn
    case 5: return !E;                           // ec
    case 6: return !L;                           // lc
    case 7: return !(Z || (N != V));             // gt
    case 8: return C;                            // cs (lo)
    case 9: return N != V;                       // lt
    case 10: return Z;                           // eq
    case 11: return N;                           // mi
    case 12: return Z || (!U && !E);             // nr
    case 13: return E;                           // es
    case 14: return L;                           // ls
    default: return Z || (N != V);               // le
    }
}

template <Dsp56300::Alu A>
void Dsp56300::AluOp(const Insn& d) {
    // A is a template argument: every parallel form has its own handler, the switch folds away
    u64& D = Acc(d.dst);
    bool c = false, v = false, sv = false;
    switch (A) {
    case Alu::None: return;
    case Alu::Mpy:
    case Alu::Mac: {
        u64 p = FracProduct(*plain_[d.src], *plain_[d.src2]);
        if (d.neg) p = (0 - p) & kMask56;
        u64 res = d.accumulate ? Add56(D, p, c, v) : p;
        if (d.round) res = Round(res);
        res = Saturate(res, sv);
        D = res;
        Flags56(res, v || sv, false, false);     // C is not affected by the multiplier instructions
        return;
    }
    case Alu::Add:
    case Alu::Sub: {
        const u64 s = Source56(d.src);
        u64 res = A == Alu::Add ? Add56(D, s, c, v) : Sub56(D, s, c, v);
        res = Saturate(res, sv);
        D = res;
        Flags56(res, v || sv, true, c);
        return;
    }
    case Alu::Cmp: {
        const u64 res = Sub56(D, Source56(d.src), c, v);
        Flags56(res, v, true, c);
        return;
    }
    case Alu::Tfr: D = Source56(d.src); return;  // no flags
    case Alu::Clr:
        D = 0;
        sr = (sr & ~(kN | kE | kV)) | kZ | kU;
        return;
    case Alu::Tst: Flags56(D, false, true, false); return;
    case Alu::Abs:
    case Alu::Neg: {
        const s64 sd = Sext56(D);
        const s64 rv = (A == Alu::Neg || sd < 0) ? -sd : sd;
        v = D == (u64(1) << 55);                 // the most negative value has no positive counterpart
        const u64 res = Saturate(static_cast<u64>(rv) & kMask56, sv);
        D = res;
        Flags56(res, v || sv, false, false);
        return;
    }
    case Alu::Asl: {
        c = (D >> 55) & 1;
        v = ((D >> 55) ^ (D >> 54)) & 1;
        const u64 res = Saturate((D << 1) & kMask56, sv);
        D = res;
        Flags56(res, v || sv, true, c);
        return;
    }
    case Alu::Asr: {
        c = D & 1;
        const u64 res = static_cast<u64>(Sext56(D) >> 1) & kMask56;
        D = res;
        Flags56(res, false, true, c);
        return;
    }
    case Alu::Rnd: {
        const u64 res = Saturate(Round(D), sv);
        D = res;
        Flags56(res, sv, false, false);
        return;
    }
    case Alu::Max:                               // MAX A,B: if B - A <= 0 then A -> B (signed)
    case Alu::Maxm: {                            // MAXM A,B: if |B| - |A| <= 0 then A -> B
        const u64 S = Acc(d.src == kB ? 1 : 0);
        s64 sd = Sext56(D), ss = Sext56(S);
        if constexpr (A == Alu::Maxm) sd = sd < 0 ? -sd : sd, ss = ss < 0 ? -ss : ss;
        if (sd - ss <= 0) {
            D = S;
            sr &= ~kC;
        } else {
            sr |= kC;                            // C: set when no transfer happened (L)
        }
        return;
    }
    case Alu::And:
    case Alu::Or:
    case Alu::Eor: {
        const u32 s = ReadReg(d.src);
        const u32 d1 = static_cast<u32>(D >> 24) & kMask24;
        const u32 res = A == Alu::And ? d1 & s : A == Alu::Or ? d1 | s : d1 ^ s;
        D = (D & ~(u64(kMask24) << 24)) | (u64(res) << 24);
        sr = (sr & ~(kN | kZ | kV)) | ((res & 0x800000) ? kN : 0) | (res == 0 ? kZ : 0);
        return;
    }
    case Alu::Bad:
    case Alu::Count: Fault("data ALU operation not modelled"); return;
    }
}

// ---- decoding -------------------------------------------------------------------------------------------------------
void Dsp56300::DecodeAluByte(u32 byte, Insn& d) {
    d.dst = (byte >> 3) & 1;
    const u8 other = d.dst ? kA : kB;
    if (byte & 0x80) {                           // multiply: 1QQQ dkoo, oo = mpy mpyr mac macr
        d.alu = (byte & 2) ? Alu::Mac : Alu::Mpy;
        d.accumulate = (byte >> 1) & 1;
        d.round = byte & 1;
        d.neg = (byte >> 2) & 1;
        d.src = kQQQ[(byte >> 4) & 7][0];
        d.src2 = kQQQ[(byte >> 4) & 7][1];
        return;
    }
    const u32 jjj = (byte >> 4) & 7, kkk = byte & 7;
    d.alu = Alu::Bad;
    switch (jjj) {
    case 0:                                      // move / tfr / addr / tst / - / cmp / subr / cmpm (other acc)
        if (byte == 0) d.alu = Alu::None;
        else if (kkk == 1) d.alu = Alu::Tfr, d.src = other;
        else if (kkk == 3) d.alu = Alu::Tst;
        else if (kkk == 5) d.alu = Alu::Cmp, d.src = other;
        break;
    case 1:                                      // add / rnd / addl / clr / sub / max(m) / subl / not
        if (kkk == 0) d.alu = Alu::Add, d.src = other;
        else if (kkk == 1) d.alu = Alu::Rnd;
        else if (kkk == 3) d.alu = Alu::Clr;
        else if (kkk == 4) d.alu = Alu::Sub, d.src = other;
        else if (kkk == 5) d.alu = d.dst ? Alu::Max : Alu::Maxm, d.src = kA, d.dst = 1;   // 0x1D MAX, 0x15 MAXM: A,B
        break;
    case 2:                                      // X: add / adc / asr / lsr / sub / sbc / abs / ror
        if (kkk == 0) d.alu = Alu::Add, d.src = kSrcX48;
        else if (kkk == 2) d.alu = Alu::Asr;
        else if (kkk == 4) d.alu = Alu::Sub, d.src = kSrcX48;
        else if (kkk == 6) d.alu = Alu::Abs;
        break;
    case 3:                                      // Y: add / adc / asl / lsl / sub / sbc / neg / rol
        if (kkk == 0) d.alu = Alu::Add, d.src = kSrcY48;
        else if (kkk == 2) d.alu = Alu::Asl;
        else if (kkk == 4) d.alu = Alu::Sub, d.src = kSrcY48;
        else if (kkk == 6) d.alu = Alu::Neg;
        break;
    default: {                                   // x0 / y0 / x1 / y1: add tfr or eor sub cmp and cmpm
        d.src = kJJJ[jjj - 4];
        constexpr Alu ops[8] = {Alu::Add, Alu::Tfr, Alu::Or, Alu::Eor, Alu::Sub, Alu::Cmp, Alu::And, Alu::Bad};
        d.alu = ops[kkk];
        break;
    }
    }
}

void Dsp56300::DecodeParallel(u32 w, u32 ext, Insn& d) const {
    d.op = Op::Parallel;
    DecodeAluByte(w & 0xFF, d);
    if (d.alu == Alu::Bad) {
        d.op = Op::Unsupported;
        return;
    }
    auto bad = [&] { d.op = Op::Unsupported; };
    if ((w >> 21) == 1) {                        // 001.....
        if (((w >> 18) & 7) == 0) {              // 0010 00..
            const u32 hi = w >> 8;
            if (hi == 0x2000) d.move = Move::None;
            else if ((hi & 0xFFE0) == 0x2040) bad();              // address register update: not used
            else if ((hi & 0xFFE0) == 0x2020) {                   // IFcc / IFcc.U
                d.move = Move::Ifcc, d.cond = hi & 0xF, d.ccrUpdate = (hi >> 4) & 1;
                if (d.alu == Alu::None) bad();
            } else {                                              // 001000ee eeeddddd: register to register
                d.move = Move::Reg, d.r1 = (w >> 13) & 0x1F, d.r2 = (w >> 8) & 0x1F;
                if (!ValidReg(d.r1) || !ValidReg(d.r2)) bad();
            }
        } else {                                                  // 001ddddd iiiiiiii: #xx,D
            d.move = Move::Imm8, d.r2 = (w >> 16) & 0x1F, d.imm = (w >> 8) & 0xFF;
            if (!ValidReg(d.r2)) bad();
        }
        return;
    }
    if ((w >> 20) == 1) return bad();            // class I X:R / R:Y moves: not used
    if ((w >> 22) == 1) {                        // 01dd.ddd W.MMMRRR: X: / Y: (01.0 0.. = L:, not used)
        if (((w >> 20) & 3) == 0 && ((w >> 18) & 1) == 0) return bad();
        MemOp& o = d.m0;
        o.space = (w >> 19) & 1;
        o.reg = static_cast<u8>(((w >> 17) & 0x18) | ((w >> 16) & 7));
        o.toReg = (w >> 15) & 1;
        if (!ValidReg(o.reg)) return bad();
        if ((w >> 14) & 1) {                     // effective address
            o.mode = (w >> 11) & 7, o.ear = (w >> 8) & 7;
            if (o.mode == 6) {
                if (o.ear == 0) o.mode = kAbs;
                else if (o.ear == 4 && o.toReg) o.mode = kImm;
                else return bad();
                o.addr = ext;
                d.len = 2;
            }
        } else {                                 // absolute short X:aa / Y:aa
            o.mode = kShort, o.addr = (w >> 8) & 0x3F;
        }
        d.move = Move::Mem;
        return;
    }
    if ((w >> 23) == 1) {                        // 1wmmeeff WrrMMRRR: X:<ea> and Y:<ea> at once
        MemOp& xo = d.m0;
        MemOp& yo = d.m1;
        constexpr u8 xRegs[4] = {kX0, kX1, kA, kB}, yRegs[4] = {kY0, kY1, kA, kB};
        xo.space = kX, xo.toReg = (w >> 15) & 1, xo.mode = kXYMode[(w >> 11) & 3], xo.ear = (w >> 8) & 7;
        xo.reg = xRegs[(w >> 18) & 3];
        yo.space = kY, yo.toReg = (w >> 22) & 1, yo.mode = kXYMode[(w >> 20) & 3];
        yo.ear = static_cast<u8>(((w >> 13) & 3) + (xo.ear < 4 ? 4 : 0));   // the other register bank
        yo.reg = yRegs[(w >> 16) & 3];
        d.move = Move::MemMem;
        return;
    }
    bad();
}

void Dsp56300::Decode(u32 addr, Insn& d) {
    d = Insn{};
    const u32 w = p_[addr] & kMask24;
    const u32 ext = addr + 1 < p_.size() ? p_[addr + 1] & kMask24 : 0;
    if ((w >> 20) != 0) return DecodeParallel(w, ext, d);
    d.op = Op::Unsupported;
    if ((w >> 17) == 4 && !((w >> 14) & 1)) return;           // class II parallel moves: not used
    auto two = [&] { d.len = 2, d.imm = ext; };
    auto memEa = [&](MemOp& o, u32 field) {      // MMMRRR with the absolute (110 000) / immediate (110 100) forms
        o.mode = (field >> 3) & 7, o.ear = field & 7;
        if (o.mode != 6) return true;
        if (o.ear == 0) o.mode = kAbs;
        else if (o.ear == 4 && o.toReg) o.mode = kImm;
        else return false;
        o.addr = ext;
        d.len = 2;
        return true;
    };
    if (w == 0x000000) { d.op = Op::Nop; return; }
    if (w == 0x00000C) { d.op = Op::Rts; return; }
    if ((w & 0xFF00FC) == 0x0000F8 || (w & 0xFF00FC) == 0x0000B8) {     // ori / andi #xx,mr|ccr
        if ((w & 3) > 1) return;                 // com / eom: not used
        d.op = Op::AndOrSr, d.k = (w & 0xFF00FC) == 0x0000F8, d.imm = (w >> 8) & 0xFF, d.r1 = w & 3;
        return;
    }
    if ((w & 0xFFC0F0) == 0x014080 || (w & 0xFFFFF0) == 0x0140C0) {     // add/sub/cmp/and/or/eor #xx | #xxxxxx
        const u32 k = w & 7;
        if (k == 1 || k == 7) return;
        d.op = Op::AluImm, d.k = static_cast<u8>(k), d.dst = (w >> 3) & 1;
        if ((w & 0xFFFFF0) == 0x0140C0) two(), d.r1 = 1;          // 24-bit immediate (sign-extended)
        else d.imm = (w >> 8) & 0x3F;                             // 6-bit immediate (zero-extended)
        return;
    }
    if ((w & 0xFFFFC0) == 0x0141C0) {            // mpyi #xxxxxx,S,D (maci / mpyri / macri not used)
        if (w & 3) return;
        d.op = Op::Mpyi, d.src = kJJJ[(w >> 4) & 3], d.dst = (w >> 3) & 1, d.neg = (w >> 2) & 1;
        two();
        return;
    }
    if ((w & 0xFE0080) == 0x020080) {            // move X|Y:(Rn+xx),D | S,X|Y:(Rn+xx)  (7-bit displacement)
        d.op = Op::MoveDisp, d.m0.space = (w >> 5) & 1, d.m0.toReg = (w >> 4) & 1, d.m0.reg = w & 0xF;
        d.m0.ear = (w >> 8) & 7;
        d.imm = static_cast<u32>(SextN(((w >> 11) & 0x3F) << 1 | ((w >> 6) & 1), 7));
        if (d.m0.reg < kX0) d.op = Op::Unsupported;
        return;
    }
    if ((w & 0xFEF880) == 0x0A7080) {            // move X|Y:(Rn+xxxxxx),D | S,X|Y:(Rn+xxxxxx)
        d.op = Op::MoveDisp, d.m0.space = (w >> 16) & 1, d.m0.toReg = (w >> 6) & 1, d.m0.reg = w & 0x3F;
        d.m0.ear = (w >> 8) & 7;
        d.len = 2, d.imm = static_cast<u32>(Sext24(ext));
        if (!ValidReg(d.m0.reg)) d.op = Op::Unsupported;
        return;
    }
    if ((w & 0xFFC000) == 0x040000) {            // lua (Rn+aa),D
        d.op = Op::Lua, d.m0.ear = (w >> 8) & 7;
        d.imm = static_cast<u32>(SextN(((w >> 11) & 7) << 4 | ((w >> 4) & 0xF), 7));
        d.r2 = static_cast<u8>(((w & 8) ? kN0 : kR0) + (w & 7));
        return;
    }
    if ((w & 0xFF40A0) == 0x054020 || (w & 0xFF40E0) == 0x0440A0) {     // movec ea | reg
        d.op = Op::Movec, d.r1 = static_cast<u8>(0x20 | (w & 0x1F)), d.m0.toReg = (w >> 15) & 1;
        if (!ValidReg(d.r1) || d.r1 > 0x27 && d.r1 != kSR && d.r1 != kOMR && d.r1 != kLA && d.r1 != kLC)
            return void(d.op = Op::Unsupported);
        if ((w & 0xFF40A0) == 0x054020) {
            d.k = 0, d.m0.space = (w >> 6) & 1;
            if (!memEa(d.m0, (w >> 8) & 0x3F)) d.op = Op::Unsupported;
        } else {
            d.k = 1, d.r2 = (w >> 8) & 0x3F;
            if (!ValidReg(d.r2)) d.op = Op::Unsupported;
        }
        return;
    }
    if ((w & 0xFF40C0) == 0x074080) {            // movem P:ea,D | S,P:ea
        d.op = Op::Movem, d.m0.space = kP, d.m0.toReg = (w >> 15) & 1, d.m0.reg = w & 0x3F;
        if (!ValidReg(d.m0.reg) || !memEa(d.m0, (w >> 8) & 0x3F) || d.m0.mode == kImm) d.op = Op::Unsupported;
        return;
    }
    if ((w & 0xFF4080) == 0x074000 || (w & 0xFE4080) == 0x084080 || (w & 0xFE40C0) == 0x084000) {   // movep
        d.op = Op::Movep, d.m0.toReg = (w >> 15) & 1;            // W = 1: to the peripheral
        if ((w & 0xFF4080) == 0x074000) {                         // X|Y:ea <-> X:qq
            d.k = 0, d.r1 = kX, d.target = 0xFFFF80 + (w & 0x3F), d.m0.space = (w >> 6) & 1;
            if (!memEa(d.m0, (w >> 8) & 0x3F)) d.op = Op::Unsupported;
        } else if ((w & 0xFE4080) == 0x084080) {                  // X|Y:ea <-> X|Y:pp
            d.k = 0, d.r1 = (w >> 16) & 1, d.target = 0xFFFFC0 + (w & 0x3F), d.m0.space = (w >> 6) & 1;
            if (!memEa(d.m0, (w >> 8) & 0x3F)) d.op = Op::Unsupported;
        } else {                                                  // register <-> X|Y:pp
            d.k = 1, d.r1 = (w >> 16) & 1, d.target = 0xFFFFC0 + (w & 0x3F), d.m0.reg = (w >> 8) & 0x3F;
            if (!ValidReg(d.m0.reg)) d.op = Op::Unsupported;
        }
        return;
    }
    if ((w & 0xFF00F0) == 0x060090 || (w & 0xFFC0FF) == 0x06C010) {     // dor #n | S,label (LA = here + disp)
        d.op = Op::Dor;
        if ((w & 0xFF00F0) == 0x060090) d.k = 0, d.imm = ((w & 0xF) << 8) | ((w >> 8) & 0xFF);
        else d.k = 1, d.r1 = (w >> 8) & 0x3F;
        d.len = 2;
        d.target = (addr + static_cast<u32>(Sext24(ext))) & kMask24;
        if (d.k && !ValidReg(d.r1)) d.op = Op::Unsupported;
        return;
    }
    if ((w & 0xFF00F0) == 0x0600A0 || (w & 0xFFC0FF) == 0x06C020) {     // rep #n | S
        d.op = Op::Rep;
        if ((w & 0xFF00F0) == 0x0600A0) d.k = 0, d.imm = ((w & 0xF) << 8) | ((w >> 8) & 0xFF);
        else d.k = 1, d.r1 = (w >> 8) & 0x3F;
        if (d.k && !ValidReg(d.r1)) d.op = Op::Unsupported;
        return;
    }
    if (w == 0x0D10C0 || w == 0x0D1080 || (w & 0xFFFFF0) == 0x0D1040) {  // bra / bsr / bcc xxxxxx
        d.op = w == 0x0D10C0 ? Op::Bra : w == 0x0D1080 ? Op::Bsr : Op::Bcc, d.cond = w & 0xF;
        d.len = 2;
        d.target = (addr + static_cast<u32>(Sext24(ext))) & kMask24;
        return;
    }
    if ((w & 0xFFFC20) == 0x050C00 || (w & 0xFFFC20) == 0x050800 || (w & 0xFF0C20) == 0x050400) {  // 9-bit
        d.op = (w & 0xFFFC20) == 0x050C00 ? Op::Bra : (w & 0xFFFC20) == 0x050800 ? Op::Bsr : Op::Bcc;
        d.cond = (w >> 12) & 0xF;
        d.target = (addr + static_cast<u32>(SextN(((w >> 6) & 0xF) << 5 | (w & 0x1F), 9))) & kMask24;
        return;
    }
    if (w == 0x0BF080) {                         // jsr xxxxxx
        d.op = Op::Jsr, d.len = 2, d.target = ext;
        return;
    }
    if ((w & 0xFFC0C0) == 0x0AC040) {            // bclr / bset #n,D
        d.op = Op::BitReg, d.r1 = (w >> 8) & 0x3F, d.k = w & 0x1F, d.cond = (w >> 5) & 1;
        if (!ValidReg(d.r1) || d.r1 == kA || d.r1 == kB) d.op = Op::Unsupported;
        return;
    }
    if ((w & 0xFFC080) == 0x0CC000 || (w & 0xFFC0C0) == 0x0CC080) {     // brclr / brset #n,X|Y:pp | S,xxxxxx
        d.op = Op::BitBranch, d.k = w & 0x1F, d.cond = (w >> 5) & 1, d.len = 2;
        d.target = (addr + static_cast<u32>(Sext24(ext))) & kMask24;
        if ((w & 0xFFC080) == 0x0CC000) d.m0.mode = kAbs, d.m0.addr = 0xFFFFC0 + ((w >> 8) & 0x3F), d.m0.space = (w >> 6) & 1;
        else d.m0.mode = kRegOperand, d.m0.reg = (w >> 8) & 0x3F;
        if (d.m0.mode == kRegOperand && !ValidReg(d.m0.reg)) d.op = Op::Unsupported;
        return;
    }
    if ((w & 0xFFC0A0) == 0x0A0080 || (w & 0xFFC0A0) == 0x0A8080) {     // jclr #n,X|Y:aa | X|Y:pp,xxxx
        d.op = Op::BitJump, d.k = w & 0x1F, d.cond = 0, d.len = 2, d.target = ext;
        d.m0.mode = kAbs, d.m0.space = (w >> 6) & 1;
        d.m0.addr = (w & 0x008000) ? 0xFFFFC0 + ((w >> 8) & 0x3F) : (w >> 8) & 0x3F;
        return;
    }
    if ((w & 0xFFFF00) == 0x0C1D00 || (w & 0xFFFF00) == 0x0C1C00) {     // asl / asr #n,S,D
        d.op = Op::ShiftImm, d.r1 = (w & 0xFFFF00) == 0x0C1C00, d.k = (w >> 1) & 0x3F;
        d.src = (w >> 7) & 1, d.dst = w & 1;
        if (d.k > 40) d.op = Op::Unsupported;
        return;
    }
    if ((w & 0xFFFFF0) == 0x0C1FF0) {            // cmpu S,D
        const u32 g = (w >> 1) & 7;
        if (g != 0 && g < 4) return;
        d.op = Op::Cmpu, d.dst = w & 1, d.src = g == 0 ? (d.dst ? kA : kB) : kJJJ[g - 4];
        return;
    }
    // anything else stays Unsupported (asl / asr S1,S2,D of the dead shift helpers included)
}

u32* Dsp56300::PlainReg(u32 code) {
    switch (code) {
    case kX0: return &x0;
    case kX1: return &x1;
    case kY0: return &y0;
    case kY1: return &y1;
    default:
        if (code >= kR0 && code < kR0 + 8) return &r[code - kR0];
        if (code >= kN0 && code < kN0 + 8) return &n[code - kN0];
        if (code >= kM0 && code < kM0 + 8) return &m[code - kM0];
        return nullptr;                          // accumulators (limiter, partial words), SR, LA, LC, ...
    }
}

Dsp56300::Handler Dsp56300::ParallelHandler(Move mv, Alu alu) {
    // one handler per (move kind, ALU operation): ExecParallel<M, A> instantiated for every pair
    constexpr std::size_t kAlus = static_cast<std::size_t>(Alu::Count);
    static constexpr auto table = []<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<Handler, sizeof...(I)>{
            &ExecParallel<static_cast<Move>(I / kAlus), static_cast<Alu>(I % kAlus)>...};
    }(std::make_index_sequence<static_cast<std::size_t>(Move::Count) * kAlus>{});
    return table[static_cast<std::size_t>(mv) * kAlus + static_cast<std::size_t>(alu)];
}

void Dsp56300::Resolve(Insn& d) {
    if (d.op == Op::Parallel) {
        d.exec = ParallelHandler(d.move, d.alu);
    } else {
        d.exec = d.op == Op::MoveDisp ? &ExecMoveDisp : &ExecNonParallel;
    }
}

DSP_INLINE const Dsp56300::Insn& Dsp56300::Fetch(u32 addr) {
    if (addr >= p_.size()) return outside_;
    Insn& d = decoded_[addr];
    if (!d.exec) {
        Decode(addr, d);
        Resolve(d);
    }
    return d;
}

// ---- execution ------------------------------------------------------------------------------------------------------
void Dsp56300::Push(u32 hi, u32 lo) {
    if (sp >= 15) return Fault("system stack overflow");
    ++sp;
    ssh[sp] = hi & kMask24, ssl[sp] = lo & kMask24;
}

void Dsp56300::Pop(u32& hi, u32& lo) {
    if (sp == 0) {
        hi = lo = 0;
        return Fault("system stack underflow");
    }
    hi = ssh[sp], lo = ssl[sp];
    --sp;
}

namespace {
// A parallel move's destination, written after the data ALU operation.
struct MoveDest {
    u32* plain;                                  // a plain 24-bit register, or
    u8 reg;                                      // another register (0: memory)
    u8 space;
    u32 addr, value;
};
}  // namespace

DSP_INLINE void Dsp56300::MoveOperand(const MemOp& o, void* out) {
    MoveDest& dst = *static_cast<MoveDest*>(out);
    if (o.toReg) {
        const u32 v = o.mode == kImm ? o.addr : Read(static_cast<Space>(o.space), EffectiveAddress(o));
        dst = {plain_[o.reg], o.reg, 0, 0, v};
    } else {
        const u32 ad = EffectiveAddress(o);
        const u32* p = plain_[o.reg];
        dst = {nullptr, 0, o.space, ad, p ? *p : ReadReg(o.reg)};
    }
}

template <Dsp56300::Move M, Dsp56300::Alu A>
void Dsp56300::ExecParallel(Dsp56300& c, const Insn& d, u32, u32&) {
    // sources and effective addresses first (with the register values at the start of the instruction), then the
    // data ALU operation, then the destinations
    MoveDest dest[2];
    u32 count = 0;
    if constexpr (M == Move::Mem || M == Move::MemMem) {
        c.MoveOperand(d.m0, &dest[count++]);
        if constexpr (M == Move::MemMem) c.MoveOperand(d.m1, &dest[count++]);
    }
    if constexpr (M == Move::Imm8) {
        // #xx: a fraction (MSB-aligned) for X0..Y1 / A / B, an integer (right-aligned) for the others
        const bool frac = (d.r2 >= kX0 && d.r2 <= kY1) || d.r2 == kA || d.r2 == kB;
        dest[count++] = {nullptr, d.r2, 0, 0, frac ? (d.imm << 16) & kMask24 : d.imm};
    } else if constexpr (M == Move::Reg) {
        dest[count++] = {nullptr, d.r2, 0, 0, c.ReadReg(d.r1)};
    } else if constexpr (M == Move::Ifcc) {
        if (c.Condition(d.cond)) {
            const u32 saved = c.sr;
            c.AluOp<A>(d);
            if (!d.ccrUpdate) c.sr = saved;      // IFcc leaves the CCR unchanged (IFcc.U updates it)
        }
        return;
    } else if constexpr (M == Move::Bad || M == Move::Count) {
        c.Fault("parallel move not modelled");
        return;
    }
    if constexpr (M != Move::Ifcc && M != Move::Bad && M != Move::Count) {
        c.AluOp<A>(d);
        for (u32 i = 0; i < count; ++i) {
            if (dest[i].plain) *dest[i].plain = dest[i].value & kMask24;
            else if (dest[i].reg) c.WriteReg(dest[i].reg, dest[i].value);
            else c.Write(static_cast<Space>(dest[i].space), dest[i].addr, dest[i].value);
        }
    }
}

void Dsp56300::ExecNonParallel(Dsp56300& c, const Insn& d, u32 addr, u32& next) { c.Execute(d, addr, next); }

void Dsp56300::ExecMoveDisp(Dsp56300& c, const Insn& d, u32, u32&) {
    // move X|Y:(Rn+disp),D / S,X|Y:(Rn+disp) (the most frequent non-parallel form: the effects' state accesses)
    const u32 ad = c.Offset(d.m0.ear, static_cast<s32>(d.imm));
    if (d.m0.toReg) c.WriteReg(d.m0.reg, c.Read(static_cast<Space>(d.m0.space), ad));
    else c.Write(static_cast<Space>(d.m0.space), ad, c.ReadReg(d.m0.reg));
}

void Dsp56300::LoopCheck(u32 lastWord, u32& next) {
    // end of a DO / DOR body: the instruction whose last word is at LA ran without a taken branch
    if (!(sr & kLF) || lastWord != la) return;
    if (lc <= 1) {
        u32 start = 0, savedSr = 0;
        Pop(start, savedSr);                     // (loop start, SR): restore LF
        sr = (sr & ~kLF) | (savedSr & kLF);
        Pop(la, lc);                             // the enclosing loop's LA / LC
    } else {
        lc = (lc - 1) & kMask24;
        next = ssh[sp];
    }
}

void Dsp56300::Execute(const Insn& d, u32 addr, u32& next) {
    switch (d.op) {
    case Op::Parallel: d.exec(*this, d, addr, next); return;
    case Op::Nop: return;
    case Op::Rts: {
        u32 hi = 0, lo = 0;
        Pop(hi, lo);                             // RTS restores the PC only
        next = hi;
        return;
    }
    case Op::Jsr:
    case Op::Bsr:
        Push(next, sr);
        next = d.target;
        return;
    case Op::Bra: next = d.target; return;
    case Op::Bcc:
        if (Condition(d.cond)) next = d.target;
        return;
    case Op::AluImm: {
        u64& D = Acc(d.dst);
        bool c = false, v = false, sv = false;
        if (d.k == 0 || d.k == 4 || d.k == 5) {  // add / sub / cmp: #xx right-aligned in A1, #xxxxxx sign-extended
            const u64 s = d.r1 ? Acc24(d.imm) : u64(d.imm) << 24;
            u64 res = d.k == 0 ? Add56(D, s, c, v) : Sub56(D, s, c, v);
            if (d.k == 5) return Flags56(res, v, true, c);
            res = Saturate(res, sv);
            D = res;
            Flags56(res, v || sv, true, c);
        } else {                                 // or / eor / and on A1 (A2 / A0 kept)
            const u32 d1 = static_cast<u32>(D >> 24) & kMask24;
            const u32 res = d.k == 2 ? d1 | d.imm : d.k == 3 ? d1 ^ d.imm : d1 & d.imm;
            D = (D & ~(u64(kMask24) << 24)) | (u64(res & kMask24) << 24);
            sr = (sr & ~(kN | kZ | kV)) | ((res & 0x800000) ? kN : 0) | ((res & kMask24) == 0 ? kZ : 0);
        }
        return;
    }
    case Op::Mpyi: {
        u64 p = FracProduct(ReadReg(d.src), d.imm);
        if (d.neg) p = (0 - p) & kMask56;
        bool sv = false;
        p = Saturate(p, sv);
        Acc(d.dst) = p;
        Flags56(p, sv, false, false);
        return;
    }
    case Op::ShiftImm: {
        const u64 S = Acc(d.src);
        const u32 sh = d.k;
        bool sv = false;
        if (!d.r1) {                             // asl #n: C = last bit out, V = bits 55..55-n not all equal
            const bool c = sh ? (S >> (56 - sh)) & 1 : false;
            const s64 top = Sext56(S) >> (55 - sh);
            const bool v = top != 0 && top != -1;
            const u64 res = Saturate((S << sh) & kMask56, sv);
            Acc(d.dst) = res;
            Flags56(res, v || sv, true, c);
        } else {                                 // asr #n: C = last bit out, V = 0
            const bool c = sh ? (S >> (sh - 1)) & 1 : false;
            const u64 res = static_cast<u64>(Sext56(S) >> sh) & kMask56;
            Acc(d.dst) = res;
            Flags56(res, false, true, c);
        }
        return;
    }
    case Op::Cmpu: {                             // (L) unsigned compare of the low 48 bits; executive statistics only
        const u64 dv = Acc(d.dst) & kMask48;
        const u64 sv = (d.src == kA || d.src == kB) ? Acc(d.src == kB) & kMask48 : u64(ReadReg(d.src)) << 24;
        const u64 res = (dv - sv) & kMask48;
        sr = (sr & ~(kC | kZ | kN | kV)) | (dv < sv ? kC : 0) | (res == 0 ? kZ : 0);
        return;
    }
    case Op::AndOrSr: {
        const u32 shift = d.r1 == 0 ? 8 : 0;     // EE: 0 mr (SR 15..8), 1 ccr (SR 7..0)
        const u32 cur = (sr >> shift) & 0xFF;
        const u32 nv = d.k ? cur | d.imm : cur & d.imm;
        sr = (sr & ~(0xFFu << shift)) | (nv << shift);
        return;
    }
    case Op::BitReg: {
        const u32 val = ReadReg(d.r1);
        const bool old = (val >> d.k) & 1;
        WriteReg(d.r1, d.cond ? val | (1u << d.k) : val & ~(1u << d.k));
        if (!(d.r1 == kSR && d.k < 8)) sr = (sr & ~kC) | (old ? kC : 0);   // C = the tested bit (not for CCR bits)
        return;
    }
    case Op::BitBranch:
    case Op::BitJump: {
        const u32 val = d.m0.mode == kRegOperand ? ReadReg(d.m0.reg) : Read(static_cast<Space>(d.m0.space), d.m0.addr);
        if (((val >> d.k) & 1) == d.cond) next = d.target;          // the CCR is not changed
        return;
    }
    case Op::Dor: {
        const u32 count = d.k ? ReadReg(d.r1) : d.imm;
        if (count == 0) return Fault("DO with a count of 0 (SC-dependent, not modelled)");
        Push(la, lc);
        Push(next, sr);
        la = d.target, lc = count;
        sr |= kLF;
        return;
    }
    case Op::Rep: {
        const u32 count = d.k ? ReadReg(d.r1) : d.imm;
        if (count == 0) return Fault("REP with a count of 0 (not modelled)");
        const Insn& rep = Fetch(addr + 1);
        if (rep.op != Op::Parallel || rep.len != 1) return Fault("REP of an instruction that is not a 1-word parallel one");
        const u32 savedLc = lc;
        u32 unused = 0;
        for (u32 i = 0; i < count && !faulted_; ++i) {
            rep.exec(*this, rep, addr + 1, unused);
            ++instructionCount;
        }
        lc = savedLc;
        next = addr + 2;
        LoopCheck(addr + 1, next);               // the repeated instruction may end a DO body
        return;
    }
    case Op::MoveDisp: ExecMoveDisp(*this, d, addr, next); return;
    case Op::Lua: WriteReg(d.r2, Offset(d.m0.ear, static_cast<s32>(d.imm))); return;
    case Op::Movec:
        if (d.k == 1) {
            if (d.m0.toReg) WriteReg(d.r1, ReadReg(d.r2));
            else WriteReg(d.r2, ReadReg(d.r1));
        } else if (d.m0.toReg) {
            WriteReg(d.r1, d.m0.mode == kImm ? d.m0.addr : Read(static_cast<Space>(d.m0.space), EffectiveAddress(d.m0)));
        } else {
            Write(static_cast<Space>(d.m0.space), EffectiveAddress(d.m0), ReadReg(d.r1));
        }
        return;
    case Op::Movem: {
        const u32 ad = EffectiveAddress(d.m0);
        if (d.m0.toReg) WriteReg(d.m0.reg, Read(kP, ad));
        else Write(kP, ad, ReadReg(d.m0.reg));
        return;
    }
    case Op::Movep: {
        const Space ps = static_cast<Space>(d.r1);
        if (d.k == 1) {                          // register <-> peripheral
            if (d.m0.toReg) Write(ps, d.target, ReadReg(d.m0.reg));
            else WriteReg(d.m0.reg, Read(ps, d.target));
        } else if (d.m0.toReg) {                 // #xxxxxx / X|Y:ea -> peripheral
            Write(ps, d.target, d.m0.mode == kImm ? d.m0.addr : Read(static_cast<Space>(d.m0.space), EffectiveAddress(d.m0)));
        } else {                                 // peripheral -> X|Y:ea
            const u32 v = Read(ps, d.target);
            Write(static_cast<Space>(d.m0.space), EffectiveAddress(d.m0), v);
        }
        return;
    }
    case Op::Undecoded:
    case Op::Unsupported: Fault("instruction not modelled"); return;
    }
}

DSP_INLINE void Dsp56300::StepOne() {
    const u32 addr = pc;
    curPc_ = addr;
    const Insn& d = Fetch(addr);
    ++instructionCount;
    const u32 fallThrough = addr + d.len;
    u32 next = fallThrough;
    d.exec(*this, d, addr, next);
    if ((sr & kLF) && fallThrough - 1 == la && next == fallThrough && d.op != Op::Rep) LoopCheck(la, next);
    pc = next & kMask24;
}

void Dsp56300::Step() { StepOne(); }

bool Dsp56300::RunUntil(u32 stopPc, u64 maxInstructions) {
    const u64 end = instructionCount + maxInstructions;
    while (pc != stopPc && !faulted_) {
        if (instructionCount > end) {
            curPc_ = pc;
            Fault("instruction limit reached");
            break;
        }
        StepOne();
    }
    return !faulted_;
}

}  // namespace fut::audio::dsp
