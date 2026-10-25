#include "net/socket.h"

#include <cstring>
#include <format>
#include <mutex>

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace simpleplc::net {

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.release();
    }
    return *this;
}

socket_t Socket::release() {
    socket_t fd = fd_;
    fd_ = kInvalidSocket;
    return fd;
}

void Socket::close() {
    if (fd_ == kInvalidSocket) return;
#ifdef _WIN32
    ::closesocket(fd_);
#else
    ::close(fd_);
#endif
    fd_ = kInvalidSocket;
}

void initialize() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

int lastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string errorText(int error) {
#ifdef _WIN32
    char* text = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(error), 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string out = text ? text : std::format("error {}", error);
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '.')) out.pop_back();
    return out;
#else
    return std::strerror(error);
#endif
}

bool isWouldBlock(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
#endif
}

bool isInterrupted(int error) {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

static bool isPermissionDenied(int error) {
#ifdef _WIN32
    return error == WSAEACCES;
#else
    return error == EACCES || error == EPERM;
#endif
}

static bool isAddressInUse(int error) {
#ifdef _WIN32
    return error == WSAEADDRINUSE;
#else
    return error == EADDRINUSE;
#endif
}

bool setNonBlocking(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

void setNoDelay(socket_t fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

static std::string addressText(const sockaddr* sa) {
    char host[INET6_ADDRSTRLEN] = "?";
    uint16_t port = 0;
    if (sa->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
        port = ntohs(in->sin_port);
        return std::format("{}:{}", host, port);
    }
    if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        port = ntohs(in6->sin6_port);
        return std::format("[{}]:{}", host, port);
    }
    return "?";
}

static std::string normalizeHost(const std::string& host) {
    return host.empty() || host == "*" ? "0.0.0.0" : host;
}

std::expected<Socket, NetError> listenTcp(const std::string& hostIn, uint16_t port, int backlog) {
    initialize();
    std::string host = normalizeHost(hostIn);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    addrinfo* list = nullptr;
    std::string service = std::to_string(port);
    if (int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &list); rc != 0) {
        return std::unexpected(NetError{std::format("cannot resolve '{}': {}", host, gai_strerror(rc))});
    }

    NetError lastFailure{std::format("no usable address for '{}'", host)};
    for (addrinfo* ai = list; ai; ai = ai->ai_next) {
        Socket sock(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!sock) continue;
#ifdef _WIN32
        // Exclusive use, so a second process cannot silently share the port
        int exclusive = 1;
        setsockopt(sock.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
        int one = 1;
        setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
        if (ai->ai_family == AF_INET6) {
            int v6only = 0;  // "::" accepts IPv4 too
            setsockopt(sock.get(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
        }
        if (::bind(sock.get(), ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) != 0) {
            int err = lastError();
            lastFailure = NetError{std::format("cannot listen on {}:{}: {}", host, port, errorText(err)),
                                   isPermissionDenied(err), isAddressInUse(err)};
            continue;
        }
        if (::listen(sock.get(), backlog) != 0 || !setNonBlocking(sock.get())) {
            lastFailure = NetError{std::format("cannot listen on {}:{}: {}", host, port, errorText(lastError()))};
            continue;
        }
        freeaddrinfo(list);
        return sock;
    }
    freeaddrinfo(list);
    return std::unexpected(lastFailure);
}

std::expected<Socket, std::string> connectTcp(const std::string& host, uint16_t port,
                                              std::chrono::milliseconds timeout,
                                              const std::function<bool()>& keepWaiting) {
    initialize();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* list = nullptr;
    std::string service = std::to_string(port);
    if (int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &list); rc != 0) {
        return std::unexpected(std::format("cannot resolve '{}': {}", host, gai_strerror(rc)));
    }

    std::string failure = std::format("cannot connect to {}:{}", host, port);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (addrinfo* ai = list; ai; ai = ai->ai_next) {
        Socket sock(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!sock || !setNonBlocking(sock.get())) continue;
        setNoDelay(sock.get());
        if (::connect(sock.get(), ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) != 0) {
            int err = lastError();
            if (!isWouldBlock(err)) {
                failure = std::format("cannot connect to {}:{}: {}", host, port, errorText(err));
                continue;
            }
            PollFd pfd{};
            pfd.fd = sock.get();
            pfd.events = POLLOUT;
            int ready = 0;
            while (true) {
                auto remaining = std::max<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                         deadline - std::chrono::steady_clock::now()).count(), 0);
                int wait = keepWaiting ? static_cast<int>(std::min<long long>(remaining, kWaitSliceMs))
                                       : static_cast<int>(remaining);
                ready = net::poll(&pfd, 1, wait);
                if (ready != 0 || wait >= remaining) break;
                if (keepWaiting && !keepWaiting()) {
                    freeaddrinfo(list);
                    return std::unexpected(std::format("cannot connect to {}:{}: cancelled", host, port));
                }
            }
            if (ready <= 0) {
                failure = std::format("cannot connect to {}:{}: timed out after {} ms", host, port,
                                      timeout.count());
                continue;
            }
            int soError = 0;
            socklen_t len = sizeof(soError);
            getsockopt(sock.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
            if (soError != 0) {
                failure = std::format("cannot connect to {}:{}: {}", host, port, errorText(soError));
                continue;
            }
        }
        freeaddrinfo(list);
        return sock;
    }
    freeaddrinfo(list);
    return std::unexpected(failure);
}

Socket accept(socket_t listener) {
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    return Socket(::accept(listener, reinterpret_cast<sockaddr*>(&addr), &len));
}

uint16_t localPort(socket_t fd) {
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    if (addr.ss_family == AF_INET) return ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
    if (addr.ss_family == AF_INET6) return ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
    return 0;
}

std::string peerName(socket_t fd) {
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "?";
    return addressText(reinterpret_cast<sockaddr*>(&addr));
}

long sendSome(socket_t fd, const uint8_t* data, size_t size) {
#ifdef _WIN32
    return ::send(fd, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
#elif defined(MSG_NOSIGNAL)
    return static_cast<long>(::send(fd, data, size, MSG_NOSIGNAL));
#else
    return static_cast<long>(::send(fd, data, size, 0));
#endif
}

long recvSome(socket_t fd, uint8_t* data, size_t size) {
#ifdef _WIN32
    return ::recv(fd, reinterpret_cast<char*>(data), static_cast<int>(size), 0);
#else
    return static_cast<long>(::recv(fd, data, size, 0));
#endif
}

int poll(PollFd* fds, size_t count, int timeoutMs) {
#ifdef _WIN32
    return WSAPoll(fds, static_cast<ULONG>(count), timeoutMs);
#else
    return ::poll(fds, static_cast<nfds_t>(count), timeoutMs);
#endif
}

bool portAvailable(const std::string& host, uint16_t port) {
    return listenTcp(host, port, 1).has_value();
}

std::string primaryAddress() {
    initialize();
    // Connecting a UDP socket sends nothing; it only selects the outgoing interface
    Socket sock(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!sock) return "localhost";
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(53);
    inet_pton(AF_INET, "192.0.2.1", &target.sin_addr);  // TEST-NET-1, never routed anywhere real
    if (::connect(sock.get(), reinterpret_cast<sockaddr*>(&target), sizeof(target)) != 0) return "localhost";
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (getsockname(sock.get(), reinterpret_cast<sockaddr*>(&local), &len) != 0) return "localhost";
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
    std::string address = text;
    return address.empty() || address == "0.0.0.0" ? "localhost" : address;
}

std::string hostname() {
    initialize();
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0) return {};
    return name;
}

}  // namespace simpleplc::net
