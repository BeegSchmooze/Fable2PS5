extern "C" void ps5p_run_thread_destructors(void);
#include <rex/platform.h>

#if !REX_PLATFORM_PS5
#error PS5 synchronization backend only
#endif

#include <rex/thread.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <pthread.h>


namespace rex::thread {

namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

struct Monitor {
    std::mutex mutex;
    std::condition_variable changed;
};

Monitor& monitor() {
    static Monitor instance;
    return instance;
}

// All methods ending in _locked require monitor().mutex.
class Ps5Waitable {
public:
    virtual ~Ps5Waitable() = default;

    virtual bool ready_locked() const = 0;
    virtual void consume_locked() = 0;
    virtual bool signal_locked() = 0;
};

class Ps5Event final : public Event, public Ps5Waitable {
public:
    Ps5Event(bool manual_reset, bool initial_state)
        : manual_reset_(manual_reset),
          signaled_(initial_state) {}

    void* native_handle() const override {
        // This is an emulated synchronization object, not
        // an OS-native waitable handle.
        return nullptr;
    }

    void Set() override {
        auto& m = monitor();
        std::lock_guard<std::mutex> lock(m.mutex);
        signal_locked();
        m.changed.notify_all();
    }

    void Reset() override {
        auto& m = monitor();
        std::lock_guard<std::mutex> lock(m.mutex);
        signaled_ = false;
    }

    void Pulse() override {
        // Best-effort compatibility with the existing
        // ReXGlue POSIX implementation.
        Set();
        std::this_thread::yield();
        Sleep(std::chrono::microseconds(10));
        Reset();
    }

    bool ready_locked() const override {
        return signaled_;
    }

    void consume_locked() override {
        if (!manual_reset_) {
            signaled_ = false;
        }
    }

    bool signal_locked() override {
        signaled_ = true;
        return true;
    }

private:
    bool manual_reset_;
    bool signaled_;
};

class Ps5Semaphore final : public Semaphore,
                           public Ps5Waitable {
public:
    Ps5Semaphore(int initial_count, int maximum_count)
        : count_(initial_count),
          maximum_count_(maximum_count) {}

    void* native_handle() const override {
        return nullptr;
    }

    bool Release(
        int release_count,
        int* out_previous_count
    ) override {
        if (release_count <= 0) {
            return false;
        }

        auto& m = monitor();
        std::lock_guard<std::mutex> lock(m.mutex);

        if (release_count > maximum_count_ - count_) {
            return false;
        }

        if (out_previous_count) {
            *out_previous_count = count_;
        }

        count_ += release_count;
        m.changed.notify_all();

        return true;
    }

    bool ready_locked() const override {
        return count_ > 0;
    }

    void consume_locked() override {
        --count_;
    }

    bool signal_locked() override {
        if (count_ >= maximum_count_) {
            return false;
        }

        ++count_;
        return true;
    }

private:
    int count_;
    int maximum_count_;
};

class Ps5Mutant final : public Mutant,
                        public Ps5Waitable {
public:
    explicit Ps5Mutant(bool initial_owner)
        : depth_(initial_owner ? 1 : 0),
          owner_(initial_owner
                     ? std::this_thread::get_id()
                     : std::thread::id{}) {}

    void* native_handle() const override {
        return nullptr;
    }

    bool Release() override {
        auto& m = monitor();
        std::lock_guard<std::mutex> lock(m.mutex);

        const bool result = signal_locked();

        if (result && depth_ == 0) {
            m.changed.notify_all();
        }

        return result;
    }

    bool ready_locked() const override {
        return depth_ == 0 ||
               owner_ == std::this_thread::get_id();
    }

    void consume_locked() override {
        if (depth_ == 0) {
            owner_ = std::this_thread::get_id();
        }

        ++depth_;
    }

    bool signal_locked() override {
        if (depth_ == 0 ||
            owner_ != std::this_thread::get_id()) {
            return false;
        }

        --depth_;

        if (depth_ == 0) {
            owner_ = std::thread::id{};
        }

        return true;
    }

private:
    unsigned depth_;
    std::thread::id owner_;
};


// PS5 waitable timers use ReXGlue's shared timer queue.
//
// All waitable state is protected by the synchronization
// monitor. Rearming is serialized separately.
//
// Queue callbacks retain weak ownership of timer state,
// avoiding a raw dangling pointer after destruction.
class Ps5Timer final : public Timer, public Ps5Waitable {
public:
    explicit Ps5Timer(bool manual_reset)
        : state_(std::make_shared<State>(manual_reset)) {}

    ~Ps5Timer() override {
        Cancel();
    }

    void* native_handle() const override {
        return nullptr;
    }

    bool SetOnceAfter(
        rex::chrono::hundrednanoseconds duration,
        std::function<void()> callback = nullptr
    ) override {
        return SetOnceAt(
            GClock_::now() + duration,
            std::move(callback)
        );
    }

    bool SetOnceAt(
        WClock_::time_point due,
        std::function<void()> callback = nullptr
    ) override {
        return SetOnceAt(
            ToSteady(due),
            std::move(callback)
        );
    }

    bool SetOnceAt(
        GClock_::time_point due,
        std::function<void()> callback = nullptr
    ) override {
        return Arm(
            due,
            GClock_::duration::zero(),
            std::move(callback)
        );
    }

    bool SetRepeatingAfter(
        rex::chrono::hundrednanoseconds delay,
        std::chrono::milliseconds period,
        std::function<void()> callback = nullptr
    ) override {
        return SetRepeatingAt(
            GClock_::now() + delay,
            period,
            std::move(callback)
        );
    }

    bool SetRepeatingAt(
        WClock_::time_point due,
        std::chrono::milliseconds period,
        std::function<void()> callback = nullptr
    ) override {
        return SetRepeatingAt(
            ToSteady(due),
            period,
            std::move(callback)
        );
    }

    bool SetRepeatingAt(
        GClock_::time_point due,
        std::chrono::milliseconds period,
        std::function<void()> callback = nullptr
    ) override {
        if (period <= std::chrono::milliseconds::zero()) {
            return false;
        }

        return Arm(
            due,
            period,
            std::move(callback)
        );
    }

    bool Cancel() override {
        std::shared_ptr<TimerQueueWaitItem> previous;

        {
            std::lock_guard<std::mutex> arm_lock(arm_mutex_);

            previous = pending_.lock();
            pending_.reset();

            auto& m = monitor();
            std::lock_guard<std::mutex> lock(m.mutex);

            ++state_->generation;
            state_->callback = nullptr;

            // Preserve the current signaled state.
        }

        // Never hold the monitor or arm mutex while
        // waiting for a running callback to finish.
        if (previous) {
            previous->Disarm();
        }

        return true;
    }

    bool ready_locked() const override {
        return state_->signaled;
    }

    void consume_locked() override {
        if (!state_->manual_reset) {
            state_->signaled = false;
        }
    }

    bool signal_locked() override {
        state_->signaled = true;
        return true;
    }

private:
    // Translate a Windows/NT absolute deadline into
    // the host's monotonic steady-clock domain.
    //
    // These clocks have different epochs and cannot
    // be converted by clock_cast directly.
    static GClock_::time_point ToSteady(
        WClock_::time_point due
    ) {
        const auto wall_now = WClock_::now();
        const auto steady_now = GClock_::now();

        const auto remaining =
            due - wall_now;

        return steady_now +
            std::chrono::duration_cast<
                GClock_::duration
            >(remaining);
    }

    struct State {
        explicit State(bool manual)
            : manual_reset(manual) {}

        const bool manual_reset;
        bool signaled = false;
        uint64_t generation = 0;
        std::function<void()> callback;
    };

    bool Arm(
        GClock_::time_point due,
        GClock_::duration interval,
        std::function<void()> callback
    ) {
        std::shared_ptr<TimerQueueWaitItem> previous;

        {
            std::lock_guard<std::mutex> arm_lock(arm_mutex_);

            previous = pending_.lock();

            uint64_t generation;

            {
                auto& m = monitor();
                std::lock_guard<std::mutex> lock(m.mutex);

                generation = ++state_->generation;
                state_->signaled = false;
                state_->callback = std::move(callback);
            }

            std::weak_ptr<State> weak_state = state_;

            auto completion = [weak_state, generation](void*) {
                auto state = weak_state.lock();

                if (!state) {
                    return;
                }

                std::function<void()> callback_copy;

                {
                    auto& m = monitor();
                    std::lock_guard<std::mutex> lock(m.mutex);

                    if (generation != state->generation) {
                        return;
                    }

                    state->signaled = true;
                    callback_copy = state->callback;

                    m.changed.notify_all();
                }

                // Run user code outside the monitor lock.
                if (callback_copy) {
                    callback_copy();
                }
            };

            if (interval == GClock_::duration::zero()) {
                pending_ = QueueTimerOnce(
                    std::move(completion),
                    nullptr,
                    due
                );
            } else {
                pending_ = QueueTimerRecurring(
                    std::move(completion),
                    nullptr,
                    due,
                    interval
                );
            }
        }

        if (previous) {
            previous->Disarm();
        }

        return true;
    }

    std::shared_ptr<State> state_;
    std::mutex arm_mutex_;
    std::weak_ptr<TimerQueueWaitItem> pending_;
};


// ============================================================
// Experimental PS5 native thread integration.
//
// Supports:
// - Native thread creation
// - Initially suspended startup
// - Resuming before guest execution
// - Thread completion as a waitable object
// - FIFO user APC queue
// - Current-thread tracking
//
// Not yet supported:
// - Asynchronous suspension of an already-running thread
// - Safe forced termination of an arbitrary running thread
// - Native priority / affinity scheduling
//
// This is NOT a finished game-runtime backend.
// ============================================================

struct Ps5ThreadState {
    std::mutex mutex;
    std::condition_variable changed;

    std::deque<std::function<void()>> callbacks;

    std::atomic<bool> finished{false};
    std::atomic<int32_t> priority{0};
    std::atomic<uint64_t> affinity{~uint64_t(0)};

    uint32_t id = 0;
    uint32_t suspend_count = 0;

    bool stop_before_start = false;
};

thread_local std::shared_ptr<Ps5ThreadState> current_ps5_state;
thread_local Thread* current_ps5_thread = nullptr;

void finish_ps5_thread(
    const std::shared_ptr<Ps5ThreadState>& state
) {
    if (!state) {
        return;
    }

    auto& m = monitor();

    {
        std::lock_guard<std::mutex> lock(m.mutex);
        state->finished.store(true, std::memory_order_release);
    }

    m.changed.notify_all();
    state->changed.notify_all();
}

bool dispatch_current_ps5_apc() {
    // External threads, including the initial main thread,
    // need a Thread object for queued APC delivery.
    if (!current_ps5_state) {
        (void)Thread::GetCurrentThread();
    }

    auto state = current_ps5_state;

    if (!state) {
        return false;
    }

    std::function<void()> callback;

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        if (state->callbacks.empty()) {
            return false;
        }

        callback = std::move(state->callbacks.front());
        state->callbacks.pop_front();
    }

    if (callback) {
        callback();
    }

    return true;
}

class Ps5Thread final : public Thread, public Ps5Waitable {
public:
    explicit Ps5Thread(
        CreationParameters params,
        bool external
    ) : state_(std::make_shared<Ps5ThreadState>()),
        external_(external) {

        static std::atomic<uint32_t> next_id{10000};

        state_->id = next_id.fetch_add(
            1, std::memory_order_relaxed
        );

        state_->priority.store(params.initial_priority);

        state_->suspend_count =
            params.create_suspended ? 1u : 0u;
    }

    ~Ps5Thread() override {
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) {
                worker_.detach();
            } else {
                worker_.join();
            }
        }
    }

    bool Start(std::function<void()> routine) {
        if (external_) {
            return false;
        }

        auto state = state_;
        Ps5Thread* self = this;

        try {
            worker_ = std::thread(
                [state, self, routine = std::move(routine)]() mutable {
                    current_ps5_state = state;
                    current_ps5_thread = self;

                    {
                        std::unique_lock<std::mutex> lock(
                            state->mutex
                        );

                        state->changed.wait(
                            lock,
                            [&] {
                                return state->suspend_count == 0 ||
                                       state->stop_before_start;
                            }
                        );

                        if (state->stop_before_start) {
                            lock.unlock();
                            finish_ps5_thread(state);
                            return;
                        }
                    }

                    routine();

                    finish_ps5_thread(state);

                    current_ps5_thread = nullptr;
                    current_ps5_state.reset();
                }
            );
        } catch (...) {
            return false;
        }

        return true;
    }

    void* native_handle() const override {
        // The wait API uses Ps5Waitable, not this value.
        return nullptr;
    }

    bool ready_locked() const override {
        return state_->finished.load(
            std::memory_order_acquire
        );
    }

    void consume_locked() override {
        // Thread completion is manual-reset signaling.
    }

    bool signal_locked() override {
        return false;
    }

    uint32_t system_id() const override {
        return state_->id;
    }

    int32_t priority() override {
        return state_->priority.load();
    }

    void set_priority(int32_t priority) override {
        // Cached only: native scheduler mapping is TODO.
        state_->priority.store(priority);
    }

    uint64_t affinity_mask() override {
        return state_->affinity.load();
    }

    void set_affinity_mask(uint64_t mask) override {
        // Cached only: native CPU-affinity mapping is TODO.
        state_->affinity.store(mask);
    }

    void QueueUserCallback(
        std::function<void()> callback
    ) override {
        if (!callback) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->callbacks.push_back(std::move(callback));
        }

        state_->changed.notify_all();
        monitor().changed.notify_all();
    }

    bool Resume(
        uint32_t* previous_count = nullptr
    ) override {
        std::lock_guard<std::mutex> lock(state_->mutex);

        if (previous_count) {
            *previous_count = state_->suspend_count;
        }

        if (state_->suspend_count == 0) {
            return false;
        }

        --state_->suspend_count;

        if (state_->suspend_count == 0) {
            state_->changed.notify_all();
        }

        return true;
    }

    bool Suspend(
        uint32_t* previous_count = nullptr
    ) override {
        std::lock_guard<std::mutex> lock(state_->mutex);

        if (previous_count) {
            *previous_count = state_->suspend_count;
        }

        // Full asynchronous suspension requires a separately
        // validated console-specific mechanism. Don't claim
        // that an already running thread is suspended.
        if (state_->suspend_count == 0) {
            return false;
        }

        ++state_->suspend_count;
        return true;
    }

    void Terminate(int exit_code) override {
        (void)exit_code;

        if (current_ps5_thread == this) {
            Thread::Exit(exit_code);
        }

        {
            std::lock_guard<std::mutex> lock(state_->mutex);

            if (state_->finished.load()) {
                return;
            }

            if (state_->suspend_count > 0) {
                state_->stop_before_start = true;
                state_->changed.notify_all();
                return;
            }
        }

        // Fail explicitly rather than reporting that an active
        // guest thread was terminated when it wasn't.
        std::fprintf(
            stderr,
            "PS5: external termination of a running "
            "thread is not implemented\n"
        );
        std::abort();
    }

std::shared_ptr<Ps5ThreadState> state_for_tls() const {
        return state_;
    }

private:
    std::shared_ptr<Ps5ThreadState> state_;
    std::thread worker_;
    bool external_;
};

thread_local std::unique_ptr<Ps5Thread> external_ps5_thread;

Ps5Waitable* as_waitable(WaitHandle* handle) {
    return dynamic_cast<Ps5Waitable*>(handle);
}

WaitResult wait_one_locked(
    Ps5Waitable* target,
    std::unique_lock<std::mutex>& lock,
    Milliseconds timeout
) {
    auto& m = monitor();

    const bool infinite = timeout == Milliseconds::max();

    const auto deadline = infinite
        ? Clock::time_point::max()
        : Clock::now() + timeout;

    while (!target->ready_locked()) {
        if (!infinite && Clock::now() >= deadline) {
            return WaitResult::kTimeout;
        }

        if (infinite) {
            m.changed.wait(lock);
        } else {
            m.changed.wait_until(lock, deadline);
        }
    }

    target->consume_locked();
    return WaitResult::kSuccess;
}


WaitResult wait_one_alertable_locked(
    Ps5Waitable* target,
    std::unique_lock<std::mutex>& lock,
    Milliseconds timeout
) {
    const bool infinite = timeout == Milliseconds::max();

    const auto deadline = infinite
        ? Clock::time_point::max()
        : Clock::now() + timeout;

    for (;;) {
        lock.unlock();
        const bool dispatched = dispatch_current_ps5_apc();
        lock.lock();

        if (dispatched) {
            return WaitResult::kUserCallback;
        }

        if (target->ready_locked()) {
            target->consume_locked();
            return WaitResult::kSuccess;
        }

        if (!infinite && Clock::now() >= deadline) {
            return WaitResult::kTimeout;
        }

        Milliseconds slice(1);

        if (!infinite) {
            auto remaining =
                std::chrono::duration_cast<Milliseconds>(
                    deadline - Clock::now()
                );

            if (remaining < slice) {
                slice = remaining;
            }

            if (slice <= Milliseconds::zero()) {
                continue;
            }
        }

        const auto result =
            wait_one_locked(target, lock, slice);

        if (result != WaitResult::kTimeout) {
            return result;
        }
    }
}

} // anonymous namespace

// ============================================================
// Native thread API - experimental PS5 implementation.
// ============================================================

std::unique_ptr<Thread> Thread::Create(
    CreationParameters params,
    std::function<void()> start_routine
) {
    auto thread = std::make_unique<Ps5Thread>(
        params, false
    );

    if (!thread->Start(std::move(start_routine))) {
        return nullptr;
    }

    return thread;
}

Thread* Thread::GetCurrentThread() {
    if (current_ps5_thread) {
        return current_ps5_thread;
    }

    if (!external_ps5_thread) {
        external_ps5_thread = std::make_unique<Ps5Thread>(
            CreationParameters{}, true
        );
    }

    current_ps5_thread = external_ps5_thread.get();

    // The external thread's shared state is used for APC
    // callbacks but does not own another native thread.
    current_ps5_state = external_ps5_thread->state_for_tls();

    return current_ps5_thread;
}

void Thread::Exit(int exit_code) {
    (void)exit_code;

    auto state = current_ps5_state;

    finish_ps5_thread(state);
    // Match the SDK thread wrapper's normal-return ordering. libkernel key
    // destructors otherwise may free emutls before C++ TLS destructors run.
    ps5p_run_thread_destructors();

    ::pthread_exit(nullptr);

    std::abort();
}

SleepResult AlertableSleep(
    std::chrono::microseconds duration
) {
    if (!current_ps5_state) {
        (void)Thread::GetCurrentThread();
    }

    auto state = current_ps5_state;

    const bool infinite =
        duration == std::chrono::microseconds::max();

    const auto deadline = infinite
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + duration;

    for (;;) {
        if (dispatch_current_ps5_apc()) {
            return SleepResult::kAlerted;
        }

        if (!infinite &&
            std::chrono::steady_clock::now() >= deadline) {
            return SleepResult::kSuccess;
        }

        std::unique_lock<std::mutex> lock(state->mutex);

        if (infinite) {
            state->changed.wait_for(
                lock, std::chrono::milliseconds(1)
            );
        } else {
            state->changed.wait_until(
                lock,
                std::min(
                    deadline,
                    std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(1)
                )
            );
        }
    }
}


std::unique_ptr<Event>
Event::CreateManualResetEvent(bool initial_state) {
    return std::make_unique<Ps5Event>(
        true, initial_state
    );
}

std::unique_ptr<Event>
Event::CreateAutoResetEvent(bool initial_state) {
    return std::make_unique<Ps5Event>(
        false, initial_state
    );
}

std::unique_ptr<Semaphore>
Semaphore::Create(int initial_count, int maximum_count) {
    if (maximum_count <= 0 ||
        initial_count < 0 ||
        initial_count > maximum_count) {
        return nullptr;
    }

    return std::make_unique<Ps5Semaphore>(
        initial_count, maximum_count
    );
}

std::unique_ptr<Mutant>
Mutant::Create(bool initial_owner) {
    return std::make_unique<Ps5Mutant>(initial_owner);
}


std::unique_ptr<Timer>
Timer::CreateManualResetTimer() {
    return std::make_unique<Ps5Timer>(true);
}

std::unique_ptr<Timer>
Timer::CreateSynchronizationTimer() {
    return std::make_unique<Ps5Timer>(false);
}

WaitResult Wait(
    WaitHandle* handle,
    bool is_alertable,
    Milliseconds timeout
) {

    auto* target = as_waitable(handle);

    if (!target) {
        return WaitResult::kFailed;
    }

    auto& m = monitor();
    std::unique_lock<std::mutex> lock(m.mutex);

    return is_alertable
        ? wait_one_alertable_locked(target, lock, timeout)
        : wait_one_locked(target, lock, timeout);
}

std::pair<WaitResult, size_t> WaitMultiple(
    WaitHandle* handles[],
    size_t count,
    bool wait_all,
    bool is_alertable,
    Milliseconds timeout
) {
    if (!handles ||
        count == 0 ||
        count > 64) {
        return {WaitResult::kFailed, 0};
    }

    std::vector<Ps5Waitable*> targets;
    targets.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        auto* target = as_waitable(handles[i]);

        if (!target) {
            return {WaitResult::kFailed, 0};
        }

        // Duplicate wait handles are invalid.
        if (std::find(
                targets.begin(),
                targets.end(),
                target
            ) != targets.end()) {
            return {WaitResult::kFailed, 0};
        }

        targets.push_back(target);
    }

    auto& m = monitor();
    std::unique_lock<std::mutex> lock(m.mutex);

    const bool infinite = timeout == Milliseconds::max();

    const auto deadline = infinite
        ? Clock::time_point::max()
        : Clock::now() + timeout;

    for (;;) {
        if (is_alertable) {
            lock.unlock();
            const bool dispatched =
                dispatch_current_ps5_apc();
            lock.lock();

            if (dispatched) {
                return {WaitResult::kUserCallback, 0};
            }
        }
        if (wait_all) {
            bool all_ready = true;

            for (auto* target : targets) {
                if (!target->ready_locked()) {
                    all_ready = false;
                    break;
                }
            }

            if (all_ready) {
                // Atomic consumption under the same lock.
                for (auto* target : targets) {
                    target->consume_locked();
                }

                return {WaitResult::kSuccess, 0};
            }
        } else {
            for (size_t i = 0; i < targets.size(); ++i) {
                if (targets[i]->ready_locked()) {
                    targets[i]->consume_locked();
                    return {WaitResult::kSuccess, i};
                }
            }
        }

        if (!infinite && Clock::now() >= deadline) {
            return {WaitResult::kTimeout, 0};
        }

        if (is_alertable) {
            m.changed.wait_for(
                lock, std::chrono::milliseconds(1)
            );
        } else if (infinite) {
            m.changed.wait(lock);
        } else {
            m.changed.wait_until(lock, deadline);
        }
    }
}

WaitResult SignalAndWait(
    WaitHandle* to_signal,
    WaitHandle* to_wait_on,
    bool is_alertable,
    Milliseconds timeout
) {

    auto* signal = as_waitable(to_signal);
    auto* target = as_waitable(to_wait_on);

    if (!signal || !target) {
        return WaitResult::kFailed;
    }

    auto& m = monitor();

    // Keep signal and initial wait under one mutex:
    // no lost wakeup between these operations.
    std::unique_lock<std::mutex> lock(m.mutex);

    if (!signal->signal_locked()) {
        return WaitResult::kFailed;
    }

    m.changed.notify_all();

    return is_alertable
        ? wait_one_alertable_locked(target, lock, timeout)
        : wait_one_locked(target, lock, timeout);
}

} // namespace rex::thread
