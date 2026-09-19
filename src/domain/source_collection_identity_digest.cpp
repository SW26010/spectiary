#include "domain/source_collection_identity_digest.h"

#include "domain/stable_sha256.h"

namespace spectiary {

bool IsLegacySourceCollectionIdentity(std::string_view value)
{
    return value.starts_with("name=") && value.find("|fingerprint=") != std::string_view::npos &&
           value.find("|count=") != std::string_view::npos;
}

std::string NormalizePersistedSourceCollectionIdentity(std::string value)
{
    if (!IsLegacySourceCollectionIdentity(value)) {
        return value;
    }
    return VersionedSha256Digest(value);
}

std::string NormalizeLegacySourceCollectionFingerprint(std::string value, bool legacy_identity)
{
    if (!legacy_identity || value.empty() || IsVersionedSha256Digest(value)) {
        return value;
    }
    return VersionedSha256Digest(value);
}

}  // namespace spectiary
