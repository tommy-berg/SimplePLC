#include "lua_hooks.h"
#include <iostream>

LuaHooks::LuaHooks(const std::string& script) {
    L = luaL_newstate();
    luaL_openlibs(L);
    if (luaL_dofile(L, script.c_str()) != LUA_OK) {
        std::cerr << "[Error] Failed to load Lua script: " << lua_tostring(L, -1) << "\n";
        L = nullptr;
    }
}

LuaHooks::~LuaHooks() {
    if (L) lua_close(L);
}

bool LuaHooks::override_register(int address, uint16_t& value_out) {
    if (!L) return false;

    lua_getglobal(L, "override_register");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }

    lua_pushinteger(L, address);

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        std::cerr << "Lua error: " << lua_tostring(L, -1) << "\n";
        lua_pop(L, 1);
        return false;
    }

    if (!lua_isinteger(L, -1)) {
        lua_pop(L, 1);
        return false;
    }

    value_out = static_cast<uint16_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return true;
}

void LuaHooks::update_all_registers(modbus_mapping_t* mapping) {
    if (!mapping) return;

    // Update coils (0xxxx)
    for (int i = 0; i < mapping->nb_bits; i++) {
        uint16_t value;
        if (override_register(i, value)) {
            mapping->tab_bits[i] = value ? 1 : 0;
        }
    }

    // Update discrete inputs (1xxxx)
    for (int i = 0; i < mapping->nb_input_bits; i++) {
        uint16_t value;
        if (override_register(10000 + i, value)) {
            mapping->tab_input_bits[i] = value ? 1 : 0;
        }
    }

    // Update input registers (3xxxx)
    for (int i = 0; i < mapping->nb_input_registers; i++) {
        uint16_t value;
        if (override_register(30000 + i, value)) {
            mapping->tab_input_registers[i] = value;
        }
    }

    // Update holding registers (4xxxx)
    for (int i = 0; i < mapping->nb_registers; i++) {
        uint16_t value;
        if (override_register(40000 + i, value)) {
            mapping->tab_registers[i] = value;
        }
    }
}
