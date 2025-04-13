#include "server.h"
#include <iostream>
#include "device_config.h"

int main() {
    DeviceConfig::load("settings.ini");
    std::cout << "[Info] Starting Modbus server...\n";
    ModbusServer server;
    return server.run();
}
