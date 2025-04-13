#pragma once
#include <modbus.h>

class ModbusServer {
public:
    int run();
    modbus_mapping_t* get_mapping();
private:
    modbus_mapping_t* mapping_ = nullptr;
};

