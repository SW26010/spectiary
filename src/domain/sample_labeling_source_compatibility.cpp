#include "domain/sample_labeling_source_compatibility.h"

namespace specforge {

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
