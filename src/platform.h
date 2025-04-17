#pragma once
#include <string>

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