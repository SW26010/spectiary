#pragma once

#include <string>
#include <string_view>

namespace specforge {

enum class SampleLabelingFilePurpose {
    CanonicalOutput,
    NpyExport,
    CsvExport,
};

// Produces only the initial filename shown by the platform save dialog. The
// user's final path is not rewritten or otherwise constrained by this policy.
[[nodiscard]] std::string SuggestedSampleLabelingFilename(
    std::string_view task_name,
    SampleLabelingFilePurpose purpose);

}  // namespace specforge
