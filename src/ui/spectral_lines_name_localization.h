#pragma once

#include "overlays/spectral_line_session_state.h"
#include "ui/ui_text.h"

#include <string>
#include <string_view>

namespace spectiary {

[[nodiscard]] std::string LocalizedSpectralLineName(
    UiLanguage language,
    std::string_view stored_name,
    const GeneratedNameMetadata& generated_name);

[[nodiscard]] std::string ResolveSpectralLineRenameSubmission(
    std::string_view edited_name,
    std::string_view stored_name,
    bool user_edited);

[[nodiscard]] std::string SpectralLineCatalogOptionLabel(
    std::string_view visible_name,
    std::string_view catalog_id);

}  // namespace spectiary
