#include "modbus/pdu.h"

#include <algorithm>
#include <format>

namespace simpleplc::modbus {
namespace {

uint16_t be16(std::span<const uint8_t> p, size_t offset) {
    return static_cast<uint16_t>((p[offset] << 8) | p[offset + 1]);
}

void put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

std::vector<uint8_t> exception(uint8_t function, uint8_t code) {
    return {static_cast<uint8_t>(function | 0x80), code};
}

std::vector<uint8_t> readEntries(std::span<const uint8_t> pdu, ProcessImage& image, Area area,
                                 uint16_t maxCount) {
    const uint8_t function = pdu[0];
    if (pdu.size() != 5) return exception(function, kIllegalDataValue);
    uint16_t address = be16(pdu, 1);
    uint16_t count = be16(pdu, 3);
    if (count < 1 || count > maxCount) return exception(function, kIllegalDataValue);
    if (!image.contains(address, count)) return exception(function, kIllegalDataAddress);

    std::vector<uint16_t> values(count);
    image.read(area, address, values);

    std::vector<uint8_t> out{function};
    if (isBitArea(area)) {
        size_t bytes = (count + 7u) / 8u;
        out.push_back(static_cast<uint8_t>(bytes));
        out.resize(2 + bytes, 0);
        for (size_t i = 0; i < count; i++) {
            if (values[i]) out[2 + i / 8] = static_cast<uint8_t>(out[2 + i / 8] | (1u << (i % 8)));
        }
    } else {
        out.push_back(static_cast<uint8_t>(count * 2));
        for (uint16_t v : values) put16(out, v);
    }
    return out;
}

std::vector<uint8_t> writeSingleCoil(std::span<const uint8_t> pdu, ProcessImage& image) {
    if (pdu.size() != 5) return exception(pdu[0], kIllegalDataValue);
    uint16_t address = be16(pdu, 1);
    uint16_t value = be16(pdu, 3);
    if (value != 0x0000 && value != 0xFF00) return exception(pdu[0], kIllegalDataValue);
    if (!image.contains(address)) return exception(pdu[0], kIllegalDataAddress);
    image.write(Area::Coil, address, value ? 1 : 0, Origin::Modbus);
    return {pdu.begin(), pdu.end()};
}

std::vector<uint8_t> writeSingleRegister(std::span<const uint8_t> pdu, ProcessImage& image) {
    if (pdu.size() != 5) return exception(pdu[0], kIllegalDataValue);
    uint16_t address = be16(pdu, 1);
    if (!image.contains(address)) return exception(pdu[0], kIllegalDataAddress);
    // A float tag never becomes NaN or infinite from outside, not even one word at a time
    uint16_t value = be16(pdu, 3);
    if (!image.writeChecked(Area::HoldingRegister, address, std::span<const uint16_t>(&value, 1), Origin::Modbus)) {
        return exception(pdu[0], kIllegalDataValue);
    }
    return {pdu.begin(), pdu.end()};
}

std::vector<uint8_t> writeMultipleCoils(std::span<const uint8_t> pdu, ProcessImage& image) {
    if (pdu.size() < 6) return exception(pdu[0], kIllegalDataValue);
    uint16_t address = be16(pdu, 1);
    uint16_t count = be16(pdu, 3);
    uint8_t bytes = pdu[5];
    if (count < 1 || count > kMaxWriteBits || bytes != (count + 7) / 8 ||
        pdu.size() != 6u + bytes) {
        return exception(pdu[0], kIllegalDataValue);
    }
    if (!image.contains(address, count)) return exception(pdu[0], kIllegalDataAddress);
    std::vector<uint16_t> values(count);
    for (size_t i = 0; i < count; i++) values[i] = (pdu[6 + i / 8] >> (i % 8)) & 1;
    image.write(Area::Coil, address, values, Origin::Modbus);
    return {pdu.begin(), pdu.begin() + 5};
}

std::vector<uint8_t> writeMultipleRegisters(std::span<const uint8_t> pdu, ProcessImage& image) {
    if (pdu.size() < 6) return exception(pdu[0], kIllegalDataValue);
    uint16_t address = be16(pdu, 1);
    uint16_t count = be16(pdu, 3);
    uint8_t bytes = pdu[5];
    if (count < 1 || count > kMaxWriteRegisters || bytes != count * 2 || pdu.size() != 6u + bytes) {
        return exception(pdu[0], kIllegalDataValue);
    }
    if (!image.contains(address, count)) return exception(pdu[0], kIllegalDataAddress);
    std::vector<uint16_t> values(count);
    for (size_t i = 0; i < count; i++) values[i] = be16(pdu, 6 + i * 2);
    // A float tag never becomes NaN or infinite from outside: programs would compute with it
    if (!image.writeChecked(Area::HoldingRegister, address, values, Origin::Modbus)) return exception(pdu[0], kIllegalDataValue);
    return {pdu.begin(), pdu.begin() + 5};
}

std::vector<uint8_t> maskWriteRegister(std::span<const uint8_t> pdu, ProcessImage& image) {
    if (pdu.size() != 7) return exception(pdu[0], kIllegalDataValue);
    uint16_t address = be16(pdu, 1);
    if (!image.contains(address)) return exception(pdu[0], kIllegalDataAddress);
    // Result = (Current AND And_Mask) OR (Or_Mask AND (NOT And_Mask)), read and written under one lock
    if (!image.maskWrite(address, be16(pdu, 3), be16(pdu, 5), Origin::Modbus)) return exception(pdu[0], kIllegalDataValue);
    return {pdu.begin(), pdu.end()};
}

std::vector<uint8_t> readWriteMultipleRegisters(std::span<const uint8_t> pdu, ProcessImage& image) {
    if (pdu.size() < 10) return exception(pdu[0], kIllegalDataValue);
    uint16_t readAddress = be16(pdu, 1);
    uint16_t readCount = be16(pdu, 3);
    uint16_t writeAddress = be16(pdu, 5);
    uint16_t writeCount = be16(pdu, 7);
    uint8_t bytes = pdu[9];
    if (readCount < 1 || readCount > kMaxReadRegisters || writeCount < 1 || writeCount > 121 ||
        bytes != writeCount * 2 || pdu.size() != 10u + bytes) {
        return exception(pdu[0], kIllegalDataValue);
    }
    if (!image.contains(readAddress, readCount) || !image.contains(writeAddress, writeCount)) {
        return exception(pdu[0], kIllegalDataAddress);
    }
    std::vector<uint16_t> values(writeCount);
    for (size_t i = 0; i < writeCount; i++) values[i] = be16(pdu, 10 + i * 2);
    // The write happens first
    if (!image.writeChecked(Area::HoldingRegister, writeAddress, values, Origin::Modbus)) {
        return exception(pdu[0], kIllegalDataValue);
    }

    std::vector<uint16_t> read(readCount);
    image.read(Area::HoldingRegister, readAddress, read);
    std::vector<uint8_t> out{pdu[0], static_cast<uint8_t>(readCount * 2)};
    for (uint16_t v : read) put16(out, v);
    return out;
}

std::vector<uint8_t> reportServerId(std::span<const uint8_t> pdu, const DeviceIdentity& id) {
    if (pdu.size() != 1) return exception(pdu[0], kIllegalDataValue);
    std::string name = id.productName.substr(0, 240);
    std::vector<uint8_t> out{pdu[0], static_cast<uint8_t>(2 + name.size()), id.serverId,
                             static_cast<uint8_t>(id.running ? 0xFF : 0x00)};
    out.insert(out.end(), name.begin(), name.end());
    return out;
}

std::vector<uint8_t> readDeviceIdentification(std::span<const uint8_t> pdu, const DeviceIdentity& id) {
    if (pdu.size() < 2 || pdu[1] != fc::kMeiReadDeviceId) return exception(pdu[0], kIllegalFunction);
    if (pdu.size() != 4) return exception(pdu[0], kIllegalDataValue);
    uint8_t code = pdu[2];
    uint8_t objectId = pdu[3];

    const std::string* objects[] = {&id.vendorName, &id.productCode, &id.revision,
                                    &id.vendorUrl,  &id.productName, &id.modelName,
                                    &id.applicationName};
    constexpr uint8_t kObjectCount = 7;
    constexpr uint8_t kBasicCount = 3;

    uint8_t first = 0;
    uint8_t last = 0;  // exclusive
    switch (code) {
        case 1:  // basic stream
            first = objectId < kBasicCount ? objectId : 0;
            last = kBasicCount;
            break;
        case 2:  // regular stream (we have no extended objects, so 3 behaves like 2)
        case 3:
            first = objectId < kObjectCount ? objectId : 0;
            last = kObjectCount;
            break;
        case 4:  // one specific object
            if (objectId >= kObjectCount) return exception(pdu[0], kIllegalDataAddress);
            first = objectId;
            last = static_cast<uint8_t>(objectId + 1);
            break;
        default:
            return exception(pdu[0], kIllegalDataValue);
    }

    std::vector<uint8_t> out{pdu[0], fc::kMeiReadDeviceId, code, 0x82 /* regular, stream + individual */,
                             0x00 /* more follows */, 0x00 /* next object */, 0x00 /* count */};
    uint8_t count = 0;
    for (uint8_t obj = first; obj < last; obj++) {
        std::string value = objects[obj]->substr(0, 64);
        if (out.size() + 2 + value.size() > kMaxPduSize) {
            out[4] = 0xFF;  // more follows; the client continues at this object
            out[5] = obj;
            break;
        }
        out.push_back(obj);
        out.push_back(static_cast<uint8_t>(value.size()));
        out.insert(out.end(), value.begin(), value.end());
        count++;
    }
    out[6] = count;
    return out;
}

uint8_t readFunction(Area area) {
    switch (area) {
        case Area::Coil:            return fc::kReadCoils;
        case Area::DiscreteInput:   return fc::kReadDiscreteInputs;
        case Area::HoldingRegister: return fc::kReadHoldingRegisters;
        case Area::InputRegister:   return fc::kReadInputRegisters;
    }
    return fc::kReadCoils;
}

std::expected<void, std::string> checkException(std::span<const uint8_t> response,
                                                std::span<const uint8_t> request) {
    if (response.empty()) return std::unexpected("empty response");
    if (response[0] == (request[0] | 0x80)) {
        if (response.size() < 2) return std::unexpected("truncated exception response");
        return std::unexpected(std::format("device returned exception {} ({})", response[1],
                                           exceptionText(response[1])));
    }
    if (response[0] != request[0]) {
        return std::unexpected(std::format("unexpected function code 0x{:02X} in response", response[0]));
    }
    return {};
}

}  // namespace

std::string exceptionText(uint8_t code) {
    switch (code) {
        case kIllegalFunction:     return "illegal function";
        case kIllegalDataAddress:  return "illegal data address";
        case kIllegalDataValue:    return "illegal data value";
        case kServerDeviceFailure: return "server device failure";
        case 0x06:                 return "server device busy";
        case 0x0A:                 return "gateway path unavailable";
        case 0x0B:                 return "gateway target device failed to respond";
        default:                   return "unknown exception";
    }
}

std::vector<uint8_t> handleRequest(std::span<const uint8_t> pdu, ProcessImage& image,
                                   const DeviceIdentity& identity) {
    if (pdu.empty()) return exception(0, kIllegalFunction);
    switch (pdu[0]) {
        case fc::kReadCoils:              return readEntries(pdu, image, Area::Coil, kMaxReadBits);
        case fc::kReadDiscreteInputs:     return readEntries(pdu, image, Area::DiscreteInput, kMaxReadBits);
        case fc::kReadHoldingRegisters:   return readEntries(pdu, image, Area::HoldingRegister, kMaxReadRegisters);
        case fc::kReadInputRegisters:     return readEntries(pdu, image, Area::InputRegister, kMaxReadRegisters);
        case fc::kWriteSingleCoil:        return writeSingleCoil(pdu, image);
        case fc::kWriteSingleRegister:    return writeSingleRegister(pdu, image);
        case fc::kWriteMultipleCoils:     return writeMultipleCoils(pdu, image);
        case fc::kWriteMultipleRegisters: return writeMultipleRegisters(pdu, image);
        case fc::kReportServerId:         return reportServerId(pdu, identity);
        case fc::kMaskWriteRegister:      return maskWriteRegister(pdu, image);
        case fc::kReadWriteMultipleRegisters: return readWriteMultipleRegisters(pdu, image);
        case fc::kEncapsulatedInterface:  return readDeviceIdentification(pdu, identity);
        default:                          return exception(pdu[0], kIllegalFunction);
    }
}

std::vector<uint8_t> buildReadRequest(Area area, uint16_t address, uint16_t count) {
    std::vector<uint8_t> out{readFunction(area)};
    put16(out, address);
    put16(out, count);
    return out;
}

std::expected<std::vector<uint8_t>, std::string> buildWriteRequest(Area area, uint16_t address,
                                                                   std::span<const uint16_t> values) {
    if (!isClientWritable(area)) {
        return std::unexpected(std::format("{} entries are read-only over Modbus", areaName(area)));
    }
    if (values.empty()) return std::unexpected("nothing to write");
    std::vector<uint8_t> out;
    if (area == Area::Coil) {
        if (values.size() > kMaxWriteBits) {
            return std::unexpected(std::format("at most {} coils per write", kMaxWriteBits));
        }
        if (values.size() == 1) {
            out = {fc::kWriteSingleCoil};
            put16(out, address);
            put16(out, values[0] ? 0xFF00 : 0x0000);
            return out;
        }
        out = {fc::kWriteMultipleCoils};
        put16(out, address);
        put16(out, static_cast<uint16_t>(values.size()));
        size_t bytes = (values.size() + 7) / 8;
        out.push_back(static_cast<uint8_t>(bytes));
        size_t base = out.size();
        out.resize(base + bytes, 0);
        for (size_t i = 0; i < values.size(); i++) {
            if (values[i]) out[base + i / 8] = static_cast<uint8_t>(out[base + i / 8] | (1u << (i % 8)));
        }
        return out;
    }
    if (values.size() > kMaxWriteRegisters) {
        return std::unexpected(std::format("at most {} registers per write", kMaxWriteRegisters));
    }
    if (values.size() == 1) {
        out = {fc::kWriteSingleRegister};
        put16(out, address);
        put16(out, values[0]);
        return out;
    }
    out = {fc::kWriteMultipleRegisters};
    put16(out, address);
    put16(out, static_cast<uint16_t>(values.size()));
    out.push_back(static_cast<uint8_t>(values.size() * 2));
    for (uint16_t v : values) put16(out, v);
    return out;
}

std::expected<std::vector<uint16_t>, std::string> parseReadResponse(std::span<const uint8_t> response,
                                                                    std::span<const uint8_t> request) {
    if (auto ok = checkException(response, request); !ok) return std::unexpected(ok.error());
    uint16_t count = be16(request, 3);
    bool bits = request[0] == fc::kReadCoils || request[0] == fc::kReadDiscreteInputs;
    size_t expectedBytes = bits ? (count + 7u) / 8u : count * 2u;
    if (response.size() < 2 || response[1] != expectedBytes || response.size() != 2 + expectedBytes) {
        return std::unexpected("malformed read response");
    }
    std::vector<uint16_t> values(count);
    for (size_t i = 0; i < count; i++) {
        values[i] = bits ? static_cast<uint16_t>((response[2 + i / 8] >> (i % 8)) & 1)
                         : be16(response, 2 + i * 2);
    }
    return values;
}

std::expected<void, std::string> parseWriteResponse(std::span<const uint8_t> response,
                                                    std::span<const uint8_t> request) {
    if (auto ok = checkException(response, request); !ok) return ok;
    if (response.size() != 5 || !std::equal(response.begin(), response.end(), request.begin())) {
        return std::unexpected("malformed write response");
    }
    return {};
}

}  // namespace simpleplc::modbus
