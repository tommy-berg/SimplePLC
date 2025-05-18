#include "platform.h"
#include <iostream>
#include <modbus.h>
#include <cstring>

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    int port = 502;
    
    // Parse command line arguments
    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);
    
    std::cout << "Checking Modbus server at " << host << ":" << port << std::endl;
    
    // Create a new Modbus context
    modbus_t* ctx = modbus_new_tcp(host, port);
    if (ctx == nullptr) {
        std::cerr << "Failed to create Modbus context: " << modbus_strerror(errno) << std::endl;
        return 1;
    }
    
    // Enable debug mode
    modbus_set_debug(ctx, TRUE);
    
    // Set timeout (2 seconds)
    modbus_set_response_timeout(ctx, 2, 0);
    
    // Try to connect
    if (modbus_connect(ctx) == -1) {
        int errnum = errno;
        std::cerr << "Connection failed: " << modbus_strerror(errnum) << " (errno: " << errnum << ")" << std::endl;
        
        switch (errnum) {
            case ECONNREFUSED:
                std::cerr << "Connection refused. The server is not running or port is wrong." << std::endl;
                break;
            case ETIMEDOUT:
                std::cerr << "Connection timed out. Check if host is reachable." << std::endl;
                break;
            default:
                std::cerr << "Check firewall settings and ensure server is running." << std::endl;
                break;
        }
        
        modbus_free(ctx);
        return 1;
    }
    
    std::cout << "Successfully connected to Modbus server!" << std::endl;
    
    // Try reading some registers to make sure communication works
    uint8_t bits[10];
    int result = modbus_read_bits(ctx, 0, 10, bits);
    
    if (result == -1) {
        std::cerr << "Failed to read bits: " << modbus_strerror(errno) << std::endl;
    } else {
        std::cout << "Successfully read " << result << " bits from the server" << std::endl;
        std::cout << "Values: ";
        for (int i = 0; i < result; i++) {
            std::cout << (bits[i] ? "1" : "0") << " ";
        }
        std::cout << std::endl;
    }
    
    // Cleanup
    modbus_close(ctx);
    modbus_free(ctx);
    
    return (result == -1) ? 1 : 0;
} 