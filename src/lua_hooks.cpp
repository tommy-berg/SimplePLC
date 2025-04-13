#include "lua_hooks.h"
#include <iostream>

LuaHooks::LuaHooks(const std::string& script) {
    L = luaL_newstate();
    luaL_openlibs(L);
    if (luaL_dofile(L, script.c_str()) != LUA_OK) {
        std::cerr << "Failed to load Lua script: " << lua_tostring(L, -1) << "\n";
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
