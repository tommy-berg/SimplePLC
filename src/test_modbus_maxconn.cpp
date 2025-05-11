#include "platform.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include <modbus.h>
#include <sys/time.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <sstream>
#include <condition_variable>
#include <functional>

// Global verbose flag
bool g_verbose = false;

// Thread synchronization mechanisms
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_ready = false;  // Signal to start the test
std::atomic<int> g_threadsReady(0);  // Count of threads ready to start
std::atomic<int> g_totalThreads(0);  // Total number of threads (flooders + legitimate)

enum OperationType {
    READ,
    WRITE
};

enum ConnectionMode {
    CONNECT_ONCE,    // Connect at the beginning, disconnect at the end
    CONNECT_PER_OP   // Connect/disconnect for each operation
};

struct BenchmarkStats {
    int successfulRequests = 0;
    int failedRequests = 0;
    int timeoutRequests = 0;  // Requests that specifically timed out
    int threadId = 0;
    double avgResponseTime = 0.0;
    double maxResponseTime = 0.0;
    double minResponseTime = std::numeric_limits<double>::max();
    double connectTime = 0.0;     // Time spent in connect operations
    double operationTime = 0.0;   // Time spent in actual Modbus operations
    double disconnectTime = 0.0;  // Time spent in disconnect operations
    std::vector<double> responseTimes;
};

struct LegitimateRequestStats {
    std::vector<double> responseTimes;
    std::vector<bool> success;
    std::vector<double> connectTimes;
    std::vector<double> operationTimes;
    std::vector<double> disconnectTimes;
    std::atomic<bool> running{true};
};

// Global shared variables for communication between threads
std::mutex statsMutex;
LegitimateRequestStats legitStats;
std::vector<BenchmarkStats> floodStats;

// Wait for all threads to be ready, then start them simultaneously
void waitForAllThreads() {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_threadsReady++;
    
    if (g_verbose) {
        std::cout << "Thread ready: " << g_threadsReady << "/" << g_totalThreads << std::endl;
    }
    
    // Wait until all threads are ready or for the start signal
    g_cv.wait(lock, []{ return g_ready || (g_threadsReady >= g_totalThreads); });
    
    // If we're the last thread to be ready, signal all to start
    if (!g_ready && g_threadsReady >= g_totalThreads) {
        g_ready = true;
        std::cout << "All threads ready, starting test simultaneously..." << std::endl;
        g_cv.notify_all();
    }
}

// Function to run legitimate requests in parallel with the flood
void runLegitimateRequests(const char* host, int port, int address, int numBits, 
                          OperationType operation, int measurementInterval, int numRequests) {
    // Wait for synchronization with flood threads
    waitForAllThreads();
    
    // Create Modbus context for legitimate requests
    modbus_t* ctx = modbus_new_tcp(host, port);
    if (ctx == nullptr) {
        std::cerr << "[Legitimate] Failed to create Modbus context" << std::endl;
        return;
    }
    
    // Set timeout to detect degradation (2 seconds)
    modbus_set_response_timeout(ctx, 2, 0);
    
    std::vector<uint8_t> data(static_cast<size_t>(numBits), 0);
    if (g_verbose) {
        std::cout << "[Legitimate] Starting legitimate request thread" << std::endl;
    }
    
    // Prepare write data
    if (operation == WRITE) {
        for (int i = 0; i < numBits; i++) {
            data[static_cast<size_t>(i)] = (i % 2 == 0) ? 1 : 0;
        }
    }
    
    // Run legitimate requests at a fixed interval until signal to stop
    int requestCount = 0;
    while (legitStats.running && (numRequests <= 0 || requestCount < numRequests)) {
        auto totalStartTime = std::chrono::high_resolution_clock::now();
        bool success = false;
        
        // Detailed timing
        auto connectStartTime = std::chrono::high_resolution_clock::now();
        
        // Connect for each request (simulating a normal client)
        if (modbus_connect(ctx) == -1) {
            std::cerr << "[Legitimate] Connection failed: " << modbus_strerror(errno) << std::endl;
        } else {
            auto connectEndTime = std::chrono::high_resolution_clock::now();
            auto operationStartTime = std::chrono::high_resolution_clock::now();
            
            // Perform the operation and measure time
            int result = -1;
            if (operation == READ) {
                result = modbus_read_bits(ctx, address, numBits, data.data());
            } else {
                result = modbus_write_bits(ctx, address, numBits, data.data());
            }
            
            auto operationEndTime = std::chrono::high_resolution_clock::now();
            auto disconnectStartTime = std::chrono::high_resolution_clock::now();
            
            // Check result
            if (result != -1) {
                success = true;
            } else {
                if (g_verbose) {
                    std::cerr << "[Legitimate] Request failed: " << modbus_strerror(errno) << std::endl;
                }
            }
            
            // Close connection
            modbus_close(ctx);
            
            auto disconnectEndTime = std::chrono::high_resolution_clock::now();
            
            // Calculate detailed timing
            double connectTime = std::chrono::duration<double, std::milli>(connectEndTime - connectStartTime).count();
            double operationTime = std::chrono::duration<double, std::milli>(operationEndTime - operationStartTime).count();
            double disconnectTime = std::chrono::duration<double, std::milli>(disconnectEndTime - disconnectStartTime).count();
            
            // Store detailed timing
            std::lock_guard<std::mutex> lock(statsMutex);
            legitStats.connectTimes.push_back(connectTime);
            legitStats.operationTimes.push_back(operationTime);
            legitStats.disconnectTimes.push_back(disconnectTime);
        }
        
        auto totalEndTime = std::chrono::high_resolution_clock::now();
        double responseTime = std::chrono::duration<double, std::milli>(totalEndTime - totalStartTime).count();
        
        // Store the result
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            legitStats.responseTimes.push_back(responseTime);
            legitStats.success.push_back(success);
        }
        
        // Report progress only if verbose
        if (g_verbose) {
            std::cout << "[Legitimate] Request " << legitStats.responseTimes.size() 
                      << " - Response time: " << std::fixed << std::setprecision(2) << responseTime 
                      << "ms, Success: " << (success ? "Yes" : "No") << std::endl;
        }
        
        requestCount++;
        
        // Wait for the measurement interval (only if we have more requests to do)
        if (numRequests <= 0 || requestCount < numRequests) {
            std::this_thread::sleep_for(std::chrono::milliseconds(measurementInterval));
        }
    }
    
    modbus_free(ctx);
    
    if (g_verbose) {
        std::cout << "[Legitimate] Thread finished with " << requestCount << " requests" << std::endl;
    }
}

// Function for flood thread
void runFloodThread(const char* targetHost, int port, 
                   int modbusAddr, int numBits, int numIterations,
                   int requestRate, OperationType operation,
                   ConnectionMode connMode, int threadId) {
    
    // Wait for synchronization with other threads
    waitForAllThreads();
    
    BenchmarkStats stats;
    stats.threadId = threadId;
    
    // Calculate the delay between requests based on target request rate
    double delayMs = 0.0;
    if (requestRate > 0) {
        delayMs = 1000.0 / requestRate;
    }
    
    if (g_verbose) {
        std::cerr << "[Flood " << threadId << "] Starting flood with " 
                  << numIterations << " iterations, delay: " << delayMs << "ms\n";
    }
    
    // Create Modbus context
    modbus_t* ctx = modbus_new_tcp(targetHost, port);
    if (ctx == nullptr) {
        std::cerr << "[Flood " << threadId << "] Failed to create Modbus context: " << modbus_strerror(errno) << std::endl;
        return;
    }
    
    // Enable debug mode if in verbose mode
    if (g_verbose) {
        modbus_set_debug(ctx, TRUE);
    }
    
    // Set a lower timeout for flood requests to speed up failure detection
    modbus_set_response_timeout(ctx, 1, 0);
    
    // Prepare data buffer for read/write operations
    std::vector<uint8_t> data(static_cast<size_t>(numBits), 0);
    if (operation == WRITE) {
        // For write operations, set alternating bits
        for (int i = 0; i < numBits; i++) {
            data[static_cast<size_t>(i)] = (i % 2 == 0) ? 1 : 0;
        }
    }
    
    // Connect if using CONNECT_ONCE mode
    double totalConnectTime = 0.0;
    double totalOperationTime = 0.0;
    double totalDisconnectTime = 0.0;
    
    if (connMode == CONNECT_ONCE) {
        auto connectStartTime = std::chrono::high_resolution_clock::now();
        
        if (g_verbose) {
            std::cerr << "[Flood " << threadId << "] Connecting to " << targetHost << ":" << port << std::endl;
        }
        
        if (modbus_connect(ctx) == -1) {
            std::cerr << "[Flood " << threadId << "] Connection failed: " << modbus_strerror(errno) << std::endl;
            modbus_free(ctx);
            return;
        }
        
        auto connectEndTime = std::chrono::high_resolution_clock::now();
        totalConnectTime = std::chrono::duration<double, std::milli>(connectEndTime - connectStartTime).count();
        
        if (g_verbose) {
            std::cerr << "[Flood " << threadId << "] Connected successfully in " 
                      << std::fixed << std::setprecision(2) << totalConnectTime << "ms" << std::endl;
        }
    }
    
    // Run benchmark iterations with consistent flood rate
    for (int i = 0; i < numIterations; i++) {
        auto startTime = std::chrono::high_resolution_clock::now();
        int rslt = -1;
        
        // Connect if using CONNECT_PER_OP mode
        double connectTime = 0.0;
        double operationTime = 0.0;
        double disconnectTime = 0.0;
        
        if (connMode == CONNECT_PER_OP) {
            auto connectStartTime = std::chrono::high_resolution_clock::now();
            
            if (modbus_connect(ctx) == -1) {
                stats.failedRequests++;
                // Track if it's specifically a timeout error
                if (errno == ETIMEDOUT) {
                    stats.timeoutRequests++;
                }
                continue;
            }
            
            auto connectEndTime = std::chrono::high_resolution_clock::now();
            connectTime = std::chrono::duration<double, std::milli>(connectEndTime - connectStartTime).count();
            totalConnectTime += connectTime;
        }
        
        // Perform the operation
        auto operationStartTime = std::chrono::high_resolution_clock::now();
        
        if (operation == READ) {
            rslt = modbus_read_bits(ctx, modbusAddr, numBits, data.data());
        } else if (operation == WRITE) {
            rslt = modbus_write_bits(ctx, modbusAddr, numBits, data.data());
        }
        
        auto operationEndTime = std::chrono::high_resolution_clock::now();
        operationTime = std::chrono::duration<double, std::milli>(operationEndTime - operationStartTime).count();
        totalOperationTime += operationTime;
        
        // Disconnect if using CONNECT_PER_OP mode
        if (connMode == CONNECT_PER_OP) {
            auto disconnectStartTime = std::chrono::high_resolution_clock::now();
            modbus_close(ctx);
            auto disconnectEndTime = std::chrono::high_resolution_clock::now();
            disconnectTime = std::chrono::duration<double, std::milli>(disconnectEndTime - disconnectStartTime).count();
            totalDisconnectTime += disconnectTime;
        }
        
        // Record end time and response time
        auto endTime = std::chrono::high_resolution_clock::now();
        double responseTime = std::chrono::duration<double, std::milli>(endTime - startTime).count();
        
        // Update statistics
        if (rslt == -1) {
            stats.failedRequests++;
            // Track if it's specifically a timeout error
            if (errno == ETIMEDOUT) {
                stats.timeoutRequests++;
            }
        } else {
            stats.successfulRequests++;
            stats.responseTimes.push_back(responseTime);
            
            // Update min/max response time
            if (responseTime < stats.minResponseTime) {
                stats.minResponseTime = responseTime;
            }
            if (responseTime > stats.maxResponseTime) {
                stats.maxResponseTime = responseTime;
            }
        }
        
        // Report progress occasionally
        if (g_verbose || i % 20 == 0 || i == numIterations - 1) {
            std::cerr << "[Flood " << threadId << "] Progress: " << i + 1 << "/" << numIterations 
                      << " success: " << stats.successfulRequests 
                      << " failed: " << stats.failedRequests
                      << " timeouts: " << stats.timeoutRequests << std::endl;
        }
        
        // Maintain flood rate by adding appropriate delay
        if (delayMs > 0) {
            auto elapsedTime = std::chrono::high_resolution_clock::now() - startTime;
            auto elapsedMs = std::chrono::duration<double, std::milli>(elapsedTime).count();
            
            if (elapsedMs < delayMs) {
                double remainingDelayMs = delayMs - elapsedMs;
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(remainingDelayMs)));
            }
        }
    }
    
    // Disconnect if using CONNECT_ONCE mode
    if (connMode == CONNECT_ONCE) {
        auto disconnectStartTime = std::chrono::high_resolution_clock::now();
        modbus_close(ctx);
        auto disconnectEndTime = std::chrono::high_resolution_clock::now();
        totalDisconnectTime = std::chrono::duration<double, std::milli>(disconnectEndTime - disconnectStartTime).count();
    }
    
    // Free the context
    modbus_free(ctx);
    
    // Calculate average response time
    if (!stats.responseTimes.empty()) {
        stats.avgResponseTime = std::accumulate(stats.responseTimes.begin(), stats.responseTimes.end(), 0.0) / stats.responseTimes.size();
    }
    
    // Record timing breakdown
    stats.connectTime = totalConnectTime / (connMode == CONNECT_ONCE ? 1 : numIterations);
    stats.operationTime = totalOperationTime / numIterations;
    stats.disconnectTime = totalDisconnectTime / (connMode == CONNECT_ONCE ? 1 : numIterations);
    
    // Store the stats in the global vector
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        floodStats.push_back(stats);
    }
    
    if (g_verbose) {
        std::cerr << "[Flood " << threadId << "] Thread finished: successful=" << stats.successfulRequests 
                  << " failed=" << stats.failedRequests 
                  << " timeouts=" << stats.timeoutRequests << std::endl;
    }
}

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [OPTIONS]\n"
              << "Options:\n"
              << "  -h, --host HOST         Modbus server host (default: 127.0.0.1)\n"
              << "  -p, --port PORT         Modbus server port (default: 502)\n"
              << "  -a, --address ADDR      Modbus start address (default: 0)\n"
              << "  -b, --bits BITS         Number of bits to read/write (default: 10)\n"
              << "  -c, --children NUM      Number of flood threads (default: 4)\n"
              << "  -n, --iterations NUM    Number of iterations per flood (default: 100)\n"
              << "  -l, --legitimate NUM    Number of legitimate requests (default: 0=continuous)\n"
              << "  -r, --rate RATE         Target requests per second per flood thread (default: 0=max speed)\n"
              << "  -i, --interval MS       Interval between legitimate requests in ms (default: 1000)\n"
              << "  -m, --mode MODE         Connection mode (1=once, 2=per-operation) (default: 2)\n"
              << "  --read                  Perform read operation (default)\n"
              << "  --write                 Perform write operation\n"
              << "  -v, --verbose           Enable verbose output (default: off)\n"
              << "  --help                  Display this help message\n";
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    int port = 502;
    int address = 0;
    int bits = 10;
    int numFloodThreads = 4;
    int iterations = 100;
    int legitimateRequests = 0;  // 0 means run continuously
    int requestRate = 0; // Requests per second (0 = max speed)
    int measurementInterval = 1000; // ms between legitimate requests
    OperationType operation = READ;
    ConnectionMode connMode = CONNECT_PER_OP; // Changed default to per-op for DoS simulation
    
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
        } else if (arg == "-c" || arg == "--children") {
            if (i + 1 < argc) {
                numFloodThreads = std::atoi(argv[++i]);
            }
        } else if (arg == "-n" || arg == "--iterations") {
            if (i + 1 < argc) {
                iterations = std::atoi(argv[++i]);
            }
        } else if (arg == "-l" || arg == "--legitimate") {
            if (i + 1 < argc) {
                legitimateRequests = std::atoi(argv[++i]);
            }
        } else if (arg == "-r" || arg == "--rate") {
            if (i + 1 < argc) {
                requestRate = std::atoi(argv[++i]);
            }
        } else if (arg == "-i" || arg == "--interval") {
            if (i + 1 < argc) {
                measurementInterval = std::atoi(argv[++i]);
            }
        } else if (arg == "-m" || arg == "--mode") {
            if (i + 1 < argc) {
                int mode = std::atoi(argv[++i]);
                connMode = (mode == 2) ? CONNECT_PER_OP : CONNECT_ONCE;
            }
        } else if (arg == "--read") {
            operation = READ;
        } else if (arg == "--write") {
            operation = WRITE;
        } else if (arg == "-v" || arg == "--verbose") {
            g_verbose = true;
        }
    }
    
    std::cout << "MODBUS DOS ATTACK SIMULATION (Thread-based)\n"
              << "=======================================\n"
              << "Host: " << host << ":" << port << "\n"
              << "Operation: " << (operation == READ ? "READ" : "WRITE") << " bits\n"
              << "Connection mode: " << (connMode == CONNECT_ONCE ? "Once per thread" : "Per operation") << "\n"
              << "Number of flood threads: " << numFloodThreads << "\n"
              << "Iterations per flood: " << iterations << "\n"
              << "Legitimate requests: " << (legitimateRequests > 0 ? std::to_string(legitimateRequests) : "continuous") << "\n"
              << "Target request rate: " << (requestRate > 0 ? std::to_string(requestRate) : "MAX") << " requests/second/thread\n"
              << "Legitimate request interval: " << measurementInterval << "ms\n"
              << "Data size: " << bits << " bits\n"
              << "Verbose mode: " << (g_verbose ? "ON" : "OFF") << "\n\n";
    
    // Initialize stats storage
    floodStats.clear();
    
    // Set total threads count for synchronization
    g_totalThreads = numFloodThreads + 1;  // +1 for legitimate thread
    
    // Start the legitimate request thread
    std::thread legitThread(runLegitimateRequests, host, port, address, bits, 
                           operation, measurementInterval, legitimateRequests);
    
    // Create and start flood threads
    std::vector<std::thread> floodThreads;
    
    // Get start time
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Create flood threads
    std::cout << "Starting flood threads...\n";
    for (int i = 0; i < numFloodThreads; i++) {
        floodThreads.emplace_back(runFloodThread, host, port, address, bits, iterations,
                                 requestRate, operation, connMode, i + 1);
    }
    
    std::cout << "Started " << numFloodThreads << " flood threads\n";
    
    // Wait for all flood threads to complete
    for (auto& thread : floodThreads) {
        thread.join();
    }
    
    // Stop the legitimate request thread
    std::cout << "Flood threads completed. Stopping legitimate thread...\n";
    legitStats.running = false;
    legitThread.join();
    
    // Get end time
    auto endTime = std::chrono::high_resolution_clock::now();
    
    // Calculate elapsed time in seconds
    double elapsedTime = std::chrono::duration<double>(endTime - startTime).count();
    
    // Analyze flood thread statistics
    int totalSuccess = 0;
    int totalFailed = 0;
    int totalTimeouts = 0;
    double totalAvgResponseTime = 0.0;
    double maxResponseTime = 0.0;
    double minResponseTime = std::numeric_limits<double>::max();
    double avgConnectTime = 0.0;
    double avgOperationTime = 0.0;
    double avgDisconnectTime = 0.0;
    int floodStatsCount = 0;
    
    for (const auto& stats : floodStats) {
        if (g_verbose) {
            std::cout << "Flood " << stats.threadId << " stats: success=" << stats.successfulRequests 
                      << ", failed=" << stats.failedRequests
                      << ", timeouts=" << stats.timeoutRequests
                      << ", avg=" << std::fixed << std::setprecision(2) << stats.avgResponseTime << "ms"
                      << ", min=" << stats.minResponseTime << "ms"
                      << ", max=" << stats.maxResponseTime << "ms" << std::endl;
                      
            std::cout << "  Timing breakdown: connect=" << std::fixed << std::setprecision(2) << stats.connectTime 
                     << "ms, operation=" << stats.operationTime << "ms, disconnect=" << stats.disconnectTime << "ms" << std::endl;
        }
        
        totalSuccess += stats.successfulRequests;
        totalFailed += stats.failedRequests;
        totalTimeouts += stats.timeoutRequests;
        
        if (stats.successfulRequests > 0) {
            totalAvgResponseTime += stats.avgResponseTime;
            floodStatsCount++;
            
            avgConnectTime += stats.connectTime;
            avgOperationTime += stats.operationTime;
            avgDisconnectTime += stats.disconnectTime;
            
            if (stats.maxResponseTime > maxResponseTime) {
                maxResponseTime = stats.maxResponseTime;
            }
            if (stats.minResponseTime < minResponseTime) {
                minResponseTime = stats.minResponseTime;
            }
        }
    }
    
    // Calculate the average response time across all floods
    double avgResponseTime = 0.0;
    if (floodStatsCount > 0) {
        avgResponseTime = totalAvgResponseTime / floodStatsCount;
        avgConnectTime /= floodStatsCount;
        avgOperationTime /= floodStatsCount;
        avgDisconnectTime /= floodStatsCount;
    }
    
    // Analyze legitimate request stats
    double legitAvgResponseTime = 0.0;
    double legitMaxResponseTime = 0.0;
    double legitMinResponseTime = std::numeric_limits<double>::max();
    double legitAvgConnectTime = 0.0;
    double legitAvgOperationTime = 0.0;
    double legitAvgDisconnectTime = 0.0;
    int legitSuccessCount = 0;
    int legitFailCount = 0;
    
    std::lock_guard<std::mutex> lock(statsMutex);
    if (!legitStats.responseTimes.empty()) {
        legitAvgResponseTime = std::accumulate(legitStats.responseTimes.begin(), legitStats.responseTimes.end(), 0.0) / 
                              legitStats.responseTimes.size();
        legitMaxResponseTime = *std::max_element(legitStats.responseTimes.begin(), legitStats.responseTimes.end());
        legitMinResponseTime = *std::min_element(legitStats.responseTimes.begin(), legitStats.responseTimes.end());
        
        // Calculate connect/operation/disconnect time averages
        if (!legitStats.connectTimes.empty()) {
            legitAvgConnectTime = std::accumulate(legitStats.connectTimes.begin(), legitStats.connectTimes.end(), 0.0) / 
                                  legitStats.connectTimes.size();
        }
        if (!legitStats.operationTimes.empty()) {
            legitAvgOperationTime = std::accumulate(legitStats.operationTimes.begin(), legitStats.operationTimes.end(), 0.0) / 
                                   legitStats.operationTimes.size();
        }
        if (!legitStats.disconnectTimes.empty()) {
            legitAvgDisconnectTime = std::accumulate(legitStats.disconnectTimes.begin(), legitStats.disconnectTimes.end(), 0.0) / 
                                    legitStats.disconnectTimes.size();
        }
        
        for (size_t i = 0; i < legitStats.success.size(); i++) {
            if (legitStats.success[i]) {
                legitSuccessCount++;
            } else {
                legitFailCount++;
            }
        }
    }
    
    // Calculate requests per second
    double requestsPerSecond = totalSuccess / elapsedTime;
    double failuresPerSecond = totalFailed / elapsedTime;
    
    // Display DoS attack simulation results
    std::cout << "\nDOS ATTACK SIMULATION RESULTS\n"
              << "==============================\n"
              << "Total time: " << std::fixed << std::setprecision(3) << elapsedTime << " seconds\n\n"
              
              << "FLOOD STATISTICS:\n"
              << "  Successful requests: " << totalSuccess << "\n"
              << "  Failed requests: " << totalFailed << "\n"
              << "  Timeout requests: " << totalTimeouts << "\n"
              << "  Requests per second: " << std::fixed << std::setprecision(1) << requestsPerSecond << "\n"
              << "  Failures per second: " << failuresPerSecond << "\n"
              << "  Minimum response time: " << std::fixed << std::setprecision(2) << minResponseTime << "ms\n"
              << "  Average response time: " << avgResponseTime << "ms\n"
              << "  Maximum response time: " << maxResponseTime << "ms\n"
              << "  Timing breakdown:\n" 
              << "    Connect: " << std::fixed << std::setprecision(2) << avgConnectTime << "ms\n"
              << "    Operation: " << avgOperationTime << "ms\n"
              << "    Disconnect: " << avgDisconnectTime << "ms\n\n"
              
              << "LEGITIMATE USER STATISTICS:\n"
              << "  Total requests: " << legitStats.responseTimes.size() << "\n"
              << "  Successful requests: " << legitSuccessCount << "\n"
              << "  Failed requests: " << legitFailCount << "\n"
              << "  Success rate: " << (legitStats.responseTimes.size() > 0 ? 
                                      (100.0 * legitSuccessCount / legitStats.responseTimes.size()) : 0.0) << "%\n"
              << "  Minimum response time: " << std::fixed << std::setprecision(2) << legitMinResponseTime << "ms\n"
              << "  Average response time: " << legitAvgResponseTime << "ms\n"
              << "  Maximum response time: " << legitMaxResponseTime << "ms\n"
              << "  Timing breakdown:\n" 
              << "    Connect: " << std::fixed << std::setprecision(2) << legitAvgConnectTime << "ms\n"
              << "    Operation: " << legitAvgOperationTime << "ms\n"
              << "    Disconnect: " << legitAvgDisconnectTime << "ms\n\n"
              
              << "COMPARISON (FLOOD vs LEGITIMATE):\n"
              << "  Connect time ratio: " << std::fixed << std::setprecision(2) 
              << (legitAvgConnectTime > 0 ? (avgConnectTime / legitAvgConnectTime) : 0) << "x\n"
              << "  Operation time ratio: " << std::fixed << std::setprecision(2)
              << (legitAvgOperationTime > 0 ? (avgOperationTime / legitAvgOperationTime) : 0) << "x\n"
              << "  Disconnect time ratio: " << std::fixed << std::setprecision(2)
              << (legitAvgDisconnectTime > 0 ? (avgDisconnectTime / legitAvgDisconnectTime) : 0) << "x\n"
              << "  Total time ratio: " << std::fixed << std::setprecision(2)
              << (legitAvgResponseTime > 0 ? (avgResponseTime / legitAvgResponseTime) : 0) << "x\n";
    
    return 0;
} 