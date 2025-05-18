#pragma once

#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <modbus.h>
#include <string>
#include <map>
#include <thread>
#include <atomic>

struct TagInfo {
    enum class Type {
        Coil,
        DiscreteInput,
        HoldingRegister,
        InputRegister
    };
    
    std::string name;
    uint16_t modbusAddress;
    Type type;
};

class OpcUaServer {
public:
    OpcUaServer(modbus_mapping_t* mapping);
    ~OpcUaServer();
    
    bool start();
    void stop();
    
    void addTag(const std::string& name, uint16_t modbusAddress, TagInfo::Type type);
    
private:
    UA_Server* server;
    modbus_mapping_t* mb_mapping;
    std::map<std::string, TagInfo> tags;
    std::atomic<bool> running;
    std::thread event_loop_thread;
    
    UA_NodeId addVariable(const TagInfo& tag);
    static void updateCallback(UA_Server* server, void* data);
    void updateValues();
    void runEventLoop();
    
    // Value callback for handling writes - must match void return type required by UA_ValueCallback
    static void writeVariableCallback(UA_Server *server,
                                     const UA_NodeId *sessionId, void *sessionContext,
                                     const UA_NodeId *nodeId, void *nodeContext,
                                     const UA_NumericRange *range, const UA_DataValue *data);
    
    // Helper method to find tag by node ID
    TagInfo* findTagByNodeId(const UA_NodeId* nodeId);
}; 