#include "web/crypto.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <cstdlib>
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace simpleplc::crypto {
namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t kInitialState[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

constexpr uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
    uint32_t state[8];
    uint8_t buffer[64];
    size_t buffered = 0;
    uint64_t length = 0;

    Sha256() { std::memcpy(state, kInitialState, sizeof(state)); }

    void compress(const uint8_t* block) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t{block[i * 4]} << 24) | (uint32_t{block[i * 4 + 1]} << 16) |
                   (uint32_t{block[i * 4 + 2]} << 8) | uint32_t{block[i * 4 + 3]};
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                          kRoundConstants[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    void update(const uint8_t* data, size_t size) {
        length += size;
        while (size > 0) {
            size_t take = std::min(size, 64 - buffered);
            std::memcpy(buffer + buffered, data, take);
            buffered += take;
            data += take;
            size -= take;
            if (buffered == 64) {
                compress(buffer);
                buffered = 0;
            }
        }
    }

    Digest finish() {
        uint64_t bits = length * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buffered != 56) update(&zero, 1);
        uint8_t lengthBytes[8];
        for (int i = 0; i < 8; i++) lengthBytes[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        update(lengthBytes, 8);
        Digest out;
        for (int i = 0; i < 8; i++) {
            out[static_cast<size_t>(i) * 4] = static_cast<uint8_t>(state[i] >> 24);
            out[static_cast<size_t>(i) * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
            out[static_cast<size_t>(i) * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
            out[static_cast<size_t>(i) * 4 + 3] = static_cast<uint8_t>(state[i]);
        }
        return out;
    }
};

/// HMAC with the inner and outer key states computed once (PBKDF2 reuses them).
struct Hmac {
    Sha256 inner;
    Sha256 outer;

    explicit Hmac(std::span<const uint8_t> key) {
        uint8_t block[64] = {};
        if (key.size() > 64) {
            Digest hashed = sha256(key);
            std::memcpy(block, hashed.data(), hashed.size());
        } else {
            std::memcpy(block, key.data(), key.size());
        }
        uint8_t ipad[64], opad[64];
        for (int i = 0; i < 64; i++) {
            ipad[i] = block[i] ^ 0x36;
            opad[i] = block[i] ^ 0x5c;
        }
        inner.update(ipad, 64);
        outer.update(opad, 64);
    }

    Digest compute(const uint8_t* message, size_t size) const {
        Sha256 in = inner;
        in.update(message, size);
        Digest innerHash = in.finish();
        Sha256 out = outer;
        out.update(innerHash.data(), innerHash.size());
        return out.finish();
    }
};

}  // namespace

Digest sha256(std::span<const uint8_t> data) {
    Sha256 h;
    h.update(data.data(), data.size());
    return h.finish();
}

Digest sha256(std::string_view data) {
    return sha256(std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

Digest hmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> message) {
    return Hmac(key).compute(message.data(), message.size());
}

Digest pbkdf2Sha256(std::string_view password, std::span<const uint8_t> salt, uint32_t iterations) {
    Hmac prf(std::span(reinterpret_cast<const uint8_t*>(password.data()), password.size()));
    std::vector<uint8_t> first(salt.begin(), salt.end());
    first.insert(first.end(), {0, 0, 0, 1});  // block index 1: one block gives 32 bytes
    Digest u = prf.compute(first.data(), first.size());
    Digest result = u;
    for (uint32_t i = 1; i < iterations; i++) {
        u = prf.compute(u.data(), u.size());
        for (size_t j = 0; j < result.size(); j++) result[j] ^= u[j];
    }
    return result;
}

std::vector<uint8_t> randomBytes(size_t count) {
    std::vector<uint8_t> out(count);
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("BCryptGenRandom failed");
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    arc4random_buf(out.data(), count);
#else
    size_t filled = 0;
    while (filled < count) {
        ssize_t n = getrandom(out.data() + filled, count - filled, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("getrandom failed");
        }
        filled += static_cast<size_t>(n);
    }
#endif
    return out;
}

std::string toHex(std::span<const uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}

std::vector<uint8_t> fromHex(std::string_view hex) {
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    if (hex.size() % 2 != 0) return out;
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = value(hex[i]);
        int lo = value(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>(hi * 16 + lo));
    }
    return out;
}

bool constantTimeEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

}  // namespace simpleplc::crypto
