#pragma once
#include <string>
#include <cstdint>

struct DeviceInfo {
    std::string slave_name;         
    std::string device_id_string;  
    uint8_t slave_id;
    uint8_t run_indicator;
};

class DeviceConfig {
public:
    static void load(const std::string& ini_file = "settings.ini");
    static const DeviceInfo& get();
};
