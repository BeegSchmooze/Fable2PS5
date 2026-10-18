#include <rex/platform.h>
#include <rex/thread/fiber.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#if !REX_PLATFORM_PS5
#error PS5 backend only
#endif

// System V x86-64 ABI:
// rdi = previous context
// rsi = target context
//
// The return address is already on the previous stack.
// Saving RSP and restoring another RSP lets RET resume that fiber.
//
// Context offsets:
//   rsp  0, rbx  8, rbp 16
//   r12 24, r13 32, r14 40, r15 48
//   mxcsr 56, x87 control word 60

extern "C" __attribute__((naked, noinline))
void rex_ps5_native_swap(void*, const void*) {
    __asm__ volatile(
        "movq %rsp,   0(%rdi)\n\t"
        "movq %rbx,   8(%rdi)\n\t"
        "movq %rbp,  16(%rdi)\n\t"
        "movq %r12,  24(%rdi)\n\t"
        "movq %r13,  32(%rdi)\n\t"
        "movq %r14,  40(%rdi)\n\t"
        "movq %r15,  48(%rdi)\n\t"
        "stmxcsr     56(%rdi)\n\t"
        "fnstcw      60(%rdi)\n\t"

        "movq   8(%rsi), %rbx\n\t"
        "movq  16(%rsi), %rbp\n\t"
        "movq  24(%rsi), %r12\n\t"
        "movq  32(%rsi), %r13\n\t"
        "movq  40(%rsi), %r14\n\t"
        "movq  48(%rsi), %r15\n\t"
        "ldmxcsr     56(%rsi)\n\t"
        "fldcw       60(%rsi)\n\t"
        "movq   0(%rsi), %rsp\n\t"
        "retq\n\t"
    );
}

namespace rex::thread {

constinit thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
    if (tls_current_) {
        return nullptr;
    }

    Fiber* f = new Fiber();
    f->is_thread_fiber_ = true;
    tls_current_ = f;

    return f;
}

Fiber* Fiber::Create(
    size_t stack_size,
    void (*entry)(void*),
    void* arg
) {
    if (!entry) {
        return nullptr;
    }

    Fiber* f = new Fiber();

    f->entry_ = entry;
    f->arg_ = arg;

    stack_size = std::max(stack_size, size_t(64 * 1024));
    f->stack_.resize(stack_size);

    // x86-64 stack grows downward.
    uintptr_t top =
        reinterpret_cast<uintptr_t>(f->stack_.data()) +
        f->stack_.size();

    // Align to 16 bytes.
    top &= ~uintptr_t(15);

    // Fake initial return address for the first context switch.
    // After RET, the trampoline sees the ABI-required RSP % 16 == 8.
    uintptr_t* frame =
        reinterpret_cast<uintptr_t*>(top - 16);

    frame[0] =
        reinterpret_cast<uintptr_t>(&Fiber::Trampoline);
    frame[1] = 0;

    f->context_.rsp = reinterpret_cast<uintptr_t>(frame);

    // Inherit the current thread's FP control state.
    __asm__ volatile(
        "stmxcsr %0"
        : "=m"(f->context_.mxcsr)
    );

    __asm__ volatile(
        "fnstcw %0"
        : "=m"(f->context_.fcw)
    );

    return f;
}

void Fiber::Trampoline() {
    Fiber* f = tls_current_;

    if (!f || !f->entry_) {
        std::abort();
    }

    f->entry_(f->arg_);

    // Fiber entry functions must transfer control elsewhere
    // rather than return to a nonexistent caller.
    std::abort();
}

void Fiber::SwitchTo(Fiber* target) {
    static_assert(offsetof(Context, rsp) == 0);
    static_assert(offsetof(Context, rbx) == 8);
    static_assert(offsetof(Context, rbp) == 16);
    static_assert(offsetof(Context, r12) == 24);
    static_assert(offsetof(Context, r13) == 32);
    static_assert(offsetof(Context, r14) == 40);
    static_assert(offsetof(Context, r15) == 48);
    static_assert(offsetof(Context, mxcsr) == 56);
    static_assert(offsetof(Context, fcw) == 60);
    static_assert(sizeof(Context) == 64);

    Fiber* from = tls_current_;

    if (!from || !target) {
        std::abort();
    }

    if (from == target) {
        return;
    }

    tls_current_ = target;

    rex_ps5_native_swap(
        &from->context_,
        &target->context_
    );
}

void Fiber::Destroy() {
    if (this == tls_current_) {
        if (!is_thread_fiber_) {
            std::abort();
        }
        tls_current_ = nullptr;
    }

    delete this;
}

} // namespace rex::thread
