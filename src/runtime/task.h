#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/log.h"
#include "core/pi_mutex.h"
#include "core/process_image.h"
#include "vm/vm.h"

namespace simpleplc::runtime {

struct TaskOptions {
    std::string plcId;
    std::string name = "main";
    int cycleMs = 10;
    int priority = 0;        ///< 1–99: SCHED_FIFO priority when `realtime`; 0: normal scheduling
    bool realtime = false;
    std::chrono::milliseconds watchdog{0};  ///< wall-clock limit per cycle; 0: max(100 ms, 10 × cycle)
    uint64_t budget = 50'000'000;           ///< instructions per cycle (deterministic watchdog)
    /// Where the PLC's time (TIME(), the timers) is zero: its power-up, moved back by the time retentive
    /// memory kept. The epoch of the steady clock when not set.
    std::chrono::steady_clock::time_point timeOrigin{};
};

/// Where and why a program stopped.
struct FaultInfo {
    std::string task;
    std::string program;
    std::string code;       ///< F_OVERFLOW …
    std::string message;
    std::string file;
    uint32_t line = 0;
    uint64_t cycle = 0;
    int64_t atMs = 0;       ///< Unix epoch ms
    std::vector<std::pair<std::string, uint32_t>> calledFrom;  ///< file, line; innermost first
};

struct TaskStats {
    bool running = false;
    bool hasProgram = false;
    std::string program;
    int cycleMs = 0;
    uint64_t cycles = 0;
    uint64_t exceeded = 0;      ///< cycles whose cycle time was exceeded: slots skipped, not caught up
    uint64_t nanRejected = 0;   ///< float tags that were NaN/∞ at copy-in (the last good value was kept)
    double lastExecUs = 0, avgExecUs = 0, maxExecUs = 0;
    double lastLateUs = 0, maxLateUs = 0;   ///< start of the cycle after its planned time
    double p50LateUs = 0, p99LateUs = 0, p999LateUs = 0;
    int64_t loadedAtMs = 0;
    uint32_t dataBytes = 0, stackBytes = 0, codeInstructions = 0;
    bool realtime = false;      ///< real-time scheduling is in effect
    std::optional<FaultInfo> fault;
    int watchdogMs = 0;         ///< the wall-clock limit per cycle in effect (the default when none is set)
};

/**
 * Runs one compiled program on a fixed cycle grid in its own thread. Each cycle:
 * copy-in of every bound tag (one lock), the program on the VM, copy-out of
 * the tags the program stored to (one lock). The cycle path allocates nothing
 * and takes the image lock twice.
 *
 * load() is an online change: the new program replaces the old one at a cycle
 * boundary and takes over every variable with the same path and the same type
 * definition (an enumeration whose values changed goes over by name; constants
 * always take the new program's values). A fault
 * stops the task (no copy-out from the faulted cycle) and calls the fault
 * handler on the task's thread; the handler must not call stop() on this task.
 * All public methods are thread-safe.
 */
class Task {
public:
    using FaultHandler = std::function<void(const FaultInfo&)>;

    Task(TaskOptions options, std::shared_ptr<ProcessImage> image, FaultHandler onFault);
    ~Task();
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    struct LoadOutcome {
        /// Variables whose value is not taken over: they start from their initial value (all of them
        /// when there was no program before); constants only when their value changed
        std::vector<std::string> reset;
        bool replaced = false;           ///< an older program's values were taken over
    };
    /// Installs a verified image (at the next cycle boundary while running).
    LoadOutcome load(std::shared_ptr<const vm::Image> image, std::string programName);
    /**
     * What an online change from `from` (null: no program before) to `to` does with the values: the one
     * rule of `load` and of its preview. A variable keeps its value when it has the same path and its
     * type the same definition; an enumeration whose values changed goes over by name; constants take
     * the new program's values. `reset` gets the variables that start from their initial value
     * (constants only when their value changes; tags and the standard library's internals are not
     * listed), `kept` (optional) counts the listed variables that keep their value.
     */
    static void onlineChangeOutcome(const vm::Image* from, const vm::Image& to, std::vector<std::string>& reset,
                                    size_t* kept = nullptr);

    void start();
    void stop();
    /// New real-time settings: a running task's thread is stopped and started again with them; the
    /// program and its values stay.
    void setScheduling(int priority, bool realtime);
    /// Wall-clock limit per cycle in ms; 0: max(100 ms, 10 × cycle).
    void setWatchdog(int watchdogMs);
    bool running() const { return running_.load(); }
    /// One cycle at time `nowNs` while the task is stopped: the deterministic cycle the test environment
    /// drives (no product path calls it). False after a fault.
    bool step(int64_t nowNs);
    /// Initial values for every variable (cold) or all but RETAIN ones (warm, the start after a
    /// fault), and FIRST_CYCLE at the next cycle.
    void coldReset() { reset(false); }
    void warmReset() { reset(true); }
    void clearFault();

    void setCycleMs(int cycleMs);
    const std::string& name() const { return options_.name; }
    int cycleMs() const;
    TaskStats stats() const;
    std::shared_ptr<const vm::Image> image() const;
    /// The program the task will run next: a pending one (loaded, waiting for its cycle boundary) or the running one.
    std::shared_ptr<const vm::Image> latestImage() const;

    /// The program's data between two cycles, with the image that describes its layout.
    struct Snapshot {
        std::shared_ptr<const vm::Image> image;  ///< null when there is no program
        std::shared_ptr<const std::vector<uint8_t>> data;
    };
    /// A consistent copy taken by the task thread after its next cycle (at once when stopped).
    Snapshot snapshot() const;
    /// Writes bytes into the program's data before the next cycle (monitoring writes).
    std::expected<void, std::string> write(const std::string& path, std::vector<uint8_t> bytes);

private:
    struct BoundTag {
        vm::Binding binding;
        Slot slot;
        TagType tagType = TagType::Bool;
        size_t word = 0;  ///< index of its first word in the exchange buffer
    };
    struct Transfer {
        uint32_t from, to, size;
    };
    /// An enumeration whose definition changed: its value goes over by name.
    struct EnumTransfer {
        uint32_t from, fromSize;
        bool fromSigned;
        uint32_t to, toSize;
        size_t map;  ///< index into Plan::maps
    };
    /// What an online change takes over from the program it replaces.
    struct Plan {
        std::vector<Transfer> copies;
        std::vector<EnumTransfer> enums;
        std::vector<std::vector<std::pair<int64_t, int64_t>>> maps;  ///< old value → new value
        bool empty() const { return copies.empty() && enums.empty(); }
    };
    struct Pending {
        std::shared_ptr<const vm::Image> image;
        std::shared_ptr<const vm::Image> base;  // the program `transfers` were computed from
        std::unique_ptr<vm::Machine> machine;
        Plan transfers;
        std::string program;
        std::vector<uint8_t> snapshotBuffer;  // allocated by load(), not by the task thread
    };
    struct Write {
        std::shared_ptr<const vm::Image> image;  // the layout `offset` belongs to: dropped after an online change
        uint32_t offset;
        std::vector<uint8_t> bytes;
    };

    void run();
    bool cycleOnce(int64_t nowNs, std::chrono::steady_clock::time_point deadline);
    void swapPending();      // task thread (or a stopped task's caller), machineMutex_ held
    static Plan transfers(const vm::Image& from, const vm::Image& to, std::vector<std::string>* reset, size_t* kept = nullptr);
    static void applyPlan(const Plan& plan, const uint8_t* from, uint8_t* to);
    bool bind();             // resolve bindings against the tag table; false: F_BINDING
    void copyIn();
    void copyOut();
    void recordFault(const vm::RunResult& r);
    void recordLateness(double us);
    void reset(bool keepRetain);
    std::chrono::milliseconds watchdog() const;

    TaskOptions options_;
    std::shared_ptr<ProcessImage> image_;
    FaultHandler onFault_;
    log::Scope log_;

    // Owned by the task thread while it runs; by callers (under machineMutex_) while stopped.
    // Priority inheritance: a caller holding it briefly must not keep a real-time cycle waiting.
    mutable PiMutex machineMutex_;
    std::unique_ptr<vm::Machine> machine_;
    std::shared_ptr<const vm::Image> program_;
    std::string programName_;
    std::vector<BoundTag> bound_;
    std::vector<Slot> slots_;
    std::vector<uint16_t> words_;
    std::vector<uint16_t> inWords_;  // the words of the last copy-in (copy-out writes what differs)
    std::vector<uint8_t> mask_;
    uint64_t boundTagsVersion_ = UINT64_MAX;

    mutable PiMutex mutex_;  // guards the members below (taken briefly by the task thread)
    std::unique_ptr<Pending> pending_;
    std::shared_ptr<const vm::Image> current_;  // = program_, readable without machineMutex_
    std::vector<Write> writes_;
    std::vector<Write> writesApplied_;  // freed by the next write(), not on the task thread
    TaskStats stats_;
    std::array<uint64_t, 64> lateHistogram_{};
    uint64_t lateCount_ = 0;
    bool resetRequested_ = false;
    bool resetKeepsRetain_ = false;
    // Snapshots: the task thread copies into the buffer only when it gets the lock at once
    // (try_lock), so a monitoring request never makes it wait
    mutable PiMutex snapshotMutex_;
    std::vector<uint8_t> snapshotBuffer_;
    std::shared_ptr<const vm::Image> snapshotImage_;  // the image of that copy
    mutable uint64_t snapshotCycle_ = 0;
    mutable std::atomic<bool> snapshotWanted_{false};
    std::atomic<bool> hasPending_{false};

    mutable std::mutex wakeMutex_;
    mutable std::condition_variable wake_;  // stop(), period changes, snapshot answers
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<int> cycleMs_;
    std::atomic<int> watchdogMs_;
    std::thread thread_;
};

/// Locks all current and future memory of the process (real-time mode); false with the reason.
std::expected<void, std::string> lockMemory();

}  // namespace simpleplc::runtime
