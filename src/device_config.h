#pragma once
#include <string>
#include <cstdint>
#include <vector>

/**
 * @struct DeviceInfo
 * @brief Holds device identification and slave configuration
 */
struct DeviceInfo {
    std::string slave_name = "SimplePLC";        
    std::string device_id_string = "SimplePLC v0.1";  
    uint8_t slave_id = 1;
    uint8_t run_indicator = 1;
};

/**
 * @struct ModbusServerConfig
 * @brief Holds Modbus server configuration
 */
struct ModbusServerConfig {
    std::string listen_address = "0.0.0.0";
    int port = 502;
    int max_connections = 5;
    int mapping_size = 255;
};

/**
 * @struct OpcUaServerConfig
 * @brief Holds OPC UA server configuration
 */
struct OpcUaServerConfig {
    std::string listen_address = "0.0.0.0";
    int port = 4840;
    std::string server_name = "SimplePLC OPC UA Server";
    std::string application_uri = "urn:simpleplc.opcua.server";
};

/**
 * @struct TagDefinition
 * @brief Holds tag configuration for OPC UA
 */
struct TagDefinition {
    std::string name;
    uint16_t address;
    int type;  // 0=Coil, 1=DiscreteInput, 2=HoldingRegister, 3=InputRegister
};

/**
 * @class DeviceConfig
 * @brief Manages application configuration from settings.ini
 */
class DeviceConfig {
public:
    /**
     * @brief Load configuration from ini file
     * 
     * If the file cannot be opened or a setting is missing, default values will be used.
     * 
     * @param ini_file Path to the configuration file
     */
    static void load(const std::string& ini_file = "settings.ini");
    
    /**
     * @brief Get device information
     * @return Const reference to device information
     */
    static const DeviceInfo& getDeviceInfo();
    
    /**
     * @brief Get Modbus server configuration
     * @return Const reference to Modbus server configuration
     */
    static const ModbusServerConfig& getModbusConfig();
    
    /**
     * @brief Get OPC UA server configuration
     * @return Const reference to OPC UA server configuration
     */
    static const OpcUaServerConfig& getOpcUaConfig();
    
    /**
     * @brief Get tag definitions
     * @return Const reference to vector of tag definitions
     */
    static const std::vector<TagDefinition>& getTags();
};
