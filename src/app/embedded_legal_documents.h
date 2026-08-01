#pragma once

#include <string_view>

namespace specforge {

enum class LegalDocument {
    Eula,
    ThirdPartyNotices,
    DataSources,
};

[[nodiscard]] std::string_view EmbeddedLegalDocumentContent(
    LegalDocument document) noexcept;

}  // namespace specforge
