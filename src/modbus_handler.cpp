#include "modbus_handler.h"
#include <cstring>
#include <iostream>
#include "platform.h"
#include <sys/socket.h>
#include <random>
#include "lua_hooks.h"
#include "device_config.h"

static LuaHooks hooks("world.lua"); 

void ModbusHandler::send_report_slave_id(int socket, modbus_t*, const uint8_t* req, int) {
    
    // Get the device configuration
    const auto& config = DeviceConfig::get();
    const char* device_name = config.slave_name.c_str();
    uint8_t slave_id = config.slave_id;
    uint8_t run_indicator = config.run_indicator;
    size_t name_len = std::strlen(device_name);
    
    uint8_t response[256];    

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

    const auto& config = DeviceConfig::get();
    const char* device_info = config.device_id_string.c_str();
    uint8_t len = std::strlen(device_info);

    response[14] = 0x00;
    response[15] = len;
    std::memcpy(&response[16], device_info, len);

    uint16_t data_len = (16 + len) - 6;
    response[4] = (data_len >> 8) & 0xFF;
    response[5] = data_len & 0xFF;
    if (send(socket, response, 16 + len, 0) < 0)
        std::cerr << "send_read_device_id failed" << std::endl;
}

void ModbusHandler::handle_standard_function(modbus_t* ctx, const uint8_t* query, int rc, modbus_mapping_t* mapping) {
    // Update all registers first
    hooks.update_all_registers(mapping);

    // Now handle the specific write operations
    uint8_t function_code = query[7];
    int addr = (query[8] << 8) | query[9];

    switch (function_code) {
        case MODBUS_FC_WRITE_SINGLE_COIL:       // FC 5
            mapping->tab_bits[addr] = (query[10] == 0xFF) ? 1 : 0;
            break;

        case MODBUS_FC_WRITE_SINGLE_REGISTER:   // FC 6
            mapping->tab_registers[addr] = (query[10] << 8) | query[11];
            break;

        case MODBUS_FC_WRITE_MULTIPLE_COILS:    // FC 15
            {
                int count = (query[10] << 8) | query[11];
                uint8_t byte_count = query[12];
                for (int i = 0; i < count; ++i) {
                    int byte_index = i / 8;
                    int bit_index = i % 8;
                    if (byte_index < byte_count) {
                        mapping->tab_bits[addr + i] = (query[13 + byte_index] >> bit_index) & 0x01;
                    }
                }
            }
            break;

        case MODBUS_FC_WRITE_MULTIPLE_REGISTERS: // FC 16
            {
                int count = (query[10] << 8) | query[11];
                uint8_t byte_count = query[12];
                for (int i = 0; i < count && (i * 2) < byte_count; ++i) {
                    mapping->tab_registers[addr + i] = (query[13 + i * 2] << 8) | query[14 + i * 2];
                }
            }
            break;
    }

    if (modbus_reply(ctx, const_cast<uint8_t*>(query), rc, mapping) == -1) {
        std::cerr << "Error in modbus_reply: " << modbus_strerror(errno) << std::endl;
    }
}
