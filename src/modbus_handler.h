#pragma once
#include <modbus.h>
#include <cstdint>
#include <memory>

// Forward declaration
class ModbusServer;

/**
 * @class ModbusHandler
 * @brief Handles Modbus protocol functions and data processing
 * 
 * This class provides static methods to handle different Modbus protocol
 * function codes, process requests and generate responses.
 */
class ModbusHandler {
public:
    /**
     * @brief Initialize Lua hooks for simulation
     * 
     * @param mapping Modbus data mapping to update periodically
     * @param server Pointer to the ModbusServer instance for mutex locking
     */
    static void init_lua_hooks(modbus_mapping_t* mapping, ModbusServer* server = nullptr);
    
    /**
     * @brief Handles the Report Slave ID function (0x11)
     * 
     * @param socket Client socket descriptor
     * @param ctx Modbus context
     * @param req Request data
     * @param len Length of request
     */
    static void send_report_slave_id(int socket, modbus_t* ctx, const uint8_t* req, int len);
    
    /**
     * @brief Handles the Read Device Identification function (0x2B/0x0E)
     * 
     * @param socket Client socket descriptor
     * @param ctx Modbus context
     * @param req Request data
     * @param len Length of request
     */
    static void send_read_device_id(int socket, modbus_t* ctx, const uint8_t* req, int len);
    
    /**
     * @brief Handles standard Modbus functions (read/write registers & bits)
     * 
     * Processes standard Modbus functions like:
     * - Read Coils (0x01)
     * - Read Discrete Inputs (0x02)
     * - Read Holding Registers (0x03)
     * - Read Input Registers (0x04)
     * - Write Single Coil (0x05)
     * - Write Single Register (0x06)
     * - Write Multiple Coils (0x0F)
     * - Write Multiple Registers (0x10)
     * 
     * @param ctx Modbus context
     * @param query Request data
     * @param rc Length of request
     * @param mapping Modbus data mapping
     */
    static void handle_standard_function(modbus_t* ctx, const uint8_t* query, int rc, modbus_mapping_t* mapping);
};
