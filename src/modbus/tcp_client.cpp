#include "modbus/tcp_client.h"

#include <algorithm>
#include <format>
#include <optional>

#include "modbus/pdu.h"

namespace simpleplc::modbus {
namespace {

using Clock = std::chrono::steady_clock;

int remainingMs(Clock::time_point deadline) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::max<long long>(left, 0));
}

}  // namespace

std::string TcpClient::fail(std::string message, bool keepConnection) {
    // One late answer is no reason to give a device up: the next call tries again (on the same
    // connection when it is still in step). Only a device that keeps failing (a frozen PLC that accepts
    // connections but never answers) is given a second between attempts: one timeout per second
    // instead of one per call, which would stretch every cycle
    failuresInRow_++;
    lastFailure_ = Failure::Transport;
    if (!keepConnection || failuresInRow_ >= kFailuresBeforeBackoff) socket_.close();
    if (failuresInRow_ >= kFailuresBeforeBackoff) retryAfter_ = Clock::now() + std::chrono::seconds(1);
    stats_.errors++;
    stats_.lastError = message;
    return message;
}

std::string TcpClient::deviceFailure(std::string message) {
    lastFailure_ = Failure::Device;
    stats_.errors++;
    stats_.lastError = message;
    return message;
}

int TcpClient::waitFor(short events, Clock::time_point deadline) {
    net::PollFd pfd{};
    pfd.fd = socket_.get();
    pfd.events = events;
    while (true) {
        int remaining = remainingMs(deadline);
        int wait = keepWaiting_ ? std::min(remaining, net::kWaitSliceMs) : remaining;
        int ready = net::poll(&pfd, 1, wait);
        if (ready > 0) return 1;
        if (ready < 0) {
            if (net::isInterrupted(net::lastError())) continue;
            return -1;
        }
        if (wait >= remaining) return 0;
        if (keepWaiting_ && !keepWaiting_()) return -2;
    }
}

std::expected<void, std::string> TcpClient::ensureConnected() {
    if (socket_) return {};
    if (Clock::now() < retryAfter_) {
        lastFailure_ = Failure::Transport;
        return std::unexpected(stats_.lastError.empty()
                                   ? std::format("not connected to {}:{}", options_.host, options_.port)
                                   : stats_.lastError);
    }
    auto sock = net::connectTcp(options_.host, options_.port, options_.timeout, keepWaiting_);
    if (!sock) {
        if (keepWaiting_ && !keepWaiting_()) {
            lastFailure_ = Failure::Cancelled;
            return std::unexpected(std::string("cancelled"));
        }
        failuresInRow_++;
        if (failuresInRow_ >= kFailuresBeforeBackoff) retryAfter_ = Clock::now() + std::chrono::seconds(1);
        lastFailure_ = Failure::Transport;
        stats_.errors++;
        stats_.lastError = sock.error();
        return std::unexpected(sock.error());
    }
    socket_ = std::move(*sock);
    connects_++;
    return {};
}

std::expected<std::vector<uint8_t>, std::string> TcpClient::transact(const std::vector<uint8_t>& pdu) {
    stats_.requests++;
    for (int attempt = 0;; attempt++) {
        bool reused = static_cast<bool>(socket_);
        if (auto ok = ensureConnected(); !ok) return std::unexpected(ok.error());
        bool stale = false;
        bool clean = true;
        bool cancelled = false;
        auto response = exchange(pdu, stale, clean, cancelled);
        if (response) {
            failuresInRow_ = 0;  // the device answers (an exception is an answer too)
            lastFailure_ = Failure::None;
            return response;
        }
        if (cancelled) {
            lastFailure_ = Failure::Cancelled;
            if (!clean) socket_.close();
            return std::unexpected(std::string("cancelled"));
        }
        // A device closes connections that sat idle (its idle timeout, a restart): that is no failure
        // of the device, so connect again at once
        if (stale && reused && attempt == 0) {
            socket_.close();
            continue;
        }
        return std::unexpected(fail(response.error(), clean));
    }
}

std::expected<std::vector<uint8_t>, std::string> TcpClient::exchange(const std::vector<uint8_t>& pdu, bool& stale, bool& clean,
                                                                     bool& cancelled) {
    transaction_++;
    uint16_t length = static_cast<uint16_t>(pdu.size() + 1);
    std::vector<uint8_t> frame = {static_cast<uint8_t>(transaction_ >> 8),
                                  static_cast<uint8_t>(transaction_ & 0xFF),
                                  0,
                                  0,
                                  static_cast<uint8_t>(length >> 8),
                                  static_cast<uint8_t>(length & 0xFF),
                                  options_.unit};
    frame.insert(frame.end(), pdu.begin(), pdu.end());

    auto deadline = Clock::now() + options_.timeout;
    size_t sent = 0;
    while (sent < frame.size()) {
        long n = net::sendSome(socket_.get(), frame.data() + sent, frame.size() - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        int err = net::lastError();
        if (n < 0 && (net::isWouldBlock(err) || net::isInterrupted(err))) {
            int ready = waitFor(POLLOUT, deadline);
            if (ready == 1) continue;
            clean = sent == 0;  // half a frame leaves the stream out of step
            if (ready == -2) {
                cancelled = true;
                return std::unexpected(std::string("cancelled"));
            }
            return std::unexpected(std::format("{}:{}: send timed out", options_.host, options_.port));
        }
        stale = true;
        clean = false;
        return std::unexpected(std::format("{}:{}: send failed: {}", options_.host, options_.port, net::errorText(err)));
    }

    // Read until a response with our transaction id arrives; stale ones (answers that came after their
    // call had timed out) are skipped
    std::vector<uint8_t> rx;
    bool received = false;
    std::optional<uint16_t> otherTid;  // an answer to another request: late, or a device that does not copy the id
    while (true) {
        if (rx.size() >= kMbapSize) {
            size_t frameSize = 6u + static_cast<size_t>((rx[4] << 8) | rx[5]);
            if (frameSize < kMbapSize + 1 || frameSize > kMaxAduSize) {
                clean = false;
                return std::unexpected(std::format("{}:{}: invalid response frame", options_.host, options_.port));
            }
            if (rx.size() >= frameSize) {
                uint16_t tid = static_cast<uint16_t>((rx[0] << 8) | rx[1]);
                if (tid == transaction_) {
                    return std::vector<uint8_t>(rx.begin() + kMbapSize, rx.begin() + static_cast<std::ptrdiff_t>(frameSize));
                }
                otherTid = tid;
                rx.erase(rx.begin(), rx.begin() + static_cast<std::ptrdiff_t>(frameSize));
                continue;
            }
        }
        int ready = waitFor(POLLIN, deadline);
        if (ready != 1) {
            // Still in step when no part of a frame has arrived: the late answer is skipped by its id
            clean = rx.empty();
            if (ready == -2) {
                cancelled = true;
                return std::unexpected(std::string("cancelled"));
            }
            // A device that answers every request with the same transaction id (some gateways send 0) looks
            // silent from here; the message says what did arrive, so the fault is found at the device
            std::string message = std::format("{}:{}: no response within {} ms", options_.host, options_.port, options_.timeout.count());
            if (otherTid) {
                message += std::format(" (an answer with transaction id {} arrived, this request's is {}: a late answer, or the device "
                                       "does not copy the transaction id of the request)", *otherTid, transaction_);
            }
            return std::unexpected(message);
        }
        uint8_t buffer[512];
        long n = net::recvSome(socket_.get(), buffer, sizeof(buffer));
        if (n == 0) {
            stale = !received;
            clean = false;
            return std::unexpected(std::format("{}:{}: connection closed by device", options_.host, options_.port));
        }
        if (n < 0) {
            int err = net::lastError();
            if (net::isWouldBlock(err) || net::isInterrupted(err)) continue;
            stale = !received;
            clean = false;
            return std::unexpected(std::format("{}:{}: receive failed: {}", options_.host, options_.port, net::errorText(err)));
        }
        received = true;
        rx.insert(rx.end(), buffer, buffer + n);
    }
}

std::expected<std::vector<uint16_t>, std::string> TcpClient::read(Area area, uint16_t address,
                                                                  uint16_t count) {
    uint16_t max = isBitArea(area) ? kMaxReadBits : kMaxReadRegisters;
    if (count < 1 || count > max) {
        lastFailure_ = Failure::Device;
        return std::unexpected(std::format("count must be 1-{} for {}", max, areaName(area)));
    }
    std::vector<uint8_t> request = buildReadRequest(area, address, count);
    auto response = transact(request);
    if (!response) return std::unexpected(response.error());
    auto values = parseReadResponse(*response, request);
    if (!values) return std::unexpected(deviceFailure(values.error()));
    return values;
}

std::expected<TcpClient::Identity, std::string> TcpClient::readIdentity(std::chrono::milliseconds timeout) {
    // Modbus Application Protocol 1.1b3, 6.21: request 2B 0E <read device id code 01: basic> <object id>;
    // response 2B 0E 01 <conformity> <more follows: 00 or FF> <next object id> <number of objects>, then each
    // object as <id> <length> <value>. A long answer continues ("more follows") at the next object id.
    if (auto ok = ensureConnected(); !ok) return std::unexpected(ok.error());
    auto saved = options_.timeout;
    // A given `timeout` bounds the whole identification, however many requests it takes: a link asks in the
    // time left before its next cycle, and a device that answers each request slowly must not make it late
    std::optional<Clock::time_point> deadline;
    if (timeout.count() > 0) deadline = Clock::now() + std::min(timeout, saved);
    Identity id;
    std::string* objects[] = {&id.vendor, &id.product, &id.revision};
    uint8_t next = 0;
    std::string problem;
    for (int request = 0; request < 3 && problem.empty(); request++) {
        if (deadline) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - Clock::now());
            if (left <= std::chrono::milliseconds(0)) {
                lastFailure_ = Failure::Transport;
                problem = std::format("{}:{}: no time left for the rest of the device identification", options_.host, options_.port);
                break;
            }
            options_.timeout = std::min(saved, left);
        }
        std::vector<uint8_t> pdu = {fc::kEncapsulatedInterface, fc::kMeiReadDeviceId, 0x01, next};
        bool stale = false, clean = true, cancelled = false;
        auto response = exchange(pdu, stale, clean, cancelled);
        if (!response) {
            if (!clean) socket_.close();  // out of step: the next call connects again (no backoff)
            lastFailure_ = cancelled ? Failure::Cancelled : Failure::Transport;
            problem = response.error();
            break;
        }
        const std::vector<uint8_t>& r = *response;
        lastFailure_ = Failure::Device;
        if (r.size() >= 2 && r[0] == (fc::kEncapsulatedInterface | 0x80)) {
            problem = r[1] == kIllegalFunction ? std::string("not supported (exception 1: illegal function)")
                                               : std::format("device returned exception {} ({})", r[1], exceptionText(r[1]));
            break;
        }
        if (r.size() < 7 || r[0] != fc::kEncapsulatedInterface || r[1] != fc::kMeiReadDeviceId) {
            problem = "malformed device identification response";
            break;
        }
        size_t at = 7;
        for (uint8_t n = 0; n < r[6]; n++) {
            if (at + 2 > r.size() || at + 2 + r[at + 1] > r.size()) {
                problem = "malformed device identification response";
                break;
            }
            uint8_t object = r[at];
            std::string value(r.begin() + static_cast<std::ptrdiff_t>(at + 2), r.begin() + static_cast<std::ptrdiff_t>(at + 2 + r[at + 1]));
            for (char& c : value) {
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
            }
            if (object < 3) *objects[object] = std::move(value);
            at += 2u + r[at + 1];
        }
        if (r[4] != 0xFF || r[5] >= 3 || r[5] <= next) break;  // complete (or a continuation that goes nowhere)
        next = r[5];
    }
    options_.timeout = saved;
    if (!problem.empty()) return std::unexpected(problem);
    lastFailure_ = Failure::None;
    return id;
}

std::expected<void, std::string> TcpClient::write(Area area, uint16_t address,
                                                  std::span<const uint16_t> values) {
    auto request = buildWriteRequest(area, address, values);
    if (!request) {
        lastFailure_ = Failure::Device;
        return std::unexpected(request.error());
    }
    auto response = transact(*request);
    if (!response) return std::unexpected(response.error());
    auto ok = parseWriteResponse(*response, *request);
    if (!ok) return std::unexpected(deviceFailure(ok.error()));
    return ok;
}

}  // namespace simpleplc::modbus
