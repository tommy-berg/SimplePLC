#include "platform.h"  // Include platform.h first for platform-specific definitions
#include "modbus_handler.h"
#include <cstring>
#include <iostream>
#include <random>
#include "lua_hooks.h"
#include "device_config.h"
#include "server.h"

// Global LuaHooks instance used by ModbusHandler
static std::unique_ptr<LuaHooks> hooks;
// Reference to the ModbusServer for locking (will be set in init_lua_hooks)
static ModbusServer* server_instance = nullptr;

// Initialize and start Lua hooks
void ModbusHandler::init_lua_hooks(modbus_mapping_t* mapping, ModbusServer* server) {
    server_instance = server;
    
    if (!hooks) {
        try {
            hooks = std::make_unique<LuaHooks>("world.plc");
            hooks->start_periodic_updates(mapping, 100); // Update every 100ms
            std::cout << "[Modbus] Initialized Lua hooks with world.plc" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[Modbus] Failed to initialize Lua hooks: " << e.what() << std::endl;
        }
    }
}

void ModbusHandler::send_report_slave_id(int socket, modbus_t*, const uint8_t* req, int) {
    try {
        // Get the device configuration
        const auto& config = DeviceConfig::getDeviceInfo();
        const char* device_name = config.slave_name.c_str();
        uint8_t slave_id = config.slave_id;
        uint8_t run_indicator = config.run_indicator;
        size_t name_len = std::strlen(device_name);
        
        if (name_len > 240) {  // Limit name length to avoid buffer overflow
            std::cerr << "[Modbus] Device name too long, truncating" << std::endl;
            name_len = 240;
        }
        
        uint8_t response[256] = {0};    

        // Copy header fields
        std::memcpy(response, req, 6);  // TID, PID
        response[6] = req[6];           // Unit ID
        response[7] = 0x11;             // Function code
        response[8] = static_cast<uint8_t>(2 + name_len);     // Length of the response
        response[9] = slave_id;
        response[10] = run_indicator;
        
        // Copy device name
        std::memcpy(&response[11], device_name, name_len);

        // Calculate total message length
        uint16_t len = 3 + response[8];
        response[4] = static_cast<uint8_t>((len >> 8) & 0xFF);
        response[5] = static_cast<uint8_t>(len & 0xFF);
        int total = 6 + len;

        // Send the response
        if (::send(socket, reinterpret_cast<const char*>(response), static_cast<size_t>(total), 0) < 0) {
#ifdef _WIN32
            std::cerr << "[Modbus] send_report_slave_id failed: " << WSAGetLastError() << std::endl;
#else
            std::cerr << "[Modbus] send_report_slave_id failed: " << strerror(errno) << std::endl;
#endif
        } else {
            std::cout << "[Modbus] Sent Report Slave ID response (" << total << " bytes)" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Modbus] Error in send_report_slave_id: " << e.what() << std::endl;
    }
}

void ModbusHandler::send_read_device_id(int socket, modbus_t*, const uint8_t* req, int) {
    try {
        // Device identification response (MEI Type 0x0E)
        uint8_t response[256] = {0};
        
        // Copy header fields
        std::memcpy(response, req, 6);
        response[6] = req[6];
        response[7] = 0x2B;             // Function code (MEI)
        response[8] = 0x0E;             // MEI Type (Read Device ID)
        response[9] = 0x01;             // ReadDevIdCode
        response[10] = 0x01;            // Conformity level
        response[11] = 0x00;            // More follows
        response[12] = 0x00;            // Next object ID
        response[13] = 0x01;            // Number of objects

        // Get device ID string from configuration
        const auto& config = DeviceConfig::getDeviceInfo();
        const char* device_info = config.device_id_string.c_str();
        size_t len = std::strlen(device_info);
        
        if (len > 235) {  // Limit ID length to avoid buffer overflow
            std::cerr << "[Modbus] Device ID string too long, truncating" << std::endl;
            len = 235;
        }

        // Object ID 0x00 (VendorName)
        response[14] = 0x00;
        response[15] = static_cast<uint8_t>(len);
        std::memcpy(&response[16], device_info, len);

        // Calculate total message length
        uint16_t data_len = static_cast<uint16_t>((16 + len) - 6);
        response[4] = static_cast<uint8_t>((data_len >> 8) & 0xFF);
        response[5] = static_cast<uint8_t>(data_len & 0xFF);
        
        // Send the response
        if (::send(socket, reinterpret_cast<const char*>(response), static_cast<size_t>(16 + len), 0) < 0) {
#ifdef _WIN32
            std::cerr << "[Modbus] send_read_device_id failed: " << WSAGetLastError() << std::endl;
#else
            std::cerr << "[Modbus] send_read_device_id failed: " << strerror(errno) << std::endl;
#endif
        } else {
            std::cout << "[Modbus] Sent Read Device ID response (" << (16 + len) << " bytes)" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Modbus] Error in send_read_device_id: " << e.what() << std::endl;
    }
}

void ModbusHandler::handle_standard_function(modbus_t* /* ctx */, const uint8_t* query, int /* rc */, modbus_mapping_t* mapping) {
    try {
        // Make sure Lua hooks are initialized
        if (!hooks) {
            if (server_instance) {
                init_lua_hooks(mapping, server_instance);
            } else {
                init_lua_hooks(mapping, nullptr);
            }
        }
        
        // Process only write operations - the reply will be handled separately
        uint8_t function_code = query[7];
        
        // Only handle write operations, skip read operations
        switch (function_code) {
            case MODBUS_FC_WRITE_SINGLE_COIL: {       // FC 5
                int addr = (query[8] << 8) | query[9];
                if (addr >= 0 && addr < mapping->nb_bits) {
                    mapping->tab_bits[addr] = (query[10] == 0xFF) ? 1 : 0;
                    std::cout << "[Modbus] Write coil " << addr << " = " 
                              << (mapping->tab_bits[addr] ? "ON" : "OFF") << std::endl;
                } else {
                    std::cerr << "[Modbus] Address out of range for write coil: " << addr << std::endl;
                }
                break;
            }

            case MODBUS_FC_WRITE_SINGLE_REGISTER: {   // FC 6
                int addr = (query[8] << 8) | query[9];
                if (addr >= 0 && addr < mapping->nb_registers) {
                    mapping->tab_registers[addr] = static_cast<uint16_t>((query[10] << 8) | query[11]);
                    std::cout << "[Modbus] Write register " << addr << " = " 
                              << mapping->tab_registers[addr] << std::endl;
                } else {
                    std::cerr << "[Modbus] Address out of range for write register: " << addr << std::endl;
                }
                break;
            }

            case MODBUS_FC_WRITE_MULTIPLE_COILS: {    // FC 15
                int addr = (query[8] << 8) | query[9];
                int count = (query[10] << 8) | query[11];
                uint8_t byte_count = query[12];
                
                if (addr + count > mapping->nb_bits) {
                    std::cerr << "[Modbus] Address range out of bounds for write multiple coils" << std::endl;
                } else {
                    for (int i = 0; i < count; ++i) {
                        int byte_index = i / 8;
                        int bit_index = i % 8;
                        if (byte_index < byte_count) {
                            mapping->tab_bits[addr + i] = (query[13 + byte_index] >> bit_index) & 0x01;
                        }
                    }
                    std::cout << "[Modbus] Write " << count << " coils starting at " << addr << std::endl;
                }
                break;
            }

            case MODBUS_FC_WRITE_MULTIPLE_REGISTERS: { // FC 16
                int addr = (query[8] << 8) | query[9];
                int count = (query[10] << 8) | query[11];
                uint8_t byte_count = query[12];
                
                if (addr + count > mapping->nb_registers) {
                    std::cerr << "[Modbus] Address range out of bounds for write multiple registers" << std::endl;
                } else {
                    for (int i = 0; i < count && (i * 2) < byte_count; ++i) {
                        mapping->tab_registers[addr + i] = static_cast<uint16_t>((query[13 + i * 2] << 8) | query[14 + i * 2]);
                    }
                    std::cout << "[Modbus] Write " << count << " registers starting at " << addr << std::endl;
                }
                break;
            }
                
            // Read functions just need a reply, no preprocessing needed
            case MODBUS_FC_READ_COILS:              // FC 1
            case MODBUS_FC_READ_DISCRETE_INPUTS:    // FC 2
            case MODBUS_FC_READ_HOLDING_REGISTERS:  // FC 3
            case MODBUS_FC_READ_INPUT_REGISTERS:    // FC 4
                // Nothing to do for read functions
                break;
                
            default:
                std::cout << "[Modbus] Received unhandled function code: 0x" 
                         << std::hex << static_cast<int>(function_code) << std::dec << std::endl;
                break;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Modbus] Error in handle_standard_function: " << e.what() << std::endl;
        throw; // Rethrow to allow the caller to handle it
    }
}
