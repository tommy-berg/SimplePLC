#include "plc_logic.h"
#include <chrono>
#include <iostream>
#include "plc_timer.h"

std::atomic<bool> PlcLogic::running = false;
std::thread PlcLogic::thread;
modbus_mapping_t* PlcLogic::mb_mapping = nullptr;

static uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
void PlcLogic::start(modbus_mapping_t* mapping) {
    mb_mapping = mapping;
    running = true;
    thread = std::thread(loop);
}

void PlcLogic::stop() {
    running = false;
    if (thread.joinable())
        thread.join();
}

void PlcLogic::loop() {
    // TODO: Implement the logic for the PLC and map to Lua hooks.
    // This is a simple example that toggles the first bit of the Modbus mapping every second.
    
    std::cout << "[PLC] Logic thread starting... " << std::endl;
    
    TonTimer blink_timer;
    constexpr int on_duration_ms = 1000;
    constexpr int off_duration_ms = 1000;

    constexpr std::chrono::milliseconds scan_time(100);
    bool input = true;
    bool output = false;
    bool waiting_for_off = false;

    blink_timer.start(on_duration_ms);

    while (running) {
        blink_timer.update(input);

        if (blink_timer.is_done()) {
            output = !output;  
            mb_mapping->tab_bits[0] = output;
            std::cout << "[PLC] Blinking bit to: " << output << "\n";

            blink_timer.start(output ? off_duration_ms : on_duration_ms);
            input = false;  
            waiting_for_off = true;
        }

        if (waiting_for_off && !input) {
            input = true;
            waiting_for_off = false;
        }

        std::this_thread::sleep_for(scan_time);
    }

    std::cout << "[PLC] Logic thread stopped.\n";
}