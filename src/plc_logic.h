#pragma once

#include <modbus.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <lua.hpp>

class PlcLogic {
public:
    static void start(modbus_mapping_t* mapping);
    static void stop();
    static void loadScript(const std::string& scriptPath);
    static void reloadScript(const std::string& scriptPath);

private:
    static void loop();
    static void setupLuaBindings(lua_State* L);
    
    static int lua_readCoil(lua_State* L);
    static int lua_writeCoil(lua_State* L);
    static int lua_readDiscreteInput(lua_State* L);
    static int lua_readHoldingRegister(lua_State* L);
    static int lua_writeHoldingRegister(lua_State* L);
    static int lua_print(lua_State* L);
    static int lua_readInputRegister(lua_State* L);
    static int lua_writeInputRegister(lua_State* L);
    static int lua_writeDiscreteInput(lua_State* L);
    
    static std::atomic<bool> running;
    static std::thread thread;
    static modbus_mapping_t* mb_mapping;
    static std::timed_mutex mb_mutex;
    static lua_State* lua_state;
};
