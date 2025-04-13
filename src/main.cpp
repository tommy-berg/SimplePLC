#include "server.h"
#include <iostream>

int main() {
    std::cout << "Starting Modbus server...\n";
    ModbusServer server;
    return server.run();
}
