#pragma once

#include <string>
#include <string_view>

namespace specforge {

[[nodiscard]] bool IsLegacySourceCollectionIdentity(std::string_view value);
[[nodiscard]] std::string NormalizePersistedSourceCollectionIdentity(std::string value);
[[nodiscard]] std::string NormalizeLegacySourceCollectionFingerprint(std::string value, bool legacy_identity);

}  // namespace specforge
