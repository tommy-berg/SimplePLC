#include "platform.h"
#include "server.h"
#include "device_config.h"
#include "opcua_server.h"
#include <iostream>
#include <memory>
#include <thread>
#include <atomic>
#include <csignal>
#include <condition_variable>
#include <mutex>
#include <filesystem>

namespace {
    std::atomic<bool> running{true};
    std::condition_variable shutdown_cv;
    std::mutex shutdown_mutex;

    void signal_handler(int) {
        running = false;
        shutdown_cv.notify_all();
    }
}

int main(int argc, char* argv[]) {
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Print program banner
    std::cout << "SimplePLC - Combined Modbus and OPC UA Server" << std::endl;
    std::cout << "------------------------------------------------" << std::endl;
    
    // Determine configuration file path
    std::string config_file = "settings.ini";
    
    // Check if a config file was specified as a command line argument
    if (argc > 1) {
        config_file = argv[1];
    }
    
    // Load configuration
    try {
        // Check if the specified config file exists
        if (std::filesystem::exists(config_file)) {
            std::cout << "[Main] Using configuration from: " << config_file << std::endl;
        } else {
            std::cout << "[Main] Configuration file not found: " << config_file << std::endl;
            std::cout << "[Main] Using default settings" << std::endl;
        }
        
        DeviceConfig::load(config_file);
    } catch (const std::exception& e) {
        std::cerr << "[Main] Error loading configuration: " << e.what() << std::endl;
        std::cerr << "[Main] Continuing with default settings" << std::endl;
    }
    
    // Create the Modbus server
    std::cout << "[Main] Starting Modbus server..." << std::endl;
    ModbusServer modbus_server;
    
    // Create and start OPC UA server
    std::cout << "[Main] Starting OPC UA server..." << std::endl;
    std::unique_ptr<OpcUaServer> opcua_server(new OpcUaServer(modbus_server.get_mapping()));
    
    // Add tags from configuration
    const auto& tags = DeviceConfig::getTags();
    if (!tags.empty()) {
        std::cout << "[Main] Adding " << tags.size() << " tags from configuration..." << std::endl;
        for (const auto& tag : tags) {
            TagInfo::Type type;
            switch (tag.type) {
                case 0: type = TagInfo::Type::Coil; break;
                case 1: type = TagInfo::Type::DiscreteInput; break;
                case 2: type = TagInfo::Type::HoldingRegister; break;
                case 3: type = TagInfo::Type::InputRegister; break;
                default:
                    std::cerr << "[Main] Invalid tag type for " << tag.name << ": " << tag.type << std::endl;
                    continue;
            }
            opcua_server->addTag(tag.name, tag.address, type);
            std::cout << "[Main] Added tag: " << tag.name << std::endl;
        }
    } else {
        // Fallback to default tags if none defined in config
        std::cout << "[Main] No tags defined in configuration, using defaults..." << std::endl;
        opcua_server->addTag("Conveyor1_Running", 0, TagInfo::Type::Coil);
        opcua_server->addTag("Sensor1_Active", 0, TagInfo::Type::DiscreteInput);
        opcua_server->addTag("Speed_Setpoint", 0, TagInfo::Type::HoldingRegister);
        opcua_server->addTag("Temperature1", 0, TagInfo::Type::InputRegister);
    }
    
    // Start OPC UA server
    if (!opcua_server->start()) {
        std::cerr << "[Main] Failed to start OPC UA server" << std::endl;
        return 1;
    }
    
    const auto& opcua_config = DeviceConfig::getOpcUaConfig();
    std::cout << "[Main] OPC UA server started on opc.tcp://" 
              << opcua_config.listen_address << ":" << opcua_config.port << std::endl;
    
    // Wait for shutdown signal
    {
        std::unique_lock<std::mutex> lock(shutdown_mutex);
        shutdown_cv.wait(lock, []{ return !running.load(); });
    }
    
    // Cleanup
    std::cout << "\nShutting down..." << std::endl;
    opcua_server->stop();
    
    std::cout << "Shutdown complete" << std::endl;
    return 0;
}
