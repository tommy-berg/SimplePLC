#include "plc_logic.h"
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include "platform.h"
#include "device_config.h"

// These constants were likely part of an earlier implementation or for future use
// Commenting out to avoid unused variable warnings
/*
constexpr int BLINK_OUTPUT_BIT = 0;
constexpr int ON_DURATION_MS = 1000;
constexpr int OFF_DURATION_MS = 1000;
constexpr std::chrono::milliseconds SCAN_TIME(100);
*/

std::atomic<bool> PlcLogic::running = false;
std::thread PlcLogic::thread;
modbus_mapping_t* PlcLogic::mb_mapping = nullptr;
std::timed_mutex PlcLogic::mb_mutex;
lua_State* PlcLogic::lua_state = nullptr;

// This function is not currently used - commenting out to avoid warnings
/*
static uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
*/

void PlcLogic::start(modbus_mapping_t* mapping) {
    if (!mapping) {
        throw std::runtime_error("Invalid Modbus mapping");
    }
    
    if (!mapping->tab_input_bits) {
        throw std::runtime_error("Input bits array is null");
    }
    
    if (mapping->nb_input_bits <= 0) {
        throw std::runtime_error("No input bits allocated");
    }
    
    if (running) {
        throw std::runtime_error("PLC logic already running");
    }
    
    mb_mapping = mapping;
    
    std::cout << "[PLC-DEBUG] Modbus mapping sizes:" << std::endl
              << "  Coils (bits): " << mb_mapping->nb_bits << std::endl
              << "  Input bits: " << mb_mapping->nb_input_bits << std::endl
              << "  Registers: " << mb_mapping->nb_registers << std::endl
              << "  Input registers: " << mb_mapping->nb_input_registers << std::endl;
    
    lua_state = luaL_newstate();
    luaL_openlibs(lua_state);
    setupLuaBindings(lua_state);
    
    running = true;
    
    try {
        thread = std::thread(loop);
    } catch (const std::exception& e) {
        running = false;
        mb_mapping = nullptr;
        if (lua_state) {
            lua_close(lua_state);
            lua_state = nullptr;
        }
        throw;
    }
}

void PlcLogic::stop() {
    running = false;
    if (thread.joinable())
        thread.join();
    
    if (lua_state) {
        lua_close(lua_state);
        lua_state = nullptr;
    }
    
    platform::disableRawMode();
}

void PlcLogic::loadScript(const std::string& scriptPath) {
    std::cout << "[PLC] Loading Lua script from: " << scriptPath << std::endl;
    std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
        throw std::runtime_error("Failed to acquire mutex when loading script");
    }
    
    if (luaL_dofile(lua_state, scriptPath.c_str()) != 0) {
        std::string error = lua_tostring(lua_state, -1);
        lua_pop(lua_state, 1);
        std::cerr << "[PLC] Failed to load Lua script: " << error << std::endl;
        throw std::runtime_error("Failed to load Lua script: " + error);
    }
    std::cout << "[PLC] Lua script loaded successfully" << std::endl;
}

void PlcLogic::reloadScript(const std::string& scriptPath) {
    std::cout << "\n[PLC] Reloading script: " << scriptPath << std::endl;
    std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
        std::cerr << "[PLC] Failed to acquire mutex for script reload" << std::endl;
        return;
    }

    if (lua_state) {
        lua_close(lua_state);
    }
    
    lua_state = luaL_newstate();
    luaL_openlibs(lua_state);
    setupLuaBindings(lua_state);
    
    if (luaL_dofile(lua_state, scriptPath.c_str()) != 0) {
        std::string error = lua_tostring(lua_state, -1);
        lua_pop(lua_state, 1);
        std::cerr << "[PLC] Failed to reload Lua script: " << error << std::endl;
    } else {
        std::cout << "[PLC] Script reloaded successfully" << std::endl;
    }
}

int PlcLogic::lua_readCoil(lua_State* L) {
    lua_Integer addr_val = luaL_checkinteger(L, 1);
    int addr = static_cast<int>(addr_val);
    std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
        lua_pushnil(L);
        return 1;
    }
    if (addr >= 0 && addr < mb_mapping->nb_bits) {
        lua_pushboolean(L, mb_mapping->tab_bits[addr]);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int PlcLogic::lua_writeCoil(lua_State* L) {
    lua_Integer addr_val = luaL_checkinteger(L, 1);
    int addr = static_cast<int>(addr_val);
    bool value = lua_toboolean(L, 2);
    std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
        lua_pushboolean(L, false);
        return 1;
    }
    if (addr >= 0 && addr < mb_mapping->nb_bits) {
        mb_mapping->tab_bits[addr] = value;
        lua_pushboolean(L, true);
    } else {
        lua_pushboolean(L, false);
    }
    return 1;
}

int PlcLogic::lua_readDiscreteInput(lua_State* L) {
    lua_Integer addr_val = luaL_checkinteger(L, 1);
    int addr = static_cast<int>(addr_val);
    std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
        lua_pushnil(L);
        return 1;
    }
    if (addr >= 0 && addr < mb_mapping->nb_input_bits) {
        lua_pushboolean(L, mb_mapping->tab_input_bits[addr]);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int PlcLogic::lua_readHoldingRegister(lua_State* L) {
    lua_Integer addr_val = luaL_checkinteger(L, 1);
    int addr = static_cast<int>(addr_val);
    std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
        lua_pushnil(L);
        return 1;
    }
    if (addr >= 0 && addr < mb_mapping->nb_registers) {
        lua_pushinteger(L, mb_mapping->tab_registers[addr]);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int PlcLogic::lua_writeHoldingRegister(lua_State* L) {
    lua_Integer addr_val = luaL_checkinteger(L, 1);
    int addr = static_cast<int>(addr_val);
    lua_Integer value_val = luaL_checkinteger(L, 2);
    int value = static_cast<int>(value_val);
    std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
        lua_pushboolean(L, false);
        return 1;
    }
    if (addr >= 0 && addr < mb_mapping->nb_registers) {
        mb_mapping->tab_registers[addr] = static_cast<uint16_t>(value);
        lua_pushboolean(L, true);
    } else {
        lua_pushboolean(L, false);
    }
    return 1;
}

int PlcLogic::lua_print(lua_State* L) {
    int nargs = lua_gettop(L);
    std::string output = "[LUA] ";
    
    for (int i = 1; i <= nargs; i++) {
        if (i > 1) output += " ";
        
        if (lua_isstring(L, i)) {
            output += lua_tostring(L, i);
        }
        else if (lua_isnil(L, i)) {
            output += "nil";
        }
        else if (lua_isboolean(L, i)) {
            output += lua_toboolean(L, i) ? "true" : "false";
        }
        else if (lua_isnumber(L, i)) {
            output += std::to_string(lua_tonumber(L, i));
        }
        else {
            output += lua_typename(L, lua_type(L, i));
        }
    }
    
    std::cout << output << std::endl << std::flush;
    return 0;
}

int PlcLogic::lua_readInputRegister(lua_State* L) {
    lua_Integer addr_val = luaL_checkinteger(L, 1);
    int addr = static_cast<int>(addr_val);
    std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
        lua_pushnil(L);
        return 1;
    }
    if (addr >= 0 && addr < mb_mapping->nb_input_registers) {
        lua_pushinteger(L, mb_mapping->tab_input_registers[addr]);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

int PlcLogic::lua_writeInputRegister(lua_State* L) {
    lua_Integer addr_val = luaL_checkinteger(L, 1);
    int addr = static_cast<int>(addr_val);
    lua_Integer value_val = luaL_checkinteger(L, 2);
    int value = static_cast<int>(value_val);
    std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
        lua_pushboolean(L, false);
        return 1;
    }
    if (addr >= 0 && addr < mb_mapping->nb_input_registers) {
        mb_mapping->tab_input_registers[addr] = static_cast<uint16_t>(value);
        lua_pushboolean(L, true);
    } else {
        lua_pushboolean(L, false);
    }
    return 1;
}

int PlcLogic::lua_writeDiscreteInput(lua_State* L) {
    lua_Integer addr_val = luaL_checkinteger(L, 1);
    int addr = static_cast<int>(addr_val);
    bool value = lua_toboolean(L, 2);
    std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
        lua_pushboolean(L, false);
        return 1;
    }
    if (addr >= 0 && addr < mb_mapping->nb_input_bits) {
        mb_mapping->tab_input_bits[addr] = value;
        lua_pushboolean(L, true);
    } else {
        lua_pushboolean(L, false);
    }
    return 1;
}

void PlcLogic::setupLuaBindings(lua_State* L) {
    lua_pushcfunction(L, lua_print);
    lua_setglobal(L, "print");
    
    lua_newtable(L);
    
    lua_pushcfunction(L, lua_readCoil);
    lua_setfield(L, -2, "readCoil");
    
    lua_pushcfunction(L, lua_writeCoil);
    lua_setfield(L, -2, "writeCoil");
    
    lua_pushcfunction(L, lua_readDiscreteInput);
    lua_setfield(L, -2, "readDiscreteInput");
    
    lua_pushcfunction(L, lua_writeDiscreteInput);
    lua_setfield(L, -2, "writeDiscreteInput");

    lua_pushcfunction(L, lua_readHoldingRegister);
    lua_setfield(L, -2, "readHoldingRegister");
    
    lua_pushcfunction(L, lua_writeHoldingRegister);
    lua_setfield(L, -2, "writeHoldingRegister");
    
    lua_pushcfunction(L, lua_readInputRegister);
    lua_setfield(L, -2, "readInputRegister");
    
    lua_pushcfunction(L, lua_writeInputRegister);
    lua_setfield(L, -2, "writeInputRegister");
    

    
    lua_setglobal(L, "modbus");
}

void PlcLogic::loop() {
    std::cout << "[PLC] Logic thread starting... " << std::endl;
    std::cout << "[PLC] Press SPACE to reload the script" << std::endl;
    
    platform::enableRawMode();
    constexpr std::chrono::milliseconds SCAN_INTERVAL(1000);
    int cycle_count = 0;
    
    // Get script path from config
    std::string scriptPath = DeviceConfig::getDeviceInfo().run_script;
    std::cout << "[PLC] Using script: " << scriptPath << std::endl;
    
    // Load the script initially
    try {
        loadScript(scriptPath);
    } catch (const std::exception& e) {
        std::cerr << "[PLC] Failed to load initial script: " << e.what() << std::endl;
    }

    while (running) {
        // Check for space key press
        if (platform::kbhit()) {
            int key_pressed = platform::getch();
            if (key_pressed == ' ') {
                reloadScript(scriptPath);
            }
        }

        lua_State* current_state = nullptr;
        
        // cycle function mutex locked
        {
            std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
            if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
                std::cerr << "[PLC] Failed to acquire mutex in cycle " << cycle_count << std::endl;
                std::this_thread::sleep_for(SCAN_INTERVAL);
                continue;
            }
            
            current_state = lua_state;
            lua_getglobal(lua_state, "cycle");
            
            if (!lua_isfunction(lua_state, -1)) {
                std::cerr << "[PLC] Error: cycle function not found in Lua script" << std::endl;
                break;
            }
        }
        
        if (lua_pcall(current_state, 0, 0, 0) != 0) {
            std::cerr << "[PLC] Lua error in cycle " << cycle_count << ": " 
                      << lua_tostring(current_state, -1) << std::endl;
            
            lua_getglobal(current_state, "debug");
            if (!lua_isnil(current_state, -1)) {
                lua_getfield(current_state, -1, "traceback");
                if (lua_isfunction(current_state, -1)) {
                    lua_pushstring(current_state, "Stack traceback:");
                    lua_pcall(current_state, 1, 1, 0);
                    std::cerr << "[PLC] Lua stack trace: " << lua_tostring(current_state, -1) << std::endl;
                    lua_pop(current_state, 1);
                }
                lua_pop(current_state, 1);
            }
            lua_pop(current_state, 1);
        }
        
        cycle_count++;
        std::this_thread::sleep_for(SCAN_INTERVAL);
    }

    std::cout << "[PLC] Logic thread stopped after " << cycle_count << " cycles.\n";
}