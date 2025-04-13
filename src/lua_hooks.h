#pragma once
#include <lua.hpp>
#include <string>

class LuaHooks {
public:
    LuaHooks(const std::string& script);
    ~LuaHooks();
    bool override_register(int address, uint16_t& value_out);

private:
    lua_State* L;

};
