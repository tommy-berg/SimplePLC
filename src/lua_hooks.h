#pragma once
#include <lua.hpp>
#include <string>
#include <modbus.h>
#include <thread>
#include <atomic>
#include <mutex>

class LuaHooks {
public:
    LuaHooks(const std::string& script);
    ~LuaHooks();
    bool override_register(int address, uint16_t& value_out);
    void update_all_registers(modbus_mapping_t* mapping);
    
    // Start periodic update thread
    void start_periodic_updates(modbus_mapping_t* mapping, int update_ms = 100);

private:
    lua_State* L;
    std::thread update_thread;
    std::atomic<bool> running{false};
    std::mutex mapping_mutex;
    modbus_mapping_t* mb_mapping{nullptr};
    
    void update_thread_func(int update_ms);
};
