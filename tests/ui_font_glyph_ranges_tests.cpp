#include "ui/ui_font.h"

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

bool ContainsGlyph(const ImWchar* ranges, ImWchar glyph)
{
    for (const ImWchar* range = ranges; range[0] != 0; range += 2) {
        if (glyph >= range[0] && glyph <= range[1]) {
            return true;
        }
    }
    return false;
}

void TestScientificGlyphRangesCoverSpectralLineNotation()
{
    Require(ContainsGlyph(specforge::kScientificGlyphRanges, 0x03b1), "Greek alpha should be covered");
    Require(ContainsGlyph(specforge::kScientificGlyphRanges, 0x03b4), "Greek delta should be covered");
    Require(ContainsGlyph(specforge::kScientificGlyphRanges, 0x00b9), "superscript 1 should be covered");
    Require(ContainsGlyph(specforge::kScientificGlyphRanges, 0x00b2), "superscript 2 should be covered");
    Require(ContainsGlyph(specforge::kScientificGlyphRanges, 0x00b3), "superscript 3 should be covered");
    Require(ContainsGlyph(specforge::kScientificGlyphRanges, 0x2074), "superscript 4 should be covered");
    Require(ContainsGlyph(specforge::kScientificGlyphRanges, 0x2082), "subscript 2 should be covered");
    Require(ContainsGlyph(specforge::kSpectralLabelGlyphRanges, 'H'), "spectral labels should cover Latin");
    Require(
        ContainsGlyph(specforge::kSpectralLabelGlyphRanges, 0x03b1),
        "spectral labels should cover Greek alpha");
}

struct ImGuiContextDeleter {
    void operator()(ImGuiContext* context) const
    {
        ImGui::DestroyContext(context);
    }
};

void TestProductionFontSelection()
{
    const std::unique_ptr<ImGuiContext, ImGuiContextDeleter> context(ImGui::CreateContext());
    Require(context != nullptr, "ImGui context should be created");

    ImGuiIO& io = ImGui::GetIO();
    const specforge::UiFontSelection selection = specforge::AddUiFonts(io);
    Require(selection.scientific_font.has_value(), "a Windows scientific fallback font should load");
    Require(selection.cjk_font.has_value(), "a Windows CJK fallback font should load");
    Require(
        selection.cjk_font->filename() == L"msyh.ttc",
        "Microsoft YaHei should be the preferred CJK font when available");
    Require(selection.spectral_label_font != nullptr, "a dedicated spectral-label font should load");
    Require(!io.Fonts->Fonts.empty(), "the primary UI font should exist");

    ImFont* primary_font = io.Fonts->Fonts.front();
    Require(primary_font->IsGlyphInFont(0x03b1), "the selected font should contain Greek alpha");
    Require(primary_font->IsGlyphInFont(0x03b4), "the selected font should contain Greek delta");
    Require(primary_font->IsGlyphInFont(0x00b9), "the selected font should contain superscript 1");
    Require(primary_font->IsGlyphInFont(0x00b2), "the selected font should contain superscript 2");
    Require(primary_font->IsGlyphInFont(0x2082), "the selected font should contain subscript 2");
    Require(primary_font->IsGlyphInFont(0x4e2d), "the selected font should contain CJK glyphs");
    Require(selection.spectral_label_font->IsGlyphInFont('H'), "spectral labels should contain Latin H");
    Require(
        selection.spectral_label_font->IsGlyphInFont(0x03b1),
        "spectral labels should contain Greek alpha in the same font");
    Require(
        selection.spectral_label_font->IsGlyphInFont('1') &&
            selection.spectral_label_font->IsGlyphInFont('3'),
        "spectral labels should contain isotope digits in the same font");
}

}  // namespace

int main()
{
    try {
        TestScientificGlyphRangesCoverSpectralLineNotation();
        TestProductionFontSelection();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
