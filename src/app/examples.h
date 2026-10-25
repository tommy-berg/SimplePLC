#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app/config.h"

namespace simpleplc {

/// A ready-to-run PLC program with its tags, compiled into the binary from examples/.
struct Example {
    std::string name;   ///< directory name, e.g. "tank"
    std::string title;
    std::string description;
    std::vector<TaskConfig> tasks;
    std::vector<TagDef> tags;
    std::vector<LinkConfig> links;
    std::map<std::string, std::string> programs;  ///< program name -> Structured Text source
};

std::vector<Example> examples();
std::optional<Example> findExample(const std::string& name);

}  // namespace simpleplc
