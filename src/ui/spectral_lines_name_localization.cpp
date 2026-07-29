#include "ui/spectral_lines_name_localization.h"

#include <algorithm>

namespace specforge {

std::string LocalizedSpectralLineName(
    UiLanguage language,
    std::string_view stored_name,
    const GeneratedNameMetadata& generated_name)
{
    std::string result;
    switch (generated_name.source) {
    case GeneratedNameSource::CatalogGroupingView:
        result =
            UiText(
                language,
                UiTextId::CatalogGroupingView);
        break;
    case GeneratedNameSource::DefaultGroupingView:
        result =
            std::string(
                UiText(
                    language,
                    UiTextId::DefaultGroupingViewPrefix)) +
            std::to_string(generated_name.ordinal);
        break;
    case GeneratedNameSource::DefaultGroup:
        result =
            std::string(
                UiText(
                    language,
                    UiTextId::DefaultGroupPrefix)) +
            std::to_string(generated_name.ordinal);
        break;
    case GeneratedNameSource::None:
        result =
            generated_name.copy_count == 0
                ? std::string(stored_name)
                : generated_name.copy_base_name;
        break;
    }

    const std::string_view copy_suffix =
        UiText(
            language,
            UiTextId::CopySuffix);
    const std::size_t copy_count =
        std::min(
            generated_name.copy_count,
            kMaximumGeneratedNameCopyCount);
    for (std::size_t index = 0;
         index < copy_count;
         ++index) {
        result += copy_suffix;
    }
    return result;
}

std::string ResolveSpectralLineRenameSubmission(
    std::string_view edited_name,
    std::string_view stored_name,
    bool user_edited)
{
    return user_edited
        ? std::string(edited_name)
        : std::string(stored_name);
}

std::string SpectralLineCatalogOptionLabel(
    std::string_view visible_name,
    std::string_view catalog_id)
{
    std::string label(visible_name);
    label += "###";
    label += catalog_id;
    return label;
}

}  // namespace specforge
