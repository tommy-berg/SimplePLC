#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Small, dependency-free primitives for password hashing and tokens.
namespace simpleplc::crypto {

using Digest = std::array<uint8_t, 32>;

Digest sha256(std::span<const uint8_t> data);
Digest sha256(std::string_view data);
Digest hmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> message);
/// PBKDF2-HMAC-SHA256 (RFC 8018) with a 32-byte output.
Digest pbkdf2Sha256(std::string_view password, std::span<const uint8_t> salt, uint32_t iterations);

/// Bytes from the operating system's secure random generator.
std::vector<uint8_t> randomBytes(size_t count);

std::string toHex(std::span<const uint8_t> bytes);
std::vector<uint8_t> fromHex(std::string_view hex);

/// Compares without leaking timing information about where they differ.
bool constantTimeEqual(std::string_view a, std::string_view b);

}  // namespace simpleplc::crypto
