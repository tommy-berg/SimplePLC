#pragma once
#include <cstddef>
#include <span>
#include <string_view>

/**
 * Files compiled into the binary (web panel, Structured Text standard library, examples,
 * documentation). Generated at build time by cmake/EmbedFiles.cmake.
 */
namespace simpleplc::embedded {

struct File {
    std::string_view path;  ///< e.g. "web/index.html", "st/stdlib.st", "examples/tank/main.st"
    const unsigned char* data;
    size_t size;

    std::string_view text() const { return {reinterpret_cast<const char*>(data), size}; }
};

std::span<const File> all();
const File* find(std::string_view path);

}  // namespace simpleplc::embedded
