#include "device_config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdint>

static DeviceInfo device;

void DeviceConfig::load(const std::string& ini_file) {
    std::ifstream file(ini_file);
    if (!file) {
        std::cerr << "[Warning] Could not open " << ini_file << ". Using defaults." << std::endl;
        return;
    }

    std::string line, current_section;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        if (line.front() == '[' && line.back() == ']') {
            current_section = line.substr(1, line.size() - 2);
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        auto trim = [](std::string& s) {
            size_t start = s.find_first_not_of(" \t");
            size_t end = s.find_last_not_of(" \t");
            if (start == std::string::npos || end == std::string::npos) {
                s.clear();
            } else {
                s = s.substr(start, end - start + 1);
            }
        };

        trim(key);
        trim(value);

        if (current_section == "Device") {
            if (key == "slave_name") {
                device.slave_name = value;
                std::cout << "[Info] Device name set to: " << value << std::endl;
        }
            else if (key == "device_identification") {device.device_id_string = value;
                std::cout << "[Info] Device ID string set to: " << value << std::endl;
            }
            else if (key == "slave_id") device.slave_id = static_cast<uint8_t>(std::stoi(value));
            else if (key == "run_indicator") device.run_indicator = static_cast<uint8_t>(std::stoi(value));
        }
    }
}

const DeviceInfo& DeviceConfig::get() {
    return device;
}
