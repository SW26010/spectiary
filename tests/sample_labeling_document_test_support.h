#pragma once

#include "domain/sample_labeling_document.h"
#include "domain/sample_labeling_source_compatibility.h"
#include "domain/source_collection_manifest.h"

#include <utility>

namespace specforge::test_support {

// Fixture adapter only: production retains the durable source descriptor.
inline SampleLabelingDocument BuildSampleLabelingDocument(
    std::string source_kind,
    const SourceCollectionContext& context,
    const SampleLabelingTask& task)
{
    SampleLabelingCanonicalSourceDescriptor source;
    source.base_identity = context.identity.id;
    source.source_kind = std::move(source_kind);
    source.source_name = context.identity.source_name;
    source.source_fingerprint = context.identity.source_fingerprint;
    source.sample_count = context.identity.spectrum_count;
    source.sample_names = context.manifest.sample_names;
    return specforge::BuildSampleLabelingDocument(source, task);
}

using specforge::BuildSampleLabelingDocument;

}  // namespace specforge::test_support
