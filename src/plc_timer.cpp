#include "plc_timer.h"

void TonTimer::start(unsigned int duration_ms) {
    preset = duration_ms;
    running = false;
    done = false;
}

void TonTimer::stop() {
    running = false;
    done = false;
}

void TonTimer::update(bool input) {
    if (input) {
        if (!running) {
            running = true;
            t_start = std::chrono::steady_clock::now();
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - t_start).count();

        if (elapsed >= preset) {
            done = true;
        }
    } else {
        running = false;
        done = false;
    }
}

bool TonTimer::is_done() const {
    return done;
}
