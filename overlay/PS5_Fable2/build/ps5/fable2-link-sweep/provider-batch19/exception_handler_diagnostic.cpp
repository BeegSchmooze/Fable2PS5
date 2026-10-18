#include <cstdio>
#include <ps5platform/klog.h>
#include <rex/exception_handler.h>
#include <rex/platform.h>
#include <ps5platform/context.h>

#include <signal.h>
#include <ucontext.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

static_assert(REX_PLATFORM_PS5, "PS5 backend only");

namespace rex::arch {

namespace {

constexpr size_t kMaxHandlers = 8;
std::pair<ExceptionHandler::Handler, void*> handlers[kMaxHandlers]{};

struct sigaction oldSegv{};
struct sigaction oldBus{};
struct sigaction oldIll{};
bool installed = false;

const struct sigaction* previousAction(int signal) {
    switch (signal) {
    case SIGSEGV: return &oldSegv;
    case SIGBUS:  return &oldBus;
    case SIGILL:  return &oldIll;
    default:      return nullptr;
    }
}

void handleSignal(int signal, siginfo_t* info, void* opaque) {
    if (!opaque || !info)
        std::_Exit(128 + signal);

    auto& mc = reinterpret_cast<ucontext_t*>(opaque)->uc_mcontext;

    // x86-64 register order required by ReXGlue.
    using Reg = decltype(mc.mc_rax);
    Reg* regs[16] = {
        &mc.mc_rax, &mc.mc_rcx, &mc.mc_rdx, &mc.mc_rbx,
        &mc.mc_rsp, &mc.mc_rbp, &mc.mc_rsi, &mc.mc_rdi,
        &mc.mc_r8,  &mc.mc_r9,  &mc.mc_r10, &mc.mc_r11,
        &mc.mc_r12, &mc.mc_r13, &mc.mc_r14, &mc.mc_r15
    };

    HostThreadContext ctx{};
    ctx.rip = static_cast<uint64_t>(mc.mc_rip);
    ctx.eflags = static_cast<uint32_t>(mc.mc_rflags);

    for (unsigned i = 0; i < 16; ++i)
        ctx.int_registers[i] = static_cast<uint64_t>(*regs[i]);

    // FreeBSD x86-64 uses a 512-byte FXSAVE area.
    // Its XMM0-XMM15 registers begin at byte offset 160.
    const bool hasXmm =
        mc.mc_fpformat == _MC_FPFMT_XMM &&
        mc.mc_ownedfp != _MC_FPOWNED_NONE;

    constexpr size_t kXmmOffset = 160;
    static_assert(sizeof(mc.mc_fpstate) >=
                  kXmmOffset + sizeof(ctx.xmm_registers));

    if (hasXmm) {
        std::memcpy(ctx.xmm_registers,
                    reinterpret_cast<const uint8_t*>(mc.mc_fpstate)
                        + kXmmOffset,
                    sizeof(ctx.xmm_registers));
    }

    Exception ex;

    if (signal == SIGILL) {
        ex.InitializeIllegalInstruction(&ctx);
    } else {
        const auto operation =
            (static_cast<uint64_t>(mc.mc_err) & 2)
            ? Exception::AccessViolationOperation::kWrite
            : Exception::AccessViolationOperation::kRead;

        ex.InitializeAccessViolation(
            &ctx,
            reinterpret_cast<uint64_t>(info->si_addr),
            operation);
    }

    for (const auto& entry : handlers) {
        if (!entry.first)
            break;

        if (!entry.first(&ex, entry.second))
            continue;

        // Write modified CPU state back to the signal context.
        mc.mc_rip = static_cast<Reg>(ctx.rip);
        mc.mc_rflags = static_cast<Reg>(ctx.eflags);

        const uint16_t changed = ex.modified_int_registers();

        for (unsigned i = 0; i < 16; ++i) {
            if (changed & (uint16_t(1) << i))
                *regs[i] = static_cast<Reg>(ctx.int_registers[i]);
        }

        const uint16_t changedXmm = ex.modified_xmm_registers();

        if (changedXmm && !hasXmm)
            std::_Exit(128 + signal);

        auto* fp = reinterpret_cast<uint8_t*>(mc.mc_fpstate);

        for (unsigned i = 0; i < 16; ++i) {
            if (changedXmm & (uint16_t(1) << i)) {
                std::memcpy(fp + kXmmOffset + i * 16,
                            &ctx.xmm_registers[i], 16);
            }
        }

        return;
    }

    char report[256];
    int length=std::snprintf(report,sizeof(report),"[FABLE2-FAULT] signal=%d rip=0x%llx address=0x%llx err=0x%llx rdi=0x%llx rsi=0x%llx\n",signal,(unsigned long long)mc.mc_rip,(unsigned long long)info->si_addr,(unsigned long long)mc.mc_err,(unsigned long long)mc.mc_rdi,(unsigned long long)mc.mc_rsi);
    if(length>0) sceKernelDebugOutText(0,report);
    // Never silently retry an unhandled fault forever.
    if (const auto* previous = previousAction(signal)) {
        sigaction(signal, previous, nullptr);
        raise(signal);
    }
    std::_Exit(128 + signal);
}

} // namespace

void ExceptionHandler::Install(Handler fn, void* data) {
    if (!fn)
        std::abort();

    size_t slot = kMaxHandlers;
    for (size_t i = 0; i < kMaxHandlers; ++i) {
        if (!handlers[i].first) {
            slot = i;
            break;
        }
    }

    if (slot == kMaxHandlers)
        std::abort();

    if (!installed) {
        struct sigaction action{};
        action.sa_sigaction = handleSignal;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);

        if (sigaction(SIGSEGV, &action, &oldSegv) != 0)
            std::abort();

        if (sigaction(SIGBUS, &action, &oldBus) != 0) {
            sigaction(SIGSEGV, &oldSegv, nullptr);
            std::abort();
        }

        if (sigaction(SIGILL, &action, &oldIll) != 0) {
            sigaction(SIGBUS, &oldBus, nullptr);
            sigaction(SIGSEGV, &oldSegv, nullptr);
            std::abort();
        }

        installed = true;
    }

    handlers[slot] = {fn, data};
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
    for (size_t i = 0; i < kMaxHandlers; ++i) {
        if (handlers[i].first == fn &&
            handlers[i].second == data) {
            for (size_t j = i; j + 1 < kMaxHandlers; ++j)
                handlers[j] = handlers[j + 1];

            handlers[kMaxHandlers - 1] = {};
            break;
        }
    }

    bool active = false;
    for (const auto& entry : handlers)
        active |= entry.first != nullptr;

    if (!active && installed) {
        sigaction(SIGILL, &oldIll, nullptr);
        sigaction(SIGBUS, &oldBus, nullptr);
        sigaction(SIGSEGV, &oldSegv, nullptr);
        installed = false;
    }
}

} // namespace rex::arch
