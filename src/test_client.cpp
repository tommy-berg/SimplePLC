#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/plugin/log_stdout.h>
#include <iostream>

int main() {
    UA_Client *client = UA_Client_new();
    UA_ClientConfig_setDefault(UA_Client_getConfig(client));

    // Connect to the server
    UA_StatusCode retval = UA_Client_connect(client, "opc.tcp://localhost:4840");
    if(retval != UA_STATUSCODE_GOOD) {
        std::cerr << "Could not connect to OPC UA server!" << std::endl;
        UA_Client_delete(client);
        return 1;
    }
    std::cout << "Connected to OPC UA server" << std::endl;

    // Browse for tags
    UA_BrowseRequest bReq;
    UA_BrowseRequest_init(&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new();
    bReq.nodesToBrowseSize = 1;
    bReq.nodesToBrowse[0].nodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL;

    UA_BrowseResponse bResp = UA_Client_Service_browse(client, bReq);
    
    if(bResp.resultsSize > 0) {
        for(size_t i = 0; i < bResp.results[0].referencesSize; i++) {
            UA_ReferenceDescription *ref = &(bResp.results[0].references[i]);
            if(ref->nodeId.nodeId.identifierType == UA_NODEIDTYPE_STRING) {
                std::cout << "Found node: " << ref->displayName.text.data << std::endl;
                
                // Try to read the value
                UA_Variant value;
                UA_Variant_init(&value);
                UA_StatusCode status = UA_Client_readValueAttribute(client, ref->nodeId.nodeId, &value);
                
                if(status == UA_STATUSCODE_GOOD) {
                    if(UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_BOOLEAN])) {
                        bool val = *(UA_Boolean*)value.data;
                        std::cout << "  Value (boolean): " << (val ? "true" : "false") << std::endl;
                    }
                    else if(UA_Variant_hasScalarType(&value, &UA_TYPES[UA_TYPES_UINT16])) {
                        uint16_t val = *(UA_UInt16*)value.data;
                        std::cout << "  Value (uint16): " << val << std::endl;
                    }
                }
                UA_Variant_clear(&value);
            }
        }
    }

    UA_BrowseRequest_clear(&bReq);
    UA_BrowseResponse_clear(&bResp);

    UA_Client_disconnect(client);
    UA_Client_delete(client);
    return 0;
} 