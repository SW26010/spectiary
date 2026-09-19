#pragma once

#include "domain/sample_labeling.h"
#include "ui/ui_text.h"

#include <string>

namespace spectiary {

[[nodiscard]] std::string LocalizedSampleLabelValue(
    UiLanguage language,
    const SampleLabelSet& label_set,
    int code);

// Compact label values are used when the surrounding presentation already
// identifies the value as a label. Unset values deliberately retain the same
// localized sentinel presentation used by the labeling panel.
[[nodiscard]] std::string LocalizedCompactSampleLabelValue(
    UiLanguage language,
    const SampleLabelSet& label_set,
    int code);

}  // namespace spectiary
