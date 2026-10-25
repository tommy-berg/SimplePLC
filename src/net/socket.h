#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <poll.h>
#endif

/**
 * The only place that knows about the differences between POSIX and Windows
 * sockets. Everything else uses these helpers.
 */
namespace simpleplc::net {

#ifdef _WIN32
using socket_t = SOCKET;
using PollFd = WSAPOLLFD;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
using PollFd = pollfd;
inline constexpr socket_t kInvalidSocket = -1;
#endif

/// Owns a socket and closes it on destruction.
class Socket {
public:
    Socket() = default;
    explicit Socket(socket_t fd) : fd_(fd) {}
    ~Socket() { close(); }
    Socket(Socket&& other) noexcept : fd_(other.release()) {}
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    socket_t get() const { return fd_; }
    explicit operator bool() const { return fd_ != kInvalidSocket; }
    socket_t release();
    void close();

private:
    socket_t fd_ = kInvalidSocket;
};

/// Starts the socket library (Windows); safe to call repeatedly.
void initialize();

int lastError();
std::string errorText(int error);
bool isWouldBlock(int error);
bool isInterrupted(int error);

struct NetError {
    std::string message;
    bool permissionDenied = false;
    bool addressInUse = false;
};

/// Listening, non-blocking TCP socket. `host` may be an IPv4/IPv6 address, a host
/// name or empty/"0.0.0.0" for all IPv4 interfaces ("::" for IPv4 and IPv6).
std::expected<Socket, NetError> listenTcp(const std::string& host, uint16_t port, int backlog);

/// Connects with a timeout; the returned socket is non-blocking with TCP_NODELAY set. While it
/// waits for the connection, `keepWaiting` (when given) is asked every few milliseconds; false gives up.
std::expected<Socket, std::string> connectTcp(const std::string& host, uint16_t port,
                                              std::chrono::milliseconds timeout,
                                              const std::function<bool()>& keepWaiting = {});

/// How often a wait that can be interrupted asks whether to go on, in milliseconds.
inline constexpr int kWaitSliceMs = 5;

/// Accepts one pending connection; an empty Socket when there is none (see lastError()).
Socket accept(socket_t listener);

bool setNonBlocking(socket_t fd);
void setNoDelay(socket_t fd);
uint16_t localPort(socket_t fd);
/// "address:port" of the remote end, IPv6 in brackets.
std::string peerName(socket_t fd);

/// send() without SIGPIPE; returns bytes sent or -1.
long sendSome(socket_t fd, const uint8_t* data, size_t size);
/// recv(); returns bytes read, 0 on orderly close, -1 on error.
long recvSome(socket_t fd, uint8_t* data, size_t size);

int poll(PollFd* fds, size_t count, int timeoutMs);

/// Whether `port` can currently be bound on `host` (used to pick free ports).
bool portAvailable(const std::string& host, uint16_t port);

/// This machine's address on the local network (for URLs to show the user), or "localhost".
std::string primaryAddress();

/// This machine's host name (a container's name inside Docker), or empty.
std::string hostname();

}  // namespace simpleplc::net
