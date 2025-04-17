#pragma once
#include <modbus.h>
#include <thread>
#include <vector>
#include <mutex>
#include <map>
#include <string>
#include <atomic>
#include <chrono>
#include <memory>

/**
 * @class ClientConnection
 * @brief Represents a client connection to the Modbus server
 */
class ClientConnection {
public:
    /**
     * @brief Constructor for a new client connection
     * 
     * @param socket Socket file descriptor
     * @param ip Client IP address
     */
    ClientConnection(int socket, const std::string& ip);
    
    /**
     * @brief Destructor - closes the socket if still open
     */
    ~ClientConnection();
    
    /**
     * @brief Get the socket file descriptor
     * 
     * @return Socket file descriptor
     */
    int getSocket() const { return socket_; }
    
    /**
     * @brief Get the client IP address
     * 
     * @return Client IP address
     */
    const std::string& getIp() const { return ip_; }
    
    /**
     * @brief Get the connection creation time
     * 
     * @return Connection creation time
     */
    std::chrono::system_clock::time_point getCreationTime() const { return creation_time_; }
    
    /**
     * @brief Get the last activity time
     * 
     * @return Last activity time
     */
    std::chrono::system_clock::time_point getLastActivity() const { return last_activity_; }
    
    /**
     * @brief Update the last activity time to now
     */
    void updateLastActivity();
    
    /**
     * @brief Check if the connection is active
     * 
     * @return true if active, false otherwise
     */
    bool isActive() const { return is_active_; }
    
    /**
     * @brief Mark the connection as inactive
     */
    void markInactive() { is_active_ = false; }
    
    /**
     * @brief Get the number of requests processed
     * 
     * @return Request count
     */
    uint64_t getRequestCount() const { return request_count_; }
    
    /**
     * @brief Increment the request count
     */
    void incrementRequestCount() { request_count_++; }
    
private:
    int socket_;                                    ///< Socket file descriptor
    std::string ip_;                                ///< Client IP address
    std::chrono::system_clock::time_point creation_time_; ///< Connection creation time
    std::chrono::system_clock::time_point last_activity_; ///< Last activity time
    std::atomic<bool> is_active_{true};             ///< Whether the connection is active
    std::atomic<uint64_t> request_count_{0};        ///< Number of requests processed
};

/**
 * @class ModbusServer
 * @brief Implements a Modbus TCP server that runs in its own thread.
 * 
 * This class provides a Modbus TCP server that handles client connections
 * and processes Modbus protocol messages. It runs in a separate thread to
 * allow concurrent operation with other services.
 */
class ModbusServer {
public:
    /**
     * @brief Constructor - initializes the server and starts the thread
     */
    ModbusServer();
    
    /**
     * @brief Destructor - stops the server thread and cleans up resources
     */
    ~ModbusServer();
    
    /**
     * @brief Legacy method kept for API compatibility
     * @return Always returns 0
     */
    int run();
    
    /**
     * @brief Legacy method kept for API compatibility
     * @return Always returns 0
     */
    int poll();
    
    /**
     * @brief Get the Modbus mapping used by this server
     * @return Pointer to the mapping structure
     */
    modbus_mapping_t* get_mapping();
    
    /**
     * @brief Lock the mapping mutex to protect access to the mapping
     */
    void lock_mapping();
    
    /**
     * @brief Unlock the mapping mutex
     */
    void unlock_mapping();
    
    /**
     * @brief Get the number of active connections
     * @return Number of active connections
     */
    size_t getActiveConnectionCount();
    
    /**
     * @brief Get statistics about the server
     * @return String containing server statistics
     */
    std::string getStatistics();
    
private:
    /**
     * @brief Main server thread function
     */
    void run_server();
    
    /**
     * @brief Configure a client socket
     * @param client_socket The client socket to configure
     * @return true if successful, false otherwise
     */
    bool configure_client_socket(int client_socket);
    
    /**
     * @brief Add a new client connection
     * @param socket Client socket
     * @param ip Client IP address
     * @return Pointer to the new connection object
     */
    std::shared_ptr<ClientConnection> addConnection(int socket, const std::string& ip);
    
    /**
     * @brief Remove a client connection
     * @param socket Client socket
     */
    void removeConnection(int socket);
    
    /**
     * @brief Get a client connection by socket
     * @param socket Client socket
     * @return Pointer to the connection object, or nullptr if not found
     */
    std::shared_ptr<ClientConnection> getConnection(int socket);
    
    modbus_mapping_t* mapping_ = nullptr;  ///< Modbus data mapping
    std::thread* thread_ = nullptr;        ///< Server thread
    std::mutex mapping_mutex; ///< Mutex to protect access to the Modbus mapping
    std::mutex connections_mutex; ///< Mutex to protect access to the connections map
    
    // Connection tracking
    std::map<int, std::shared_ptr<ClientConnection>> connections_; ///< Map of socket FD to connection object
    std::atomic<uint64_t> total_connections_{0}; ///< Total number of connections since server start
    std::atomic<uint64_t> total_requests_{0}; ///< Total number of requests since server start
    std::chrono::system_clock::time_point start_time_; ///< Server start time
    
    /**
     * @brief Clean up inactive connections
     */
    void cleanup_inactive_connections();
};

