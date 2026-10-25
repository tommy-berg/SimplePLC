#include "app/device_info.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/version.h"
#include "net/socket.h"
#include "opcua/opcua_server.h"
#include "st/compiler.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <cstdlib>
#endif

#ifdef __linux__
#include <linux/if_packet.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <net/if_dl.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#endif

namespace simpleplc {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

#ifndef _WIN32
std::string trim(std::string s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char c) { return c != '\0' && !std::isspace(c); }).base(), s.end());
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); }));
    return s;
}
#endif

json textOrNull(const std::string& s) { return s.empty() ? json(nullptr) : json(s); }

#ifdef __linux__
/// A file's whole content, or empty (also for files that do not exist, like /proc on macOS).
std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

/// "Key: value" lines (/proc/meminfo, /proc/cpuinfo, /proc/self/status) and KEY="value" (/etc/os-release).
std::map<std::string, std::string> keyValues(const std::string& text, char separator) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t at = line.find(separator);
        if (at == std::string::npos) continue;
        std::string key = trim(line.substr(0, at));
        std::string value = trim(line.substr(at + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (!key.empty() && !out.contains(key)) out[key] = value;
    }
    return out;
}

/// "16384 kB" -> 16384, "4" -> 4
json leadingNumber(const std::map<std::string, std::string>& values, const std::string& key) {
    auto it = values.find(key);
    if (it == values.end()) return nullptr;
    try {
        return std::stoll(it->second);
    } catch (...) {
        return nullptr;
    }
}
#endif

#ifdef __APPLE__
std::string sysctlText(const char* name) {
    size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) return {};
    return trim(value);
}

template <typename T>
json sysctlNumber(const char* name) {
    T value{};
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) return nullptr;
    return value;
}
#endif

#ifdef __linux__
/// Docker, Podman or Kubernetes, from the marks they leave; null on bare metal.
json containerKind() {
    std::error_code ec;
    if (fs::exists("/.dockerenv", ec)) return "docker";
    if (fs::exists("/run/.containerenv", ec)) return "podman";
    std::string cgroup = readText("/proc/1/cgroup");
    if (cgroup.find("kubepods") != std::string::npos) return "kubernetes";
    if (cgroup.find("docker") != std::string::npos) return "docker";
    return nullptr;
}

/// The CPU temperature: the thermal zone of the CPU/SoC, or the first one (Raspberry Pi: cpu-thermal).
json cpuTemperature() {
    json first = nullptr;
    std::error_code ec;
    for (int i = 0; i < 16; i++) {
        fs::path zone = std::format("/sys/class/thermal/thermal_zone{}", i);
        if (!fs::exists(zone, ec)) break;
        std::string raw = trim(readText(zone / "temp"));
        if (raw.empty()) continue;
        double celsius = 0;
        try {
            celsius = std::stod(raw) / 1000.0;
        } catch (...) {
            continue;
        }
        celsius = std::round(celsius * 10.0) / 10.0;
        std::string type = trim(readText(zone / "type"));
        if (type.find("cpu") != std::string::npos || type.find("soc") != std::string::npos || type.find("x86_pkg") != std::string::npos) {
            return celsius;
        }
        if (first.is_null()) first = celsius;
    }
    return first;
}

/// The memory limit of this container (cgroup v2 or v1), in kB; null when there is none.
json memoryLimitKb() {
    std::string v2 = trim(readText("/sys/fs/cgroup/memory.max"));
    std::string v1 = trim(readText("/sys/fs/cgroup/memory/memory.limit_in_bytes"));
    for (const std::string& raw : {v2, v1}) {
        if (raw.empty() || raw == "max") continue;
        try {
            long long bytes = std::stoll(raw);
            if (bytes > 0 && bytes < (1LL << 50)) return bytes / 1024;  // v1 reports "no limit" as a huge number
        } catch (...) {
        }
    }
    return nullptr;
}
#endif

#ifndef _WIN32
json networkInterfaces() {
    json out = json::array();
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    std::map<std::string, json> byName;
    std::vector<std::string> order;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || (a->ifa_flags & IFF_LOOPBACK)) continue;
        std::string name = a->ifa_name;
        if (!byName.contains(name)) {
            byName[name] = {{"name", name}, {"up", (a->ifa_flags & IFF_UP) != 0}, {"mac", nullptr}, {"addresses", json::array()}};
            order.push_back(name);
        }
        json& iface = byName[name];
        int family = a->ifa_addr->sa_family;
        if (family == AF_INET6 && IN6_IS_ADDR_LINKLOCAL(&reinterpret_cast<sockaddr_in6*>(a->ifa_addr)->sin6_addr)) {
            continue;  // fe80:: exists on every interface and reaches nothing beyond the link
        }
        if (family == AF_INET || family == AF_INET6) {
            char text[INET6_ADDRSTRLEN] = {};
            const void* addr = family == AF_INET ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr)
                                                 : static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(a->ifa_addr)->sin6_addr);
            if (!inet_ntop(family, addr, text, sizeof(text))) continue;
            int prefix = 0;
            if (a->ifa_netmask) {
                const auto* mask = family == AF_INET ? reinterpret_cast<const unsigned char*>(&reinterpret_cast<sockaddr_in*>(a->ifa_netmask)->sin_addr)
                                                     : reinterpret_cast<const unsigned char*>(&reinterpret_cast<sockaddr_in6*>(a->ifa_netmask)->sin6_addr);
                for (int i = 0; i < (family == AF_INET ? 4 : 16); i++) prefix += std::popcount(static_cast<unsigned>(mask[i]));
            }
            iface["addresses"].push_back(std::format("{}/{}", text, prefix));
        }
#ifdef __linux__
        else if (family == AF_PACKET) {
            const auto* ll = reinterpret_cast<const sockaddr_ll*>(a->ifa_addr);
            if (ll->sll_halen == 6) {
                const unsigned char* m = ll->sll_addr;
                iface["mac"] = std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", m[0], m[1], m[2], m[3], m[4], m[5]);
            }
        }
#endif
#ifdef __APPLE__
        else if (family == AF_LINK) {
            const auto* dl = reinterpret_cast<const sockaddr_dl*>(a->ifa_addr);
            if (dl->sdl_alen == 6) {
                const auto* m = reinterpret_cast<const unsigned char*>(LLADDR(dl));
                iface["mac"] = std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", m[0], m[1], m[2], m[3], m[4], m[5]);
            }
        }
#endif
    }
    freeifaddrs(list);
    // Interfaces with an address first: those are the ones other devices reach this one on
    for (const auto& name : order) {
        if (!byName[name]["addresses"].empty()) out.push_back(byName[name]);
    }
    return out;
}
#endif

}  // namespace

json collectDeviceInfo(const fs::path& dataDir, std::chrono::steady_clock::time_point started) {
    json device = {{"hostname", textOrNull(net::hostname())}, {"model", nullptr}, {"container", nullptr}};
    json system = {{"os", nullptr}, {"kernel", nullptr}, {"arch", nullptr}, {"uptime_s", nullptr},
                   {"time_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch()).count()}};
    json cpu = {{"model", nullptr}, {"cores", std::thread::hardware_concurrency()}, {"load", nullptr}, {"temperature_c", nullptr}};
    json memory = {{"total_kb", nullptr}, {"available_kb", nullptr}, {"limit_kb", nullptr}};
    json process = {{"uptime_s", std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count()},
                    {"rss_kb", nullptr}, {"threads", nullptr}, {"cpu_s", nullptr}};
    json network = json::array();

#ifndef _WIN32
    utsname u{};
    if (uname(&u) == 0) {
        system["kernel"] = std::format("{} {}", u.sysname, u.release);
        system["arch"] = u.machine;
    }
    double load[3] = {};
    if (getloadavg(load, 3) == 3) {
        cpu["load"] = {std::round(load[0] * 100) / 100, std::round(load[1] * 100) / 100, std::round(load[2] * 100) / 100};
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        double seconds = static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                         static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
        process["cpu_s"] = std::round(seconds * 100) / 100;
    }
    network = networkInterfaces();
#endif

#ifdef __linux__
    std::string model = trim(readText("/proc/device-tree/model"));  // Raspberry Pi and other boards
    if (model.empty()) {
        std::string vendor = trim(readText("/sys/class/dmi/id/sys_vendor"));
        std::string product = trim(readText("/sys/class/dmi/id/product_name"));
        model = trim(vendor + " " + product);
    }
    device["model"] = textOrNull(model);
    device["container"] = containerKind();
    auto osRelease = keyValues(readText("/etc/os-release"), '=');
    if (osRelease.contains("PRETTY_NAME")) system["os"] = osRelease["PRETTY_NAME"];
    std::string uptime = readText("/proc/uptime");
    if (!uptime.empty()) {
        try {
            system["uptime_s"] = static_cast<int64_t>(std::stod(uptime));
        } catch (...) {
        }
    }
    auto cpuinfo = keyValues(readText("/proc/cpuinfo"), ':');
    for (const char* key : {"model name", "Model", "Hardware", "cpu model"}) {
        if (cpuinfo.contains(key) && !cpuinfo[key].empty()) {
            cpu["model"] = cpuinfo[key];
            break;
        }
    }
    cpu["temperature_c"] = cpuTemperature();
    auto meminfo = keyValues(readText("/proc/meminfo"), ':');
    memory["total_kb"] = leadingNumber(meminfo, "MemTotal");
    memory["available_kb"] = leadingNumber(meminfo, "MemAvailable");
    memory["limit_kb"] = memoryLimitKb();
    auto status = keyValues(readText("/proc/self/status"), ':');
    process["rss_kb"] = leadingNumber(status, "VmRSS");
    process["threads"] = leadingNumber(status, "Threads");
#endif

#ifdef __APPLE__
    device["model"] = textOrNull(sysctlText("hw.model"));
    std::string version = sysctlText("kern.osproductversion");
    if (!version.empty()) system["os"] = "macOS " + version;
    cpu["model"] = textOrNull(sysctlText("machdep.cpu.brand_string"));
    timeval boot{};
    size_t size = sizeof(boot);
    if (sysctlbyname("kern.boottime", &boot, &size, nullptr, 0) == 0) {
        system["uptime_s"] = static_cast<int64_t>(time(nullptr) - boot.tv_sec);
    }
    json total = sysctlNumber<uint64_t>("hw.memsize");
    if (!total.is_null()) memory["total_kb"] = total.get<uint64_t>() / 1024;
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
        uint64_t pages = static_cast<uint64_t>(vm.free_count) + vm.inactive_count + vm.purgeable_count;
        memory["available_kb"] = pages * vm_page_size / 1024;
    }
    mach_task_basic_info_data_t task{};
    mach_msg_type_number_t taskCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task), &taskCount) == KERN_SUCCESS) {
        process["rss_kb"] = task.resident_size / 1024;
    }
#endif

#ifdef _WIN32
    system["os"] = "Windows";
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: system["arch"] = "x86_64"; break;
        case PROCESSOR_ARCHITECTURE_ARM64: system["arch"] = "arm64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: system["arch"] = "x86"; break;
        default: break;
    }
    system["uptime_s"] = static_cast<int64_t>(GetTickCount64() / 1000);
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        memory["total_kb"] = mem.ullTotalPhys / 1024;
        memory["available_kb"] = mem.ullAvailPhys / 1024;
    }
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        process["rss_kb"] = counters.WorkingSetSize / 1024;
    }
#endif

    json storage = {{"path", dataDir.string()}, {"total_kb", nullptr}, {"free_kb", nullptr}};
    std::error_code ec;
    fs::space_info space = fs::space(dataDir, ec);
    if (!ec) {
        storage["total_kb"] = space.capacity / 1024;
        storage["free_kb"] = space.available / 1024;
    }

    json software = {{"simpleplc", kVersion}, {"structured_text", st::kCompilerVersion}, {"open62541", opcua::libraryVersion()}};

    return {{"device", device}, {"system", system}, {"cpu", cpu}, {"memory", memory}, {"storage", storage},
            {"network", network}, {"process", process}, {"software", software}};
}

}  // namespace simpleplc
