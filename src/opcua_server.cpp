#include "opcua_server.h"
#include <iostream>
#include <signal.h>
#include "device_config.h"

OpcUaServer::OpcUaServer(modbus_mapping_t* mapping) 
    : mb_mapping(mapping), running(false) {
    // Get the OPC UA server configuration
    const auto& config = DeviceConfig::getOpcUaConfig();
    
    server = UA_Server_new();
    UA_ServerConfig* server_config = UA_Server_getConfig(server);
    
    // Initialize with configuration settings
    UA_StatusCode retval = UA_ServerConfig_setMinimal(server_config, static_cast<UA_UInt16>(config.port), nullptr);
    if (retval != UA_STATUSCODE_GOOD) {
        throw std::runtime_error("Failed to set server configuration");
    }
    
    // Set server name from configuration
    UA_LocalizedText_clear(&server_config->applicationDescription.applicationName);
    server_config->applicationDescription.applicationName = 
        UA_LOCALIZEDTEXT_ALLOC("en-US", const_cast<char*>(config.server_name.c_str()));
    
    // Update the application URI from configuration
    UA_String_clear(&server_config->applicationDescription.applicationUri);
    server_config->applicationDescription.applicationUri = 
        UA_STRING_ALLOC(config.application_uri.c_str());
    
    // Set network address listeners if specified
    if (config.listen_address != "0.0.0.0" && config.listen_address != "localhost") {
        // For custom bind addresses, we might need to set more options here
    }
    
    std::cout << "[OPC UA] Server created with endpoint opc.tcp://" 
              << config.listen_address << ":" << config.port << std::endl;
}

OpcUaServer::~OpcUaServer() {
    stop();
    UA_Server_delete(server);
}

void OpcUaServer::runEventLoop() {
    while (running) {
        UA_Server_run_iterate(server, true);
    }
}

bool OpcUaServer::start() {
    // Get the device configuration for naming
    const auto& device_info = DeviceConfig::getDeviceInfo();
    
    // Create a folder for our tags
    UA_NodeId folderId;
    UA_ObjectAttributes oAttr = UA_ObjectAttributes_default;
    
    // Use device name from configuration
    std::string folderNameStr = device_info.slave_name + " Tags";
    char* folderName = strdup(folderNameStr.c_str());
    char* locale = strdup("en-US");
    oAttr.displayName = UA_LOCALIZEDTEXT(locale, folderName);
    
    UA_Server_addObjectNode(server, UA_NODEID_NULL,
                          UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                          UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                          UA_QUALIFIEDNAME(1, folderName),
                          UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE),
                          oAttr, NULL, &folderId);

    std::cout << "[OPC UA] Created folder '" << folderNameStr << "'" << std::endl;

    free(folderName);
    free(locale);

    // Create variables for all tags
    for (const auto& tag : tags) {
        addVariable(tag.second);
        std::cout << "[OPC UA] Added tag: " << tag.first << std::endl;
    }

    // Add periodic callback for updates
    UA_Server_addRepeatedCallback(server, 
                                updateCallback,
                                this,
                                100, // 100ms update interval
                                NULL);

    running = true;
    UA_StatusCode retval = UA_Server_run_startup(server);
    if (retval != UA_STATUSCODE_GOOD) {
        std::cerr << "[OPC UA] Server startup failed with status code: " << retval << std::endl;
        return false;
    }

    // Start the event loop in a separate thread
    event_loop_thread = std::thread(&OpcUaServer::runEventLoop, this);
    std::cout << "[OPC UA] Server started successfully" << std::endl;
    return true;
}

void OpcUaServer::stop() {
    if (running) {
        running = false;
        
        // Wait for event loop thread to exit before calling shutdown
        if (event_loop_thread.joinable()) {
            event_loop_thread.join();
        }
        
        // Now it's safe to call shutdown
        UA_Server_run_shutdown(server);
        std::cout << "[OPC UA] Server stopped" << std::endl;
    }
}

void OpcUaServer::addTag(const std::string& name, uint16_t modbusAddress, TagInfo::Type type) {
    TagInfo tag;
    tag.name = name;
    tag.modbusAddress = modbusAddress;
    tag.type = type;
    tags[name] = tag;

    if (running) {
        addVariable(tag);
    }
}

UA_NodeId OpcUaServer::addVariable(const TagInfo& tag) {
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    
    // Create non-const copies for the API
    char* name = strdup(tag.name.c_str());
    char* locale = strdup("en-US");
    
    attr.displayName = UA_LOCALIZEDTEXT(locale, name);
    // Set to read and events for subscriptions
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_STATUSWRITE;
    // Enable value callbacks
    attr.valueRank = UA_VALUERANK_SCALAR;
    // Set minimum sampling interval to 100ms
    attr.minimumSamplingInterval = 100.0;
    
    // Set data type based on tag type
    if (tag.type == TagInfo::Type::Coil || tag.type == TagInfo::Type::DiscreteInput) {
        attr.dataType = UA_TYPES[UA_TYPES_BOOLEAN].typeId;
        UA_Boolean value = false;
        UA_Variant_setScalar(&attr.value, &value, &UA_TYPES[UA_TYPES_BOOLEAN]);
    } else {
        attr.dataType = UA_TYPES[UA_TYPES_UINT16].typeId;
        UA_UInt16 value = 0;
        UA_Variant_setScalar(&attr.value, &value, &UA_TYPES[UA_TYPES_UINT16]);
    }

    UA_NodeId nodeId;
    UA_NodeId_init(&nodeId);
    nodeId.identifierType = UA_NODEIDTYPE_STRING;
    nodeId.namespaceIndex = 1;
    nodeId.identifier.string = UA_String_fromChars(name);

    UA_Server_addVariableNode(server, nodeId,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                            UA_QUALIFIEDNAME(1, name),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                            attr, NULL, NULL);
    
    // Note: The ValueBackend approach isn't compatible with this version of open62541
    // We're keeping the other subscription enhancements (accessLevel, minimumSamplingInterval)
    
    free(name);
    free(locale);
    
    return nodeId;
}

void OpcUaServer::updateCallback(UA_Server* /* server */, void* data) {
    OpcUaServer* self = static_cast<OpcUaServer*>(data);
    if (self->running) {
        self->updateValues();
    }
}

void OpcUaServer::updateValues() {
    UA_Variant value;
    static int update_counter = 0;
    
    // Print debug info every 10 updates (approximately every second)
    if (++update_counter % 10 == 0) {
        std::cout << "[OPC UA] Updating tag values (update #" << update_counter << ")" << std::endl;
    }
    
    for (const auto& tag : tags) {
        char* name = strdup(tag.first.c_str());
        
        UA_NodeId nodeId;
        UA_NodeId_init(&nodeId);
        nodeId.identifierType = UA_NODEIDTYPE_STRING;
        nodeId.namespaceIndex = 1;
        nodeId.identifier.string = UA_String_fromChars(name);
        
        UA_Variant_init(&value);
        
        switch (tag.second.type) {
            case TagInfo::Type::Coil: {
                UA_Boolean boolValue = mb_mapping->tab_bits[tag.second.modbusAddress];
                UA_Variant_setScalar(&value, &boolValue, &UA_TYPES[UA_TYPES_BOOLEAN]);
                
                // Debug output for value changes
                if (update_counter % 10 == 0) {
                    std::cout << "[OPC UA] Tag " << tag.first << " (Coil): " << (boolValue ? "TRUE" : "FALSE") << std::endl;
                }
                break;
            }
            case TagInfo::Type::DiscreteInput: {
                UA_Boolean boolValue = mb_mapping->tab_input_bits[tag.second.modbusAddress];
                UA_Variant_setScalar(&value, &boolValue, &UA_TYPES[UA_TYPES_BOOLEAN]);
                
                // Debug output for value changes
                if (update_counter % 10 == 0) {
                    std::cout << "[OPC UA] Tag " << tag.first << " (DiscreteInput): " << (boolValue ? "TRUE" : "FALSE") << std::endl;
                }
                break;
            }
            case TagInfo::Type::HoldingRegister: {
                UA_UInt16 intValue = mb_mapping->tab_registers[tag.second.modbusAddress];
                UA_Variant_setScalar(&value, &intValue, &UA_TYPES[UA_TYPES_UINT16]);
                
                // Debug output for value changes
                if (update_counter % 10 == 0) {
                    std::cout << "[OPC UA] Tag " << tag.first << " (HoldingRegister): " << intValue << std::endl;
                }
                break;
            }
            case TagInfo::Type::InputRegister: {
                UA_UInt16 intValue = mb_mapping->tab_input_registers[tag.second.modbusAddress];
                UA_Variant_setScalar(&value, &intValue, &UA_TYPES[UA_TYPES_UINT16]);
                
                // Debug output for value changes
                if (update_counter % 10 == 0) {
                    std::cout << "[OPC UA] Tag " << tag.first << " (InputRegister): " << intValue << std::endl;
                }
                break;
            }
        }
        
        UA_Server_writeValue(server, nodeId, value);
        UA_NodeId_clear(&nodeId);
        free(name);
    }
} 