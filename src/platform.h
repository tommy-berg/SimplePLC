#pragma once

#ifdef _WIN32
    #include <windows.h>
    #include <conio.h>
    #define STDIN_FILENO 0
#else
    #include <unistd.h>
    #include <termios.h>
    #include <sys/select.h>
#endif

// Platform-independent terminal functions
namespace platform {
    inline void enableRawMode() {
#ifdef _WIN32
        // Windows doesn't need special setup for raw mode with _kbhit()
#else
        struct termios raw;
        tcgetattr(STDIN_FILENO, &raw);
        raw.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
#endif
    }

    inline bool kbhit() {
#ifdef _WIN32
        return _kbhit() != 0;
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
        return _getch();
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
} 