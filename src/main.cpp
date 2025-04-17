#include "server.h"
#include <iostream>
#include "device_config.h"

int main() {
    DeviceConfig::load("settings.ini");
    ModbusServer server;
    return server.run();
}
