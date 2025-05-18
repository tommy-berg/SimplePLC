#include "platform.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <thread>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include <modbus.h>

enum OperationType {
    READ,
    WRITE
};

struct BenchmarkStats {
    std::vector<double> responseTimes;
    double minTime = 0.0;
    double maxTime = 0.0;
    double avgTime = 0.0;
    double medianTime = 0.0;
    double p95Time = 0.0;
    int failedRequests = 0;
    int successfulRequests = 0;
};

void calculateStats(BenchmarkStats& stats) {
    if (stats.responseTimes.empty()) {
        return;
    }
    
    // Sort for percentile calculations
    std::sort(stats.responseTimes.begin(), stats.responseTimes.end());
    
    stats.minTime = stats.responseTimes.front();
    stats.maxTime = stats.responseTimes.back();
    stats.avgTime = std::accumulate(stats.responseTimes.begin(), stats.responseTimes.end(), 0.0) / stats.responseTimes.size();
    
    // Calculate median
    size_t size = stats.responseTimes.size();
    if (size % 2 == 0) {
        stats.medianTime = (stats.responseTimes[size/2 - 1] + stats.responseTimes[size/2]) / 2;
    } else {
        stats.medianTime = stats.responseTimes[size/2];
    }
    
    // Calculate 95th percentile
    size_t p95Index = static_cast<size_t>(size * 0.95);
    stats.p95Time = stats.responseTimes[p95Index];
}

void displayStats(const BenchmarkStats& stats, OperationType opType, int numIterations, int numBits, int delayMs) {
    std::cout << "\n===== MODBUS RESPONSE TIME BENCHMARK =====\n";
    std::cout << "Operation: " << (opType == READ ? "READ" : "WRITE") << " bits\n";
    std::cout << "Iterations: " << numIterations << "\n";
    std::cout << "Data size: " << numBits << " bits\n";
    std::cout << "Delay between requests: " << delayMs << "ms\n";
    std::cout << "Successful requests: " << stats.successfulRequests << "/" << numIterations << "\n";
    std::cout << "Failed requests: " << stats.failedRequests << "\n";
    
    if (stats.successfulRequests > 0) {
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "Minimum response time: " << stats.minTime << "ms\n";
        std::cout << "Maximum response time: " << stats.maxTime << "ms\n";
        std::cout << "Average response time: " << stats.avgTime << "ms\n";
        std::cout << "Median response time: " << stats.medianTime << "ms\n";
        std::cout << "95th percentile: " << stats.p95Time << "ms\n";
    }
    std::cout << "=========================================\n";
}

void runBenchmark(const char* targetHost, int port, int modbusAddr, int numBits, 
                 OperationType operation, int numIterations, int delayMs) {
    // Create a new Modbus context
    modbus_t* ctx = modbus_new_tcp(targetHost, port);
    if (ctx == nullptr) {
        std::cerr << "Failed to create Modbus context: " << modbus_strerror(errno) << std::endl;
        return;
    }

    // Enable debug mode
    modbus_set_debug(ctx, TRUE);
    
    // Set timeouts
    modbus_set_response_timeout(ctx, 1, 0);
    
    // Prepare data buffer
    std::vector<uint8_t> data(static_cast<size_t>(numBits), 0);
    if (operation == WRITE) {
        // For write operations, set alternating bits
        for (int i = 0; i < numBits; i++) {
            data[static_cast<size_t>(i)] = (i % 2 == 0) ? 1 : 0;
        }
    }
    
    BenchmarkStats stats;
    
    std::cout << "Attempting to connect to Modbus server at " << targetHost << ":" << port << std::endl;
    
    if (modbus_connect(ctx) == -1) {
        int errnum = errno;
        std::cerr << "Connection failed: " << modbus_strerror(errnum) << " (errno: " << errnum << ")" << std::endl;
        std::cerr << "Additional details: ";
        
        // Add more specific error handling
        switch (errnum) {
            case ECONNREFUSED:
                std::cerr << "Connection refused. The server is not running or the port is wrong." << std::endl;
                break;
            case ETIMEDOUT:
                std::cerr << "Connection timed out. Check if the host is reachable." << std::endl;
                break;
            default:
                std::cerr << "Check firewall settings and ensure the server is running." << std::endl;
                break;
        }
        
        modbus_free(ctx);
        return;
    }
    
    std::cout << "Connected to Modbus server at " << targetHost << ":" << port << std::endl;
    std::cout << "Running " << numIterations << " iterations...\n";
    
    // Start benchmark
    auto totalStart = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < numIterations; i++) {
        int rslt = -1;
        
        // Measure individual request time
        auto requestStart = std::chrono::high_resolution_clock::now();
        
        if (operation == READ) {
            rslt = modbus_read_bits(ctx, modbusAddr, numBits, data.data());
        } else if (operation == WRITE) {
            rslt = modbus_write_bits(ctx, modbusAddr, numBits, data.data());
        }
        
        auto requestEnd = std::chrono::high_resolution_clock::now();
        double responseTime = std::chrono::duration<double, std::milli>(requestEnd - requestStart).count();
        
        if (rslt == -1) {
            stats.failedRequests++;
            std::cerr << "Request " << i + 1 << " failed: " << modbus_strerror(errno) << std::endl;
        } else {
            stats.successfulRequests++;
            stats.responseTimes.push_back(responseTime);
            
            // Progress indicator every 10% of iterations
            if (numIterations >= 10 && i % (numIterations / 10) == 0) {
                std::cout << "." << std::flush;
            }
        }
        
        // Add delay between requests if specified
        if (delayMs > 0 && i < numIterations - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
    }
    
    auto totalEnd = std::chrono::high_resolution_clock::now();
    double totalTime = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
    
    std::cout << "\nBenchmark completed in " << std::fixed << std::setprecision(2) 
              << totalTime / 1000.0 << " seconds" << std::endl;
    
    modbus_close(ctx);
    modbus_free(ctx);
    
    // Calculate and display statistics
    calculateStats(stats);
    displayStats(stats, operation, numIterations, numBits, delayMs);
}

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [OPTIONS]\n"
              << "Options:\n"
              << "  -h, --host HOST         Modbus server host (default: 127.0.0.1)\n"
              << "  -p, --port PORT         Modbus server port (default: 502)\n"
              << "  -a, --address ADDR      Modbus start address (default: 0)\n"
              << "  -b, --bits BITS         Number of bits to read/write (default: 10)\n"
              << "  -n, --iterations NUM    Number of iterations (default: 100)\n"
              << "  -d, --delay MS          Delay between requests in ms (default: 0)\n"
              << "  -r, --read              Perform read operation (default)\n"
              << "  -w, --write             Perform write operation\n"
              << "  --help                  Display this help message\n";
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    int port = 502;
    int address = 0;
    int bits = 10;
    int iterations = 100;
    int delay = 0;
    OperationType operation = READ;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-h" || arg == "--host") {
            if (i + 1 < argc) {
                host = argv[++i];
            }
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 < argc) {
                port = std::atoi(argv[++i]);
            }
        } else if (arg == "-a" || arg == "--address") {
            if (i + 1 < argc) {
                address = std::atoi(argv[++i]);
            }
        } else if (arg == "-b" || arg == "--bits") {
            if (i + 1 < argc) {
                bits = std::atoi(argv[++i]);
            }
        } else if (arg == "-n" || arg == "--iterations") {
            if (i + 1 < argc) {
                iterations = std::atoi(argv[++i]);
            }
        } else if (arg == "-d" || arg == "--delay") {
            if (i + 1 < argc) {
                delay = std::atoi(argv[++i]);
            }
        } else if (arg == "-r" || arg == "--read") {
            operation = READ;
        } else if (arg == "-w" || arg == "--write") {
            operation = WRITE;
        }
    }
    
    // Run the benchmark
    runBenchmark(host, port, address, bits, operation, iterations, delay);
    
    return 0;
} 