#pragma once

#include <imgui.h>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace spectiary {

enum class ScientificLabelScript {
    Baseline,
    Superscript,
    Subscript,
};

struct ScientificLabelRun {
    std::string text;
    ScientificLabelScript script = ScientificLabelScript::Baseline;
};

struct ScientificLabel {
    std::vector<ScientificLabelRun> runs;
};

struct ScientificLabelSize {
    float width = 0.0f;
    float height = 0.0f;
};

class ScientificLabelCache {
public:
    [[nodiscard]] const ScientificLabel& Resolve(
        std::string_view scope_id,
        std::string_view label_id,
        std::string_view text);

private:
    struct Entry {
        std::string source_text;
        ScientificLabel label;
    };

    std::string scope_id_;
    std::map<std::string, Entry, std::less<>> entries_;
};

[[nodiscard]] ScientificLabel ParseScientificLabel(std::string_view text);
[[nodiscard]] ScientificLabelSize MeasureScientificLabel(
    const ScientificLabel& label,
    ImFont& font,
    float font_size);
[[nodiscard]] float ScientificLabelLineHeight(ImFont& font, float font_size);
void DrawScientificLabel(
    ImDrawList& draw_list,
    const ScientificLabel& label,
    ImFont& font,
    float font_size,
    const ImVec2& top_left,
    ImU32 color);

}  // namespace spectiary
