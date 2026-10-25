// Level-script virtual machine: decompiled from default.xbe 0x000B9210-0x000BA3C0 (interpreter 0x000B9860).
//
// Scripts (.ful) are compiled bytecode for a stack machine with cooperative threads:
//   * instruction = u32: opcode in bits 24-31, address mode in bits 0-7, followed by 0-3 operand words
//   * every thread has a call stack (0x80 bytes) and a data stack; code addresses variables in four spaces:
//     frame (locals/args), ctx (the object a function was called on), global, and stack-relative
//   * WAIT suspends a thread for N seconds (float); ScriptProgram::Update(dt) resumes threads whose timer ran out
//   * SPAWN starts a child thread immediately; KILL ends all threads carrying a tag (and their children)
//   * NATIVE calls into the engine: Script_CallNative(classId, method, &sp) (0x000B0CD0, 382-way switch)
// tools/ful_disasm.py disassembles .ful files with the same opcode table.
//
// Port design: script code stores raw 32-bit addresses (ADDR, PUSHDATA, object pointers used as ctx), so all
// VM-visible memory lives in the game heap and is addressed by 32-bit VM addresses (mem::ToVmAddr). The thread
// and program records keep their original layouts (all fields 32-bit) and sizes, so heap layout - and therefore
// every address a script can observe - matches the original. Verified by tests/game/ScriptVMTests.cpp against the
// original interpreter running under emulation (tools/difftest/gen_script_vectors.py).
#pragma once

#include "core/Types.h"

namespace fut::script {

using VmAddr = u32;

// orig: 0x000B03C0 Script_NativeApiHash - scripts compiled against another native table are rejected.
constexpr u32 kNativeApiHash = 0x1A0A;
constexpr u32 kScriptVersion = 300;
constexpr u32 kCodeOffset = 0x28;
constexpr u32 kCallStackBytes = 0x80;

struct ScriptStack {          // capacity, base, sp
    u32 capacity;
    VmAddr base;
    VmAddr sp;
};

struct ScriptThread {         // operator_new(0x54)
    ScriptStack callStack;    // +0x00 frames of {ctx, frame, pc}
    ScriptStack dataStack;    // +0x0C
    u32 tag;                  // +0x18 SETTAG / KILL
    float timer;              // +0x1C PUSHACC/POPACC; counts down with dt, clamped to 0 when the thread resumes
    float wait;               // +0x20 WAIT seconds remaining
    u8 ended;                 // +0x24 END with live children, or illegal opcode: stops executing
    u8 finished;              // +0x25 deleted by the next Update
    u8 pad26[2];
    VmAddr globals;           // +0x28
    VmAddr ctx;               // +0x2C
    VmAddr frame;             // +0x30
    VmAddr pc;                // +0x34
    VmAddr program;           // +0x38 ScriptProgram*
    VmAddr nextInProgram;     // +0x3C
    VmAddr prevInProgram;     // +0x40
    VmAddr parent;            // +0x44
    VmAddr firstChild;        // +0x48
    VmAddr nextSibling;       // +0x4C
    VmAddr prevSibling;       // +0x50
};
static_assert(sizeof(ScriptThread) == 0x54);

struct ScriptBuffer {
    u32 size;
    VmAddr data;
};

struct ScriptProgram {        // 0x18 bytes (embedded in its owner on Xbox)
    ScriptBuffer globals;     // +0x00
    u8 started;               // +0x08
    u8 loaded;                // +0x09
    u8 pad0A[2];
    u32 codeSize;             // +0x0C bytes loaded (half of the .ful file)
    VmAddr threads;           // +0x10 head of the thread list
    VmAddr code;              // +0x14 ScriptHeader*
};
static_assert(sizeof(ScriptProgram) == 0x18);

struct ScriptHeader {
    char magic[4];            // 'L2iM'
    u32 version;              // 300
    u32 apiHash;              // kNativeApiHash
    u32 globalsSize;
    u32 stackSize;
    u32 unk14, unk18;
    u32 initEntry;            // relative to code (+0x28)
    u32 exitEntry;
    VmAddr extra;             // +0x24 freed by Unload if set (always 0 in the shipped files)
};
static_assert(sizeof(ScriptHeader) == kCodeOffset);

// Native call: classId, method, reference to the calling thread's data-stack pointer (pop args / push results).
// orig: 0x000B0CD0 Script_CallNative(cid, method, &thread->dataStack.sp), cdecl.
using NativeHandler = void (*)(u32 classId, u32 method, VmAddr& sp);
void SetNativeHandler(NativeHandler handler);

// Debugger hook (orig: g_ScriptDebugger 0x002EA2B8, null in the retail game). Not implemented in the port.

// ---- program API (orig names in comments) -----------------------------------------------------------------
void ProgramInit(ScriptProgram* p);                                  // 0x000B9F00 ScriptProgram::ScriptProgram
bool ProgramLoad(ScriptProgram* p, const void* file, u32 fileSize);  // 0x000BA200 Load (+ 0x000B9210 LoadFile)
void ProgramUnload(ScriptProgram* p);                                // 0x000B9F20
void ProgramStart(ScriptProgram* p);                                 // 0x000B9FA0
void ProgramStop(ScriptProgram* p);                                  // 0x000BA020
void ProgramUpdate(ScriptProgram* p, float dt);                      // 0x000BA0F0
void ProgramDestroy(ScriptProgram* p);                               // 0x000BA1A0 ~ScriptProgram

// ---- lower level ------------------------------------------------------------------------------------------
void ThreadRun(ScriptThread* t);                                     // 0x000B9860
VmAddr ThreadCreate(VmAddr program, VmAddr pc, VmAddr globals, VmAddr ctx, u32 stackSize);  // new + 0x000B9760
void ThreadDestroy(ScriptThread* t);                                 // 0x000B95F0 ~ScriptThread
void KillThreadsWithTag(ScriptProgram* p, u32 tag);                  // 0x000B9420
void UnlinkCurrentThread();                                          // 0x000B9550 (native 393 __detach__)

struct Globals {
    VmAddr currentThread;     // g_ScriptCurrentThread 0x002EA2BC
    s32 threadCount;          // g_ScriptThreadCount   0x002EA2C0
    s32 memoryUsed;           // g_ScriptMemoryUsed    0x002EA2C4
    u32 tagCounter;           // g_ScriptTagCounter    0x002D6838
};
Globals& VmGlobals();

}  // namespace fut::script
