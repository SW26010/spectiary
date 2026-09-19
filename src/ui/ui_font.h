#pragma once

#include <imgui.h>

#include <filesystem>
#include <optional>

namespace spectiary {

inline constexpr ImWchar kScientificGlyphRanges[] = {
    0x00a0, 0x00ff,  // Latin-1 supplement, including superscript 1, 2, and 3.
    0x0370, 0x03ff,  // Greek and Coptic.
    0x2070, 0x209f,  // Superscripts and subscripts.
    0,
};

inline constexpr ImWchar kCjkGlyphRanges[] = {
    0x2000, 0x206f,  // General punctuation.
    0x2e80, 0x2eff,  // CJK radicals supplement.
    0x2f00, 0x2fdf,  // Kangxi radicals.
    0x3000, 0x30ff,  // CJK symbols, punctuation, hiragana, and katakana.
    0x31f0, 0x31ff,  // Katakana phonetic extensions.
    0x3400, 0x4dbf,  // CJK unified ideographs extension A.
    0x4e00, 0x9fff,  // CJK unified ideographs.
    0xf900, 0xfaff,  // CJK compatibility ideographs.
    0xff00, 0xffef,  // Halfwidth and fullwidth forms.
    0,
};

inline constexpr ImWchar kSpectralLabelGlyphRanges[] = {
    0x0020, 0x007e,  // Basic Latin.
    0x00a0, 0x00ff,  // Latin-1 supplement.
    0x0370, 0x03ff,  // Greek and Coptic.
    0x2000, 0x206f,  // General punctuation.
    0,
};

struct UiFontSelection {
    std::optional<std::filesystem::path> scientific_font;
    std::optional<std::filesystem::path> cjk_font;
    ImFont* spectral_label_font = nullptr;
};

[[nodiscard]] UiFontSelection AddUiFonts(ImGuiIO& io);

}  // namespace spectiary
