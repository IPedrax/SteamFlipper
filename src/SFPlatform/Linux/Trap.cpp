#include "include/Trap.h"

#include "include/Log.h"

#include <signal.h>
#include <ucontext.h>

#include <cstdint>
#include <cstring>

namespace SFPlatform::Trap {
namespace {

Handler g_handler = nullptr;
struct sigaction g_oldSigTrap;
struct sigaction g_oldSigSegv;
bool g_installed = false;

ExceptionKind FromSignal(int sig, siginfo_t* info) {
    if (sig == SIGTRAP) {
        if (info && info->si_code == TRAP_TRACE) return ExceptionKind::SingleStep;
        return ExceptionKind::Breakpoint;
    }
    return ExceptionKind::Other;
}

ucontext_t* AsUContext(void* nativeContext) {
    return static_cast<ucontext_t*>(nativeContext);
}

#if defined(__x86_64__)
constexpr int kIpReg = REG_RIP;
#elif defined(__i386__)
constexpr int kIpReg = REG_EIP;
#endif

void SignalHandler(int sig, siginfo_t* info, void* ucontext) {
    if (!g_handler || !ucontext) return;

    Context context(ucontext);
    ExceptionKind kind = FromSignal(sig, info);

#if defined(__x86_64__) || defined(__i386__)
    // An int3 trap leaves the instruction pointer one byte PAST the 0xCC, but
    // the Windows VEH this shared code was written against reports the address
    // of the int3 itself. Rewind so VehCommon::IsAt() matches, and so resuming
    // re-executes the original instruction once the byte has been restored --
    // without this the CPU resumes mid-instruction and segfaults.
    // Single-step traps (TRAP_TRACE) legitimately point at the next
    // instruction and must not be moved.
    ucontext_t* uctx = AsUContext(ucontext);
    const bool rewound = (kind == ExceptionKind::Breakpoint) && uctx;
    if (rewound) --uctx->uc_mcontext.gregs[kIpReg];
#else
    constexpr bool rewound = false;
#endif

    bool handled = g_handler(kind, context);

#if defined(__x86_64__) || defined(__i386__)
    // The CPU clears TF for the handler, but the SAVED EFLAGS still has it set,
    // and sigreturn restores that -- so a single EnableSingleStep() would trap
    // on every subsequent instruction forever. Clear it once the step is done.
    if (handled && kind == ExceptionKind::SingleStep && uctx)
        uctx->uc_mcontext.gregs[REG_EFL] &= ~0x100L;
#endif

    if (!handled) {
#if defined(__x86_64__) || defined(__i386__)
        // Not ours: hand the next handler the context exactly as the kernel
        // delivered it.
        if (rewound) ++uctx->uc_mcontext.gregs[kIpReg];
#endif
        if (sig == SIGTRAP && g_oldSigTrap.sa_sigaction) {
            g_oldSigTrap.sa_sigaction(sig, info, ucontext);
        } else if (sig == SIGSEGV && g_oldSigSegv.sa_sigaction) {
            g_oldSigSegv.sa_sigaction(sig, info, ucontext);
        }
    }
}

} // namespace

// gregs are signed (greg_t), so a raw static_cast to uint64_t would sign-extend
// any address with the top bit set — on i386 that turns a normal 0xBFFF_xxxx
// stack pointer into 0xFFFFFFFF_BFFFxxxx. Narrow to the unsigned native width
// first.
namespace {
inline uint64_t Reg(const ucontext_t* uctx, int reg) {
    return static_cast<uint64_t>(
        static_cast<uintptr_t>(uctx->uc_mcontext.gregs[reg]));
}
} // namespace

uint64_t Context::InstructionPointer() const {
#if defined(__x86_64__) || defined(__i386__)
    ucontext_t* uctx = AsUContext(nativeContext_);
    if (!uctx) return 0;
#if defined(__x86_64__)
    return Reg(uctx, REG_RIP);
#else
    return Reg(uctx, REG_EIP);
#endif
#else
    return 0;
#endif
}

uint64_t Context::Argument(int index) const {
#if defined(__x86_64__) || defined(__i386__)
    ucontext_t* uctx = AsUContext(nativeContext_);
    if (!uctx || index <= 0) return 0;

#if defined(__x86_64__)
    // System V AMD64 ABI: RDI, RSI, RDX, RCX, R8, R9, then the stack.
    switch (index) {
    case 1: return Reg(uctx, REG_RDI);
    case 2: return Reg(uctx, REG_RSI);
    case 3: return Reg(uctx, REG_RDX);
    case 4: return Reg(uctx, REG_RCX);
    case 5: return Reg(uctx, REG_R8);
    case 6: return Reg(uctx, REG_R9);
    default: {
        uintptr_t stack = static_cast<uintptr_t>(Reg(uctx, REG_RSP));
        uintptr_t slot = stack + static_cast<uintptr_t>(index - 6) * sizeof(uint64_t);
        return *reinterpret_cast<const uint64_t*>(slot);
    }
    }
#else
    // System V i386 (cdecl): every argument is on the stack. ESP points at the
    // return address on function entry, so argument N sits at ESP + N*4.
    uintptr_t stack = static_cast<uintptr_t>(Reg(uctx, REG_ESP));
    uintptr_t slot = stack + static_cast<uintptr_t>(index) * sizeof(uint32_t);
    return *reinterpret_cast<const uint32_t*>(slot);
#endif
#else
    return 0;
#endif
}

void Context::EnableSingleStep() {
#if defined(__x86_64__) || defined(__i386__)
    ucontext_t* uctx = AsUContext(nativeContext_);
    if (!uctx) return;
    uctx->uc_mcontext.gregs[REG_EFL] |= 0x100;   // TF
#endif
}

HandlerHandle AddVectoredHandler(Handler handler) {
    if (!handler || g_installed) return nullptr;

    g_handler = handler;

    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sa.sa_sigaction = SignalHandler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGTRAP, &sa, &g_oldSigTrap);

    g_installed = true;
    return reinterpret_cast<HandlerHandle>(handler);
}

void RemoveVectoredHandler(HandlerHandle handle) {
    if (!handle || !g_installed) return;

    sigaction(SIGTRAP, &g_oldSigTrap, nullptr);

    g_handler = nullptr;
    g_installed = false;
}

} // namespace SFPlatform::Trap
