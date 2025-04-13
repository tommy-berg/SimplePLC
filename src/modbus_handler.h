#pragma once
#include <modbus.h>
#include <cstdint>

class ModbusHandler {
public:
    static void send_report_slave_id(int socket, modbus_t* ctx, const uint8_t* req, int len);
    static void send_read_device_id(int socket, modbus_t* ctx, const uint8_t* req, int len);
    static void handle_standard_function(modbus_t* ctx, const uint8_t* query, int rc, modbus_mapping_t* mapping);
};
