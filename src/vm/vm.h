#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "vm/image.h"

namespace simpleplc::vm {

struct RunLimits {
    /// Instructions per cycle before F_WATCHDOG (deterministic: independent of CPU speed).
    uint64_t budget = 50'000'000;
    /// Wall-clock limit; checked every few thousand instructions. max() disables it.
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
};

struct RunResult {
    Fault fault = Fault::None;
    uint32_t pc = 0;               ///< instruction that faulted
    uint64_t executed = 0;         ///< instructions executed (cost units)
    uint8_t depth = 0;             ///< entries in `callers`
    std::array<uint32_t, 8> callers{};  ///< return addresses, innermost first (for "called from")
};

/**
 * Runs a verified image. One data block, allocated when the machine is made;
 * a cycle allocates nothing, takes no locks and makes no system calls except
 * reading the clock for the wall-clock watchdog. Not thread-safe: one thread
 * owns a machine.
 */
class Machine {
public:
    /// `image` must have passed verify().
    explicit Machine(std::shared_ptr<const Image> image);

    /// Restores the initial values: every variable (cold start), or all but the RETAIN ones (warm start).
    void reset(bool keepRetain = false);
    /// Runs the PROGRAM's body once. `nowNs` is the cycle's time (TIME, nanoseconds).
    RunResult cycle(int64_t nowNs, const RunLimits& limits = {});
    /// The next cycle reports FIRST_CYCLE() = TRUE (after a start, a reset or an online change's cold start).
    void markFirstCycle() { firstCycle_ = true; }
    void clearFirstCycle() { firstCycle_ = false; }
    /// No cycle has run since the machine was made, reset or marked: the next one is a first cycle.
    bool firstCyclePending() const { return firstCycle_; }
    /// One flag per binding: set when the program stored to it; the caller clears them after copy-out.
    uint8_t* dirty() { return dirty_.data(); }

    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }
    uint32_t size() const { return image_->blockSize; }
    const Image& image() const { return *image_; }
    std::shared_ptr<const Image> imagePtr() const { return image_; }

private:
    struct Frame {
        uint32_t pou;
        uint32_t returnPc;
        uint32_t selfBase;
        uint32_t frameBase;
    };
    struct Catch {
        uint32_t depth;
        uint32_t handler;
        uint32_t enoAddr;
    };

    std::shared_ptr<const Image> image_;
    std::vector<uint64_t> storage_;  // 8-byte aligned backing of data_
    uint8_t* data_ = nullptr;
    std::vector<Frame> frames_;      // capacity fixed at construction
    std::vector<Catch> catches_;
    std::vector<uint8_t> dirty_;
    bool firstCycle_ = true;
};

}  // namespace simpleplc::vm
