#pragma once
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "core/process_image.h"

/**
 * Modbus application protocol: request handling for the server and request
 * building/response parsing for the client. Pure functions, no I/O.
 */
namespace simpleplc::modbus {

namespace fc {
inline constexpr uint8_t kReadCoils = 0x01;
inline constexpr uint8_t kReadDiscreteInputs = 0x02;
inline constexpr uint8_t kReadHoldingRegisters = 0x03;
inline constexpr uint8_t kReadInputRegisters = 0x04;
inline constexpr uint8_t kWriteSingleCoil = 0x05;
inline constexpr uint8_t kWriteSingleRegister = 0x06;
inline constexpr uint8_t kWriteMultipleCoils = 0x0F;
inline constexpr uint8_t kWriteMultipleRegisters = 0x10;
inline constexpr uint8_t kReportServerId = 0x11;
inline constexpr uint8_t kMaskWriteRegister = 0x16;
inline constexpr uint8_t kReadWriteMultipleRegisters = 0x17;
inline constexpr uint8_t kEncapsulatedInterface = 0x2B;
inline constexpr uint8_t kMeiReadDeviceId = 0x0E;
}  // namespace fc

enum Exception : uint8_t {
    kIllegalFunction = 0x01,
    kIllegalDataAddress = 0x02,
    kIllegalDataValue = 0x03,
    kServerDeviceFailure = 0x04,
};

inline constexpr size_t kMbapSize = 7;      ///< transaction(2) protocol(2) length(2) unit(1)
inline constexpr size_t kMaxPduSize = 253;
inline constexpr size_t kMaxAduSize = kMbapSize + kMaxPduSize;  // 260

inline constexpr uint16_t kMaxReadBits = 2000;
inline constexpr uint16_t kMaxReadRegisters = 125;
inline constexpr uint16_t kMaxWriteBits = 1968;
inline constexpr uint16_t kMaxWriteRegisters = 123;

/// Values reported by Report Server ID (0x11) and Read Device Identification (0x2B/0x0E).
struct DeviceIdentity {
    uint8_t serverId = 1;
    bool running = true;
    std::string vendorName = "SimplePLC";
    std::string productCode = "SPLC";
    std::string revision;  ///< the PLC fills in kVersion
    std::string vendorUrl = "https://github.com/tommy-berg/SimplePLC";
    std::string productName = "SimplePLC";
    std::string modelName = "Soft PLC";
    std::string applicationName;
};

std::string exceptionText(uint8_t code);

/// Handles one request PDU (function code and data) and returns the response PDU.
/// Writes go to the image with Origin::Modbus.
std::vector<uint8_t> handleRequest(std::span<const uint8_t> pdu, ProcessImage& image,
                                   const DeviceIdentity& identity);

// ── Client side ──

/// Read request for `count` entries of `area` starting at `address`.
std::vector<uint8_t> buildReadRequest(Area area, uint16_t address, uint16_t count);

/// Write request for coils (FC 5/15) or holding registers (FC 6/16). Bits are 0/1.
std::expected<std::vector<uint8_t>, std::string> buildWriteRequest(Area area, uint16_t address,
                                                                   std::span<const uint16_t> values);

/// Values from a read response; bits as 0/1.
std::expected<std::vector<uint16_t>, std::string> parseReadResponse(std::span<const uint8_t> response,
                                                                    std::span<const uint8_t> request);

std::expected<void, std::string> parseWriteResponse(std::span<const uint8_t> response,
                                                    std::span<const uint8_t> request);

}  // namespace simpleplc::modbus
