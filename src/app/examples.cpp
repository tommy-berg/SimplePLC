#include "app/examples.h"

#include "app/embedded.h"
#include "app/log.h"

namespace simpleplc {
namespace {

std::optional<Example> parse(const std::string& name, std::string_view manifest) {
    json j = json::parse(manifest, nullptr, false);
    if (!j.is_object()) return std::nullopt;

    Example example;
    example.name = name;
    example.title = j.value("title", name);
    example.description = j.value("description", "");

    json plc = {{"id", "example"}, {"tags", j.value("tags", json::array())}};
    if (j.contains("tasks")) plc["tasks"] = j["tasks"];
    if (j.contains("links")) plc["links"] = j["links"];
    auto parsed = plcFromJson(plc, "examples/" + name);
    if (!parsed) {
        for (const auto& problem : parsed.error()) log::write(log::Level::Error, "examples", "", problem);
        return std::nullopt;
    }
    example.tasks = parsed->tasks;
    example.tags = parsed->tags;
    example.links = parsed->links;
    for (const TaskConfig& task : example.tasks) {
        const embedded::File* source = embedded::find("examples/" + name + "/" + task.program + ".st");
        if (!source) return std::nullopt;
        example.programs[task.program] = std::string(source->text());
    }
    return example;
}

}  // namespace

std::vector<Example> examples() {
    std::vector<Example> out;
    for (const embedded::File& file : embedded::all()) {
        std::string_view path = file.path;
        if (!path.starts_with("examples/") || !path.ends_with("/plc.json")) continue;
        std::string name(path.substr(9, path.size() - 9 - 9));
        if (auto example = parse(name, file.text())) out.push_back(std::move(*example));
    }
    // The demo first, then alphabetically
    std::stable_sort(out.begin(), out.end(), [](const Example& a, const Example& b) {
        if ((a.name == "tank") != (b.name == "tank")) return a.name == "tank";
        return a.name < b.name;
    });
    return out;
}

std::optional<Example> findExample(const std::string& name) {
    const embedded::File* manifest = embedded::find("examples/" + name + "/plc.json");
    if (!manifest) return std::nullopt;
    return parse(name, manifest->text());
}

}  // namespace simpleplc
