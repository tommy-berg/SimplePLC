/**
 * @file device_config.cpp
 * @brief Implementation of DeviceConfig class for managing PLC configuration
 * 
 * This file implements the configuration loading and access methods for
 * the SimplePLC device, including Modbus and OPC UA server settings.
 */
#include "device_config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdint>

// Static configuration instances with default values
// These store the currently loaded configuration values
static DeviceInfo device;                    // Device identification information
static ModbusServerConfig modbus_config;     // Modbus server configuration 
static OpcUaServerConfig opcua_config;       // OPC UA server configuration
static std::vector<TagDefinition> tags;      // Tag definitions for data points

/**
 * Trims leading and trailing whitespace from a string
 * @param s String to trim
 */
static void trim(std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    size_t end = s.find_last_not_of(" \t");
    if (start == std::string::npos || end == std::string::npos) {
        s.clear();
    } else {
        s = s.substr(start, end - start + 1);
    }
}

// Helper function to split a string by a delimiter
static std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    
    while (std::getline(tokenStream, token, delimiter)) {
        trim(token);
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    
    return tokens;
}

/**
 * Loads configuration from the specified INI file.
 * Format expected:
 * [Section]
 * key=value
 * 
 * Special handling for [Tags] section where entries are in CSV format:
 * name,address,type
 */
void DeviceConfig::load(const std::string& ini_file) {
    std::ifstream file(ini_file);
    if (!file) {
        std::cerr << "[Config] Could not open " << ini_file << ". Using defaults." << std::endl;
        return;
    }

    std::cout << "[Config] Loading configuration from " << ini_file << std::endl;
    
    // Clear the tags list before loading
    tags.clear();
    
    std::string line, current_section;
    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        // Process section headers - format: [SectionName]
        if (line.front() == '[' && line.back() == ']') {
            current_section = line.substr(1, line.size() - 2);
            continue;
        }

        // Process key=value pairs for configuration settings
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            trim(key);
            trim(value);

            // Route settings to appropriate configuration structures
            if (current_section == "Device") {
                if (key == "slave_name") {
                    device.slave_name = value;
                }
                else if (key == "device_identification") {
                    device.device_id_string = value;
                }
                else if (key == "slave_id") {
                    try {
                        device.slave_id = static_cast<uint8_t>(std::stoi(value));
                    } catch (const std::exception& e) {
                        std::cerr << "[Config] Error parsing slave_id: " << e.what() << std::endl;
                    }
                }
                else if (key == "run_indicator") {
                    try {
                        device.run_indicator = static_cast<uint8_t>(std::stoi(value));
                    } catch (const std::exception& e) {
                        std::cerr << "[Config] Error parsing run_indicator: " << e.what() << std::endl;
                    }
                }
                else if (key == "run_script") {
                    device.run_script = value;
                    std::cout << "[Config] Script to run: " << value << std::endl;
                }
            }
            else if (current_section == "ModbusServer") {
                if (key == "listen") {
                    modbus_config.listen_address = value;
                }
                else if (key == "port") {
                    try {
                        modbus_config.port = std::stoi(value);
                    } catch (const std::exception& e) {
                        std::cerr << "[Config] Error parsing Modbus port: " << e.what() << std::endl;
                    }
                }
                else if (key == "max_connections") {
                    try {
                        modbus_config.max_connections = std::stoi(value);
                    } catch (const std::exception& e) {
                        std::cerr << "[Config] Error parsing max_connections: " << e.what() << std::endl;
                    }
                }
                else if (key == "mapping_size") {
                    try {
                        modbus_config.mapping_size = std::stoi(value);
                    } catch (const std::exception& e) {
                        std::cerr << "[Config] Error parsing mapping_size: " << e.what() << std::endl;
                    }
                }
            }
            else if (current_section == "OPCUA") {
                if (key == "listen") {
                    opcua_config.listen_address = value;
                }
                else if (key == "port") {
                    try {
                        opcua_config.port = std::stoi(value);
                    } catch (const std::exception& e) {
                        std::cerr << "[Config] Error parsing OPC UA port: " << e.what() << std::endl;
                    }
                }
                else if (key == "server_name") {
                    opcua_config.server_name = value;
                }
                else if (key == "application_uri") {
                    opcua_config.application_uri = value;
                }
            }
        }
        // Process tag definitions in CSV format (name,address,type)
        else if (current_section == "Tags") {
            // Parse CSV format for tags: name,address,type
            auto parts = split(line, ',');
            if (parts.size() >= 3) {
                try {
                    TagDefinition tag;
                    tag.name = parts[0];
                    tag.address = static_cast<uint16_t>(std::stoi(parts[1]));
                    tag.type = std::stoi(parts[2]);
                    
                    // Add the tag to our list
                    tags.push_back(tag);
                    std::cout << "[Config] Added tag: " << tag.name << " (address: " << tag.address 
                              << ", type: " << tag.type << ")" << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "[Config] Error parsing tag definition '" << line << "': " << e.what() << std::endl;
                }
            } else {
                std::cerr << "[Config] Invalid tag format: " << line << std::endl;
            }
        }
    }
    
    // Log the loaded configuration
    std::cout << "[Config] Device: " << device.device_id_string
              << ", Slave ID: " << static_cast<int>(device.slave_id) << std::endl;
    std::cout << "[Config] Modbus Server: " << modbus_config.listen_address
              << ":" << modbus_config.port << std::endl;
    std::cout << "[Config] OPC UA Server: " << opcua_config.listen_address
              << ":" << opcua_config.port << std::endl;
    std::cout << "[Config] Loaded " << tags.size() << " tag definitions" << std::endl;
}

const DeviceInfo& DeviceConfig::getDeviceInfo() {
    return device;
}

const ModbusServerConfig& DeviceConfig::getModbusConfig() {
    return modbus_config;
}

const OpcUaServerConfig& DeviceConfig::getOpcUaConfig() {
    return opcua_config;
}

const std::vector<TagDefinition>& DeviceConfig::getTags() {
    return tags;
}
