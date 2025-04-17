#pragma once
#include <lua.hpp>
#include <string>
#include <modbus.h>

class LuaHooks {
public:
    LuaHooks(const std::string& script);
    ~LuaHooks();
    bool override_register(int address, uint16_t& value_out);
    void update_all_registers(modbus_mapping_t* mapping);

private:
    lua_State* L;

};
