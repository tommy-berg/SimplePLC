#include "opcua/opcua_server.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <format>
#include <set>
#include <vector>

#include <open62541/server.h>
#include <open62541/server_config_default.h>

#include "app/version.h"
#include "net/socket.h"

namespace simpleplc::opcua {

struct Server::Variable {
    Server* server = nullptr;
    TagDef tag;
    UA_NodeId nodeId = UA_NODEID_NULL;
};

namespace {

// open62541 takes char* but copies; tag names cannot contain ':' so this id never collides
constexpr const char* kFolderId = "simpleplc:tags";

char* cstr(const std::string& s) { return const_cast<char*>(s.c_str()); }
char* cstr(const char* s) { return const_cast<char*>(s); }

const UA_DataType* dataType(TagType type) {
    switch (type) {
        case TagType::Bool:    return &UA_TYPES[UA_TYPES_BOOLEAN];
        case TagType::UInt16:  return &UA_TYPES[UA_TYPES_UINT16];
        case TagType::Int16:   return &UA_TYPES[UA_TYPES_INT16];
        case TagType::UInt32:  return &UA_TYPES[UA_TYPES_UINT32];
        case TagType::Int32:   return &UA_TYPES[UA_TYPES_INT32];
        case TagType::Float32: return &UA_TYPES[UA_TYPES_FLOAT];
    }
    return &UA_TYPES[UA_TYPES_UINT16];
}

/// Any numeric or boolean scalar as a double; false for other types. (open62541 has
/// already checked the value against the node's data type before we get here.)
bool toDouble(const UA_Variant& v, double& out) {
    if (!UA_Variant_isScalar(&v) || !v.data) return false;
    const void* d = v.data;
    if (v.type == &UA_TYPES[UA_TYPES_BOOLEAN]) out = *static_cast<const UA_Boolean*>(d) ? 1 : 0;
    else if (v.type == &UA_TYPES[UA_TYPES_SBYTE]) out = *static_cast<const UA_SByte*>(d);
    else if (v.type == &UA_TYPES[UA_TYPES_BYTE]) out = *static_cast<const UA_Byte*>(d);
    else if (v.type == &UA_TYPES[UA_TYPES_INT16]) out = *static_cast<const UA_Int16*>(d);
    else if (v.type == &UA_TYPES[UA_TYPES_UINT16]) out = *static_cast<const UA_UInt16*>(d);
    else if (v.type == &UA_TYPES[UA_TYPES_INT32]) out = *static_cast<const UA_Int32*>(d);
    else if (v.type == &UA_TYPES[UA_TYPES_UINT32]) out = *static_cast<const UA_UInt32*>(d);
    else if (v.type == &UA_TYPES[UA_TYPES_INT64]) out = static_cast<double>(*static_cast<const UA_Int64*>(d));
    else if (v.type == &UA_TYPES[UA_TYPES_UINT64]) out = static_cast<double>(*static_cast<const UA_UInt64*>(d));
    else if (v.type == &UA_TYPES[UA_TYPES_FLOAT]) out = static_cast<double>(*static_cast<const UA_Float*>(d));
    else if (v.type == &UA_TYPES[UA_TYPES_DOUBLE]) out = *static_cast<const UA_Double*>(d);
    else return false;
    return true;
}

UA_StatusCode readValue(UA_Server*, const UA_NodeId*, void*, const UA_NodeId*, void* nodeContext,
                        UA_Boolean includeSourceTimeStamp, const UA_NumericRange* range,
                        UA_DataValue* value) {
    auto* variable = static_cast<Server::Variable*>(nodeContext);
    if (range) {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_BADINDEXRANGEINVALID;
        return UA_STATUSCODE_GOOD;
    }
    const TagDef& tag = variable->tag;
    double x = variable->server->image().readTag(tag);
    union {
        UA_Boolean b;
        UA_UInt16 u16;
        UA_Int16 i16;
        UA_UInt32 u32;
        UA_Int32 i32;
        UA_Float f;
    } typed{};
    switch (tag.type) {
        case TagType::Bool:    typed.b = x != 0.0; break;
        case TagType::UInt16:  typed.u16 = static_cast<UA_UInt16>(x); break;
        case TagType::Int16:   typed.i16 = static_cast<UA_Int16>(x); break;
        case TagType::UInt32:  typed.u32 = static_cast<UA_UInt32>(x); break;
        case TagType::Int32:   typed.i32 = static_cast<UA_Int32>(x); break;
        case TagType::Float32: typed.f = static_cast<UA_Float>(x); break;
    }
    UA_StatusCode rc = UA_Variant_setScalarCopy(&value->value, &typed, dataType(tag.type));
    if (rc != UA_STATUSCODE_GOOD) return rc;
    value->hasValue = true;
    // A link's read tag while its device does not answer: the last value it got, not a live one (Part 8)
    if (variable->server->image().isStale(tag.area, tag.address)) {
        value->hasStatus = true;
        value->status = UA_STATUSCODE_UNCERTAINLASTUSABLEVALUE;
    }
    if (includeSourceTimeStamp) {
        value->sourceTimestamp = UA_DateTime_now();
        value->hasSourceTimestamp = true;
    }
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode writeValue(UA_Server*, const UA_NodeId*, void*, const UA_NodeId*, void* nodeContext,
                         const UA_NumericRange* range, const UA_DataValue* data) {
    auto* variable = static_cast<Server::Variable*>(nodeContext);
    if (range) return UA_STATUSCODE_BADINDEXRANGEINVALID;
    if (!isClientWritable(variable->tag.area)) return UA_STATUSCODE_BADNOTWRITABLE;
    double x = 0;
    if (!data || !data->hasValue || !toDouble(data->value, x)) return UA_STATUSCODE_BADTYPEMISMATCH;
    if (!variable->server->image().writeTag(variable->tag, x, Origin::OpcUa)) {
        return UA_STATUSCODE_BADOUTOFRANGE;
    }
    return UA_STATUSCODE_GOOD;
}

struct LogContext {
    std::string plcId;
};

void logMessage(void* context, UA_LogLevel level, UA_LogCategory, const char* msg, va_list args) {
    // open62541 warns at every start that access control and encryption are not
    // configured (expected: SimplePLC uses neither) and logs each connection at info level;
    // for SimplePLC users only its errors are worth a warning.
    log::Level ours = level >= UA_LOGLEVEL_ERROR ? log::Level::Warn : log::Level::Debug;
    if (level < UA_LOGLEVEL_INFO || !log::enabled(ours)) return;
    std::string text;
#if UA_OPEN62541_VER_MAJOR > 1 || UA_OPEN62541_VER_MINOR >= 5
    UA_String out = UA_STRING_NULL;
    if (UA_String_vformat(&out, msg, args) == UA_STATUSCODE_GOOD) {
        text.assign(reinterpret_cast<const char*>(out.data), out.length);
    }
    UA_String_clear(&out);
#else
    char buffer[512];
    std::vsnprintf(buffer, sizeof(buffer), msg, args);
    text = buffer;
#endif
    // Only one server per process can own the signal handler; nothing here uses it
    if (text.find("Interrupt Manager") != std::string::npos) ours = log::Level::Debug;
    if (!log::enabled(ours)) return;
    log::write(ours, "opcua", static_cast<LogContext*>(context)->plcId, std::move(text));
}

}  // namespace

Server::Server(ProcessImage& image, log::Scope log) : image_(image), log_(std::move(log)) {}

Server::~Server() { stop(); }

std::expected<uint16_t, net::NetError> Server::start(const Options& options) {
    stop();
    options_ = options;

    uint16_t port = options.port;
    if (port == 0) {
        auto probe = net::listenTcp(options.listen, 0, 1);
        if (!probe) return std::unexpected(probe.error());
        port = net::localPort(probe->get());
    }
    // open62541 only logs a failed bind; check first so the error reaches the user
    if (auto probe = net::listenTcp(options.listen, port, 1); !probe) return std::unexpected(probe.error());

    auto* logContext = new LogContext{options.plcId};
    auto* logger = new UA_Logger{logMessage, logContext, [](UA_Logger* l) {
                                     delete static_cast<LogContext*>(l->context);
                                     delete l;
                                 }};

    UA_ServerConfig config;
    std::memset(&config, 0, sizeof(config));
    config.logging = logger;
    if (UA_ServerConfig_setMinimal(&config, port, nullptr) != UA_STATUSCODE_GOOD) {
        UA_ServerConfig_clear(&config);
        return std::unexpected(net::NetError{"cannot configure the OPC UA server"});
    }

    // A client that crashes leaves its sessions behind until they time out, and the 100 session slots are
    // shared by every client: whatever timeout a client asks for, its sessions end a minute after its
    // last request, so a crashed or reconnect-looping HMI cannot lock the others out for longer
    config.maxSessions = options.maxSessions;
    config.maxSessionTimeout = options.maxSessionTimeoutMs;

    UA_LocalizedText_clear(&config.applicationDescription.applicationName);
    config.applicationDescription.applicationName = UA_LOCALIZEDTEXT_ALLOC("en-US", cstr(options.name));
    UA_String_clear(&config.applicationDescription.applicationUri);
    config.applicationDescription.applicationUri = UA_STRING_ALLOC(cstr(options.applicationUri));
    UA_BuildInfo_clear(&config.buildInfo);
    config.buildInfo.productUri = UA_STRING_ALLOC("https://github.com/tommy-berg/SimplePLC");
    config.buildInfo.manufacturerName = UA_STRING_ALLOC("SimplePLC");
    config.buildInfo.productName = UA_STRING_ALLOC("SimplePLC");
    config.buildInfo.softwareVersion = UA_STRING_ALLOC(cstr(kVersion));
    config.buildInfo.buildNumber = UA_STRING_ALLOC(cstr(kVersion));
    config.buildInfo.buildDate = UA_DateTime_now();

    if (!options.listen.empty() && options.listen != "0.0.0.0") {
        bool ipv6 = options.listen.find(':') != std::string::npos;
        std::string host = ipv6 && options.listen != "::" ? "[" + options.listen + "]" : options.listen;
        std::string url = options.listen == "::" ? std::format("opc.tcp://:{}", port)
                                                 : std::format("opc.tcp://{}:{}", host, port);
        UA_Array_delete(config.serverUrls, config.serverUrlsSize, &UA_TYPES[UA_TYPES_STRING]);
        config.serverUrls = UA_String_new();
        *config.serverUrls = UA_STRING_ALLOC(cstr(url));
        config.serverUrlsSize = 1;
    }

    server_ = UA_Server_newWithConfig(&config);
    if (!server_) return std::unexpected(net::NetError{"cannot create the OPC UA server"});

    if (UA_StatusCode rc = UA_Server_run_startup(server_); rc != UA_STATUSCODE_GOOD) {
        UA_Server_delete(server_);
        server_ = nullptr;
        return std::unexpected(net::NetError{std::format("OPC UA server failed to start on port {}: {}",
                                                         port, UA_StatusCode_name(rc))});
    }

    // run_iterate() otherwise sleeps until the next internal timer (up to a second);
    // this keeps tag-table changes and stop() within 100 ms
    UA_Server_addRepeatedCallback(server_, [](UA_Server*, void*) {}, nullptr, 100, nullptr);

    port_ = port;
    syncedTagsVersion_ = UINT64_MAX;
    running_ = true;
    thread_ = std::thread(&Server::run, this);
    return port;
}

void Server::stop() {
    running_ = false;
    if (server_ && thread_.joinable()) {
        UA_EventLoop* loop = UA_Server_getConfig(server_)->eventLoop;
        if (loop && loop->cancel) loop->cancel(loop);  // wake run_iterate() at once
    }
    if (thread_.joinable()) thread_.join();
    if (server_) {
        // Shutdown still runs due timers, among them monitored items that sample the variables
        // (readValue with their node context): free the variables only once the server is gone
        UA_Server_run_shutdown(server_);
        UA_Server_delete(server_);
        server_ = nullptr;
        for (auto& [name, variable] : variables_) UA_NodeId_clear(&variable->nodeId);
        variables_.clear();
    }
    variableCount_ = 0;
    sessions_ = 0;
    port_ = 0;
}

Server::Stats Server::stats() const {
    Stats s;
    s.listening = running_.load();
    s.port = port_.load();
    s.variables = variableCount_.load();
    s.sessions = sessions_.load();
    return s;
}

void Server::run() {
    UA_NodeId folder = UA_NODEID_NULL;
    UA_ObjectAttributes folderAttr = UA_ObjectAttributes_default;
    folderAttr.displayName = UA_LOCALIZEDTEXT(cstr("en-US"), cstr(options_.name));
    folderAttr.description = UA_LOCALIZEDTEXT(cstr("en-US"), cstr("PLC tags"));
    UA_Server_addObjectNode(server_, UA_NODEID_STRING(1, cstr(kFolderId)),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                            UA_QUALIFIEDNAME(1, cstr(options_.plcId)),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE), folderAttr, nullptr, &folder);
    UA_NodeId_clear(&folder);

    while (running_) {
        if (image_.tagsVersion() != syncedTagsVersion_) syncTags();
        UA_Server_run_iterate(server_, true);
        sessions_ = UA_Server_getStatistics(server_).ss.currentSessionCount;
    }
}

void Server::syncTags() {
    syncedTagsVersion_ = image_.tagsVersion();
    std::vector<TagDef> tags = image_.tags();

    std::set<std::string> wanted;
    for (const TagDef& tag : tags) wanted.insert(tag.name);
    for (auto it = variables_.begin(); it != variables_.end();) {
        auto match = std::find_if(tags.begin(), tags.end(), [&](const TagDef& t) { return t.name == it->first; });
        if (match == tags.end() || !(*match == it->second->tag)) {
            UA_Server_deleteNode(server_, it->second->nodeId, true);
            UA_NodeId_clear(&it->second->nodeId);
            it = variables_.erase(it);
        } else {
            ++it;
        }
    }
    for (const TagDef& tag : tags) {
        if (!variables_.contains(tag.name)) addVariable(tag);
    }
    variableCount_ = variables_.size();
}

bool Server::addVariable(const TagDef& tag) {
    auto variable = std::make_unique<Variable>();
    variable->server = this;
    variable->tag = tag;
    variable->nodeId = UA_NODEID_STRING_ALLOC(1, cstr(tag.name));

    bool writable = isClientWritable(tag.area);
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    attr.displayName = UA_LOCALIZEDTEXT(cstr("en-US"), cstr(tag.name));
    if (!tag.description.empty()) attr.description = UA_LOCALIZEDTEXT(cstr("en-US"), cstr(tag.description));
    attr.dataType = dataType(tag.type)->typeId;
    attr.valueRank = UA_VALUERANK_SCALAR;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | (writable ? UA_ACCESSLEVELMASK_WRITE : 0);
    attr.userAccessLevel = attr.accessLevel;
    attr.minimumSamplingInterval = 50.0;

    UA_DataSource source{};
    source.read = readValue;
    source.write = writable ? writeValue : nullptr;

    UA_StatusCode rc = UA_Server_addDataSourceVariableNode(
        server_, variable->nodeId, UA_NODEID_STRING(1, cstr(kFolderId)),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), UA_QUALIFIEDNAME(1, cstr(tag.name)),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, source, variable.get(), nullptr);
    if (rc != UA_STATUSCODE_GOOD) {
        log_.warn("OPC UA variable for tag '{}' could not be created: {}", tag.name, UA_StatusCode_name(rc));
        UA_NodeId_clear(&variable->nodeId);
        return false;
    }
    variables_.emplace(tag.name, std::move(variable));
    return true;
}

const char* libraryVersion() { return UA_OPEN62541_VERSION; }

}  // namespace simpleplc::opcua
