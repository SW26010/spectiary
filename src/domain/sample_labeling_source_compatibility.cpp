#include "domain/sample_labeling_source_compatibility.h"

#include "domain/source_collection_manifest.h"

#include <algorithm>

namespace specforge {

namespace {

std::string_view SourceMetadataValue(
    const SpectrumSnapshot& snapshot,
    std::string_view key)
{
    const auto match = std::find_if(
        snapshot.source.metadata.begin(),
        snapshot.source.metadata.end(),
        [key](const SpectrumMetadataEntry& entry) {
            return entry.key == key;
        });
    return match == snapshot.source.metadata.end()
        ? std::string_view{}
        : std::string_view{match->value};
}

std::string CanonicalSourceKind(
    const SpectrumSnapshot& snapshot)
{
    const std::string_view source_type =
        SourceMetadataValue(snapshot, "source_type");
    if (source_type == "folder_collection") {
        return "folder";
    }
    const std::string_view format =
        SourceMetadataValue(snapshot, "format");
    if (!format.empty()) {
        return std::string(format);
    }
    return source_type.empty()
        ? std::string{"unknown"}
        : std::string{source_type};
}

}  // namespace

SampleLabelingCanonicalSourceDescriptor
BuildSampleLabelingCanonicalSourceDescriptor(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionContext& source_context)
{
    return BuildSampleLabelingCanonicalSourceDescriptor(
        snapshot,
        source_context.identity,
        source_context.manifest);
}

SampleLabelingCanonicalSourceDescriptor
BuildSampleLabelingCanonicalSourceDescriptor(
    const SpectrumSnapshot& snapshot,
    const SourceCollectionIdentity& identity,
    const SourceCollectionManifest& manifest)
{
    return SampleLabelingCanonicalSourceDescriptor{
        .base_identity = identity.id,
        .source_kind = CanonicalSourceKind(snapshot),
        .source_name = identity.source_name,
        .source_fingerprint = identity.source_fingerprint,
        .sample_count = identity.spectrum_count,
        .sample_names = manifest.sample_names,
    };
}

SampleLabelingSourceCompatibility
SampleLabelingCompatibilityView(
    const SampleLabelingCanonicalSourceDescriptor& source) noexcept
{
    return SampleLabelingSourceCompatibility{
        .base_identity = source.base_identity,
        .source_kind = source.source_kind,
        .source_name = source.source_name,
        .source_fingerprint = source.source_fingerprint,
        .sample_count = source.sample_count,
        .sample_names = source.sample_names,
    };
}

std::optional<SampleLabelingSourceCompatibilityError>
CheckSampleLabelingSourceCompatibility(
    const SampleLabelingDocument& document,
    const SampleLabelingSourceCompatibility& source,
    const SampleLabelingSourceCompatibilityCheckpoint& checkpoint)
{
    if (document.source.sample_count != source.sample_count) {
        return SampleLabelingSourceCompatibilityError{
            .kind = SampleLabelingSourceCompatibilityErrorKind::
                SampleCountMismatch,
            .message =
                "ASDF labeling document sample count does not match the source collection"};
    }
    if (document.source.base_identity != source.base_identity ||
        (!source.source_kind.empty() &&
         document.source.kind != source.source_kind) ||
        document.source.name != source.source_name ||
        document.source.fingerprint != source.source_fingerprint) {
        return SampleLabelingSourceCompatibilityError{
            .kind = SampleLabelingSourceCompatibilityErrorKind::
                SourceIdentityMismatch,
            .message =
                "ASDF labeling document source identity does not match the source collection"};
    }
    if (document.source.roster.identity_kind ==
        kSampleLabelingDocumentExplicitNamesRoster) {
        if (source.sample_names.size() != source.sample_count ||
            document.source.roster.sample_names.size() !=
                source.sample_names.size()) {
            return SampleLabelingSourceCompatibilityError{
                .kind = SampleLabelingSourceCompatibilityErrorKind::
                    RosterMismatch,
                .message =
                    "ASDF labeling document roster does not match the source collection order"};
        }
        for (std::size_t index = 0;
             index < source.sample_names.size();
             ++index) {
            if ((index & 0xfffU) == 0U && checkpoint) {
                checkpoint();
            }
            if (document.source.roster.sample_names[index] !=
                source.sample_names[index]) {
                return SampleLabelingSourceCompatibilityError{
                    .kind = SampleLabelingSourceCompatibilityErrorKind::
                        RosterMismatch,
                    .message =
                        "ASDF labeling document roster does not match the source collection order"};
            }
        }
    }
    return std::nullopt;
}

}  // namespace specforge
