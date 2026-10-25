#pragma once
#include <chrono>
#include <filesystem>

#include <nlohmann/json.hpp>

namespace simpleplc {

/**
 * What the device a PLC runs on looks like right now, for the panel's Device
 * page and the API: model, container, operating system, CPU (load,
 * temperature), memory, storage, network interfaces and this process.
 *
 * Reads /proc and /sys on Linux (a Raspberry Pi shows its model and CPU
 * temperature; a container its memory limit), sysctl on macOS and Win32 calls
 * on Windows. Anything a platform does not offer is null.
 */
nlohmann::json collectDeviceInfo(const std::filesystem::path& dataDir, std::chrono::steady_clock::time_point started);

}  // namespace simpleplc
