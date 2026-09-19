#include "plot/scientific_label.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace spectiary {
namespace {

constexpr float kScriptScale = 0.72f;
constexpr float kScriptTracking = -0.06f;
constexpr float kSuperscriptBaselineInset = 0.14f;
constexpr float kSubscriptBaselineOffset = 0.38f;

struct EncodedScriptDigit {
    std::string_view utf8;
    char digit = '\0';
    ScientificLabelScript script = ScientificLabelScript::Baseline;
};

constexpr std::array<EncodedScriptDigit, 20> kEncodedScriptDigits = {{
    {"\xE2\x81\xB0", '0', ScientificLabelScript::Superscript},
    {"\xC2\xB9", '1', ScientificLabelScript::Superscript},
    {"\xC2\xB2", '2', ScientificLabelScript::Superscript},
    {"\xC2\xB3", '3', ScientificLabelScript::Superscript},
    {"\xE2\x81\xB4", '4', ScientificLabelScript::Superscript},
    {"\xE2\x81\xB5", '5', ScientificLabelScript::Superscript},
    {"\xE2\x81\xB6", '6', ScientificLabelScript::Superscript},
    {"\xE2\x81\xB7", '7', ScientificLabelScript::Superscript},
    {"\xE2\x81\xB8", '8', ScientificLabelScript::Superscript},
    {"\xE2\x81\xB9", '9', ScientificLabelScript::Superscript},
    {"\xE2\x82\x80", '0', ScientificLabelScript::Subscript},
    {"\xE2\x82\x81", '1', ScientificLabelScript::Subscript},
    {"\xE2\x82\x82", '2', ScientificLabelScript::Subscript},
    {"\xE2\x82\x83", '3', ScientificLabelScript::Subscript},
    {"\xE2\x82\x84", '4', ScientificLabelScript::Subscript},
    {"\xE2\x82\x85", '5', ScientificLabelScript::Subscript},
    {"\xE2\x82\x86", '6', ScientificLabelScript::Subscript},
    {"\xE2\x82\x87", '7', ScientificLabelScript::Subscript},
    {"\xE2\x82\x88", '8', ScientificLabelScript::Subscript},
    {"\xE2\x82\x89", '9', ScientificLabelScript::Subscript},
}};

struct ScientificLabelVerticalLayout {
    float baseline_y = 0.0f;
    float superscript_y = 0.0f;
    float subscript_y = 0.0f;
};

void AppendRun(
    ScientificLabel& label,
    ScientificLabelScript script,
    std::string_view text)
{
    if (text.empty()) {
        return;
    }
    if (!label.runs.empty() && label.runs.back().script == script) {
        label.runs.back().text.append(text);
        return;
    }
    label.runs.push_back({std::string(text), script});
}

std::optional<EncodedScriptDigit> ScriptDigitAt(
    std::string_view text,
    std::size_t offset)
{
    const std::string_view remaining = text.substr(offset);
    const auto match = std::find_if(
        kEncodedScriptDigits.begin(),
        kEncodedScriptDigits.end(),
        [remaining](const EncodedScriptDigit& candidate) {
            return remaining.starts_with(candidate.utf8);
        });
    if (match == kEncodedScriptDigits.end()) {
        return std::nullopt;
    }
    return *match;
}

float RunFontSize(const ScientificLabelRun& run, float font_size)
{
    return run.script == ScientificLabelScript::Baseline
               ? font_size
               : font_size * kScriptScale;
}

bool HasScript(
    const ScientificLabel& label,
    ScientificLabelScript script)
{
    return std::any_of(
        label.runs.begin(),
        label.runs.end(),
        [script](const ScientificLabelRun& run) {
            return run.script == script;
        });
}

ScientificLabelVerticalLayout MakeVerticalLayout(
    const ScientificLabel& label,
    float font_size)
{
    const bool has_superscript =
        HasScript(label, ScientificLabelScript::Superscript);
    const float baseline_y =
        has_superscript ? font_size * kSuperscriptBaselineInset : 0.0f;
    return {
        .baseline_y = baseline_y,
        .superscript_y = 0.0f,
        .subscript_y = baseline_y + font_size * kSubscriptBaselineOffset,
    };
}

float RunY(
    const ScientificLabelRun& run,
    const ScientificLabelVerticalLayout& layout)
{
    switch (run.script) {
    case ScientificLabelScript::Superscript:
        return layout.superscript_y;
    case ScientificLabelScript::Subscript:
        return layout.subscript_y;
    case ScientificLabelScript::Baseline:
    default:
        return layout.baseline_y;
    }
}

ImVec2 MeasureRun(
    const ScientificLabelRun& run,
    ImFont& font,
    float font_size)
{
    const float run_font_size = RunFontSize(run, font_size);
    if (run.script == ScientificLabelScript::Baseline ||
        run.text.size() <= 1) {
        return font.CalcTextSizeA(
            run_font_size,
            std::numeric_limits<float>::max(),
            0.0f,
            run.text.c_str());
    }

    ImVec2 result;
    for (std::size_t index = 0; index < run.text.size(); ++index) {
        const char* begin = run.text.data() + index;
        const ImVec2 digit_size = font.CalcTextSizeA(
            run_font_size,
            std::numeric_limits<float>::max(),
            0.0f,
            begin,
            begin + 1);
        result.x += digit_size.x;
        result.y = std::max(result.y, digit_size.y);
        if (index + 1 < run.text.size()) {
            result.x += font_size * kScriptTracking;
        }
    }
    return result;
}

}  // namespace

ScientificLabel ParseScientificLabel(std::string_view text)
{
    ScientificLabel label;
    for (std::size_t offset = 0; offset < text.size();) {
        if (const std::optional<EncodedScriptDigit> digit =
                ScriptDigitAt(text, offset)) {
            AppendRun(
                label,
                digit->script,
                std::string_view(&digit->digit, 1));
            offset += digit->utf8.size();
            continue;
        }

        AppendRun(
            label,
            ScientificLabelScript::Baseline,
            text.substr(offset, 1));
        ++offset;
    }
    return label;
}

const ScientificLabel& ScientificLabelCache::Resolve(
    std::string_view scope_id,
    std::string_view label_id,
    std::string_view text)
{
    if (scope_id_ != scope_id) {
        scope_id_.assign(scope_id);
        entries_.clear();
    }

    auto entry_position = entries_.find(label_id);
    if (entry_position == entries_.end()) {
        entry_position = entries_.emplace(
            std::string(label_id),
            Entry{}).first;
    }
    Entry& entry = entry_position->second;
    if (entry.source_text != text) {
        entry.source_text.assign(text);
        entry.label = ParseScientificLabel(text);
    }
    return entry.label;
}

ScientificLabelSize MeasureScientificLabel(
    const ScientificLabel& label,
    ImFont& font,
    float font_size)
{
    ScientificLabelSize result;
    const ScientificLabelVerticalLayout vertical =
        MakeVerticalLayout(label, font_size);
    for (const ScientificLabelRun& run : label.runs) {
        const ImVec2 run_size = MeasureRun(run, font, font_size);
        result.width += run_size.x;
        result.height = std::max(
            result.height,
            RunY(run, vertical) + run_size.y);
    }
    return result;
}

float ScientificLabelLineHeight(ImFont& font, float font_size)
{
    const ImVec2 baseline_size = font.CalcTextSizeA(
        font_size,
        std::numeric_limits<float>::max(),
        0.0f,
        "H");
    const ImVec2 script_size = font.CalcTextSizeA(
        font_size * kScriptScale,
        std::numeric_limits<float>::max(),
        0.0f,
        "2");
    const float baseline_y = font_size * kSuperscriptBaselineInset;
    const float subscript_y =
        baseline_y + font_size * kSubscriptBaselineOffset;
    return std::max(
        baseline_y + baseline_size.y,
        subscript_y + script_size.y);
}

void DrawScientificLabel(
    ImDrawList& draw_list,
    const ScientificLabel& label,
    ImFont& font,
    float font_size,
    const ImVec2& top_left,
    ImU32 color)
{
    const ScientificLabelVerticalLayout vertical =
        MakeVerticalLayout(label, font_size);
    float x = top_left.x;
    for (const ScientificLabelRun& run : label.runs) {
        const ImVec2 run_size = MeasureRun(run, font, font_size);
        const float run_font_size = RunFontSize(run, font_size);
        const float y = top_left.y + RunY(run, vertical);
        if (run.script == ScientificLabelScript::Baseline ||
            run.text.size() <= 1) {
            draw_list.AddText(
                &font,
                run_font_size,
                ImVec2(x, y),
                color,
                run.text.c_str());
        } else {
            float digit_x = x;
            for (std::size_t index = 0; index < run.text.size(); ++index) {
                const char* begin = run.text.data() + index;
                const ImVec2 digit_size = font.CalcTextSizeA(
                    run_font_size,
                    std::numeric_limits<float>::max(),
                    0.0f,
                    begin,
                    begin + 1);
                draw_list.AddText(
                    &font,
                    run_font_size,
                    ImVec2(digit_x, y),
                    color,
                    begin,
                    begin + 1);
                digit_x += digit_size.x;
                if (index + 1 < run.text.size()) {
                    digit_x += font_size * kScriptTracking;
                }
            }
        }
        x += run_size.x;
    }
}

}  // namespace spectiary
