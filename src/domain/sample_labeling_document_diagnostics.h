#pragma once

#include "domain/sample_labeling_document.h"

namespace spectiary::diagnostics {

// Opt-in exhaustive diagnostics for test/hardening tools. Ordinary production
// input validation must use the bounded fail-fast entry point. Both entry
// points execute the same semantic validator.
[[nodiscard]] SampleLabelingDocumentValidationResult
ValidateSampleLabelingDocument(const SampleLabelingDocument& document);

}  // namespace spectiary::diagnostics
