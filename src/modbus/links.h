#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/log.h"
#include "core/process_image.h"
#include "modbus/tcp_client.h"

namespace simpleplc::modbus {

/// One tag a link exchanges: the remote device's entries at `address` in `area`.
struct LinkMapping {
    std::string tag;
    Area area = Area::HoldingRegister;
    int address = 0;

    bool operator==(const LinkMapping&) const = default;
};

/// How a device orders the two registers of a 32-bit value.
enum class WordOrder : uint8_t {
    Abcd,  ///< high word first, as in this PLC ("abcd", the default)
    Cdab,  ///< low word first ("cdab"), as many meters, drives and I/O modules send them
};

std::string_view wordOrderName(WordOrder order);
std::optional<WordOrder> parseWordOrder(std::string_view name);

/**
 * This PLC as Modbus TCP client of another device, without code: every `period_ms` the
 * link reads the `read` entries into their tags and writes the `write` tags' values
 * to the device (when they change, and all of them every 10 periods). `status_tag`
 * is TRUE while exchanges succeed and FALSE once no cycle has succeeded for `stale_ms`.
 */
struct LinkConfig {
    std::string name;
    std::string host;
    int port = 502;
    int unit = 1;
    int periodMs = 100;
    int timeoutMs = 200;
    std::string statusTag;
    int staleMs = 0;                  ///< 0: 3 × period_ms + timeout_ms
    WordOrder wordOrder = WordOrder::Abcd;
    std::vector<LinkMapping> read;    ///< remote → tag
    std::vector<LinkMapping> write;   ///< tag → remote (coils and holding registers)

    int staleAfterMs() const { return staleMs > 0 ? staleMs : 3 * periodMs + timeoutMs; }
    bool operator==(const LinkConfig&) const = default;
};

struct LinkStatus {
    std::string name;
    std::string host;
    uint16_t port = 0;
    uint8_t unit = 1;
    bool connected = false;
    bool ok = false;                ///< an exchange succeeded within stale_ms (the status tag)
    uint64_t cycles = 0;
    uint64_t requests = 0;
    uint64_t errors = 0;
    uint64_t exceeded = 0;          ///< cycles whose period was exceeded (the slots skipped, not caught up)
    std::string lastError;          ///< of the last cycle; empty when it succeeded
    int64_t lastRequestAgoMs = -1;  ///< -1 before the first request
    int64_t lastSuccessAgoMs = -1;
    /// What the device says it is (Read Device Identification, once per connection); none when it
    /// does not support it or has not answered it yet
    std::optional<TcpClient::Identity> identity;
};

/// What the PLC's outputs are, for the links that send them to devices (asked once per cycle).
enum class Outputs : uint8_t {
    Pending,  ///< RUN, but the programs have not completed their first cycle: send nothing yet
    Run,      ///< the programs compute them: send every write tag
    Substitute,  ///< STOP or FAULT: send what the tags hold (substitute values), but no tag nothing has written yet
};

/**
 * Runs one link in its own thread, on a fixed grid of `period_ms` (absolute
 * deadlines; a cycle whose period is exceeded skips to the next slot). A cycle reads every
 * `read` range (contiguous tags in one area share a request), then writes all the
 * values into their tags under one image lock, so the program never sees half a
 * cycle; then it writes the `write` tags whose value changed since the last
 * successful write (every one after a new connection and every 10 periods). Each
 * request stands on its own: one the device refuses, or a value that is not a
 * number, affects only its tags, and the others (above all the substitute values at STOP)
 * still go out. Values keep their last good state when the device is unreachable:
 * programs check the status tag. A cycle that has nothing to send reads one entry,
 * so the status always says whether the device answers.
 *
 * Tags are resolved by name when the tag table changes; one that no longer exists
 * is skipped and reported. Thread-safe.
 */
class LinkPoller {
public:
    /// `realtimePriority` > 0: real-time scheduling for the link's thread (below the tasks).
    /// `outputs` tells what may be sent to the device (none: Outputs::Run).
    LinkPoller(LinkConfig config, std::shared_ptr<ProcessImage> image, const std::string& plcId, int realtimePriority = 0,
               std::function<Outputs()> outputs = {});
    ~LinkPoller();
    LinkPoller(const LinkPoller&) = delete;
    LinkPoller& operator=(const LinkPoller&) = delete;

    void start();
    /// Asks the thread to end, without waiting for it. With `flush`, it first writes every write tag
    /// once more (the substitute values at shutdown), within the link's timeout; without, it ends at once,
    /// even in the middle of a request. `clearStatus`: the status tag goes FALSE (not when another
    /// link that takes this one's place writes it).
    void requestStop(bool flush, bool clearStatus = true);
    /// requestStop(false) and waits for the thread (a few milliseconds).
    void stop();
    const LinkConfig& config() const { return config_; }
    LinkStatus status() const;

private:
    using Clock = std::chrono::steady_clock;

    struct Target {
        const LinkMapping* mapping = nullptr;
        Slot local;           ///< the tag's entries in this PLC
        TagType type = TagType::Bool;
        uint32_t words = 1;   ///< entries on both sides
        bool present = false;
    };
    struct Request {
        Area area = Area::HoldingRegister;
        uint16_t address = 0;
        uint16_t count = 0;
        std::vector<size_t> targets;  ///< indexes into reads_, in address order
    };
    enum StopMode : int { kRunning = 0, kFlush = 1, kAbort = 2 };

    void run();
    void resolve();
    void cycle(Clock::time_point now);
    /// Every write tag once more, ignoring what was sent before (the final cycle at shutdown).
    void flush();
    /// Records a problem of this cycle; the first one is published at once when the link had no error.
    void addError(std::vector<std::string>& errors, std::string error);
    void readAll(std::vector<std::string>& errors, bool& sent);
    void writeChanged(bool all, Outputs outputs, std::vector<std::string>& errors, bool& sent);
    /// A device that is not asked anything in a cycle is asked for one entry, so the status means something.
    void probe(std::vector<std::string>& errors);
    /// Called every few milliseconds while the client waits: false ends the wait (stopping, or past the
    /// flush deadline). Also drops the status the moment stale_ms has passed.
    bool keepWaiting();
    /// The status goes FALSE once stale_ms has passed since the last successful cycle (timer-accurate).
    void checkStale(Clock::time_point now);
    /// Publishes the status: LinkStatus, the status tag (every time: nothing else may leave it wrong),
    /// the read tags' stale marks, and a log line when it changes.
    void publish(bool fresh, const std::string& why);
    /// Once per connection, in the time left before the next cycle (`next`), never delaying it: asks the
    /// device who it is. A device that does not answer is asked again on a later connection only.
    void identify(Clock::time_point next);
    void setStatusTag(bool ok);
    std::string describe(const char* what, Area area, uint32_t address, uint32_t count, const std::vector<size_t>& targets,
                         const std::vector<Target>& list) const;

    const LinkConfig config_;
    const std::shared_ptr<ProcessImage> image_;
    const int realtimePriority_;
    const std::function<Outputs()> outputs_;
    log::Scope log_;
    TcpClient client_;

    // Link thread only
    uint64_t tagsVersion_ = UINT64_MAX;
    std::vector<Target> reads_, writes_;
    std::vector<Request> requests_;
    std::vector<size_t> readOffsets_;   ///< where each read target's words start in readValues_
    std::vector<Slot> readSlots_;
    std::vector<Slot> presentReadSlots_;  ///< the read tags that exist (marked stale while the status is FALSE)
    std::vector<uint16_t> readValues_;
    std::vector<uint8_t> readMask_;
    std::vector<Slot> writeSlots_;
    std::vector<uint16_t> writeValues_, written_;
    std::vector<Origin> writeOrigins_;
    std::vector<uint64_t> writtenOn_;   ///< per write target: the connection written_ went out on (0: none)
    uint64_t sinceRefresh_ = 0;
    bool fresh_ = false;                ///< the published status
    bool published_ = false;
    bool staleMarked_ = false;          ///< the read tags are marked stale in the image
    bool flushing_ = false;
    std::string lastErrorText_;
    Clock::time_point lastSuccess_{};
    Clock::time_point flushDeadline_{};
    uint64_t identifiedOn_ = 0;         ///< the connection identification was asked on (0: none yet)
    Clock::time_point identifyAfter_{}; ///< a device that did not answer it: not before

    mutable std::mutex mutex_;  // guards the members below
    LinkStatus status_;
    Clock::time_point lastRequestAt_{}, lastSuccessAt_{};

    std::mutex wakeMutex_;
    std::condition_variable wake_;
    std::atomic<int> stopMode_{kRunning};
    std::atomic<bool> clearStatus_{true};
    std::thread thread_;
};

}  // namespace simpleplc::modbus
