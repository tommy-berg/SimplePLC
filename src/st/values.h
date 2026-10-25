#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "st/compiler.h"

namespace simpleplc::st {

// Program variables as text and JSON, and ST literals into their bytes: what monitoring reads and writes
// (REST and MCP get_variables / set_variables).

/// Text of a variable's current value as an ST literal (INT#5, TRUE, T#1s, 'abc', Color#Red).
std::string variableText(const vm::Image& image, const vm::VarEntry& v, const uint8_t* data);

/// The value as JSON: BOOL true/false, numbers as numbers, anything else as its ST literal text.
nlohmann::json variableJson(const vm::Image& image, const vm::VarEntry& v, const uint8_t* data);

/// The bytes a variable holds for an ST literal (TRUE, 42, -1.5, T#2s, 'text'); an error if it does not fit.
std::expected<std::vector<uint8_t>, std::string> literalBytes(const vm::Image& image, const vm::VarEntry& v,
                                                              const std::string& text);

}  // namespace simpleplc::st
