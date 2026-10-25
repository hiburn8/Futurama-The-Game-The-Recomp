// Decompiled from default.xbe 0x000B9210-0x000BA3C0. See ScriptVM.h.
#include "game/script/ScriptVM.h"

#include "core/memory/MemoryManager.h"

#include <cstring>

namespace fut::script {
namespace {

Globals g_globals{0, 0, 0, 0xFADE};   // orig: g_ScriptTagCounter starts at 0xFADE (.data 0x002D6838)
NativeHandler g_native = nullptr;

template <class T>
T* P(VmAddr a) {
    return static_cast<T*>(mem::FromVmAddr(a));
}
u32 R32(VmAddr a) {
    u32 v;
    std::memcpy(&v, mem::FromVmAddr(a), 4);
    return v;
}
void W32(VmAddr a, u32 v) { std::memcpy(mem::FromVmAddr(a), &v, 4); }
VmAddr Addr(const void* p) { return mem::ToVmAddr(p); }

float AsFloat(u32 bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
u32 AsBits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

// rep movsd + rep movsb: forward copy, one dword at a time (matters only for overlapping ranges).
void CopyForward(VmAddr dst, VmAddr src, u32 n) {
    u8* d = static_cast<u8*>(mem::FromVmAddr(dst));
    const u8* s = static_cast<const u8*>(mem::FromVmAddr(src));
    u32 i = 0;
    for (; i + 4 <= (n & ~3u); i += 4) {
        u32 v;
        std::memcpy(&v, s + i, 4);
        std::memcpy(d + i, &v, 4);
    }
    for (; i < n; ++i) d[i] = s[i];
}

void ZeroFill(VmAddr dst, u32 n) {
    if (n) std::memset(mem::FromVmAddr(dst), 0, n);
}

// orig: 0x000B92E0 ScriptStack::PushBytes
void PushBytes(ScriptStack& s, VmAddr src, u32 n) {
    if (n == 0) return;
    CopyForward(s.sp, src, n);
    s.sp += n;
}

// orig: 0x000B94F0 Script_ResolveAddress(mode, offset, frame, ctx, globals, &dataStack)
VmAddr Resolve(u32 mode, u32 offset, ScriptThread* t) {
    switch (mode) {
        case 1: return t->ctx + offset;
        case 2: return offset + t->globals;
        case 3: t->dataStack.sp -= 4; return t->dataStack.sp + offset;
        default: return offset + t->frame;
    }
}

// orig: 0x000B9710 ScriptThread::LinkToProgram
void LinkToProgram(ScriptThread* t, VmAddr program) {
    VmAddr ta = Addr(t);
    if (t->nextInProgram) P<ScriptThread>(t->nextInProgram)->prevInProgram = t->prevInProgram;
    if (t->prevInProgram == 0) {
        if (t->program) P<ScriptProgram>(t->program)->threads = t->nextInProgram;
    } else {
        P<ScriptThread>(t->prevInProgram)->nextInProgram = t->nextInProgram;
    }
    t->nextInProgram = 0;
    t->program = program;
    t->prevInProgram = 0;
    ScriptProgram* p = P<ScriptProgram>(program);
    t->nextInProgram = p->threads;
    p->threads = ta;
    if (t->nextInProgram) P<ScriptThread>(t->nextInProgram)->prevInProgram = ta;
}

// orig: 0x000B9320 ScriptThread::UnlinkFromParent (also inlined in ~ScriptThread, KillChildren, KillThreadsWithTag)
void UnlinkFromParent(ScriptThread* t) {
    if (t->nextSibling) P<ScriptThread>(t->nextSibling)->prevSibling = t->prevSibling;
    if (t->prevSibling == 0) {
        if (t->parent) P<ScriptThread>(t->parent)->firstChild = t->nextSibling;
    } else {
        P<ScriptThread>(t->prevSibling)->nextSibling = t->nextSibling;
    }
    if (t->parent) {
        ScriptThread* parent = P<ScriptThread>(t->parent);
        if (parent->ended && parent->firstChild == 0) parent->finished = 1;   // last child gone: parent finishes
    }
    t->parent = 0;
    t->nextSibling = 0;
    t->prevSibling = 0;
}

// orig: 0x000B9390 ScriptThread_KillChildren
void KillChildren(ScriptThread* t) {
    while (t->firstChild) {
        KillChildren(P<ScriptThread>(t->firstChild));
        ScriptThread* c = P<ScriptThread>(t->firstChild);
        if (c->firstChild == 0) c->finished = 1;
        else c->ended = 1;
        UnlinkFromParent(c);
    }
}

// orig: 0x000B95A0 ScriptBuffer::AllocZeroed
void BufferAllocZeroed(ScriptBuffer& b, u32 size) {
    if (b.data) {
        mem::OperatorDelete(P<void>(b.data));
        b.data = 0;
    }
    b.size = size;
    b.data = Addr(mem::OperatorNew(size));
    ZeroFill(b.data, size);
}

void DeleteAllThreads(ScriptProgram* p) {
    while (p->threads) {
        ScriptThread* t = P<ScriptThread>(p->threads);
        ThreadDestroy(t);
        mem::OperatorDelete(t);
    }
}

}  // namespace

Globals& VmGlobals() { return g_globals; }

// orig: 0x000B9550 Script_UnlinkCurrentThread (native 393): the current thread leaves its parent.
void UnlinkCurrentThread() {
    if (g_globals.currentThread) UnlinkFromParent(P<ScriptThread>(g_globals.currentThread));
}


void SetNativeHandler(NativeHandler handler) { g_native = handler; }

// orig: operator_new(0x54) + 0x000B9760 ScriptThread::ScriptThread(program, pc, globals, ctx, stackSize)
VmAddr ThreadCreate(VmAddr program, VmAddr pc, VmAddr globals, VmAddr ctx, u32 stackSize) {
    ScriptThread* t = static_cast<ScriptThread*>(mem::OperatorNew(sizeof(ScriptThread)));
    if (!t) return 0;
    VmAddr ta = Addr(t);
    t->callStack.sp = 0;
    t->callStack.base = 0;
    t->callStack.capacity = 0;
    t->dataStack.sp = 0;
    t->dataStack.base = 0;
    t->dataStack.capacity = 0;
    ++g_globals.threadCount;
    g_globals.currentThread = ta;
    t->program = 0;
    t->nextInProgram = 0;
    t->prevInProgram = 0;
    t->firstChild = 0;
    t->parent = 0;
    t->nextSibling = 0;
    t->prevSibling = 0;
    LinkToProgram(t, program);
    t->globals = globals;
    t->ctx = ctx;
    t->frame = 0;
    t->pc = pc;
    t->tag = 0;
    t->wait = 0.0f;
    t->timer = 0.0f;
    t->ended = 0;
    t->finished = 0;
    mem::OperatorDelete(P<void>(t->callStack.base));
    t->callStack.base = 0;
    t->callStack.sp = 0;
    t->callStack.capacity = kCallStackBytes;
    t->callStack.base = Addr(mem::OperatorNew(kCallStackBytes));
    t->callStack.sp = t->callStack.base;
    mem::OperatorDelete(P<void>(t->dataStack.base));
    t->dataStack.base = 0;
    t->dataStack.sp = 0;
    t->dataStack.capacity = stackSize;
    t->dataStack.base = Addr(mem::OperatorNew(stackSize));
    t->dataStack.sp = t->dataStack.base;
    g_globals.currentThread = 0;
    return ta;
}

// orig: 0x000B95F0 ScriptThread::~ScriptThread
void ThreadDestroy(ScriptThread* t) {
    if (t->nextInProgram) P<ScriptThread>(t->nextInProgram)->prevInProgram = t->prevInProgram;
    if (t->prevInProgram == 0) {
        if (t->program) P<ScriptProgram>(t->program)->threads = t->nextInProgram;
    } else {
        P<ScriptThread>(t->prevInProgram)->nextInProgram = t->nextInProgram;
    }
    t->program = 0;
    t->nextInProgram = 0;
    t->prevInProgram = 0;
    UnlinkFromParent(t);
    while (t->firstChild) UnlinkFromParent(P<ScriptThread>(t->firstChild));   // orphan the children
    --g_globals.threadCount;
    mem::OperatorDelete(P<void>(t->dataStack.base));
    t->dataStack.base = 0;
    t->dataStack.sp = 0;
    t->dataStack.capacity = 0;
    mem::OperatorDelete(P<void>(t->callStack.base));
    t->callStack.base = 0;
    t->callStack.sp = 0;
    t->callStack.capacity = 0;
}

// orig: 0x000B9420 ScriptProgram::KillThreadsWithTag
void KillThreadsWithTag(ScriptProgram* p, u32 tag) {
    for (VmAddr ta = p->threads; ta; ta = P<ScriptThread>(ta)->nextInProgram) {
        ScriptThread* t = P<ScriptThread>(ta);
        if (t->tag != tag) continue;
        while (t->firstChild) {
            KillChildren(P<ScriptThread>(t->firstChild));
            ScriptThread* c = P<ScriptThread>(t->firstChild);
            if (c->firstChild == 0) c->finished = 1;
            else c->ended = 1;
            UnlinkFromParent(c);
        }
        t->finished = 1;
    }
}

// orig: 0x000B9860 ScriptThread::Run
void ThreadRun(ScriptThread* t) {
    const VmAddr ta = Addr(t);
    g_globals.currentThread = ta;
    VmAddr& sp = t->dataStack.sp;
    for (;;) {
        const VmAddr opAddr = t->pc;
        const u32 word = R32(opAddr);
        t->pc = opAddr + 4;
        const u32 mode = word & 0xFF;
        switch (word >> 24) {
            case 0x02:    // PUSHACC
                W32(sp, AsBits(t->timer));
                sp += 4;
                break;
            case 0x03:    // POPACC
                sp -= 4;
                t->timer = AsFloat(R32(sp));
                break;
            case 0x04: {  // LOADBLK mode[off], len
                VmAddr a = Resolve(mode, R32(opAddr + 4), t);
                u32 len = R32(opAddr + 8);
                t->pc = opAddr + 12;
                PushBytes(t->dataStack, a, len);
                break;
            }
            case 0x05: {  // LOAD mode[off]
                VmAddr a = Resolve(mode, R32(opAddr + 4), t);
                t->pc += 4;
                W32(sp, R32(a));
                sp += 4;
                break;
            }
            case 0x06: {  // ADDR mode[off]
                VmAddr a = Resolve(mode, R32(opAddr + 4), t);
                t->pc += 4;
                W32(sp, a);
                sp += 4;
                break;
            }
            case 0x07: {  // PUSHBLK len, <len bytes>
                u32 len = R32(opAddr + 4);
                t->pc = opAddr + 8 + len;
                PushBytes(t->dataStack, opAddr + 8, len);
                break;
            }
            case 0x08:    // PUSHI imm
                t->pc = opAddr + 8;
                W32(sp, R32(opAddr + 4));
                sp += 4;
                break;
            case 0x09:    // PUSHDATA len, <len bytes>: push the address of the inline data
                t->pc = R32(opAddr + 4) + opAddr + 8;
                W32(sp, opAddr + 8);
                sp += 4;
                break;
            case 0x0A:    // STOREBLK mode[off], len: pop len bytes into the variable
            case 0x0C: {  // COPYBLK  mode[off], len: same, but the block stays on the stack
                VmAddr a = Resolve(mode, R32(opAddr + 4), t);
                u32 len = R32(opAddr + 8);
                t->pc = opAddr + 12;
                VmAddr src = sp - len;
                if ((word >> 24) == 0x0A) sp = src;
                CopyForward(a, src, len);
                break;
            }
            case 0x0B: {  // STORE mode[off]
                VmAddr a = Resolve(mode, R32(opAddr + 4), t);
                t->pc += 4;
                sp -= 4;
                W32(a, R32(sp));
                break;
            }
            case 0x0E:    // DROP bytes
                t->pc = opAddr + 8;
                sp -= R32(opAddr + 4);
                break;
            case 0x0F:    // JMP rel (relative to opAddr + 8)
                t->pc = opAddr + R32(opAddr + 4) + 8;
                break;
            case 0x10:    // JZ rel
            case 0x11: {  // JNZ rel
                u32 rel = R32(opAddr + 4);
                t->pc = opAddr + 8;
                sp -= 4;
                bool zero = R32(sp) == 0;
                if (zero == ((word >> 24) == 0x10)) t->pc += rel;
                break;
            }
            case 0x12:    // CALL rel, argBytes
            case 0x14: {  // SPAWN rel, argBytes, stackSize
                const bool spawn = (word >> 24) == 0x14;
                u32 rel = R32(opAddr + 4);
                t->pc = opAddr + 8;
                u32 argBytes = R32(opAddr + 8);
                t->pc = opAddr + 12;
                u32 stackSize = 0;
                if (spawn) {
                    stackSize = R32(opAddr + 12);
                    t->pc = opAddr + 16;
                }
                VmAddr args = sp - argBytes;
                sp = args;
                VmAddr objSlot = args - 4;
                sp = objSlot;
                VmAddr obj = R32(objSlot);
                VmAddr target = t->pc + rel;
                if (spawn) {
                    VmAddr na = ThreadCreate(t->program, target, t->globals, obj, stackSize);
                    ScriptThread* n = P<ScriptThread>(na);
                    UnlinkFromParent(n);
                    n->parent = ta;
                    n->prevSibling = 0;
                    n->nextSibling = t->firstChild;
                    t->firstChild = na;
                    if (n->nextSibling) P<ScriptThread>(n->nextSibling)->prevSibling = na;
                    PushBytes(n->dataStack, args, argBytes);
                    ThreadRun(n);
                    g_globals.currentThread = ta;
                } else {
                    sp = objSlot + argBytes + 4;      // arguments stay on the stack for the callee's frame
                    VmAddr& cs = t->callStack.sp;
                    W32(cs, t->ctx);
                    cs += 4;
                    W32(cs, t->frame);
                    cs += 4;
                    W32(cs, t->pc);
                    cs += 4;
                    t->ctx = obj;
                    t->frame = 0;
                    t->pc = target;
                }
                break;
            }
            case 0x13: {  // NATIVE classId, method
                u32 cid = R32(opAddr + 4);
                u32 method = R32(opAddr + 8);
                t->pc = opAddr + 12;
                if (g_native) g_native(cid, method, sp);
                break;
            }
            case 0x15:    // SETTAG
                sp -= 4;
                t->tag = R32(sp);
                break;
            case 0x16:    // NEWTAG
                if (++g_globals.tagCounter == 0) g_globals.tagCounter = 1;
                W32(sp, g_globals.tagCounter);
                sp += 4;
                break;
            case 0x17:    // KILL tag
                sp -= 4;
                KillThreadsWithTag(P<ScriptProgram>(t->program), R32(sp));
                if (t->finished) return;              // quirk: returns without clearing currentThread
                break;
            case 0x18:    // WAIT seconds
                sp -= 4;
                t->wait = AsFloat(R32(sp));
                g_globals.currentThread = 0;
                return;
            case 0x19:    // END
                if (t->firstChild == 0) t->finished = 1;
                else t->ended = 1;                    // finishes when the last child unlinks
                g_globals.currentThread = 0;
                return;
            case 0x1A: {  // RET retBytes
                u32 retBytes = R32(opAddr + 4);
                t->pc = opAddr + 8;
                VmAddr src = sp - retBytes;
                sp = src;
                u32 offset = t->frame ? src - t->frame : 0;
                sp = sp - 4 - offset;                 // drop args, locals and the object word
                if (retBytes) {
                    CopyForward(sp, src, retBytes);
                    sp += retBytes;
                }
                VmAddr cs = t->callStack.sp;
                t->pc = R32(cs - 4);
                t->frame = R32(cs - 8);
                t->ctx = R32(cs - 12);
                t->callStack.sp = cs - 12;
                break;
            }
            case 0x1B: {  // ENTER argBytes, localBytes
                u32 argBytes = R32(opAddr + 4);
                u32 localBytes = R32(opAddr + 8);
                t->pc = opAddr + 12;
                ZeroFill(sp, localBytes);
                t->frame = sp - argBytes;
                sp += localBytes;
                break;
            }
            default:      // illegal opcode
                t->ended = 1;
                g_globals.currentThread = 0;
                return;
        }
    }
}

// orig: 0x000B9F00 ScriptProgram::ScriptProgram
void ProgramInit(ScriptProgram* p) {
    p->globals.data = 0;
    p->globals.size = 0;
    p->code = 0;
    p->threads = 0;
    p->codeSize = 0;
    p->started = 0;
    p->loaded = 0;
}

// orig: 0x000B9F20 ScriptProgram::Unload
void ProgramUnload(ScriptProgram* p) {
    if (!p->loaded) return;
    p->started = 0;
    p->loaded = 0;
    DeleteAllThreads(p);
    if (p->globals.data) {
        mem::OperatorDelete(P<void>(p->globals.data));
        p->globals.data = 0;
    }
    if (p->code) {
        ScriptHeader* h = P<ScriptHeader>(p->code);   // orig: 0x000B91F0
        if (h->extra) {
            mem::OperatorDelete(P<void>(h->extra));
            h->extra = 0;
        }
        mem::OperatorDelete(h);
        p->code = 0;
    }
    g_globals.memoryUsed -= static_cast<s32>(p->codeSize);
}

// orig: 0x000BA200 ScriptProgram::Load + 0x000B9210 LoadFile (reads the little-endian half of the .ful)
bool ProgramLoad(ScriptProgram* p, const void* file, u32 fileSize) {
    if (p->loaded) ProgramUnload(p);
    p->loaded = 1;
    p->codeSize = fileSize >> 1;
    void* buf = mem::OperatorNew(p->codeSize);
    if (buf) std::memcpy(buf, file, p->codeSize);
    if (!buf || static_cast<ScriptHeader*>(buf)->version != kScriptVersion) {
        mem::OperatorDelete(buf);
        buf = nullptr;
    }
    p->code = Addr(buf);
    g_globals.memoryUsed += static_cast<s32>(p->codeSize);
    if (p->code && P<ScriptHeader>(p->code)->apiHash == kNativeApiHash) return true;
    ProgramUnload(p);
    return false;
}

// orig: 0x000B9FA0 ScriptProgram::Start
void ProgramStart(ScriptProgram* p) {
    if (p->started) return;
    p->started = 1;
    if (!p->code) return;
    ScriptHeader* h = P<ScriptHeader>(p->code);
    BufferAllocZeroed(p->globals, h->globalsSize);
    ThreadCreate(Addr(p), p->code + h->initEntry + kCodeOffset, p->globals.data, p->globals.data, h->stackSize);
}

// orig: 0x000BA020 ScriptProgram::Stop: runs the exit function to completion, then deletes everything
void ProgramStop(ScriptProgram* p) {
    if (!p->started) return;
    p->started = 0;
    if (!p->code) return;
    ScriptHeader* h = P<ScriptHeader>(p->code);
    VmAddr ta = ThreadCreate(Addr(p), p->code + h->exitEntry + kCodeOffset, p->globals.data, p->globals.data,
                             h->stackSize);
    ThreadRun(P<ScriptThread>(ta));
    DeleteAllThreads(p);
    if (p->globals.data) {
        mem::OperatorDelete(P<void>(p->globals.data));
        p->globals.data = 0;
    }
}

// orig: 0x000BA0F0 ScriptProgram::Update(dt)
void ProgramUpdate(ScriptProgram* p, float dt) {
    for (VmAddr ta = p->threads; ta;) {
        ScriptThread* t = P<ScriptThread>(ta);
        ta = t->nextInProgram;                        // captured before running: new threads wait a frame
        if (t->finished || t->ended) continue;
        float wait = t->wait - dt;
        t->wait = wait;
        float timer = t->timer - dt;
        t->timer = timer;
        if (wait <= 0.0f) {
            if (timer < 0.0f) t->timer = 0.0f;
            ThreadRun(t);
        }
    }
    for (VmAddr ta = p->threads; ta;) {
        ScriptThread* t = P<ScriptThread>(ta);
        VmAddr next = t->nextInProgram;
        if (t->finished) {
            ThreadDestroy(t);
            mem::OperatorDelete(t);
            next = p->threads;                        // restart from the head after a deletion
        }
        ta = next;
    }
}

// orig: 0x000BA1A0 ScriptProgram::~ScriptProgram
void ProgramDestroy(ScriptProgram* p) {
    ProgramUnload(p);
    if (p->globals.data) {
        mem::OperatorDelete(P<void>(p->globals.data));
        p->globals.data = 0;
    }
}

}  // namespace fut::script
