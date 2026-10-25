#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/log.h"
#include "core/process_image.h"
#include "modbus/pdu.h"
#include "net/socket.h"

namespace simpleplc::modbus {

/**
 * Modbus TCP server for one process image.
 *
 * One thread runs a poll() loop over non-blocking sockets. Every connection has
 * its own receive and send buffer, so a client that sends slowly, sends half a
 * frame or never reads its replies only affects itself.
 */
class TcpServer {
public:
    struct Options {
        std::string listen = "0.0.0.0";
        uint16_t port = 502;               ///< 0 picks a free port
        int maxConnections = 16;
        int idleTimeoutMs = 60000;         ///< 0 disables; close connections without requests
        int frameTimeoutMs = 5000;         ///< close connections that leave a frame incomplete
        size_t maxPendingOutput = 64 * 1024;
        int realtimePriority = 0;          ///< > 0: real-time scheduling for the server thread (below the tasks)
    };

    struct ConnectionInfo {
        std::string peer;
        uint64_t requests = 0;
        int64_t connectedMs = 0;
    };

    struct Stats {
        bool listening = false;
        uint16_t port = 0;
        uint64_t totalConnections = 0;
        uint64_t rejectedConnections = 0;
        uint64_t requests = 0;
        uint64_t exceptions = 0;
        int64_t lastRequestAgoMs = -1;     ///< time since the last request; -1 before the first
        std::vector<ConnectionInfo> connections;
    };

    TcpServer(ProcessImage& image, log::Scope log);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    /// Binds and starts the server thread; returns the port it listens on.
    std::expected<uint16_t, net::NetError> start(const Options& options);
    void stop();

    /// Thread-safe; used for Report Server ID and Read Device Identification.
    void setIdentity(DeviceIdentity identity);
    Stats stats() const;

private:
    struct Connection {
        net::Socket socket;
        std::string peer;
        std::vector<uint8_t> rx;
        std::vector<uint8_t> tx;
        std::chrono::steady_clock::time_point connectedAt;
        std::chrono::steady_clock::time_point lastRequest;
        std::chrono::steady_clock::time_point frameStarted;
        uint64_t requests = 0;
        bool closing = false;
    };

    void run();
    void accept();
    /// Reads and handles complete frames; false when the connection must close.
    bool receive(Connection& c);
    bool flush(Connection& c);
    void expire(std::chrono::steady_clock::time_point now);

    ProcessImage& image_;
    log::Scope log_;
    Options options_;
    net::Socket listener_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;  // guards identity_, connections_ and the counters below
    std::shared_ptr<const DeviceIdentity> identity_ = std::make_shared<DeviceIdentity>();
    std::vector<std::unique_ptr<Connection>> connections_;
    uint64_t totalConnections_ = 0;
    uint64_t rejectedConnections_ = 0;
    uint64_t requests_ = 0;
    uint64_t exceptions_ = 0;
    std::chrono::steady_clock::time_point lastRequest_{};
    uint16_t port_ = 0;
    std::chrono::steady_clock::time_point lastRejectLog_{};
};

}  // namespace simpleplc::modbus
