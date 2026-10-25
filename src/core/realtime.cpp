#include "core/realtime.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace simpleplc {

std::expected<void, std::string> makeThreadRealtime([[maybe_unused]] int priority, [[maybe_unused]] int periodMs) {
#if defined(__linux__)
    sched_param sp{};
    sp.sched_priority = std::clamp(priority, 1, 98);
    if (int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); rc != 0) return std::unexpected(std::strerror(rc));
    return {};
#elif defined(__APPLE__)
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    auto abs = [&](uint64_t ns) { return static_cast<uint32_t>(ns * tb.denom / tb.numer); };
    uint64_t period = static_cast<uint64_t>(std::max(periodMs, 1)) * 1'000'000;
    thread_time_constraint_policy_data_t pol{};
    pol.period = abs(period);
    pol.computation = abs(std::clamp<uint64_t>(period / 4, 50'000, 50'000'000));
    pol.constraint = abs(std::clamp<uint64_t>(period / 2, 100'000, 100'000'000));
    pol.preemptible = 1;
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                         reinterpret_cast<thread_policy_t>(&pol), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) return std::unexpected("the time-constraint policy was refused");
    return {};
#else
    return std::unexpected("not available on this system");
#endif
}

void preferInteractive() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

}  // namespace simpleplc
