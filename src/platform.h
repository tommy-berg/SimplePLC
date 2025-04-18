#pragma once
#include <string>

// Include standard C headers
#include <stdlib.h>
#include <stdint.h>

// Include atomic headers
#include <atomic>

// Define compatibility layer for Open62541 atomic operations if needed
#ifndef UA_ATOMIC_OPERATIONS_DEFINED
#define UA_ATOMIC_OPERATIONS_DEFINED

#ifdef __cplusplus
extern "C" {
#endif

// Check if we need to provide custom atomic operations
#if defined(__GNUC__) || defined(__clang__)
    // GCC and Clang have atomic builtins
    #define HAS_ATOMIC_BUILTINS 1
#endif

#if defined(HAS_ATOMIC_BUILTINS)
    // Define atomic operations using compiler builtins
    static inline void* UA_atomic_xchg(void** addr, void* newValue) {
        return __atomic_exchange_n(addr, newValue, __ATOMIC_SEQ_CST);
    }

    static inline void* UA_atomic_cmpxchg(void** addr, void* expected, void* newValue) {
        __atomic_compare_exchange_n(addr, &expected, newValue, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return expected;
    }

    static inline void* UA_atomic_load(void** addr) {
        return __atomic_load_n(addr, __ATOMIC_SEQ_CST);
    }
#endif

#ifdef __cplusplus
}
#endif

#endif /* UA_ATOMIC_OPERATIONS_DEFINED */

// Windows-specific defines
#ifdef _WIN32
#define PATH_MAX 260
#define ssize_t int

// Windows-specific socket definitions to mirror POSIX
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

#ifndef ETIMEDOUT
#define ETIMEDOUT WSAETIMEDOUT
#endif

#ifndef EWOULDBLOCK
#define EWOULDBLOCK WSAEWOULDBLOCK
#endif

#ifndef EAGAIN
#define EAGAIN WSAEWOULDBLOCK
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Define TCP_NODELAY if not present
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

// Define socket options if not present
#ifndef SO_REUSEADDR
#define SO_REUSEADDR 0x0004
#endif

#ifndef SO_SNDBUF
#define SO_SNDBUF 0x1001
#endif

#ifndef SO_RCVBUF
#define SO_RCVBUF 0x1002
#endif

#ifndef SO_KEEPALIVE
#define SO_KEEPALIVE 0x0008
#endif

#ifndef SO_LINGER
#define SO_LINGER 0x0080
#endif

#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif

// Define functions that may not exist in Windows
#define fcntl ioctlsocket

// Define values for fcntl
#define F_GETFL 0
#define F_SETFL 1
#define O_NONBLOCK 1
#define EINTR WSAEINTR

// Redefine function names
#define strerror_r(errnum, buf, buflen) strerror_s(buf, buflen, errnum)

#endif

// Detect macOS
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

// Platform-specific path handling
namespace Platform {
    // Get the directory part of a path
    std::string getPathDirectory(const std::string& path);
    
    // Get the filename part of a path
    std::string getPathFilename(const std::string& path);
    
    // Join path components with the correct separator
    std::string joinPaths(const std::string& path1, const std::string& path2);
    
    // Normalize a path (handle ../, ./, etc)
    std::string normalizePath(const std::string& path);
    
    // Check if a path is absolute
    bool isAbsolutePath(const std::string& path);
    
    // Get the current working directory
    std::string getCurrentDir();
    
    // Check if a file exists
    bool fileExists(const std::string& path);
    
    // Get the executable path
    std::string getExecutablePath();
    
    // Get the directory where the executable is located
    std::string getExecutableDir();
}

#ifdef _WIN32
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <conio.h>
    #pragma comment(lib, "ws2_32.lib")
    #define STDIN_FILENO 0
    #define close closesocket
#else
    #include <unistd.h>
    #include <termios.h>
    #include <sys/select.h>
    #include <sys/socket.h>
#endif

// Platform-independent terminal functions
namespace platform {
    // Forward declaration of platform-specific data
#ifdef _WIN32
    struct TerminalState {
        // Windows doesn't need state
    };
#else
    struct TerminalState {
        termios original;
    };
#endif

    static TerminalState terminal_state;

    inline void enableRawMode() {
#ifdef _WIN32
        // Windows doesn't need special setup for raw mode with _kbhit()
#else
        termios raw;
        tcgetattr(STDIN_FILENO, &terminal_state.original);
        raw = terminal_state.original;
        raw.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
#endif
    }

    inline void disableRawMode() {
#ifdef _WIN32
        // Windows doesn't need cleanup
#else
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &terminal_state.original);
#endif
    }

    inline bool kbhit() {
#ifdef _WIN32
        return ::_kbhit() != 0;
#else
        struct timeval tv = {0L, 0L};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        return select(1, &fds, NULL, NULL, &tv) > 0;
#endif
    }

    inline int getch() {
#ifdef _WIN32
        return ::_getch();
#else
        return getchar();
#endif
    }

    inline void sleep_ms(int milliseconds) {
#ifdef _WIN32
        Sleep(milliseconds);
#else
        usleep(milliseconds * 1000);
#endif
    }

#ifdef _WIN32
    // Windows-specific socket initialization
    class WinSockInit {
    public:
        WinSockInit() {
            WSADATA wsaData;
            WSAStartup(MAKEWORD(2, 2), &wsaData);
        }
        ~WinSockInit() {
            WSACleanup();
        }
    };
    static WinSockInit winsock_init;
#endif
} 