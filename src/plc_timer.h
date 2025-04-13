#pragma once
#include <chrono>

class TonTimer {
public:
    void start(unsigned int duration_ms);
    void stop();
    void update(bool input);
    bool is_done() const;

private:
    bool running = false;
    bool done = false;
    unsigned int preset = 0;
    std::chrono::steady_clock::time_point t_start;
};
