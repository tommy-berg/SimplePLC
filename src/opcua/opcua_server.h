#pragma once
#include <atomic>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "app/log.h"
#include "core/process_image.h"
#include "net/socket.h"

struct UA_Server;

namespace simpleplc::opcua {

/// The open62541 version SimplePLC was built with, e.g. "v1.5.9".
const char* libraryVersion();

/**
 * OPC UA server for one process image.
 *
 * Every tag is a variable (ns=1;s=<tag name>) inside a folder named after the
 * PLC. Variables are data sources: reads and writes go straight to the process
 * image, so OPC UA always shows the current value and needs no polling. Coils
 * and holding registers are writable, the input areas read-only.
 *
 * The tag table may change while the server runs; the address space follows
 * within one event-loop iteration. A link's read tag is served with status
 * Uncertain_LastUsableValue while the link's device does not answer.
 *
 * Sessions: at most kMaxSessions, and none outlives its client's last request by
 * more than kMaxSessionTimeoutMs, whatever timeout the client asks for.
 */
class Server {
public:
    static constexpr uint16_t kMaxSessions = 100;
    /// The longest session timeout granted, whatever a client asks for.
    static constexpr double kMaxSessionTimeoutMs = 60000.0;

    struct Options {
        std::string listen = "0.0.0.0";
        uint16_t port = 4840;  ///< 0 picks a free port
        std::string plcId;
        std::string name = "SimplePLC";
        std::string applicationUri = "urn:simpleplc";
        uint16_t maxSessions = kMaxSessions;
        double maxSessionTimeoutMs = kMaxSessionTimeoutMs;
    };

    struct Stats {
        bool listening = false;
        uint16_t port = 0;
        size_t sessions = 0;
        size_t variables = 0;
    };

    Server(ProcessImage& image, log::Scope log);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::expected<uint16_t, net::NetError> start(const Options& options);
    void stop();
    Stats stats() const;

    // For the open62541 callbacks
    struct Variable;
    ProcessImage& image() { return image_; }

private:
    void run();
    void syncTags();
    bool addVariable(const TagDef& tag);
    void removeVariable(const std::string& name);

    ProcessImage& image_;
    log::Scope log_;
    Options options_;
    UA_Server* server_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    uint64_t syncedTagsVersion_ = UINT64_MAX;
    std::map<std::string, std::unique_ptr<Variable>> variables_;  // event-loop thread only
    std::atomic<size_t> variableCount_{0};
    std::atomic<size_t> sessions_{0};
    std::atomic<uint16_t> port_{0};
};

}  // namespace simpleplc::opcua
