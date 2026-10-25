// A DSP56300 interpreter for the instruction subset of the Xbox MCPX GP program (the DSOUND executive and the
// disc's dsstdfx.bin effects; docs/analysis/audio_dsp_port.md 5). The DSP code is data: it is loaded from the user's
// files at run time (DsImage) and executed here; nothing of it is embedded in the port.
//
// Model (DSP56300 Family Manual): X0 X1 Y0 Y1 24-bit, A / B 56-bit (A2:A1:A0), R / N / M 24-bit, SR with the CCR
// (C V Z N U E L S = bits 0..7), the scaling bits S1:S0 (11:10), LF (15), SM (20), RM (21); a 15-entry system stack
// (SSH / SSL), LA / LC. Instructions are decoded once per P address (P is written only by the code DMA) and executed
// with the manual's semantics: parallel moves read their sources and compute their addresses before the data ALU
// operation and write their destinations after it; accumulator reads go through the data shifter / limiter; the
// multiplier is fractional (2 * x * y); rounding is convergent; SM = 1 saturates arithmetic results to 48 bits;
// addressing is linear; DO / DOR loops and REP use the stack and LA / LC as on the chip.
//
// "Exactly the subset used": every form the GP program contains is decoded (static census of P:0x000-0xDB4, see
// audio_dsp_port.md 11); everything else decodes to `Unsupported` and stops the interpreter with a fault naming the
// word and its address (never guessed). The same happens for situations the program never creates: a non-linear
// address modifier, a DO / REP count of 0, a stack overflow, rounding with a scaling mode or RM = 1.
//
// Uncertain readings (L), each isolated here and listed in audio_dsp_port.md 11.4:
//   - SM saturation applies to add / sub / mpy / mac (+ rounding forms) / abs / neg / asl / rnd / add / sub #imm /
//     mpyi / asl #n (only the crosstalk effect sets SM) and sets V;
//   - the ALU byte 0x1D is MAX A,B (signed), 0x15 MAXM A,B (magnitude); C = 1 when no transfer happens (the
//     reference model named 0x1D "maxm" until 2026-10-17; identical results here, both operands are >= 0);
//   - CMPU compares the low 48 bits unsigned (C, Z; N, V cleared) - only the executive's cycle statistics use it;
//   - the CCR's S bit (block floating point) is not computed (no instruction of the program reads it).
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace fut::audio::dsp {

constexpr u32 kMask24 = 0xFFFFFF;
constexpr u64 kMask48 = 0xFFFFFFFFFFFFull;
constexpr u64 kMask56 = 0xFFFFFFFFFFFFFFull;

// ---- data ALU arithmetic shared with the EP down-mix (McpxEp) -------------------------------------------------
inline s32 Sext24(u32 v) { return static_cast<s32>(v << 8) >> 8; }
inline s64 Sext56(u64 v) { return static_cast<s64>(v << 8) >> 8; }
// A 24-bit operand aligned to A1: sign-extended into A2, A0 = 0.
inline u64 Acc24(u32 v) { return (static_cast<u64>(static_cast<s64>(Sext24(v)) * (s64(1) << 24))) & kMask56; }
// Fractional product: (s1 * s2) << 1 as a 56-bit two's complement value (-1 * -1 = +1.0 = 0x00800000000000).
inline u64 FracProduct(u32 s1, u32 s2) {
    return static_cast<u64>(s64(Sext24(s1)) * s64(Sext24(s2)) * 2) & kMask56;
}
// Convergent rounding at bit 23 (no scaling, RM = 0): ties (low 24 bits = 0x800000) round to an even A1.
inline u64 RoundConvergent(u64 acc) {
    u64 r = (acc + 0x800000) & kMask56;
    if ((acc & kMask24) == 0x800000) r &= ~(u64(1) << 24);
    return r & ~u64(kMask24);
}
// The limiter of an accumulator read as a 24-bit value without scaling: A1, or 0x7FFFFF / 0x800000 when the
// extension bits 55..47 are in use.
inline u32 Limit24(u64 acc, bool* limited = nullptr) {
    const u64 top = acc >> 47;
    if (top == 0 || top == 0x1FF) return static_cast<u32>(acc >> 24) & kMask24;
    if (limited) *limited = true;
    return (acc >> 55) ? 0x800000u : 0x7FFFFFu;
}

class Dsp56300 {
public:
    // The memory-mapped peripherals (X:0xFFFF80-0xFFFFFF) belong to the chip the core sits in (McpxGp).
    class Peripherals {
    public:
        virtual ~Peripherals() = default;
        virtual u32 PeripheralRead(u32 addr) = 0;
        virtual void PeripheralWrite(u32 addr, u32 value) = 0;
    };

    enum Space : u8 { kX = 0, kY = 1, kP = 2 };

    Dsp56300(u32 xWords, u32 yWords, u32 pWords, Peripherals* periph);
    Dsp56300(const Dsp56300&) = delete;          // decoded instructions point into the registers
    Dsp56300& operator=(const Dsp56300&) = delete;

    // A read / write view of X memory: X:[base, base + size) accesses X:[target, target + size).
    void SetXView(u32 base, u32 size, u32 target) { viewBase_ = base, viewSize_ = size, viewTarget_ = target; }

    // Reset state of the registers (SR = 0xC00300, M = 0xFFFFFF, PC = 0); memory is kept.
    void Reset();

    // Executes one instruction (a REP with its repeated instruction counts as one step).
    void Step();
    // Runs until PC == stopPc (checked before each instruction), a fault, or `maxInstructions`; returns false on a
    // fault or when the limit is reached (the fault text says which).
    bool RunUntil(u32 stopPc, u64 maxInstructions);

    bool Faulted() const { return faulted_; }
    const std::string& FaultText() const { return fault_; }
    // Stops the interpreter from outside (the chip's peripherals: an unmodelled DMA or control word).
    void ExternalFault(const char* text) { Fault("%s", text); }

    // Memory as the program sees it (X through the view, peripherals included). Out-of-range accesses fault.
    u32 Read(Space s, u32 addr) {
        addr &= kMask24;
        if (s == kX && addr < x_.size()) return x_[addr];
        if (s == kY && addr < y_.size()) return y_[addr];
        return ReadSlow(s, addr);
    }
    void Write(Space s, u32 addr, u32 value) {
        addr &= kMask24;
        if (s == kX && addr < x_.size()) x_[addr] = value & kMask24;
        else if (s == kY && addr < y_.size()) y_[addr] = value & kMask24;
        else WriteSlow(s, addr, value & kMask24);
    }
    // Direct array access (no peripherals, no view) for loaders and tests; P writes invalidate the decode cache.
    u32* XData() { return x_.data(); }
    u32* YData() { return y_.data(); }
    const u32* XData() const { return x_.data(); }
    const u32* YData() const { return y_.data(); }
    u32 XSize() const { return static_cast<u32>(x_.size()); }
    u32 YSize() const { return static_cast<u32>(y_.size()); }
    void WriteP(u32 addr, u32 value);
    u32 ReadP(u32 addr) const { return addr < p_.size() ? p_[addr] : 0; }

    // Registers (public for the GP glue and the tests).
    u32 x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    u64 a = 0, b = 0;                            // 56-bit, two's complement in the low 56 bits
    u32 r[8] = {}, n[8] = {}, m[8] = {};
    u32 sr = 0xC00300, omr = 0, la = 0, lc = 0, sp = 0;
    u32 ssh[16] = {}, ssl[16] = {};
    u32 pc = 0;
    u64 instructionCount = 0;                    // the interpreter's count (one per instruction, REP iterations
                                                 // counted each); the GP timer stand-in reads it

    // SR bits
    static constexpr u32 kC = 1, kV = 2, kZ = 4, kN = 8, kU = 0x10, kE = 0x20, kL = 0x40;
    static constexpr u32 kLF = 1u << 15, kSM = 1u << 20, kRM = 1u << 21;

private:
    // ---- decoded instruction -----------------------------------------------------------------------------------
    enum class Op : u8 {
        Undecoded, Unsupported,
        Parallel,                                // data ALU op (or none) + parallel move
        Nop, Rts, Jsr, Bra, Bsr, Bcc,
        AluImm, Mpyi, ShiftImm, Cmpu, AndOrSr,
        BitReg, BitBranch, BitJump,              // bset / bclr reg; brset / brclr; jclr
        Dor, Rep, MoveDisp, Lua, Movec, Movem, Movep,
    };
    enum class Alu : u8 {
        None, Mpy, Mac, Add, Sub, Cmp, Tfr, Clr, Tst, Abs, Neg, Asl, Asr, Rnd, Max, Maxm, And, Or, Eor, Bad, Count
    };
    enum class Move : u8 { None, Imm8, Reg, Ifcc, Mem, MemMem, Bad, Count };
    // A memory operand: ea mode (MMM 0..5 / 7 with register `ear`), or kAbs / kImm / kShort with `addr`.
    struct MemOp {
        u8 space = kX;
        u8 toReg = 0;                            // 1: memory (or immediate) -> register; 0: register -> memory
        u8 reg = 0;                              // 6-bit register code
        u8 mode = 0, ear = 0;
        u32 addr = 0;
    };
    static constexpr u8 kAbs = 8, kImm = 9, kShort = 10;
    struct Insn;
    // Each decoded instruction carries the handler of its form (chosen once, at decode).
    using Handler = void (*)(Dsp56300& c, const Insn& d, u32 addr, u32& next);
    struct Insn {
        Handler exec = nullptr;                  // null: not decoded yet
        Op op = Op::Undecoded;
        u8 len = 1;
        // data ALU (parallel)
        Alu alu = Alu::None;
        u8 dst = 0;                              // 0 A, 1 B
        u8 src = 0;                              // register code (4..7 x0..y1, 0x0E / 0x0F A / B, 0x40 X, 0x41 Y)
        u8 src2 = 0;                             // second multiplier operand
        u8 neg = 0, round = 0, accumulate = 0;   // multiply: -, R (rounding), mac
        // parallel move / generic fields
        Move move = Move::None;
        u8 cond = 0, ccrUpdate = 0;              // Ifcc / Bcc condition; IFcc.U
        u8 r1 = 0, r2 = 0;                       // register codes (Reg move source / destination, ...)
        u8 k = 0;                                // sub-operation (AluImm kind, bit number, shift count, ...)
        u32 imm = 0;                             // immediate / extension word
        u32 target = 0;                          // branch target / loop address
        MemOp m0, m1;
    };

    template <Move M, Alu A>
    static void ExecParallel(Dsp56300& c, const Insn& d, u32 addr, u32& next);
    static void ExecNonParallel(Dsp56300& c, const Insn& d, u32 addr, u32& next);
    static void ExecMoveDisp(Dsp56300& c, const Insn& d, u32 addr, u32& next);
    static Handler ParallelHandler(Move m, Alu a);
    template <Alu A>
    void AluOp(const Insn& d);
    u32* PlainReg(u32 code);
    void MoveOperand(const MemOp& o, void* dest);   // a memory move's source / address (dest: a MoveDest)
    // plain_[code]: the 24-bit register of a register code when it is a plain one (x0..y1, r, n, m), else null
    u32* plain_[64] = {};
    void Decode(u32 addr, Insn& d);
    void DecodeParallel(u32 w, u32 ext, Insn& d) const;
    static void DecodeAluByte(u32 byte, Insn& d);
    void Resolve(Insn& d);                       // handler and register pointers of a decoded instruction
    const Insn& Fetch(u32 addr);
    void StepOne();
    void Execute(const Insn& d, u32 addr, u32& next);
    void LoopCheck(u32 lastWord, u32& next);

    // ---- registers, memory, address generation -----------------------------------------------------------------
    u32 ReadReg(u32 code);                       // 24-bit source read (A / B through the shifter / limiter)
    void WriteReg(u32 code, u32 value);          // 24-bit destination write (A / B: sign-extended, A0 = 0)
    bool ValidReg(u32 code) const;
    u32 EffectiveAddress(const MemOp& o);        // applies the post-update / pre-decrement
    u32 Offset(u32 rn, s32 delta);               // linear address arithmetic (faults on M != 0xFFFFFF)
    u32 ReadSlow(Space s, u32 addr);
    void WriteSlow(Space s, u32 addr, u32 value);

    // ---- data ALU ----------------------------------------------------------------------------------------------
    u64& Acc(u32 i) { return i ? b : a; }
    u64 Source56(u32 code);                      // ALU source as a 56-bit operand
    u32 IntegerShift() const;                    // 47 / 48 / 46: no scaling / scale down / scale up
    void Flags56(u64 r, bool v, bool setC, bool c);
    u64 Saturate(u64 r, bool& v) const;          // SM = 1: 48-bit arithmetic saturation (L: instruction list)
    u64 Round(u64 acc);
    u64 Add56(u64 d, u64 s, bool& c, bool& v) const;
    u64 Sub56(u64 d, u64 s, bool& c, bool& v) const;
    bool Condition(u32 cc) const;
    void Push(u32 hi, u32 lo);
    void Pop(u32& hi, u32& lo);

    void Fault(const char* fmt, ...);

    std::vector<u32> x_, y_, p_;
    std::vector<Insn> decoded_;
    Insn outside_;                               // fetched beyond P: faults
    u32 viewBase_ = 0, viewSize_ = 0, viewTarget_ = 0;
    Peripherals* periph_;
    bool faulted_ = false;
    std::string fault_;
    u32 curPc_ = 0;                              // address of the executing instruction (fault messages)
};

}  // namespace fut::audio::dsp
