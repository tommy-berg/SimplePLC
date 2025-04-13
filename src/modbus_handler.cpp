#include "modbus_handler.h"
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <random>
#include "lua_hooks.h"

static LuaHooks hooks("script.lua"); 

void ModbusHandler::send_report_slave_id(int socket, modbus_t*, const uint8_t* req, int) {
    uint8_t response[256];
    const char* device_name = "PM710PowerMeter";
    uint8_t slave_id = 0xFA, run_indicator = 0xFF;
    size_t name_len = std::strlen(device_name);

    std::memcpy(response, req, 6); // TID, PID
    response[6] = req[6]; // Unit ID
    response[7] = 0x11; // Function code
    response[8] = 2 + name_len; // Length of the response
    response[9] = slave_id;
    response[10] = run_indicator;
    std::memcpy(&response[11], device_name, name_len);

    uint16_t len = 3 + response[8];
    response[4] = (len >> 8) & 0xFF;
    response[5] = len & 0xFF;
    int total = 6 + len;

    if (send(socket, response, total, 0) < 0)
        std::cerr << "send_report_slave_id failed" << std::endl;
}

void ModbusHandler::send_read_device_id(int socket, modbus_t*, const uint8_t* req, int) {
    uint8_t response[256];
    std::memcpy(response, req, 6);
    response[6] = req[6];
    response[7] = 0x2B;
    response[8] = 0x0E;
    response[9] = 0x01;
    response[10] = 0x01;
    response[11] = 0x00;
    response[12] = 0x00;
    response[13] = 0x01;

    const char* info = "Schneider Electric PM710 v03.110";
    uint8_t len = std::strlen(info);
    response[14] = 0x00;
    response[15] = len;
    std::memcpy(&response[16], info, len);

    uint16_t data_len = (16 + len) - 6;
    response[4] = (data_len >> 8) & 0xFF;
    response[5] = data_len & 0xFF;
    if (send(socket, response, 16 + len, 0) < 0)
        std::cerr << "send_read_device_id failed" << std::endl;
}

void ModbusHandler::handle_standard_function(modbus_t* ctx, const uint8_t* query, int rc, modbus_mapping_t* mapping) {
    uint8_t function_code = query[7];

    if (function_code == MODBUS_FC_READ_HOLDING_REGISTERS) {
        int addr = (query[8] << 8) | query[9];
        int count = (query[10] << 8) | query[11];

        for (int i = 0; i < count; ++i) {
            int addr = addr + i;
            uint16_t lua_value = 0;
            if (hooks.override_register(addr, lua_value)) {
                mapping->tab_registers[addr] = lua_value;
            }
        }

    }

    if (modbus_reply(ctx, const_cast<uint8_t*>(query), rc, mapping) == -1) {
        std::cerr << "Error in modbus_reply: " << modbus_strerror(errno) << std::endl;
    }
}
