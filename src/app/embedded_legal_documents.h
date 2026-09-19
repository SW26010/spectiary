#pragma once

#include <string_view>

namespace spectiary {

enum class LegalDocument {
    ThirdPartyNotices,
    DataSources,
};

[[nodiscard]] std::string_view EmbeddedLegalDocumentContent(
    LegalDocument document) noexcept;

}  // namespace spectiary
