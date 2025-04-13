#pragma once

#include <modbus.h>
#include <thread>
#include <atomic>

class PlcLogic {
public:
    static void start(modbus_mapping_t* mapping);
    static void stop();
private:
    static void loop();
    static std::atomic<bool> running;
    static std::thread thread;
    static modbus_mapping_t* mb_mapping;
};
