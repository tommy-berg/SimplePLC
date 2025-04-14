#include "plc_logic.h"
#include <chrono>
#include <iostream>
#include <mutex>

// Constants
constexpr int BLINK_OUTPUT_BIT = 0;
constexpr int ON_DURATION_MS = 1000;
constexpr int OFF_DURATION_MS = 1000;
constexpr std::chrono::milliseconds SCAN_TIME(100);

std::atomic<bool> PlcLogic::running = false;
std::thread PlcLogic::thread;
modbus_mapping_t* PlcLogic::mb_mapping = nullptr;
std::timed_mutex PlcLogic::mb_mutex;
lua_State* PlcLogic::lua_state = nullptr;

static uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
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

int PlcLogic::lua_readCoil(lua_State* L) {
    int addr = luaL_checkinteger(L, 1);
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
    int addr = luaL_checkinteger(L, 1);
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
    try {
        std::cout << "[PLC-DEBUG] lua_readDiscreteInput called" << std::endl << std::flush;
        
        // Check if we have an argument
        if (lua_gettop(L) < 1) {
            std::cerr << "[PLC-ERROR] No address provided to readDiscreteInput" << std::endl << std::flush;
            lua_pushnil(L);
            return 1;
        }
        
        // Debug point 1
        std::cout << "[PLC-DEBUG] Checking address argument" << std::endl << std::flush;
        
        int addr = luaL_checkinteger(L, 1);
        std::cout << "[PLC-DEBUG] Reading discrete input at address: " << addr << std::endl << std::flush;
        
        // Debug point 2
        std::cout << "[PLC-DEBUG] Checking mb_mapping" << std::endl << std::flush;
        
        if (!mb_mapping) {
            std::cerr << "[PLC-ERROR] Modbus mapping is null" << std::endl << std::flush;
            lua_pushnil(L);
            return 1;
        }
        
        // Debug point 3
        std::cout << "[PLC-DEBUG] Attempting to acquire mutex" << std::endl << std::flush;
        
        std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
            std::cerr << "[PLC-ERROR] Failed to acquire mutex within timeout" << std::endl << std::flush;
            lua_pushnil(L);
            return 1;
        }
        
        std::cout << "[PLC-DEBUG] Mutex acquired successfully" << std::endl << std::flush;
        
        // Debug point 4
        std::cout << "[PLC-DEBUG] Checking input bits array" << std::endl << std::flush;
        
        if (!mb_mapping->tab_input_bits) {
            std::cerr << "[PLC-ERROR] Input bits array is null" << std::endl << std::flush;
            lua_pushnil(L);
            return 1;
        }
        
        std::cout << "[PLC-DEBUG] Number of input bits available: " << mb_mapping->nb_input_bits << std::endl << std::flush;
        std::cout << "[PLC-DEBUG] Input bits array address: " << (void*)mb_mapping->tab_input_bits << std::endl << std::flush;
        
        if (addr >= 0 && addr < mb_mapping->nb_input_bits) {
            // Debug point 5
            std::cout << "[PLC-DEBUG] Reading value from address" << std::endl << std::flush;
            
            bool value = mb_mapping->tab_input_bits[addr];
            std::cout << "[PLC-DEBUG] Read value: " << value << std::endl << std::flush;
            
            // Debug point 6
            std::cout << "[PLC-DEBUG] Pushing value to Lua stack" << std::endl << std::flush;
            
            lua_pushboolean(L, value);
            std::cout << "[PLC-DEBUG] Pushed boolean value to Lua stack" << std::endl << std::flush;
        } else {
            std::cout << "[PLC-DEBUG] Address " << addr << " out of range (max: " 
                      << mb_mapping->nb_input_bits - 1 << ")" << std::endl << std::flush;
            lua_pushnil(L);
            std::cout << "[PLC-DEBUG] Pushed nil to Lua stack" << std::endl << std::flush;
        }
        
        // Debug point 7
        std::cout << "[PLC-DEBUG] Function completing normally" << std::endl << std::flush;
        return 1;
        
    } catch (const std::exception& e) {
        std::cerr << "[PLC-ERROR] Exception in lua_readDiscreteInput: " << e.what() << std::endl << std::flush;
        lua_pushnil(L);
        return 1;
    } catch (...) {
        std::cerr << "[PLC-ERROR] Unknown exception in lua_readDiscreteInput" << std::endl << std::flush;
        lua_pushnil(L);
        return 1;
    }
}

int PlcLogic::lua_readHoldingRegister(lua_State* L) {
    int addr = luaL_checkinteger(L, 1);
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
    int addr = luaL_checkinteger(L, 1);
    int value = luaL_checkinteger(L, 2);
    std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
        lua_pushboolean(L, false);
        return 1;
    }
    if (addr >= 0 && addr < mb_mapping->nb_registers) {
        mb_mapping->tab_registers[addr] = value;
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
    int addr = luaL_checkinteger(L, 1);
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
    int addr = luaL_checkinteger(L, 1);
    int value = luaL_checkinteger(L, 2);
    std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
        lua_pushboolean(L, false);
        return 1;
    }
    if (addr >= 0 && addr < mb_mapping->nb_input_registers) {
        mb_mapping->tab_input_registers[addr] = value;
        lua_pushboolean(L, true);
    } else {
        lua_pushboolean(L, false);
    }
    return 1;
}

int PlcLogic::lua_writeDiscreteInput(lua_State* L) {
    int addr = luaL_checkinteger(L, 1);
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
    
    lua_pushcfunction(L, lua_readHoldingRegister);
    lua_setfield(L, -2, "readHoldingRegister");
    
    lua_pushcfunction(L, lua_writeHoldingRegister);
    lua_setfield(L, -2, "writeHoldingRegister");
    
    lua_pushcfunction(L, lua_readInputRegister);
    lua_setfield(L, -2, "readInputRegister");
    
    lua_pushcfunction(L, lua_writeInputRegister);
    lua_setfield(L, -2, "writeInputRegister");
    
    lua_pushcfunction(L, lua_writeDiscreteInput);
    lua_setfield(L, -2, "writeDiscreteInput");
    
    lua_setglobal(L, "modbus");
}

void PlcLogic::loop() {
    std::cout << "[PLC] Logic thread starting... " << std::endl;
    
    constexpr std::chrono::milliseconds SCAN_TIME(100);
    int cycle_count = 0;

    while (running) {
        lua_State* current_state = nullptr;
        
        // cycle function mutex locked
        {
            std::unique_lock<std::timed_mutex> lock(mb_mutex, std::defer_lock);
            if (!lock.try_lock_for(std::chrono::milliseconds(1000))) {
                std::cerr << "[PLC] Failed to acquire mutex in cycle " << cycle_count << std::endl;
                std::this_thread::sleep_for(SCAN_TIME);
                continue;
            }
            
            current_state = lua_state;
            lua_getglobal(lua_state, "cycle");
            
            if (!lua_isfunction(lua_state, -1)) {
                std::cerr << "[PLC] Error: cycle function not found in Lua script" << std::endl;
                break;
            }
        }
        
        // without mutex
        //std::cout << "[PLC] Executing cycle " << cycle_count << "..." << std::endl;
        
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
        } else {
            // std::cout << "[PLC] Cycle " << cycle_count << " completed successfully" << std::endl;
        }
        
        cycle_count++;
        std::this_thread::sleep_for(SCAN_TIME);
    }

    std::cout << "[PLC] Logic thread stopped after " << cycle_count << " cycles.\n";
}