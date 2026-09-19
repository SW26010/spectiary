#include "overlays/spectral_line_list_json.h"
#include "app/local_user_state_json.h"
#include "helpers/temporary_directory.h"
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
void Require(bool value, const std::string& message)
{
    if (!value) throw std::runtime_error(message);
}
void Run()
{
    using namespace spectiary;
    const std::filesystem::path root = SPECTIARY_SOURCE_DIR;
    auto minimal = LoadSpectralLineListFromPath(root / "tests/fixtures/spectral_line_list/minimal.json");
    Require(minimal.list.has_value(), minimal.error);
    auto packaged = LoadSpectralLineListFromPath(root / "config/spectral_lines.public.json");
    Require(packaged.list.has_value(), packaged.error);
    Require(packaged.list->markers.size() > 20, "packaged data must be complete");
    auto list = *minimal.list;
    list.name = "Hα C₂ ¹³CN";
    list.description = ""; list.creator = "作者";
    list.created_at = "2026-09-20T00:00:00Z"; list.modified_at = "2026-09-20";
    list.markers = {{"a", "Hα", line_list::MarkerKind::Line, 6564.6, {}, {}, "note"},
                    {"b", "C₂", line_list::MarkerKind::Band, {}, 5100, 5200, {}},
                    {"c", "¹³CN", line_list::MarkerKind::Line, 8000, {}, {}, ""}};
    list.grouping_views = {{"v1", "One", {{"g1", "Unassigned", {"b", "a"}}, {"g2", "Two", {"a"}}}},
                           {"v2", "Two", {{"g3", "Three", {"a"}}}}};
    list.color_schemes = {{"s1", "One", {{"a", "#FF000080"}}},
                          {"s2", "Two", {{"a", "#00ff00ff"}, {"b", "#12345678"}}}};
    for (auto unit : {line_list::Unit::Angstrom, line_list::Unit::Nanometer, line_list::Unit::Micrometer}) {
        for (auto medium : {line_list::Medium::Air, line_list::Medium::Vacuum}) {
            list.coordinate = {unit, medium, true};
            std::ostringstream bytes;
            std::string error;
            Require(WriteSpectralLineListJson(list, bytes, error), error);
            const auto parsed = ParseSpectralLineListJson(bytes.str());
            Require(parsed.list && *parsed.list == list, "round trip must preserve metadata, Unicode, and authored order");
        }
    }
    std::ostringstream bytes;
    std::string error;
    Require(WriteSpectralLineListJson(list, bytes, error), error);
    auto valid = nlohmann::json::parse(bytes.str());
    const auto reject = [&](auto mutate) {
        auto invalid = valid; mutate(invalid);
        const auto result = ParseSpectralLineListJson(invalid.dump());
        Require(!result.list && !result.error.empty(), "invalid input must fail without a partial model: " + invalid.dump());
    };
    reject([](auto& j) { j["unexpected"] = 0; });
    reject([](auto& j) { j["schema_version"] = 1.0; });
    reject([](auto& j) { j["schema_version"] = 2; });
    reject([](auto& j) { j.erase("id"); });
    reject([](auto& j) { j["description"] = nullptr; });
    reject([](auto& j) { j["coordinate"]["unit"] = "meter"; });
    reject([](auto& j) { j["coordinate"]["medium"] = "unknown"; });
    reject([](auto& j) { j["coordinate"]["laboratory_rest"] = false; });
    reject([](auto& j) { j["coordinate"]["laboratory_rest"] = 1; });
    reject([](auto& j) { j["markers"][0]["coordinate"] = 0; });
    reject([](auto& j) { j["markers"][0]["coordinate"] = "6564"; });
    reject([](auto& j) { j["markers"][0]["start"] = 1; });
    reject([](auto& j) { j["markers"][1]["end"] = 5100; });
    reject([](auto& j) { j["markers"][1]["id"] = "a"; });
    reject([](auto& j) { j["markers"][0]["source_ref"] = "source"; });
    reject([](auto& j) { j["grouping_views"][1]["groups"][0]["id"] = "g1"; });
    reject([](auto& j) { j["grouping_views"][0]["groups"][0]["marker_ids"] = {"a", "a"}; });
    reject([](auto& j) { j["grouping_views"][0]["groups"][0]["marker_ids"] = {"missing"}; });
    reject([](auto& j) { j["grouping_views"][0]["groups"][0]["is_unassigned"] = true; });
    reject([](auto& j) { j["color_schemes"][0]["colors"]["missing"] = "#12345678"; });
    reject([](auto& j) { j["color_schemes"][0]["colors"]["a"] = "#FFFFFF"; });
    reject([](auto& j) { j["color_schemes"][1]["id"] = "s1"; });
    Require(!ParseSpectralLineListJson("{\"id\":1,\"id\":2}").list, "duplicate keys rejected");
    test_support::TemporaryDirectory directory;
    const auto path = directory.path() / "list.json";
    Require(SaveSpectralLineListToPathAtomic(path, list, error), error);
    Require(LoadSpectralLineListFromPath(path).list == list, "atomic save reopens identically");
    list.markers[0].coordinate = std::numeric_limits<double>::infinity();
    Require(!SaveSpectralLineListToPathAtomic(path, list, error), "invalid save fails");
    Require(LoadSpectralLineListFromPath(path).list.has_value(), "failed save preserves old document");
    std::ostringstream untouched;
    Require(!WriteSpectralLineListJson(list, untouched, error) && untouched.str().empty(), "reject non-finite before writing");
}
}
int main()
{
    try { Run(); std::cout << "Spectral Line List tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
