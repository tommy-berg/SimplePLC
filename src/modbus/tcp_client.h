#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "core/tag.h"
#include "net/socket.h"

namespace simpleplc::modbus {

/**
 * Blocking Modbus TCP client with a timeout on every call.
 *
 * Used by the links and by the API's Modbus tools. A call blocks only the calling
 * thread, for at most the timeout. A failed call (no connection, no answer within
 * the timeout, a broken frame) is tried again at the next call: one late answer
 * costs one call, not a second. Only after kFailuresBeforeBackoff failures in a row
 * does the client wait a second between attempts, so a missing or hung device costs
 * one timeout per second instead of one per call.
 *
 * Not thread-safe; use one client per thread.
 */
class TcpClient {
public:
    struct Options {
        std::string host = "127.0.0.1";
        uint16_t port = 502;
        uint8_t unit = 1;
        std::chrono::milliseconds timeout{200};
    };

    struct Stats {
        uint64_t requests = 0;
        uint64_t errors = 0;
        std::string lastError;
    };

    /// What went wrong in the last call that failed.
    enum class Failure : uint8_t {
        None,
        Device,     ///< the device answered: an exception, or a response that does not fit the request
        Transport,  ///< no connection, no answer in time, or a broken frame
        Cancelled,  ///< the interrupt hook gave up
    };

    static constexpr int kFailuresBeforeBackoff = 3;

    explicit TcpClient(Options options) : options_(std::move(options)) {}

    /// Reads `count` entries; bits as 0/1.
    std::expected<std::vector<uint16_t>, std::string> read(Area area, uint16_t address, uint16_t count);
    /// Writes coils (0/1) or holding registers.
    std::expected<void, std::string> write(Area area, uint16_t address, std::span<const uint16_t> values);

    /// The basic objects of Read Device Identification (function 43, MEI type 14).
    struct Identity {
        std::string vendor;    ///< object 0, VendorName
        std::string product;   ///< object 1, ProductCode
        std::string revision;  ///< object 2, MajorMinorRevision
        bool operator==(const Identity&) const = default;
    };
    /// Reads the device's basic identification (objects 0-2, following "more follows"). A `timeout` bounds
    /// the whole identification, however many requests it takes (a caller with a cycle to keep gives the
    /// time it has); 0 gives each request the client's timeout. A device that refuses it (exception 1: not
    /// supported) gives Failure::Device. Connects first when needed; once connected, unlike read() and
    /// write(), no answer never counts towards the backoff, and a late answer leaves the connection usable
    /// (it is skipped by its transaction id): identification is a side issue.
    std::expected<Identity, std::string> readIdentity(std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

    /// Asked every few milliseconds while a call waits for the device; false ends the call at once
    /// (Failure::Cancelled). Lets the caller stop, or keep a deadline of its own, without waiting a timeout.
    void setInterrupt(std::function<bool()> keepWaiting) { keepWaiting_ = std::move(keepWaiting); }

    bool connected() const { return static_cast<bool>(socket_); }
    void close() { socket_.close(); }
    const Stats& stats() const { return stats_; }
    const Options& options() const { return options_; }
    Failure lastFailure() const { return lastFailure_; }
    /// Connections made so far: a new one may be to a device that restarted.
    uint64_t connects() const { return connects_; }

private:
    std::expected<std::vector<uint8_t>, std::string> transact(const std::vector<uint8_t>& pdu);
    /// One request and its response on the open connection. `stale` tells that the device had closed
    /// the connection before (send failed, or it ended before any byte of the response); `clean` that
    /// the connection is still in step (nothing of a late response has arrived yet).
    std::expected<std::vector<uint8_t>, std::string> exchange(const std::vector<uint8_t>& pdu, bool& stale, bool& clean,
                                                              bool& cancelled);
    std::expected<void, std::string> ensureConnected();
    std::string fail(std::string message, bool keepConnection);
    std::string deviceFailure(std::string message);
    /// Waits for `events` on the socket until `deadline`: 1 ready, 0 timed out, -1 error, -2 cancelled.
    int waitFor(short events, std::chrono::steady_clock::time_point deadline);

    Options options_;
    net::Socket socket_;
    uint16_t transaction_ = 0;
    std::chrono::steady_clock::time_point retryAfter_{};
    int failuresInRow_ = 0;
    uint64_t connects_ = 0;
    Failure lastFailure_ = Failure::None;
    std::function<bool()> keepWaiting_;
    Stats stats_;
};

}  // namespace simpleplc::modbus
