#include "plot/scientific_label.h"
#include "ui/ui_font.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string Utf8(std::u8string_view value)
{
    return std::string(value.begin(), value.end());
}

void TestParsesCompactBalmerLabelAsOneBaselineRun()
{
    const specforge::ScientificLabel label =
        specforge::ParseScientificLabel(Utf8(u8"H\u03b1"));
    Require(label.runs.size() == 1, "H-alpha should remain one baseline run");
    Require(label.runs[0].text == Utf8(u8"H\u03b1"), "H-alpha should not gain spacing or markup");
    Require(
        label.runs[0].script == specforge::ScientificLabelScript::Baseline,
        "H-alpha should remain on the baseline");
}

void TestGroupsAdjacentIsotopeDigitsIntoOneSuperscriptRun()
{
    const specforge::ScientificLabel label =
        specforge::ParseScientificLabel(Utf8(u8"\u00b9\u00b3C\u00b9\u00b2C"));
    Require(label.runs.size() == 4, "the isotopologue should produce four semantic runs");
    Require(
        label.runs[0].text == "13" &&
            label.runs[0].script == specforge::ScientificLabelScript::Superscript,
        "adjacent isotope digits should become one compact superscript run");
    Require(
        label.runs[1].text == "C" &&
            label.runs[1].script == specforge::ScientificLabelScript::Baseline,
        "the first carbon should remain on the baseline");
    Require(
        label.runs[2].text == "12" &&
            label.runs[2].script == specforge::ScientificLabelScript::Superscript,
        "the second isotope number should become one compact superscript run");
    Require(label.runs[3].text == "C", "the second carbon should remain present");
}

void TestConvertsMolecularAndTransitionIndicesToSubscriptRuns()
{
    const specforge::ScientificLabel molecule =
        specforge::ParseScientificLabel(Utf8(u8"C\u2082"));
    Require(molecule.runs.size() == 2, "C2 should produce baseline and subscript runs");
    Require(
        molecule.runs[1].text == "2" &&
            molecule.runs[1].script == specforge::ScientificLabelScript::Subscript,
        "the molecular atom count should become a subscript run");

    const specforge::ScientificLabel transition =
        specforge::ParseScientificLabel(Utf8(u8"Na I D\u2082"));
    Require(transition.runs.size() == 2, "Na I D2 should produce baseline and subscript runs");
    Require(transition.runs[0].text == "Na I D", "the transition prefix should remain unchanged");
    Require(transition.runs[1].text == "2", "the transition index should remain present");
}

void TestCacheReusesLabelsAndInvalidatesChangedTextOrScope()
{
    specforge::ScientificLabelCache cache;
    const specforge::ScientificLabel* first =
        &cache.Resolve("catalog-a", "isotope", Utf8(u8"\u00b9\u00b3CN"));
    const specforge::ScientificLabel* repeated =
        &cache.Resolve("catalog-a", "isotope", Utf8(u8"\u00b9\u00b3CN"));
    Require(first == repeated, "unchanged catalog labels should reuse their parsed representation");

    const specforge::ScientificLabel& changed =
        cache.Resolve("catalog-a", "isotope", Utf8(u8"\u00b9\u00b2CN"));
    Require(changed.runs[0].text == "12", "changed label text should be reparsed");

    const specforge::ScientificLabel& new_scope =
        cache.Resolve("catalog-b", "isotope", Utf8(u8"C\u2082"));
    Require(new_scope.runs[0].text == "C", "catalog scope changes should discard stale parsed labels");
}

struct ImGuiContextDeleter {
    void operator()(ImGuiContext* context) const
    {
        ImGui::DestroyContext(context);
    }
};

void TestProductionScientificFontProducesCompactScriptLayout()
{
    const std::unique_ptr<ImGuiContext, ImGuiContextDeleter> context(ImGui::CreateContext());
    Require(context != nullptr, "ImGui context should be created");

    const specforge::UiFontSelection selection = specforge::AddUiFonts(ImGui::GetIO());
    Require(selection.spectral_label_font != nullptr, "a dedicated spectral-label font should load");
    ImFont& font = *selection.spectral_label_font;
    constexpr float kFontSize = 16.0f;

    const specforge::ScientificLabel label =
        specforge::ParseScientificLabel(Utf8(u8"\u00b9\u00b3"));
    const specforge::ScientificLabelSize compact =
        specforge::MeasureScientificLabel(label, font, kFontSize);
    const std::string unicode_superscript = Utf8(u8"\u00b9\u00b3");
    const float direct_unicode_width =
        font.CalcTextSizeA(
                kFontSize,
                1000.0f,
                0.0f,
                unicode_superscript.c_str())
            .x;

    Require(compact.width > 0.0f && compact.height > 0.0f, "scientific layout should be measurable");
    Require(
        compact.width < direct_unicode_width,
        "tracked script digits should be tighter than direct Unicode superscript glyphs; compact=" +
            std::to_string(compact.width) + ", direct=" +
            std::to_string(direct_unicode_width));
    Require(
        std::abs(
            specforge::ScientificLabelLineHeight(font, kFontSize) -
            specforge::MeasureScientificLabel(
                specforge::ParseScientificLabel(Utf8(u8"\u00b9\u00b3C\u2082")),
                font,
                kFontSize)
                .height) <
            0.01f,
        "the line-height contract should cover simultaneous superscript and subscript runs");
}

}  // namespace

int main()
{
    try {
        TestParsesCompactBalmerLabelAsOneBaselineRun();
        TestGroupsAdjacentIsotopeDigitsIntoOneSuperscriptRun();
        TestConvertsMolecularAndTransitionIndicesToSubscriptRuns();
        TestCacheReusesLabelsAndInvalidatesChangedTextOrScope();
        TestProductionScientificFontProducesCompactScriptLayout();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
