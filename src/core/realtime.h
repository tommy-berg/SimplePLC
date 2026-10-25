#pragma once
#include <expected>
#include <string>

namespace simpleplc {

/**
 * Real-time scheduling for the calling thread: SCHED_FIFO at `priority` (1–98) on Linux (needs
 * CAP_SYS_NICE); on macOS the Mach time-constraint policy for a thread that wakes every
 * `periodMs` (as audio threads use: no privileges needed). Elsewhere nothing. The error says why it
 * was refused; the thread then keeps normal scheduling.
 */
std::expected<void, std::string> makeThreadRealtime(int priority, int periodMs);

/// macOS: the highest QoS class (less timer coalescing, faster cores). Elsewhere nothing.
void preferInteractive();

}  // namespace simpleplc
