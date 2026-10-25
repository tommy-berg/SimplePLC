#include "modbus/tcp_server.h"

#include "core/realtime.h"

#include <algorithm>

namespace simpleplc::modbus {
namespace {

using Clock = std::chrono::steady_clock;

uint16_t be16(const std::vector<uint8_t>& p, size_t offset) {
    return static_cast<uint16_t>((p[offset] << 8) | p[offset + 1]);
}

}  // namespace

TcpServer::TcpServer(ProcessImage& image, log::Scope log) : image_(image), log_(std::move(log)) {}

TcpServer::~TcpServer() { stop(); }

std::expected<uint16_t, net::NetError> TcpServer::start(const Options& options) {
    stop();
    options_ = options;
    auto listener = net::listenTcp(options.listen, options.port, 64);
    if (!listener) return std::unexpected(listener.error());
    listener_ = std::move(*listener);
    {
        std::lock_guard lock(mutex_);
        port_ = net::localPort(listener_.get());
    }
    running_ = true;
    thread_ = std::thread(&TcpServer::run, this);
    return port_;
}

void TcpServer::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    listener_.close();
    std::lock_guard lock(mutex_);
    connections_.clear();
    port_ = 0;
}

void TcpServer::setIdentity(DeviceIdentity identity) {
    auto shared = std::make_shared<const DeviceIdentity>(std::move(identity));
    std::lock_guard lock(mutex_);
    identity_ = std::move(shared);
}

TcpServer::Stats TcpServer::stats() const {
    std::lock_guard lock(mutex_);
    Stats s;
    s.listening = running_.load();
    s.port = port_;
    s.totalConnections = totalConnections_;
    s.rejectedConnections = rejectedConnections_;
    s.requests = requests_;
    s.exceptions = exceptions_;
    auto now = Clock::now();
    if (lastRequest_ != Clock::time_point{}) {
        s.lastRequestAgoMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRequest_).count();
    }
    for (const auto& c : connections_) {
        s.connections.push_back(
            {c->peer, c->requests,
             std::chrono::duration_cast<std::chrono::milliseconds>(now - c->connectedAt).count()});
    }
    return s;
}

void TcpServer::run() {
    // Requests are answered between the program's cycles: with real-time tasks the server must not
    // wait behind ordinary threads, or the answers (and the data the program sees) arrive late
    preferInteractive();
    if (options_.realtimePriority > 0) {
        if (auto ok = makeThreadRealtime(options_.realtimePriority, 1); !ok) log_.warn("Modbus: real-time scheduling refused ({})", ok.error());
    }
    std::vector<net::PollFd> fds;
    while (running_) {
        fds.clear();
        fds.push_back({});
        fds[0].fd = listener_.get();
        fds[0].events = POLLIN;
        for (const auto& c : connections_) {
            net::PollFd pfd{};
            pfd.fd = c->socket.get();
            pfd.events = static_cast<short>(c->tx.empty() ? POLLIN : (POLLIN | POLLOUT));
            fds.push_back(pfd);
        }

        // The timeout bounds how long stop() waits and how late timeouts fire
        int ready = net::poll(fds.data(), fds.size(), 100);
        if (ready < 0 && !net::isInterrupted(net::lastError())) {
            log_.error("Modbus poll failed: {}", net::errorText(net::lastError()));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (ready > 0) {
            // Connections first: fds[i + 1] belongs to connections_[i]
            for (size_t i = 0; i + 1 < fds.size() && i < connections_.size(); i++) {
                Connection& c = *connections_[i];
                short revents = fds[i + 1].revents;
                if (revents & (POLLIN | POLLHUP | POLLERR)) {
                    if (!receive(c)) c.closing = true;
                }
                if (!c.closing && (revents & POLLOUT)) {
                    if (!flush(c)) c.closing = true;
                }
            }
            if (fds[0].revents & POLLIN) accept();
        }

        expire(Clock::now());

        std::lock_guard lock(mutex_);
        std::erase_if(connections_, [&](const std::unique_ptr<Connection>& c) {
            if (c->closing) log_.debug("Modbus client {} disconnected", c->peer);
            return c->closing;
        });
    }
}

void TcpServer::accept() {
    while (true) {
        net::Socket sock = net::accept(listener_.get());
        if (!sock) {
            int err = net::lastError();
            if (!net::isWouldBlock(err) && !net::isInterrupted(err)) {
                log_.warn("Modbus accept failed: {}", net::errorText(err));
            }
            return;
        }
        std::string peer = net::peerName(sock.get());

        std::lock_guard lock(mutex_);
        if (connections_.size() >= static_cast<size_t>(options_.maxConnections)) {
            rejectedConnections_++;
            auto now = Clock::now();
            if (now - lastRejectLog_ > std::chrono::minutes(1)) {
                log_.warn("Modbus connection from {} rejected: limit of {} connections reached",
                          peer, options_.maxConnections);
                lastRejectLog_ = now;
            }
            continue;  // sock closes here
        }
        if (!net::setNonBlocking(sock.get())) continue;
        net::setNoDelay(sock.get());

        auto c = std::make_unique<Connection>();
        c->socket = std::move(sock);
        c->peer = std::move(peer);
        c->connectedAt = c->lastRequest = Clock::now();
        totalConnections_++;
        log_.debug("Modbus client {} connected", c->peer);
        connections_.push_back(std::move(c));
    }
}

bool TcpServer::receive(Connection& c) {
    uint8_t buffer[4096];
    // Bounded per poll round so one busy client cannot monopolise the loop
    for (int round = 0; round < 16; round++) {
        long n = net::recvSome(c.socket.get(), buffer, sizeof(buffer));
        if (n == 0) return false;
        if (n < 0) {
            int err = net::lastError();
            if (net::isInterrupted(err)) continue;
            if (net::isWouldBlock(err)) break;
            return false;
        }
        c.rx.insert(c.rx.end(), buffer, buffer + n);
        if (static_cast<size_t>(n) < sizeof(buffer)) break;
    }

    std::shared_ptr<const DeviceIdentity> identity;
    {
        std::lock_guard lock(mutex_);
        identity = identity_;
    }

    size_t offset = 0;
    while (c.rx.size() - offset >= kMbapSize) {
        uint16_t protocol = be16(c.rx, offset + 2);
        uint16_t length = be16(c.rx, offset + 4);
        if (protocol != 0 || length < 2 || length > kMaxPduSize + 1) {
            log_.debug("Modbus client {} sent a frame that is not Modbus TCP; closing", c.peer);
            return false;
        }
        size_t frameSize = 6u + length;
        if (c.rx.size() - offset < frameSize) break;

        std::span<const uint8_t> pdu(c.rx.data() + offset + kMbapSize, length - 1u);
        std::vector<uint8_t> response = handleRequest(pdu, image_, *identity);

        uint16_t responseLength = static_cast<uint16_t>(response.size() + 1);
        c.tx.insert(c.tx.end(), {c.rx[offset], c.rx[offset + 1], 0, 0,
                                 static_cast<uint8_t>(responseLength >> 8),
                                 static_cast<uint8_t>(responseLength & 0xFF), c.rx[offset + 6]});
        c.tx.insert(c.tx.end(), response.begin(), response.end());
        offset += frameSize;

        c.lastRequest = Clock::now();
        std::lock_guard lock(mutex_);
        c.requests++;
        requests_++;
        lastRequest_ = c.lastRequest;
        if (response[0] & 0x80) exceptions_++;
    }
    c.rx.erase(c.rx.begin(), c.rx.begin() + static_cast<std::ptrdiff_t>(offset));

    if (c.rx.empty()) {
        c.frameStarted = {};
    } else if (c.frameStarted == Clock::time_point{}) {
        c.frameStarted = Clock::now();
    }

    if (c.tx.size() > options_.maxPendingOutput) {
        log_.debug("Modbus client {} does not read its responses; closing", c.peer);
        return false;
    }
    return flush(c);
}

bool TcpServer::flush(Connection& c) {
    size_t sent = 0;
    while (sent < c.tx.size()) {
        long n = net::sendSome(c.socket.get(), c.tx.data() + sent, c.tx.size() - sent);
        if (n < 0) {
            int err = net::lastError();
            if (net::isInterrupted(err)) continue;
            if (net::isWouldBlock(err)) break;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    c.tx.erase(c.tx.begin(), c.tx.begin() + static_cast<std::ptrdiff_t>(sent));
    return true;
}

void TcpServer::expire(Clock::time_point now) {
    for (auto& c : connections_) {
        if (c->closing) continue;
        if (options_.idleTimeoutMs > 0 &&
            now - c->lastRequest > std::chrono::milliseconds(options_.idleTimeoutMs)) {
            log_.debug("Modbus client {} idle for {} ms; closing", c->peer, options_.idleTimeoutMs);
            c->closing = true;
        } else if (c->frameStarted != Clock::time_point{} &&
                   now - c->frameStarted > std::chrono::milliseconds(options_.frameTimeoutMs)) {
            log_.debug("Modbus client {} left a frame incomplete; closing", c->peer);
            c->closing = true;
        }
    }
}

}  // namespace simpleplc::modbus
