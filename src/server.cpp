#include "platform.h"  // Include platform.h first for platform-specific definitions
#include "server.h"
#include <iostream>
#include <thread>
#include <modbus.h>
#include "modbus_handler.h"
#include "plc_logic.h"
#include <atomic>

// Include platform-specific headers that aren't already in platform.h
#ifndef _WIN32
#include <netinet/in.h>  // For sockaddr_in definition
#include <netinet/tcp.h> // For TCP_NODELAY
#include <arpa/inet.h>   // For inet_ntop and related functions
#else
// Windows equivalents are already included in platform.h
#include <io.h>  // For _dup
#define dup _dup
#endif

#include "device_config.h"
#include <algorithm>
#include <vector>
#include <mutex>
#include <sstream>
#include <iomanip>

// Constants for configuration - now loaded from settings.ini
namespace {
    constexpr int DEFAULT_CLIENT_TIMEOUT_SEC = 1;
    constexpr int DEFAULT_CLIENT_TIMEOUT_USEC = 0;
}

// Flag to indicate if the server thread should continue running
std::atomic<bool> server_running{true};

// Add ClientConnection implementation after the namespace but before ModbusServer constructor
ClientConnection::ClientConnection(int socket, const std::string& ip)
    : socket_(socket), ip_(ip), 
      creation_time_(std::chrono::system_clock::now()),
      last_activity_(creation_time_) {
}

ClientConnection::~ClientConnection() {
    // Close the socket if it's still open
    if (socket_ >= 0 && is_active_) {
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
        socket_ = -1;
    }
}

void ClientConnection::updateLastActivity() {
    last_activity_ = std::chrono::system_clock::now();
}

ModbusServer::ModbusServer() : thread_(nullptr), start_time_(std::chrono::system_clock::now()) {
    // Get configuration
    const auto& config = DeviceConfig::getModbusConfig();
    
    // Initialize Modbus mapping
    mapping_ = modbus_mapping_new(
        config.mapping_size, 
        config.mapping_size, 
        config.mapping_size, 
        config.mapping_size
    );
    
    if (!mapping_) {
        throw std::runtime_error("Failed to allocate Modbus mapping");
    }
    
    // Initialize Lua hooks for simulation
    ModbusHandler::init_lua_hooks(mapping_, this);
    
    // Start the PLC logic
    PlcLogic::start(mapping_);
    PlcLogic::loadScript("active.plc");
    
    // Start the server in a separate thread
    thread_ = new std::thread(&ModbusServer::run_server, this);
}

ModbusServer::~ModbusServer() {
    // Signal the server thread to stop
    server_running = false;
    
    // Wait for the thread to finish
    if (thread_ && thread_->joinable()) {
        thread_->join();
        delete thread_;
        thread_ = nullptr;
    }
    
    // Close all active connections
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        for (auto& pair : connections_) {
            pair.second->markInactive();
        }
        connections_.clear();
    }
    
    // Print final statistics
    std::cout << "\n=== Final Modbus Server Statistics ===\n" << getStatistics() << std::endl;
    
    PlcLogic::stop();
    
    if (mapping_) {
        modbus_mapping_free(mapping_);
        mapping_ = nullptr;
    }
    
    std::cout << "[Info] Modbus server stopped\n";
}

void ModbusServer::run_server() {
    // Get configuration
    const auto& config = DeviceConfig::getModbusConfig();
    
    // Variables for select() based server
    fd_set refset;
    fd_set rdset;
    int max_fd = -1;
    uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
    
    try {
        // Initialize the Modbus context
        modbus_t* ctx = modbus_new_tcp(config.listen_address.c_str(), config.port);
        if (!ctx) {
            throw std::runtime_error(std::string("Failed to create context: ") + modbus_strerror(errno));
        }

        // Get device configuration for slave ID
        const auto& device_info = DeviceConfig::getDeviceInfo();
        modbus_set_slave(ctx, device_info.slave_id);
        
        // Create the server socket
        int server_socket = modbus_tcp_listen(ctx, config.max_connections);
        if (server_socket == -1) {
            throw std::runtime_error(std::string("Failed to listen: ") + modbus_strerror(errno));
        }
        
        std::cout << "[Modbus] Server listening on " << config.listen_address 
                  << ":" << config.port << " (max connections: " << config.max_connections << ")" << std::endl;
        
        // Initialize the reference set for select()
        FD_ZERO(&refset);
        FD_SET(server_socket, &refset);
        max_fd = server_socket;
        
        // Setup for periodic maintenance and statistics
        auto last_stats_time = std::chrono::steady_clock::now();
        const auto stats_interval = std::chrono::minutes(1);
        
        // Main server loop
        while (server_running) {
            // Copy the reference set to the working set
            rdset = refset;
            
            // Setup timeout for select - short to allow checking server_running flag
            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000; // 100ms for responsive checking of server_running flag
            
            // Wait for activity on any socket
            int result = select(max_fd + 1, &rdset, NULL, NULL, &timeout);
            
            if (result == -1) {
                if (errno != EINTR) {
#ifdef _WIN32
                    std::cerr << "[Modbus] Select error: " << WSAGetLastError() << std::endl;
#else
                    std::cerr << "[Modbus] Select error: " << strerror(errno) << std::endl;
#endif
                }
                continue;
            }
            
            // Check for timeout
            if (result == 0) {
                // Periodic statistics
                auto now = std::chrono::steady_clock::now();
                if (now - last_stats_time > stats_interval) {
                    std::cout << "\n=== Modbus Server Statistics ===\n" << getStatistics() << std::endl;
                    last_stats_time = now;
                }
                continue;
            }
            
            // Check each socket for activity
            for (int socket_fd = 0; socket_fd <= max_fd; socket_fd++) {
                if (!FD_ISSET(socket_fd, &rdset)) {
                    continue;
                }
                
                // If the listening socket is active, accept a new connection
                if (socket_fd == server_socket) {
                    struct sockaddr_in client_addr;
                    socklen_t addrlen = sizeof(client_addr);
                    int client_socket = accept(server_socket, reinterpret_cast<struct sockaddr*>(&client_addr), &addrlen);
                    
                    if (client_socket == -1) {
#ifdef _WIN32
                        std::cerr << "[Modbus] Accept error: " << WSAGetLastError() << std::endl;
#else
                        std::cerr << "[Modbus] Accept error: " << strerror(errno) << std::endl;
#endif
                        continue;
                    }
                    
                    // Get client IP address for logging
                    char client_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
                    std::string client_ip_str(client_ip);
                    
                    std::cout << "[Modbus] New client connection accepted from " << client_ip_str
                              << " on socket " << client_socket << std::endl;
                    
                    // Configure client socket
                    if (!configure_client_socket(client_socket)) {
                        std::cerr << "[Modbus] Failed to configure client socket " << client_socket << std::endl;
#ifdef _WIN32
                        closesocket(client_socket);
#else
                        close(client_socket);
#endif
                        continue;
                    }
                    
                    // Add the socket to the reference set
                    FD_SET(client_socket, &refset);
                    
                    // Update the maximum file descriptor if needed
                    if (client_socket > max_fd) {
                        max_fd = client_socket;
                    }
                    
                    // Track this connection
                    addConnection(client_socket, client_ip_str);
                    
                    std::cout << "[Modbus] Active connections: " << getActiveConnectionCount() << std::endl;
                } else {
                    // This is a client socket with data to read
                    auto connection = getConnection(socket_fd);
                    if (connection) {
                        connection->updateLastActivity();
                    }
                    
                    // Set the socket in the context
                    modbus_set_socket(ctx, socket_fd);
                    
                    // Process the client request
                    int rc = modbus_receive(ctx, query);
                    
                    if (rc > 0) {
                        // Successfully received data
                        if (connection) {
                            connection->incrementRequestCount();
                            total_requests_++;
                        }
                        
                        uint8_t func = query[7];
                        std::cout << "[Modbus] Received function 0x" 
                                 << std::hex << static_cast<int>(func) << std::dec 
                                 << " (length: " << rc << " bytes)"
                                 << (connection ? " from " + connection->getIp() : "") 
                                 << std::endl;
                        
                        try {
                            // Lock the mapping for thread safety during reply
                            std::lock_guard<std::mutex> lock(mapping_mutex);
                            
                            // Handle different function codes
                            if (func == 0x11) {
                                ModbusHandler::send_report_slave_id(socket_fd, ctx, query, rc);
                            }
                            else if (func == 0x2B) {
                                ModbusHandler::send_read_device_id(socket_fd, ctx, query, rc);
                            }
                            else {
                                // Process any write operations
                                ModbusHandler::handle_standard_function(ctx, query, rc, mapping_);
                                
                                // Send the reply
                                if (modbus_reply(ctx, query, rc, mapping_) == -1) {
                                    std::cerr << "[Modbus] Error in modbus_reply: " 
                                             << modbus_strerror(errno) << std::endl;
                                } else {
                                    std::cout << "[Modbus] Successfully sent reply for function 0x" 
                                             << std::hex << static_cast<int>(func) << std::dec << std::endl;
                                }
                            }
                        } catch (const std::exception& e) {
                            std::cerr << "[Modbus] Error processing request: " << e.what() << std::endl;
                        }
                    } else if (rc == -1) {
                        // Error receiving data
#ifdef _WIN32
                        if (WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAETIMEDOUT) {
#else
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ETIMEDOUT) {
#endif
                            // Real error, not timeout - close the connection
                            std::cout << "[Modbus] Connection closed on socket " << socket_fd 
                                     << (connection ? " from " + connection->getIp() : "") << std::endl;
                            
                            // Close the socket
#ifdef _WIN32
                            closesocket(socket_fd);
#else
                            close(socket_fd);
#endif
                            
                            // Remove from reference set
                            FD_CLR(socket_fd, &refset);
                            
                            // Remove from connection tracking
                            removeConnection(socket_fd);
                            
                            // Update max_fd if necessary
                            if (socket_fd == max_fd) {
                                // Find the new max_fd
                                max_fd = server_socket;
                                for (int i = 0; i <= socket_fd; i++) {
                                    if (FD_ISSET(i, &refset) && i > max_fd) {
                                        max_fd = i;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            
            // Small delay to prevent CPU spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        // Server is shutting down, close all sockets
        std::cout << "[Modbus] Closing server socket and all client connections..." << std::endl;
        
        // Close the server socket
        #ifdef _WIN32
        closesocket(server_socket);
        #else
        close(server_socket);
        #endif
        
        // Close all client sockets
        for (int socket_fd = 0; socket_fd <= max_fd; socket_fd++) {
            if (FD_ISSET(socket_fd, &refset) && socket_fd != server_socket) {
#ifdef _WIN32
                closesocket(socket_fd);
#else
                close(socket_fd);
#endif
            }
        }
        
        // Free the context
        modbus_free(ctx);
    }
    catch (const std::exception& e) {
        std::cerr << "[Modbus] Error in server thread: " << e.what() << std::endl;
    }
    
    std::cout << "[Modbus] Server thread exiting" << std::endl;
}

bool ModbusServer::configure_client_socket(int client_socket) {
    // Make socket blocking for synchronous I/O
#ifdef _WIN32
    unsigned long mode = 0;  // 0 = blocking, 1 = non-blocking
    if (ioctlsocket(client_socket, FIONBIO, &mode) != 0) {
        std::cerr << "[Modbus] Error setting socket to blocking mode: " << WSAGetLastError() << std::endl;
        return false;
    }
#else
    int flags = fcntl(client_socket, F_GETFL, 0);
    if (flags == -1) {
        std::cerr << "[Modbus] Error getting socket flags: " << strerror(errno) << std::endl;
        return false;
    }
    
    if (fcntl(client_socket, F_SETFL, flags & ~O_NONBLOCK) == -1) {
        std::cerr << "[Modbus] Error setting socket to blocking mode: " << strerror(errno) << std::endl;
        return false;
    }
#endif
    
    // Set TCP_NODELAY to disable Nagle's algorithm for better real-time performance
    int flag = 1;
    if (setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(int)) == -1) {
        std::cerr << "[Modbus] Warning: Failed to set TCP_NODELAY: " 
#ifdef _WIN32
                  << WSAGetLastError() 
#else
                  << strerror(errno) 
#endif
                  << std::endl;
        // Non-critical, continue anyway
    }
    
    // Disable TCP lingering to ensure socket close is immediate
    struct linger linger_opt = {1, 0}; // Enable linger with 0 timeout (immediate close)
    if (setsockopt(client_socket, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger_opt), sizeof(linger_opt)) == -1) {
        std::cerr << "[Modbus] Warning: Failed to set SO_LINGER: " 
#ifdef _WIN32
                  << WSAGetLastError() 
#else
                  << strerror(errno) 
#endif
                  << std::endl;
        // Non-critical, continue anyway
    }
    
    // Set receive and send timeouts
    struct timeval timeout;
    timeout.tv_sec = DEFAULT_CLIENT_TIMEOUT_SEC;
    timeout.tv_usec = DEFAULT_CLIENT_TIMEOUT_USEC;
    
    if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) < 0) {
        std::cerr << "[Modbus] Error setting socket receive timeout: " 
#ifdef _WIN32
                  << WSAGetLastError() 
#else
                  << strerror(errno) 
#endif
                  << std::endl;
        return false;
    }
    
    if (setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) < 0) {
        std::cerr << "[Modbus] Error setting socket send timeout: " 
#ifdef _WIN32
                  << WSAGetLastError() 
#else
                  << strerror(errno) 
#endif
                  << std::endl;
        return false;
    }
    
    // Set keepalive to detect dead connections
    int keepalive = 1;
    if (setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepalive), sizeof(int)) < 0) {
        std::cerr << "[Modbus] Warning: Failed to set SO_KEEPALIVE: " 
#ifdef _WIN32
                  << WSAGetLastError() 
#else
                  << strerror(errno) 
#endif
                  << std::endl;
        // Non-critical, continue anyway
    }
    
    return true;
}

// Legacy methods kept for API compatibility
int ModbusServer::poll() {
    return 0;
}

int ModbusServer::run() {
    return 0;
}

modbus_mapping_t* ModbusServer::get_mapping() {
    return mapping_;
}

void ModbusServer::lock_mapping() {
    mapping_mutex.lock();
}

void ModbusServer::unlock_mapping() {
    mapping_mutex.unlock();
}

// Add these methods to ModbusServer implementation (between existing methods)

std::shared_ptr<ClientConnection> ModbusServer::addConnection(int socket, const std::string& ip) {
    std::lock_guard<std::mutex> lock(connections_mutex);
    auto connection = std::make_shared<ClientConnection>(socket, ip);
    connections_[socket] = connection;
    total_connections_++;
    return connection;
}

void ModbusServer::removeConnection(int socket) {
    std::lock_guard<std::mutex> lock(connections_mutex);
    auto it = connections_.find(socket);
    if (it != connections_.end()) {
        it->second->markInactive();
        connections_.erase(it);
    }
}

std::shared_ptr<ClientConnection> ModbusServer::getConnection(int socket) {
    std::lock_guard<std::mutex> lock(connections_mutex);
    auto it = connections_.find(socket);
    if (it != connections_.end()) {
        return it->second;
    }
    return nullptr;
}

size_t ModbusServer::getActiveConnectionCount() {
    std::lock_guard<std::mutex> lock(connections_mutex);
    return connections_.size();
}

std::string ModbusServer::getStatistics() {
    std::lock_guard<std::mutex> lock(connections_mutex);
    
    auto now = std::chrono::system_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    
    std::ostringstream oss;
    oss << "Server Statistics:" << std::endl;
    oss << "  Uptime: " << uptime << " seconds" << std::endl;
    oss << "  Total connections: " << total_connections_ << std::endl;
    oss << "  Active connections: " << connections_.size() << std::endl;
    oss << "  Total requests: " << total_requests_ << std::endl;
    
    if (!connections_.empty()) {
        oss << std::endl << "Active Connections:" << std::endl;
        oss << "  Socket | IP Address      | Duration (s) | Requests" << std::endl;
        oss << "  -------+----------------+-------------+---------" << std::endl;
        
        for (const auto& pair : connections_) {
            const auto& conn = pair.second;
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                now - conn->getCreationTime()).count();
            
            oss << "  " << std::setw(6) << conn->getSocket() << " | "
                << std::setw(14) << conn->getIp() << " | "
                << std::setw(11) << duration << " | "
                << std::setw(8) << conn->getRequestCount() << std::endl;
        }
    }
    
    return oss.str();
}

void ModbusServer::cleanup_inactive_connections() {
    std::lock_guard<std::mutex> lock(connections_mutex);
    
    // Remove connections that are marked as inactive
    auto it = connections_.begin();
    while (it != connections_.end()) {
        if (!it->second->isActive()) {
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}