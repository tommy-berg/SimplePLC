#include "web/fleet.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <map>
#include <regex>

#include <httplib.h>

#include "app/config.h"
#include "app/log.h"
#include "app/version.h"
#include "runtime/program_store.h"
#include "web/api_service.h"

namespace simpleplc::web {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPollInterval = std::chrono::milliseconds(1000);
/// Devices started together with the fleet (docker compose up) get this long before a warning.
constexpr auto kStartGrace = std::chrono::seconds(5);
/// A poll: a device that takes longer (frozen, or answering a byte at a time) is offline.
constexpr auto kPollDeadline = std::chrono::milliseconds(3000);
/// A request through the fleet: a program load may run for the device's 5 s watchdog.
constexpr auto kRequestDeadline = std::chrono::milliseconds(10000);
/// Long requests (compiling, saving and loading programs): well past the device's own 5 s load watchdog plus
/// compiling and answering, so the caller gets the device's answer, not "did not answer".
constexpr auto kLongDeadline = std::chrono::milliseconds(30000);
/// A device's live stream sends its status every second: silent this long, it is frozen.
constexpr auto kStreamSilence = std::chrono::milliseconds(5000);
/// After a device refuses the token the next try waits 2, 4, 8 ... s, at most this long, so the fleet does not
/// trip the device's lockout of addresses that send wrong tokens.
constexpr int kMaxRefusalWaitS = 60;
/// Log records copied from a device the fleet reaches for the first time: its recent history.
constexpr int kHistoryRecords = 500;
/// Log records asked for per poll: everything a device keeps (its log holds the last 2000; the API's maximum).
constexpr int kLogPage = 2000;
/// When a page of records is too large to accept: the newest this many.
constexpr int kSmallLogPage = 100;

/// "http://tank:8080" -> "tank"
std::string hostOf(const std::string& url) {
    std::string rest = url.substr(url.find("://") + 3);
    if (rest.starts_with("[")) return rest.substr(1, rest.find(']') - 1);
    return rest.substr(0, rest.rfind(':'));
}

int portOf(const std::string& url) { return std::stoi(url.substr(url.rfind(':') + 1)); }

/// A client for one device. Each request's deadline is kept by the fleet's watchdog, which also ends
/// requests when the fleet stops: no read may wait longer than the deadline either.
std::unique_ptr<httplib::Client> connect(const std::string& url, std::chrono::milliseconds readTimeout) {
    auto client = std::make_unique<httplib::Client>(url);
    client->set_connection_timeout(std::chrono::milliseconds(800));
    client->set_read_timeout(readTimeout);
    client->set_write_timeout(std::chrono::seconds(5));
    client->set_path_encode(false);  // targets come from the panel, already encoded
    return client;
}

bool flag(const json& j, const char* key) { return j.is_object() && j.contains(key) && j[key].is_boolean() && j[key].get<bool>(); }

uint64_t unsignedOr(const json& j, const char* key, uint64_t fallback) {
    return j.is_object() && j.contains(key) && j[key].is_number_unsigned() ? j[key].get<uint64_t>() : fallback;
}

std::string inSeconds(std::chrono::milliseconds ms) { return std::format("{} s", ms.count() / 1000); }

/// A device's log record for the fleet's log: filed under the device's own PLC, whatever it claims.
struct CopiedRecord {
    log::Level level;
    std::string component;
    std::string message;
    std::string event;               ///< an event of a known kind, with its author
    std::optional<log::Author> by;
};

std::optional<CopiedRecord> copyRecord(const json& r) {
    if (!r.is_object() || !r.contains("message") || !r["message"].is_string()) return std::nullopt;
    log::Level level = log::Level::Info;
    if (r.contains("level") && r["level"].is_string()) level = log::parseLevel(r["level"].get<std::string>()).value_or(log::Level::Info);
    std::string component = "device";
    if (r.contains("component") && r["component"].is_string()) {
        std::string c = r["component"].get<std::string>();
        bool plain = !c.empty() && c.size() <= 24 && std::all_of(c.begin(), c.end(), [](unsigned char ch) {
            return std::islower(ch) || std::isdigit(ch) || ch == '_' || ch == '-';
        });
        if (plain && c != "fleet") component = c;  // the fleet's own records stay the fleet's
    }
    CopiedRecord out{level, component, r["message"].get<std::string>(), {}, std::nullopt};
    // An event keeps its kind and author, so the fleet's live log says who changed what on the device
    static const std::vector<std::string> kinds = {"program", "force", "release", "write", "config", "mode", "link", "access"};
    if (r.contains("event") && r["event"].is_string() && std::find(kinds.begin(), kinds.end(), r["event"].get<std::string>()) != kinds.end() &&
        r.contains("by") && r["by"].is_object() && r["by"].contains("kind") && r["by"]["kind"].is_string() && r["by"].contains("name") &&
        r["by"]["name"].is_string()) {
        std::string kind = r["by"]["kind"].get<std::string>();
        if (kind == "user" || kind == "token" || kind == "system") {
            out.event = r["event"].get<std::string>();
            out.by = log::Author{kind, r["by"]["name"].get<std::string>()};
        }
    }
    return out;
}

/// Writes the store with permissions for the owner only, before any secret is in it, then replaces the old one.
std::expected<void, std::string> writePrivateFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::path temp = path;
    temp += ".tmp";
    std::error_code ec;
    std::filesystem::remove(temp, ec);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected(std::format("cannot write {}", temp.string()));
        std::filesystem::permissions(temp, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
        if (ec) return std::unexpected(std::format("cannot make {} private: {}", temp.string(), ec.message()));
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) return std::unexpected(std::format("cannot write {}", temp.string()));
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(std::format("cannot replace {}: {}", path.string(), ec.message()));
    }
    return {};
}

/// What is wrong with a device's answer to GET /api/v1/plcs, if anything. The fleet passes the summary on
/// (panel, list_plcs, MCP resources), so every field it or they read must have its type.
std::optional<std::string> summaryProblem(const json& list) {
    if (!list.is_array() || list.size() != 1 || !list[0].is_object()) return "its answer is not one PLC";
    const json& p = list[0];
    if (!p.contains("id") || !p["id"].is_string() || !isValidPlcId(p["id"].get<std::string>())) return "its PLC has no valid id";
    auto wrong = [&](const char* key) { return std::format("its PLC's '{}' is {}", key, p[key].type_name()); };
    if (!p.contains("name") || !p["name"].is_string()) return p.contains("name") ? wrong("name") : "its PLC has no name";
    for (const char* key : {"description", "state"}) {
        if (p.contains(key) && !p[key].is_string()) return wrong(key);
    }
    if (p.contains("running") && !p["running"].is_boolean()) return wrong("running");
    for (const char* key : {"modbus", "opcua"}) {
        if (p.contains(key) && !p[key].is_object()) return wrong(key);
    }
    for (const char* key : {"tasks", "warnings"}) {
        if (p.contains(key) && !p[key].is_array()) return wrong(key);
    }
    if (p.contains("warnings")) {
        for (const json& w : p["warnings"]) {
            if (!w.is_string()) return "its PLC's warnings are not all text";
        }
    }
    return std::nullopt;
}

/// Why a device refuses the fleet, for its card, the log and requests that cannot go to it.
std::string refusalMessage(int status, int lockoutS) {
    if (status == 429) {
        return std::format("refuses the fleet's token for {} s: too many wrong tokens came from the fleet's address "
                           "(the fleet tries again then)", lockoutS);
    }
    if (status == 403) {
        return "refuses the fleet's token (HTTP 403): give the fleet the device's admin token (its SIMPLEPLC_TOKEN) "
               "as SIMPLEPLC_DEVICE_TOKEN";
    }
    return "refuses the fleet's token: start the device with SIMPLEPLC_TOKEN set to the fleet's device token "
           "(SIMPLEPLC_DEVICE_TOKEN); a device started without SIMPLEPLC_TOKEN accepts no token";
}

/// How long a device that answered 429 locks the fleet's address out: Retry-After, else "try again in N seconds".
int lockoutSeconds(const std::string& retryAfter, const std::string& body) {
    auto number = [](const std::string& text) -> std::optional<int> {
        if (text.empty() || text.size() > 6 || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); })) {
            return std::nullopt;
        }
        return std::stoi(text);
    };
    if (auto n = number(retryAfter)) return std::clamp(*n, 1, 3600);
    std::smatch m;
    std::string message = body.substr(0, 2048);
    if (std::regex_search(message, m, std::regex(R"(in (\d{1,6}) seconds?)"))) return std::clamp(std::stoi(m[1].str()), 1, 3600);
    return 30;  // a SimplePLC device's lockout
}

std::string pathOf(const std::string& target) { return target.substr(0, target.find('?')); }

/// "/api/v1/plcs/tank/programs/main" -> "/programs/main"; "" for the PLC itself; nullopt for other paths.
std::optional<std::string> plcRoute(const std::string& target) {
    constexpr std::string_view kPrefix = "/api/v1/plcs/";
    std::string path = pathOf(target);
    if (!path.starts_with(kPrefix) || path.size() == kPrefix.size()) return std::nullopt;
    size_t slash = path.find('/', kPrefix.size());
    return slash == std::string::npos ? std::string() : path.substr(slash);
}

std::string toolOf(const json& rpc) {
    if (!rpc.is_object() || !rpc.contains("method") || rpc["method"] != "tools/call") return {};
    if (!rpc.contains("params") || !rpc["params"].is_object()) return {};
    const json& params = rpc["params"];
    return params.contains("name") && params["name"].is_string() ? params["name"].get<std::string>() : std::string();
}

}  // namespace

/// One request's outcome, before it becomes a Reply or a Failure.
struct Fleet::Fetched {
    std::optional<int> status;  ///< nullopt: no complete answer
    httplib::Error error = httplib::Error::Success;
    Ended ended = Ended::No;    ///< why the watchdog ended it
    std::string body;
    std::string contentType;
    std::string retryAfter;
    std::string etag;
    bool tooLarge = false;
};

std::expected<std::string, std::string> Fleet::parseDeviceUrl(std::string_view text) {
    std::string rest(text);
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    if (size_t at = rest.find("://"); at != std::string::npos) {
        std::string scheme = rest.substr(0, at);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (scheme == "https") {
            return std::unexpected(std::format("{}: https is not supported; reach devices over http on a trusted network", text));
        }
        if (scheme != "http") return std::unexpected(std::format("{}: only http:// addresses", text));
        rest = rest.substr(at + 3);
    }
    if (rest.find_first_of("/?#@ ") != std::string::npos) {
        return std::unexpected(std::format("{}: give a device as host or host:port, without a path", text));
    }
    std::string host = rest;
    std::string port;
    if (rest.starts_with("[")) {  // [::1]:8080
        size_t close = rest.find(']');
        if (close == std::string::npos) return std::unexpected(std::format("{}: missing ]", text));
        host = rest.substr(0, close + 1);
        std::string after = rest.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') return std::unexpected(std::format("{}: expected :port after ]", text));
            port = after.substr(1);
        }
    } else if (size_t colon = rest.rfind(':'); colon != std::string::npos) {
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
        if (host.find(':') != std::string::npos) return std::unexpected(std::format("{}: write IPv6 addresses in brackets", text));
    }
    if (host.empty() || host == "[]") return std::unexpected(std::format("{}: no host", text));
    int number = 8080;
    if (!port.empty()) {
        bool digits = port.size() <= 5 && std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c); });
        number = digits ? std::stoi(port) : 0;
        if (number < 1 || number > 65535) return std::unexpected(std::format("{}: the port must be 1-65535", text));
    }
    return std::format("http://{}:{}", host, number);
}

std::expected<std::vector<Fleet::DeviceEntry>, std::string> Fleet::loadDevices(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return std::vector<DeviceEntry>{};
    auto text = runtime::readFile(file);
    if (!text) return std::unexpected(text.error());
    json list = parseJson(*text);
    if (!list.is_array()) return std::unexpected(std::format("{} is not a list of devices ([{{\"url\", \"token\"}}])", file.string()));
    std::vector<DeviceEntry> out;
    for (const json& d : list) {
        if (!d.is_object() || !d.contains("url") || !d["url"].is_string() || !d.contains("token") || !d["token"].is_string()) {
            return std::unexpected(std::format("{}: every device needs \"url\" and \"token\"", file.string()));
        }
        auto url = parseDeviceUrl(d["url"].get<std::string>());
        if (!url) return std::unexpected(std::format("{}: {}", file.string(), url.error()));
        out.push_back({*url, d["token"].get<std::string>(), false});
    }
    return out;
}

Fleet::Fleet(std::vector<DeviceEntry> entries, std::filesystem::path store, std::string name)
    : store_(std::move(store)), name_(std::move(name)) {
    for (DeviceEntry& entry : entries) {
        bool known = std::any_of(devices_.begin(), devices_.end(), [&](const Device& d) { return d.url == entry.url; });
        if (known) continue;  // the first wins: a flag before the store
        Device d;
        d.url = std::move(entry.url);
        d.token = std::move(entry.token);
        d.flag = entry.flag;
        devices_.push_back(std::move(d));
        slots_.emplace_back();
    }
}

Fleet::~Fleet() { stop(); }

void Fleet::start() {
    if (started_.exchange(true)) return;
    watchdog_ = std::thread([this] { watchdogLoop(); });
    std::lock_guard pollers(pollersMutex_);
    size_t count = 0;
    {
        std::lock_guard lock(mutex_);
        count = devices_.size();
    }
    for (size_t i = 0; i < count; i++) startPollerLocked(i);
}

void Fleet::startPollerLocked(size_t index) {
    if (!started_ || stopping_) return;
    pollers_.emplace_back([this, index] { pollLoop(index); });
}

void Fleet::stop() {
    {
        // Under the lock of the request list: no request starts after this, and the watchdog ends the running ones
        std::lock_guard lock(callsMutex_);
        stopping_ = true;
    }
    std::vector<std::thread> pollers;
    {
        std::lock_guard lock(pollersMutex_);  // no poller starts after this: stopping_ is set
        pollers.swap(pollers_);
    }
    for (auto& t : pollers) {
        if (t.joinable()) t.join();
    }
    if (watchdog_.joinable()) watchdog_.join();
}

void Fleet::watchdogLoop() {
    for (;;) {
        std::vector<bool> reachable;
        {
            std::lock_guard lock(mutex_);
            for (const Device& d : devices_) reachable.push_back(d.online && !d.removed);
        }
        {
            std::lock_guard lock(callsMutex_);
            if (stopping_ && calls_.empty()) return;
            auto now = Clock::now();
            for (auto& [id, call] : calls_) {
                // A device added after `reachable` was taken is not offline (yet)
                bool offline = call.device < reachable.size() && !reachable[call.device];
                Ended why = stopping_ ? Ended::Stopping
                            : now >= call.deadline ? Ended::Deadline
                            : call.stream && offline ? Ended::Offline
                                                     : Ended::No;
                if (why == Ended::No) continue;
                if (call.ended == Ended::No) call.ended = why;
                // Thread-safe in httplib: shuts the socket down under the request's feet. Repeated while the call
                // lasts, in case it had not started sending yet.
                call.client->stop();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

Fleet::Fetched Fleet::fetch(httplib::Client& client, size_t index, const std::string& token, const std::string& method,
                            const std::string& target, const std::string& body, const std::string& contentType,
                            std::chrono::milliseconds deadline, size_t maxSize, const std::function<bool(std::string_view)>* sink,
                            const std::map<std::string, std::string>* headers) {
    Fetched out;
    uint64_t id = 0;
    {
        std::lock_guard lock(callsMutex_);
        if (stopping_) {
            out.ended = Ended::Stopping;
            return out;
        }
        id = nextCall_++;
        auto due = sink ? Clock::time_point::max() : Clock::now() + deadline;
        calls_.emplace(id, Call{&client, due, index, sink != nullptr, Ended::No});
    }

    httplib::Request req;
    req.method = method;
    req.path = target;
    if (!token.empty()) req.set_header("Authorization", "Bearer " + token);
    if (headers) {
        for (const auto& [name, value] : *headers) req.set_header(name, value);
    }
    if (!body.empty()) {
        req.body = body;
        req.set_header("Content-Type", contentType.empty() ? "application/json" : contentType);
    }
    req.response_handler = [&](const httplib::Response& r) {
        out.status = r.status;
        // An error answer is not an event stream: it is not passed on
        return !sink || r.status == 200;
    };
    req.content_receiver = [&](const char* data, size_t size, uint64_t, uint64_t) {
        if (sink) return (*sink)(std::string_view(data, size));
        if (out.body.size() + size > maxSize) {
            out.tooLarge = true;
            return false;
        }
        out.body.append(data, size);
        return true;
    };
    auto res = client.send(req);
    {
        std::lock_guard lock(callsMutex_);
        out.ended = calls_.at(id).ended;
        calls_.erase(id);
    }
    if (res) {
        out.status = res->status;
        out.contentType = res->get_header_value("Content-Type");
        out.retryAfter = res->get_header_value("Retry-After");
        out.etag = res->get_header_value("ETag");
    } else {
        out.error = res.error();
        if (out.status && *out.status != 200 && sink) return out;  // a refused stream: the status is the answer
        if (!(sink && res.error() == httplib::Error::Canceled && out.ended == Ended::No)) out.status.reset();
    }
    return out;
}

void Fleet::pollLoop(size_t index) {
    auto removed = [&] {
        std::lock_guard lock(mutex_);
        return devices_[index].removed;
    };
    while (!stopping_ && !removed()) {
        auto next = Clock::now() + kPollInterval;
        poll(index);
        while (!stopping_ && Clock::now() < next) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void Fleet::noteRefusalLocked(Device& d, int status, int lockoutS) {
    d.refusals = std::min(d.refusals + 1, 30);
    int wait = d.refusals >= 6 ? kMaxRefusalWaitS : std::min(kMaxRefusalWaitS, 1 << d.refusals);
    if (status == 429) wait = std::max(wait, lockoutS + 1);
    d.retryAt = Clock::now() + std::chrono::seconds(wait);
}

void Fleet::poll(size_t index) {
    std::shared_ptr<std::mutex> onePoll;
    {
        std::lock_guard lock(mutex_);
        onePoll = devices_[index].pollLock;
    }
    std::lock_guard pollLock(*onePoll);
    if (stopping_) return;
    std::string url;
    std::string token;
    uint64_t seq = 0;
    bool logsStarted = false;
    int64_t deviceStarted = 0;
    {
        std::lock_guard lock(mutex_);
        const Device& d = devices_[index];
        // A device that refused the token is asked again later and later: wrong tokens would get its address locked out
        if (d.removed || Clock::now() < d.retryAt) return;
        url = d.url;
        token = d.token;
        seq = d.logSeq;
        logsStarted = d.logsStarted;
        deviceStarted = d.deviceStartedMs;
    }

    std::string error;
    int refusedStatus = 0;
    int lockoutS = 0;
    json plc;
    std::vector<json> records;
    uint64_t lost = 0;   // records the device no longer had when the fleet asked
    bool history = false;
    // Whatever a device answers, the poller must not die: anything unexpected makes it "not SimplePLC"
    try {
        auto client = connect(url, kPollDeadline);
        Fetched f = fetch(*client, index, token, "GET", "/api/v1/plcs", "", "", kPollDeadline, kMaxSummary);
        if (f.ended == Ended::Stopping) return;
        if (f.tooLarge) {
            error = std::format("is not a SimplePLC device (its PLC summary is larger than {} kB)", kMaxSummary >> 10);
        } else if (!f.status) {
            error = f.ended == Ended::Deadline ? std::format("not reachable (no answer within {})", inSeconds(kPollDeadline))
                                                         : std::format("not reachable ({})", httplib::to_string(f.error));
        } else if (*f.status == 401 || *f.status == 403 || *f.status == 429) {
            refusedStatus = *f.status;
            lockoutS = *f.status == 429 ? lockoutSeconds(f.retryAfter, f.body) : 0;
            error = refusalMessage(refusedStatus, lockoutS);
        } else if (*f.status != 200) {
            error = std::format("answered HTTP {}", *f.status);
        } else {
            json list = parseJson(f.body);
            if (auto problem = summaryProblem(list)) error = std::format("is not a SimplePLC device ({})", *problem);
            else plc = list[0];
        }

        // The device's log records, so the fleet's log shows the whole network
        if (error.empty()) {
            auto getLogs = [&](uint64_t since, int limit) -> std::optional<json> {
                Fetched logs = fetch(*client, index, token, "GET", std::format("/api/v1/logs?since={}&limit={}", since, limit), "", "",
                                     kPollDeadline, kMaxLogAnswer);
                if (logs.tooLarge && limit > kSmallLogPage) {
                    logs = fetch(*client, index, token, "GET", std::format("/api/v1/logs?since={}&limit={}", since, kSmallLogPage), "",
                                 "", kPollDeadline, kMaxLogAnswer);
                }
                if (!logs.status || *logs.status != 200 || logs.tooLarge) return std::nullopt;
                json j = parseJson(logs.body);
                return j.is_object() ? std::optional(j) : std::nullopt;
            };
            // First contact (the device started after the fleet, or the fleet was down): its recent history.
            // Then every record after the newest one copied: all the device keeps, so a burst between two polls arrives whole.
            uint64_t from = logsStarted ? seq : 0;
            auto j = getLogs(from, logsStarted ? kLogPage : kHistoryRecords);
            if (j) {
                int64_t started = (*j).contains("started_ms") && (*j)["started_ms"].is_number_integer()
                                      ? (*j)["started_ms"].get<int64_t>() : deviceStarted;
                uint64_t last = unsignedOr(*j, "last_seq", seq);
                if (logsStarted && (started != deviceStarted || last < seq)) {
                    // The device restarted and numbers its records from 1 again: all of them are new
                    from = 0;
                    j = getLogs(0, kLogPage);
                }
                if (j) {
                    history = !logsStarted;
                    uint64_t newest = from;
                    if ((*j).contains("records") && (*j)["records"].is_array()) {
                        for (const json& r : (*j)["records"]) {
                            records.push_back(r);
                            newest = std::max(newest, unsignedOr(r, "seq", 0));
                        }
                    }
                    // Seqs follow each other: a first record past the next one means the device dropped some first
                    uint64_t first = records.empty() ? 0 : unsignedOr(records.front(), "seq", 0);
                    if (!history && first > from + 1) lost = first - from - 1;
                    seq = newest;
                    deviceStarted = started;
                    logsStarted = true;
                }
            }
        }
    } catch (const std::exception& e) {
        error = std::format("is not a SimplePLC device ({})", e.what());
        refusedStatus = 0;
        plc = json();
        records.clear();
        lost = 0;
    }

    bool cameOnline = false;
    bool wentOffline = false;
    std::string plcId;
    std::string sameAs;
    {
        std::lock_guard lock(mutex_);
        Device& d = devices_[index];
        if (d.removed) return;  // removed while it was asked: nothing of it is news any more
        bool refused = refusedStatus != 0;
        bool graceOver = Clock::now() - startedAt_ > kStartGrace;
        cameOnline = error.empty() && !d.online;
        wentOffline = !error.empty() && (d.online || d.refused != refused || (!d.warned && graceOver));
        if (wentOffline) d.warned = true;
        if (error.empty()) d.warned = false;
        if (refused) noteRefusalLocked(d, refusedStatus, lockoutS);
        else if (error.empty()) d.refusals = 0;
        d.online = error.empty();
        d.refused = refused;
        d.error = error;
        if (d.online) {
            d.plc = plc;
            d.everReached = true;
            d.logSeq = seq;
            d.logsStarted = logsStarted;
            d.deviceStartedMs = deviceStarted;
            sameAs = sameDeviceLocked(index);
        }
        plcId = d.everReached ? idOf(d) : "";
        if (!d.sameAs.empty()) {
            // Listed twice: the address reached first copies the records and speaks for the device
            records.clear();
            lost = 0;
            cameOnline = false;
            wentOffline = false;
        }
    }
    if (!sameAs.empty()) {
        log::write(log::Level::Warn, "fleet", plcId,
                   std::format("{} is the same device as {} (listed twice): the fleet uses {}; remove one --device", url, sameAs, sameAs));
    }
    if (cameOnline) log::write(log::Level::Info, "fleet", plcId, std::format("{} is online", url));
    if (wentOffline) log::write(log::Level::Warn, "fleet", plcId, std::format("{} {}", url, error));
    if (lost > 0) {
        log::write(log::Level::Warn, "fleet", plcId,
                   std::format("{}: {} of its log records could not be copied (the device no longer had them, "
                               "or they were too large)", url, lost));
    }
    if (history && !records.empty()) {
        log::write(log::Level::Info, "fleet", plcId, std::format("{}: its last {} log records follow", url, records.size()));
    }
    for (const json& r : records) {
        auto copy = copyRecord(r);
        if (copy && copy->by) log::event(copy->level, copy->component, plcId, copy->event, *copy->by, copy->message);
        else if (copy) log::write(copy->level, copy->component, plcId, copy->message);
    }
}

std::string Fleet::sameDeviceLocked(size_t index) {
    // The same device under two addresses (127.0.0.1 and localhost): the same PLC id and the same process start.
    // The address reached first stays; the other is reported once and copies no log records.
    Device& d = devices_[index];
    auto same = [&](const Device& e) {
        return e.everReached && e.deviceStartedMs != 0 && e.deviceStartedMs == d.deviceStartedMs && idOf(e) == idOf(d);
    };
    if (!d.sameAs.empty()) {
        // A restart shows on one address a moment before the other: only a lasting difference is another device
        int primary = indexOfLocked(d.sameAs);
        d.sameAsMismatches = primary >= 0 && same(devices_[static_cast<size_t>(primary)]) ? 0 : d.sameAsMismatches + 1;
        if (d.sameAsMismatches >= 3) {
            d.sameAs.clear();
            d.sameAsMismatches = 0;
        }
        return {};
    }
    bool othersPointHere = std::any_of(devices_.begin(), devices_.end(), [&](const Device& e) { return !e.removed && e.sameAs == d.url; });
    if (othersPointHere) return {};
    for (size_t j = 0; j < devices_.size(); j++) {
        if (j != index && !devices_[j].removed && devices_[j].sameAs.empty() && same(devices_[j])) {
            d.sameAs = devices_[j].url;
            return d.sameAs;
        }
    }
    return {};
}

void Fleet::refresh(const std::string& url) {
    int index = indexOf(url);
    if (index >= 0) poll(static_cast<size_t>(index));
}

int Fleet::indexOf(const std::string& url) const {
    std::lock_guard lock(mutex_);
    return indexOfLocked(url);
}

int Fleet::indexOfLocked(const std::string& url) const {
    for (size_t i = 0; i < devices_.size(); i++) {
        if (!devices_[i].removed && devices_[i].url == url) return static_cast<int>(i);
    }
    return -1;
}

std::string Fleet::idOf(const Device& d) {
    return d.everReached ? d.plc["id"].get<std::string>() : offlinePlc(d)["id"].get<std::string>();
}

json Fleet::offlinePlc(const Device& d) {
    // Named after the host (and port, unless 8080) until the device answers: "10.0.0.5:8082" -> "plc-10-0-0-5-8082"
    std::string host = hostOf(d.url);
    bool address = host.find_first_not_of("0123456789.:") == std::string::npos;
    std::string id = plcIdFromHostname(address ? std::regex_replace(host, std::regex("[.:]"), "-") : host);
    if (int port = portOf(d.url); port != 8080) id += std::format("-{}", port);
    if (id.size() > 32) id.resize(32);
    // Nobody has reached the device: what the fleet does not know is null, not a value an agent would act on
    // (a "stopped" PLC with no tags and no Modbus). The lists are empty, the counters zero: nothing was seen
    return {{"id", id},
            {"name", host},
            {"description", ""},
            {"running", nullptr},
            {"image_size", nullptr},
            {"tag_count", nullptr},
            {"modbus", {{"enabled", nullptr}, {"listening", nullptr}, {"listen", ""}, {"port", nullptr}, {"url", nullptr},
                        {"connections", 0}, {"requests", 0}, {"last_request_ago_ms", nullptr}, {"clients", json::array()},
                        {"links", json::array()}}},
            {"opcua", {{"enabled", nullptr}, {"listening", nullptr}, {"listen", ""}, {"port", nullptr}, {"sessions", 0},
                       {"url", nullptr}}},
            {"tasks", json::array()},
            {"warnings", json::array()}};
}

std::vector<std::pair<std::string, std::vector<std::string>>> Fleet::duplicatesLocked() const {
    std::map<std::string, std::vector<std::string>> byId;
    for (const Device& d : devices_) {
        if (!d.removed && d.sameAs.empty()) byId[idOf(d)].push_back(d.url);
    }
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    for (auto& [id, urls] : byId) {
        if (urls.size() > 1) out.emplace_back(id, urls);
    }
    return out;
}

namespace {
std::string duplicateWarning(const std::string& id, const std::vector<std::string>& urls) {
    std::string list;
    for (const auto& u : urls) list += (list.empty() ? "" : ", ") + u;
    return std::format("PLC id '{}' is used by {} devices ({}): the fleet cannot tell them apart; rename one on its own panel",
                       id, urls.size(), list);
}

std::string sameDeviceWarning(const std::string& alias, const std::string& url) {
    return std::format("{} is the same device as {} (listed twice): the fleet uses {}; remove one --device", alias, url, url);
}

const char* stateName(bool online, bool refused) { return online ? "online" : refused ? "token_refused" : "offline"; }
}  // namespace

json Fleet::plcs() const {
    std::lock_guard lock(mutex_);
    auto duplicates = duplicatesLocked();
    json out = json::array();
    for (const Device& d : devices_) {
        if (d.removed || !d.sameAs.empty()) continue;  // shown once, under the address reached first
        json plc = d.everReached ? d.plc : offlinePlc(d);
        plc["online"] = d.online;
        plc["device"] = {{"url", d.url},
                         {"online", d.online},
                         {"state", stateName(d.online, d.refused)},
                         {"error", d.error.empty() ? json(nullptr) : json(d.error)}};
        if (!plc.contains("warnings") || !plc["warnings"].is_array()) plc["warnings"] = json::array();
        if (!d.online) {
            plc["running"] = nullptr;  // unknown while the device does not answer (the rest is the last answer)
            plc["state"] = d.refused ? "token_refused" : "offline";
            plc["warnings"] = json::array({std::format("{} {}", d.url, d.error.empty() ? "has not answered yet" : d.error)});
        }
        for (const auto& [id, urls] : duplicates) {
            if (id == idOf(d)) plc["warnings"].push_back(duplicateWarning(id, urls));
        }
        for (const Device& alias : devices_) {
            if (!alias.removed && alias.sameAs == d.url) plc["warnings"].push_back(sameDeviceWarning(alias.url, d.url));
        }
        out.push_back(plc);
    }
    return out;
}

json Fleet::status() const {
    std::lock_guard lock(mutex_);
    json devices = json::array();
    json warnings = json::array();
    int running = 0;
    int plcCount = 0;
    for (const Device& d : devices_) {
        if (d.removed) continue;
        json device = {{"url", d.url},
                       {"online", d.online},
                       {"state", stateName(d.online, d.refused)},
                       {"plc", d.everReached ? json(idOf(d)) : json(nullptr)},
                       {"error", d.error.empty() ? json(nullptr) : json(d.error)}};
        if (!d.sameAs.empty()) {
            device["same_as"] = d.sameAs;
            warnings.push_back(sameDeviceWarning(d.url, d.sameAs));
        } else {
            plcCount++;
            if (!d.online) warnings.push_back(std::format("{} {}", d.url, d.error.empty() ? "has not answered yet" : d.error));
            if (d.online && flag(d.plc, "running")) running++;
        }
        devices.push_back(device);
    }
    for (const auto& [id, urls] : duplicatesLocked()) warnings.push_back(duplicateWarning(id, urls));
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - startedAt_).count();
    return {{"version", kVersion}, {"mode", "fleet"},  {"uptime_s", uptime},     {"devices", devices},
            {"plcs", plcCount},    {"running", running}, {"warnings", warnings}};
}

std::expected<std::string, Fleet::Failure> Fleet::deviceFor(const std::string& plcId) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> urls;
    std::vector<std::string> ids;
    for (const Device& d : devices_) {
        if (d.removed || !d.sameAs.empty()) continue;
        if (idOf(d) == plcId) urls.push_back(d.url);
        if (ids.size() < 10) ids.push_back(idOf(d));
    }
    if (urls.empty()) {
        std::string known;
        for (const std::string& id : ids) known += (known.empty() ? "" : ", ") + id;
        return std::unexpected(Failure{404, "not_found", ids.empty() ? std::format("no PLC with id '{}': this fleet has no devices yet", plcId)
                                                                     : std::format("no PLC with id '{}' in this fleet; its PLCs: {}", plcId, known)});
    }
    if (urls.size() > 1) return std::unexpected(Failure{409, "ambiguous", duplicateWarning(plcId, urls)});
    return urls.front();
}

std::optional<std::string> Fleet::otherDeviceWith(const std::string& plcId, const std::string& url) const {
    std::lock_guard lock(mutex_);
    for (const Device& d : devices_) {
        if (!d.removed && d.url != url && d.sameAs.empty() && idOf(d) == plcId) return d.url;
    }
    return std::nullopt;
}

std::optional<Fleet::Failure> Fleet::unavailableLocked(size_t index) const {
    const Device& d = devices_[index];
    if (stopping_) return Failure{503, "fleet_stopping", "the fleet is stopping"};
    if (d.removed) return Failure{404, "not_found", std::format("{} was removed from this fleet", d.url)};
    if (d.refused) return Failure{503, "device_refused", std::format("{} {}", d.url, d.error)};
    // A device the poller cannot reach gets no requests: they would only wait for timeouts
    if (!d.online) {
        return Failure{503, "device_offline", std::format("{} is offline: {}", d.url, d.error.empty() ? "it has not answered yet" : d.error)};
    }
    return std::nullopt;
}

std::expected<void, Fleet::Failure> Fleet::available(const std::string& url) const {
    std::lock_guard lock(mutex_);
    int index = indexOfLocked(url);
    if (index < 0) return std::unexpected(Failure{404, "not_found", std::format("{} is not in this fleet", url)});
    if (auto failure = unavailableLocked(static_cast<size_t>(index))) return std::unexpected(*failure);
    return {};
}

Fleet::Kind Fleet::kindOf(const std::string& method, const std::string& target, const json& rpc) {
    if (pathOf(target) == "/mcp") {
        static const std::vector<std::string> control = {"run_plc", "stop_plc", "write_values", "write_area",
                                                         "force_values", "release_forces", "set_variables"};
        static const std::vector<std::string> slow = {"setup_plc", "set_program", "check_program", "get_compiled", "probe_device"};
        std::string tool = toolOf(rpc);
        if (std::find(control.begin(), control.end(), tool) != control.end()) return Kind::Control;
        if (std::find(slow.begin(), slow.end(), tool) != slow.end()) return Kind::Long;
        return Kind::Normal;
    }
    auto route = plcRoute(target);
    if (!route) return Kind::Normal;
    const std::string& r = *route;
    if (method == "POST" && (r == "/run" || r == "/stop" || r == "/restart")) return Kind::Control;
    if (method == "PUT" && (r == "/values" || r == "/variables" || r.starts_with("/areas/"))) return Kind::Control;
    if ((method == "POST" || method == "DELETE") && r == "/forces") return Kind::Control;
    if (method == "POST" && (r == "/setup" || r == "/probe")) return Kind::Long;  // a probe waits for another device
    if (r.starts_with("/programs/")) {
        bool program = r.find('/', 10) == std::string::npos;  // /programs/<name>
        if (method == "PUT" && program) return Kind::Long;
        if (method == "POST" && (r.ends_with("/check") || r.ends_with("/restore"))) return Kind::Long;
        if (method == "GET" && r.ends_with("/compiled")) return Kind::Long;
    }
    return Kind::Normal;
}

std::optional<std::string> Fleet::renameTo(const std::string& method, const std::string& target, const json& body,
                                           const json& rpc) {
    const json* changes = nullptr;
    if (pathOf(target) == "/mcp") {
        if (toolOf(rpc) == "update_plc" && rpc["params"].contains("arguments") && rpc["params"]["arguments"].is_object() &&
            rpc["params"]["arguments"].contains("changes")) {
            changes = &rpc["params"]["arguments"]["changes"];
        }
    } else if (auto route = plcRoute(target); route && ((method == "PATCH" && route->empty()) || (method == "POST" && *route == "/setup"))) {
        changes = &body;
    }
    if (!changes || !changes->is_object() || !changes->contains("id") || !(*changes)["id"].is_string()) return std::nullopt;
    return (*changes)["id"].get<std::string>();
}

std::expected<Fleet::Reply, Fleet::Failure> Fleet::send(const std::string& url, const std::string& method,
                                                        const std::string& target, const std::string& body,
                                                        const std::string& contentType,
                                                        const std::map<std::string, std::string>& headers) {
    int found = indexOf(url);
    if (found < 0) return std::unexpected(Failure{404, "not_found", std::format("{} is not in this fleet", url)});
    auto index = static_cast<size_t>(found);
    json rpc = method == "POST" && pathOf(target) == "/mcp" ? parseJson(body) : json();
    auto route = plcRoute(target);
    bool mayRename = route && ((method == "PATCH" && route->empty()) || (method == "POST" && *route == "/setup"));
    json changes = mayRename ? parseJson(body) : json();
    Kind kind = kindOf(method, target, rpc);

    auto newId = renameTo(method, target, changes, rpc);
    if (!newId) return sendNow(index, method, target, body, contentType, kind, headers);
    // Renames one at a time, each checked against the ids after the one before: two parallel renames to one id
    // would otherwise both pass the check, leaving two devices the fleet cannot tell apart
    std::lock_guard renameLock(renameMutex_);
    std::string current;
    {
        std::lock_guard lock(mutex_);
        current = idOf(devices_[index]);
    }
    if (*newId != current) {
        if (auto other = otherDeviceWith(*newId, url)) {
            return std::unexpected(Failure{409, "conflict", std::format("PLC id '{}' is already used by {} in this fleet", *newId, *other)});
        }
    }
    auto reply = sendNow(index, method, target, body, contentType, kind, headers);
    if (reply && reply->status < 300) poll(index);  // the next rename is checked against the new id
    return reply;
}

std::expected<Fleet::Reply, Fleet::Failure> Fleet::sendNow(size_t index, const std::string& method, const std::string& target,
                                                           const std::string& body, const std::string& contentType, Kind kind,
                                                           const std::map<std::string, std::string>& headers) {
    std::string url;
    std::string token;
    bool controlSlot = false;
    {
        // Places for requests that wait for the device. Control calls have their own, and long calls may take
        // only some of the others: whatever anyone runs, a STOP gets through
        std::lock_guard lock(mutex_);
        url = devices_[index].url;
        token = devices_[index].token;
        if (auto failure = unavailableLocked(index)) return std::unexpected(*failure);
        Slots& s = slots_[index];
        if (kind == Kind::Control && s.control < kControlSlots) {
            s.control++;
            controlSlot = true;
        } else if (s.general >= kMaxInFlight) {
            return std::unexpected(Failure{503, "device_busy", std::format("{} has {} requests waiting already", url, kMaxInFlight)});
        } else if (kind == Kind::Long && s.longCalls >= kMaxLong) {
            return std::unexpected(Failure{503, "device_busy",
                                           std::format("{} is busy with {} long requests (checks, compiles, program loads); "
                                                       "try again when one has ended", url, kMaxLong)});
        } else {
            s.general++;
            if (kind == Kind::Long) s.longCalls++;
        }
    }
    struct Release {
        Fleet& fleet;
        size_t index;
        bool control;
        bool longCall;
        ~Release() {
            std::lock_guard lock(fleet.mutex_);
            Slots& s = fleet.slots_[index];
            if (control) {
                s.control--;
            } else {
                s.general--;
                if (longCall) s.longCalls--;
            }
        }
    } release{*this, index, controlSlot, kind == Kind::Long};

    auto deadline = kind == Kind::Long ? kLongDeadline : kRequestDeadline;
    auto client = connect(url, deadline);
    Fetched f = fetch(*client, index, token, method, target, body, contentType, deadline, kMaxAnswer, nullptr, &headers);
    if (f.ended == Ended::Stopping) return std::unexpected(Failure{503, "fleet_stopping", "the fleet is stopping"});
    if (f.tooLarge) {
        return std::unexpected(Failure{502, "device_bad_answer", std::format("{} answered more than {} MB", url, kMaxAnswer >> 20)});
    }
    if (!f.status) {
        std::string why = f.ended == Ended::Deadline ? std::format("no answer within {}", inSeconds(deadline))
                                                              : httplib::to_string(f.error);
        return std::unexpected(Failure{502, "device_unreachable", std::format("{} did not answer: {}", url, why)});
    }
    // A wrong token (the device restarted with another one) or a lockout: the fleet's problem, not the user's.
    // The poller waits before it tries the token again. (403: the token is valid but not an admin's.)
    if (*f.status == 401 || *f.status == 429) {
        int lockoutS = *f.status == 429 ? lockoutSeconds(f.retryAfter, f.body) : 0;
        std::string message = refusalMessage(*f.status, lockoutS);
        std::string plcId;
        {
            std::lock_guard lock(mutex_);
            Device& d = devices_[index];
            if (!d.refused) noteRefusalLocked(d, *f.status, lockoutS);
            d.online = false;
            d.refused = true;
            d.warned = true;
            d.error = message;
            plcId = idOf(d);
        }
        log::write(log::Level::Warn, "fleet", plcId, std::format("{} {}", url, message));
        return std::unexpected(Failure{502, "device_refused", std::format("{} {}", url, message)});
    }
    return Reply{*f.status, std::move(f.body), std::move(f.contentType), std::move(f.etag)};
}

std::expected<void, Fleet::Failure> Fleet::stream(const std::string& url, const std::string& target,
                                                  const std::function<bool(std::string_view)>& chunk) {
    int found = indexOf(url);
    if (found < 0) return std::unexpected(Failure{404, "not_found", std::format("{} is not in this fleet", url)});
    auto index = static_cast<size_t>(found);
    if (auto ready = available(url); !ready) return std::unexpected(ready.error());
    std::string token;
    {
        std::lock_guard lock(mutex_);
        token = devices_[index].token;
    }
    // No overall deadline: the stream lasts while someone watches. The watchdog ends it when the device goes
    // offline; a frozen device is found by its silence (it sends its status every second).
    auto client = connect(url, kStreamSilence);
    Fetched f = fetch(*client, index, token, "GET", target, "", "", kStreamSilence, 0, &chunk);
    if (f.ended == Ended::Stopping) return {};
    if (f.status && *f.status != 200) {
        return std::unexpected(Failure{*f.status, "device_error", std::format("{} answered HTTP {}", url, *f.status)});
    }
    if (f.ended == Ended::Offline) {
        auto down = available(url);
        return std::unexpected(down ? Failure{503, "device_offline", std::format("{} is offline", url)} : down.error());
    }
    if (!f.status) {
        std::string why = f.error == httplib::Error::Read ? std::format("it sent nothing for {}", inSeconds(kStreamSilence))
                                                          : httplib::to_string(f.error);
        return std::unexpected(Failure{502, "device_unreachable", std::format("{} stopped answering: {}", url, why)});
    }
    return {};
}

// ───────────────────────────── adopting and removing devices ─────────────────────────────

namespace {

/// Adopting or removing a device: each request to it may take this long.
constexpr auto kAdoptDeadline = std::chrono::milliseconds(5000);

/// The error message of a SimplePLC answer ({"error": {"message"}}), else its HTTP status.
std::string answerError(int status, const std::string& body) {
    json j = parseJson(body);
    if (j.is_object() && j.contains("error") && j["error"].is_object() && j["error"].contains("message") && j["error"]["message"].is_string()) {
        std::string message = j["error"]["message"].get<std::string>();
        log::truncate(message, 300);
        return message;
    }
    return std::format("HTTP {}", status);
}

}  // namespace

std::expected<void, std::string> Fleet::saveLocked() const {
    if (store_.empty()) return {};
    json list = json::array();
    for (const Device& d : devices_) {
        if (!d.flag && !d.removed) list.push_back({{"url", d.url}, {"token", d.token}});
    }
    return writePrivateFile(store_, list.dump(2) + "\n");
}

Fleet::Fetched Fleet::request(const std::string& url, const std::string& method, const std::string& target, const std::string& token,
                              const std::string& body, std::chrono::milliseconds deadline) {
    auto client = connect(url, deadline);
    // Not one of the fleet's devices (yet): no place of its own, and no stream, so the watchdog keeps only its deadline
    return fetch(*client, SIZE_MAX, token, method, target, body, body.empty() ? "" : "application/json", deadline, kMaxSummary);
}

json Fleet::devices() const {
    std::lock_guard lock(mutex_);
    json out = json::array();
    for (const Device& d : devices_) {
        if (d.removed) continue;
        json device = {{"url", d.url},
                       {"plc", idOf(d)},
                       {"online", d.online},
                       {"state", stateName(d.online, d.refused)},
                       {"error", d.error.empty() ? json(nullptr) : json(d.error)},
                       {"source", d.flag ? "flag" : "added"}};
        if (!d.sameAs.empty()) device["same_as"] = d.sameAs;
        out.push_back(device);
    }
    return out;
}

std::expected<json, Fleet::Failure> Fleet::addDevice(const json& body) {
    auto refuse = [](int status, std::string code, std::string message) { return std::unexpected(Failure{status, std::move(code), std::move(message)}); };
    if (!body.is_object() || !body.contains("url") || !body["url"].is_string()) {
        return refuse(422, "invalid", "give {\"url\": \"<the device's web address>\", \"code\": \"<its pairing code>\"} (or \"token\": an admin token)");
    }
    bool byCode = body.contains("code");
    if (byCode == body.contains("token")) {
        return refuse(422, "invalid", "give either \"code\" (a pairing code from the device: its panel, or POST /api/v1/pairing on it) or \"token\" (an admin token of the device)");
    }
    const json& secret = byCode ? body["code"] : body["token"];
    if (!secret.is_string() || secret.get<std::string>().empty()) return refuse(422, "invalid", std::format("{} must be text", byCode ? "code" : "token"));
    auto url = parseDeviceUrl(body["url"].get<std::string>());
    if (!url) return refuse(422, "invalid", url.error());

    std::lock_guard adopt(adoptMutex_);  // one at a time: two adoptions of one device must not both pass the checks
    if (stopping_) return refuse(503, "fleet_stopping", "the fleet is stopping");
    if (indexOf(*url) >= 0) return refuse(409, "conflict", std::format("{} is in this fleet already", *url));

    // What it is, before anything is asked of it
    Fetched hello = request(*url, "GET", "/api/v1/hello", "", "", kAdoptDeadline);
    if (hello.ended == Ended::Stopping) return refuse(503, "fleet_stopping", "the fleet is stopping");
    if (!hello.status) {
        return refuse(502, "device_unreachable", std::format("{} did not answer: {}", *url, hello.ended == Ended::Deadline
                                                                                             ? "no answer within " + inSeconds(kAdoptDeadline)
                                                                                             : httplib::to_string(hello.error)));
    }
    json about = parseJson(hello.body);
    if (*hello.status != 200 || !about.is_object() || !about.contains("product") || about["product"] != "SimplePLC") {
        return refuse(422, "not_simpleplc", std::format("{} is not a SimplePLC device that can be adopted (GET /api/v1/hello answered HTTP {})",
                                                        *url, *hello.status));
    }
    if (about.contains("mode") && about["mode"] == "fleet") return refuse(422, "invalid", std::format("{} is a fleet, not a device", *url));
    std::string plcId = about.contains("plc") && about["plc"].is_string() ? about["plc"].get<std::string>() : std::string();
    if (!plcId.empty()) {
        if (auto other = otherDeviceWith(plcId, *url)) {
            return refuse(409, "conflict", std::format("PLC id '{}' is already used by {} in this fleet: rename one of them first", plcId, *other));
        }
    }

    std::string token;
    if (byCode) {
        Fetched paired = request(*url, "POST", "/api/v1/pair", "", json{{"code", secret}, {"fleet", name_}}.dump(), kAdoptDeadline);
        if (!paired.status) return refuse(502, "device_unreachable", std::format("{} did not answer the pairing", *url));
        json answer = parseJson(paired.body);
        if (*paired.status == 200 && answer.is_object() && answer.contains("token") && answer["token"].is_string()) {
            token = answer["token"].get<std::string>();
        } else {
            int status = *paired.status == 429 ? 429 : *paired.status >= 400 && *paired.status < 500 ? 422 : 502;
            std::string code = *paired.status == 429 ? "too_many_attempts" : *paired.status == 403 ? "wrong_pairing_code" : "device_refused";
            return refuse(status, code, std::format("{} refused the pairing: {}", *url, answerError(*paired.status, paired.body)));
        }
    } else {
        token = secret.get<std::string>();
        // Only an administrator may list tokens: an answer proves the fleet can do what it will be asked to
        Fetched check = request(*url, "GET", "/api/v1/tokens", token, "", kAdoptDeadline);
        if (!check.status) return refuse(502, "device_unreachable", std::format("{} did not answer", *url));
        if (*check.status == 403) return refuse(422, "invalid", std::format("{}: that token is not an administrator's; a fleet needs an admin token", *url));
        if (*check.status != 200) return refuse(422, "device_refused", std::format("{} does not accept that token: {}", *url, answerError(*check.status, check.body)));
    }

    size_t index = 0;
    std::expected<void, std::string> saved;
    {
        std::lock_guard lock(mutex_);
        Device d;
        d.url = *url;
        d.token = token;
        devices_.push_back(std::move(d));
        slots_.emplace_back();
        index = devices_.size() - 1;
        saved = saveLocked();
        if (!saved) devices_[index].removed = true;
    }
    if (!saved) {
        if (byCode) (void)request(*url, "DELETE", "/api/v1/pair", token, "", kAdoptDeadline);  // give the token back
        return refuse(500, "internal", saved.error());
    }
    {
        std::lock_guard pollers(pollersMutex_);
        startPollerLocked(index);
    }
    poll(index);  // online in the answer, not a second later
    log::write(log::Level::Info, "fleet", plcId, std::format("{} added to the fleet", *url));
    json out;
    for (const json& d : devices()) {
        if (d["url"] == *url) out = d;
    }
    return out;
}

std::expected<json, Fleet::Failure> Fleet::removeDevice(const std::string& plcId) {
    std::lock_guard adopt(adoptMutex_);
    std::string url;
    std::string token;
    {
        std::lock_guard lock(mutex_);
        std::vector<size_t> matches;
        for (size_t i = 0; i < devices_.size(); i++) {
            if (!devices_[i].removed && devices_[i].sameAs.empty() && idOf(devices_[i]) == plcId) matches.push_back(i);
        }
        if (matches.empty()) {
            return std::unexpected(Failure{404, "not_found", std::format("no device with PLC '{}' in this fleet", plcId)});
        }
        if (matches.size() > 1) {
            std::vector<std::string> urls;
            for (size_t i : matches) urls.push_back(devices_[i].url);
            return std::unexpected(Failure{409, "ambiguous", duplicateWarning(plcId, urls)});
        }
        Device& d = devices_[matches.front()];
        if (d.flag) {
            return std::unexpected(Failure{409, "conflict", std::format("{} was given on the command line (--device or SIMPLEPLC_DEVICES): "
                                                                        "remove it there and restart the fleet", d.url)});
        }
        d.removed = true;
        d.online = false;
        url = d.url;
        token = d.token;
        for (Device& alias : devices_) {
            if (alias.sameAs == url) alias.sameAs.clear();
        }
        if (auto saved = saveLocked(); !saved) {
            d.removed = false;
            return std::unexpected(Failure{500, "internal", saved.error()});
        }
    }
    // The fleet gives its token back, so a removed fleet holds no key to the device
    Fetched f = request(url, "DELETE", "/api/v1/pair", token, "", kAdoptDeadline);
    bool revoked = f.status && *f.status == 200;
    json out = {{"removed", url}, {"plc", plcId}, {"revoked", revoked}};
    if (!revoked) {
        out["note"] = f.status ? std::format("the device did not revoke the fleet's token ({}): revoke it under Users and tokens on the device",
                                             answerError(*f.status, f.body))
                               : std::string("the device is not reachable: revoke the fleet's token under Users and tokens on the device");
    }
    log::write(log::Level::Info, "fleet", plcId, std::format("{} removed from the fleet{}", url, revoked ? "" : " (its token is still valid on it)"));
    return out;
}

}  // namespace simpleplc::web
