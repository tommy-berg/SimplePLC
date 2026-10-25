#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace httplib {
class Client;
}

namespace simpleplc::web {

using json = nlohmann::json;

/**
 * A fleet: one panel, REST API and MCP endpoint for the PLCs of many devices
 * (SimplePLC processes, containers or Raspberry Pis), like an engineering
 * station on the plant network.
 *
 * One poller per device asks for its PLC every second and copies the device's
 * log records into this process's log, so the dashboard, the log and
 * list_plcs show the whole network. Everything about one PLC (values, programs,
 * live streams, MCP tool calls) is passed through to the device that runs it,
 * signed in with the device token.
 *
 * Devices are trusted with their own PLC only: what they answer is checked
 * before use and limited in size, their log records are filed under their own
 * PLC, every request has a deadline, and a device that hangs holds at most
 * kMaxInFlight requests. A device that refuses the fleet's token is asked again
 * less and less often, so the fleet does not trip its lockout of wrong tokens.
 *
 * Devices come from the command line (--device, signed in to with the environment's token) or are
 * adopted at run time (addDevice: with a pairing code from the device, or a token) and kept in the
 * store (fleet.json, readable by the owner only). Adding and removing devices while the fleet runs
 * is safe: a removed device keeps its place, marked removed, so nothing that runs for it loses it.
 * Thread-safe.
 */
class Fleet {
public:
    /// Requests to one device that may wait for it at the same time; more get 503.
    static constexpr int kMaxInFlight = 8;
    /// Of those, long ones (checks, compiles, program saves and loads): any user's long calls leave room for the rest.
    static constexpr int kMaxLong = 4;
    /// Extra places for control calls (run, stop, restart, writes, forces), which reads and long calls cannot take.
    static constexpr int kControlSlots = 4;
    /// Longest answer accepted from a device for a request passed through.
    static constexpr size_t kMaxAnswer = 16 * 1024 * 1024;
    /// Longest PLC summary accepted from a device's poll (a real one is a few kB).
    static constexpr size_t kMaxSummary = 1024 * 1024;
    /// Longest log answer accepted from a device's poll (2000 records of 2 kB on average).
    static constexpr size_t kMaxLogAnswer = 4 * 1024 * 1024;

    /// "tank" -> "http://tank:8080", "10.0.0.5:9000" -> "http://10.0.0.5:9000"; http only, no path.
    static std::expected<std::string, std::string> parseDeviceUrl(std::string_view text);

    /// A device and the admin token the fleet signs in to it with.
    struct DeviceEntry {
        std::string url;    ///< checked with parseDeviceUrl
        std::string token;
        bool flag = false;  ///< given on the command line: not stored, not removable at run time
    };
    /// The devices kept in `file` (fleet.json: [{url, token}]); none when it does not exist.
    static std::expected<std::vector<DeviceEntry>, std::string> loadDevices(const std::filesystem::path& file);

    /// `entries`: the devices to start with (of two with one address, the first counts). `store`: where adopted
    /// devices are kept (empty: nowhere). `name`: the fleet's name, which its tokens on the devices carry ("fleet:<name>").
    Fleet(std::vector<DeviceEntry> entries, std::filesystem::path store = {}, std::string name = "fleet");
    ~Fleet();
    Fleet(const Fleet&) = delete;
    Fleet& operator=(const Fleet&) = delete;

    void start();
    /// Ends every request to a device at once (they fail) and stops the pollers. Idempotent.
    void stop();

    /// The PLCs of all devices in the order given; one that was never reached shows as an offline PLC.
    json plcs() const;
    /// Version, devices and their state, warnings for unreachable devices and ids used twice.
    json status() const;

    /// Why a request cannot go to a device, as an HTTP status, an error code and a message.
    struct Failure {
        int status;
        std::string code;
        std::string message;
    };
    /// The address of the device that runs `plcId`; 404 when none does, 409 when several do.
    std::expected<std::string, Failure> deviceFor(const std::string& plcId) const;
    /// A device other than `url` whose PLC has this id (renaming to it would make it ambiguous).
    std::optional<std::string> otherDeviceWith(const std::string& plcId, const std::string& url) const;
    /// Whether requests can go to the device now: 503 while the poller finds it offline or refusing the token.
    std::expected<void, Failure> available(const std::string& url) const;
    /// Polls one device now (after a change made through the fleet, e.g. a renamed PLC).
    void refresh(const std::string& url);

    /// The devices: [{url, plc, online, error, source}], source "flag" (command line) or "added".
    json devices() const;
    /// Adopts a device: {url, code} trades the device's pairing code for a token, {url, token} takes an admin
    /// token as it is. The device must be a SimplePLC whose PLC id no other device has. It is stored and polled
    /// at once. Answers the device as devices() lists it.
    std::expected<json, Failure> addDevice(const json& body);
    /// Removes the device whose PLC is `plcId` and, when it is reachable, revokes the fleet's token on it.
    std::expected<json, Failure> removeDevice(const std::string& plcId);

    struct Reply {
        int status = 0;
        std::string body;
        std::string contentType;
        std::string etag;  ///< the device's ETag, passed on
    };
    /// One request to a device, with the device token. `target` is the path with its query. Renames are
    /// done one at a time and refused (409) when another device has the id. `headers` go along (If-Match).
    std::expected<Reply, Failure> send(const std::string& url, const std::string& method, const std::string& target,
                                       const std::string& body, const std::string& contentType,
                                       const std::map<std::string, std::string>& headers = {});
    /// A GET whose answer is passed on piece by piece (live streams) until `chunk` returns false or the
    /// device ends it. A device that answers with an error status gives that status as a Failure; a device
    /// that goes offline or falls silent ends the stream with a Failure.
    std::expected<void, Failure> stream(const std::string& url, const std::string& target,
                                        const std::function<bool(std::string_view)>& chunk);

private:
    struct Device {
        std::string url;
        std::string token;        ///< the admin token the fleet signs in to it with
        bool flag = false;        ///< from the command line
        bool removed = false;     ///< removed at run time: its place stays, nothing uses it
        std::shared_ptr<std::mutex> pollLock = std::make_shared<std::mutex>();  ///< one poll at a time
        bool online = false;
        bool refused = false;     ///< it answers, but refuses the fleet's token
        bool everReached = false;
        bool warned = false;      ///< its being unreachable is in the log
        std::string error;
        json plc;                 ///< last summary of its PLC (checked: an object with a valid id)
        uint64_t logSeq = 0;      ///< the newest of its log records the fleet has copied
        bool logsStarted = false;
        int64_t deviceStartedMs = 0;  ///< when the device process started: a change means it restarted
        int refusals = 0;         ///< token refusals in a row: the next try waits longer
        std::chrono::steady_clock::time_point retryAt{};  ///< no poll before (after a refusal)
        std::string sameAs;       ///< the address under which the fleet already reaches this very device
        int sameAsMismatches = 0;
    };
    /// What a request is for: how long it may take and which places it may use.
    enum class Kind { Normal, Long, Control };
    /// Why the watchdog ended a request.
    enum class Ended { No, Deadline, Stopping, Offline };
    struct Fetched;

    void pollLoop(size_t index);
    void poll(size_t index);
    void watchdogLoop();
    /// Starts the poller of the device at `index` (when the fleet runs). Caller holds pollersMutex_.
    void startPollerLocked(size_t index);
    /// Writes the adopted (not flagged) devices to the store. Caller holds mutex_.
    std::expected<void, std::string> saveLocked() const;
    /// A request to `url` that is not (yet) one of the fleet's devices, ended by the watchdog like any.
    Fetched request(const std::string& url, const std::string& method, const std::string& target, const std::string& token,
                    const std::string& body, std::chrono::milliseconds deadline);
    /// One request with a deadline: ended by the watchdog when it passes, the fleet stops or (with `sink`, a
    /// stream) the device goes offline. At most `maxSize` bytes of answer are kept (more: `tooLarge`), unless
    /// `sink` takes them.
    Fetched fetch(httplib::Client& client, size_t index, const std::string& token, const std::string& method,
                  const std::string& target, const std::string& body, const std::string& contentType,
                  std::chrono::milliseconds deadline, size_t maxSize, const std::function<bool(std::string_view)>* sink = nullptr,
                  const std::map<std::string, std::string>* headers = nullptr);
    std::expected<Reply, Failure> sendNow(size_t index, const std::string& method, const std::string& target,
                                          const std::string& body, const std::string& contentType, Kind kind,
                                          const std::map<std::string, std::string>& headers = {});
    /// The device refused the token (401/403) or locked the fleet out (429): the next poll waits.
    static void noteRefusalLocked(Device& d, int status, int lockoutS);
    /// Marks the device at `index` as the same as one reached under another address; returns that address
    /// when this is found now. Caller holds mutex_.
    std::string sameDeviceLocked(size_t index);
    static Kind kindOf(const std::string& method, const std::string& target, const json& rpc);
    /// The id a request gives the PLC, when it renames it (`body`: a REST request's JSON, `rpc`: an MCP message).
    static std::optional<std::string> renameTo(const std::string& method, const std::string& target, const json& body,
                                               const json& rpc);
    static std::string idOf(const Device& d);
    /// The PLC entry of a device nobody has reached: an id from its address, `null` for every fact the
    /// fleet does not know (running, tags, ports), empty lists.
    static json offlinePlc(const Device& d);
    /// Ids used by more than one device, with their addresses. Caller holds mutex_.
    std::vector<std::pair<std::string, std::vector<std::string>>> duplicatesLocked() const;
    /// The place in devices_ of the device at `url` (not a removed one), or -1.
    int indexOf(const std::string& url) const;
    int indexOfLocked(const std::string& url) const;
    /// Why the device at `index` takes no requests now, if it does not. Caller holds mutex_.
    std::optional<Failure> unavailableLocked(size_t index) const;

    std::filesystem::path store_;
    std::string name_;
    mutable std::mutex mutex_;
    std::vector<Device> devices_;  // only grows: a removed device is marked, so indexes stay valid
    std::mutex adoptMutex_;        // adding and removing devices, one at a time
    struct Slots {
        int general = 0;  // requests that wait for the device
        int longCalls = 0;
        int control = 0;  // control calls in their reserved places
    };
    std::vector<Slots> slots_;  // guarded by mutex_
    std::mutex renameMutex_;    // renames through the fleet, one at a time

    // Requests in progress, for the watchdog
    struct Call {
        httplib::Client* client;
        std::chrono::steady_clock::time_point deadline;
        size_t device;
        bool stream;
        Ended ended;
    };
    std::mutex callsMutex_;
    std::map<uint64_t, Call> calls_;
    uint64_t nextCall_ = 0;
    std::thread watchdog_;

    std::mutex pollersMutex_;
    std::vector<std::thread> pollers_;  // pollersMutex_
    std::atomic<bool> stopping_{false};
    std::atomic<bool> started_{false};
    std::chrono::steady_clock::time_point startedAt_ = std::chrono::steady_clock::now();
};

}  // namespace simpleplc::web
