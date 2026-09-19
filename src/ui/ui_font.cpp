#include "ui/ui_font.h"

#include <shlobj_core.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace spectiary {
namespace {

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::optional<std::filesystem::path> WindowsFontsDirectory()
{
    PWSTR fonts_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Fonts, KF_FLAG_DEFAULT, nullptr, &fonts_path)) ||
        fonts_path == nullptr) {
        return std::nullopt;
    }

    std::filesystem::path result(fonts_path);
    CoTaskMemFree(fonts_path);
    return result;
}

template <std::size_t FontCount>
std::optional<std::filesystem::path> MergeFirstAvailableFont(
    ImGuiIO& io,
    const std::filesystem::path& fonts_directory,
    const std::array<const wchar_t*, FontCount>& preferred_fonts,
    const ImWchar* glyph_ranges)
{
    ImFontConfig font_config;
    font_config.MergeMode = true;
    font_config.OversampleH = 2;
    font_config.OversampleV = 1;

    for (const wchar_t* filename : preferred_fonts) {
        const std::filesystem::path candidate = fonts_directory / filename;
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error) || error) {
            continue;
        }

        const std::string font_path = PathToUtf8(candidate);
        // ImGui 1.92 requires merged fonts to keep the same implicit reference-size
        // mode as the default vector font; passing LegacySize here asserts.
        if (io.Fonts->AddFontFromFileTTF(
                font_path.c_str(),
                0.0f,
                &font_config,
                glyph_ranges) != nullptr) {
            return candidate;
        }
    }

    return std::nullopt;
}

}  // namespace

UiFontSelection AddUiFonts(ImGuiIO& io)
{
    io.Fonts->AddFontDefaultVector();
    const std::optional<std::filesystem::path> fonts_directory = WindowsFontsDirectory();
    if (!fonts_directory) {
        return {};
    }

    constexpr std::array<const wchar_t*, 3> kPreferredScientificFonts = {
        L"segoeui.ttf",
        L"seguisym.ttf",
        L"arial.ttf",
    };
    // msyh.ttc's default face is regular Microsoft YaHei. Selecting
    // Microsoft YaHei UI would require a separately validated TTC face index.
    constexpr std::array<const wchar_t*, 5> kPreferredCjkFonts = {
        L"msyh.ttc",
        L"NotoSansSC-VF.ttf",
        L"Deng.ttf",
        L"simhei.ttf",
        L"simsun.ttc",
    };

    UiFontSelection selection;
    selection.scientific_font = MergeFirstAvailableFont(
        io,
        *fonts_directory,
        kPreferredScientificFonts,
        kScientificGlyphRanges);
    selection.cjk_font = MergeFirstAvailableFont(
        io,
        *fonts_directory,
        kPreferredCjkFonts,
        kCjkGlyphRanges);

    if (selection.scientific_font) {
        ImFontConfig font_config;
        font_config.OversampleH = 2;
        font_config.OversampleV = 1;
        const std::string font_path = PathToUtf8(*selection.scientific_font);
        selection.spectral_label_font = io.Fonts->AddFontFromFileTTF(
            font_path.c_str(),
            0.0f,
            &font_config,
            kSpectralLabelGlyphRanges);
    }
    return selection;
}

}  // namespace spectiary
