#include "ui/sample_label_presentation.h"

namespace spectiary {

std::string LocalizedSampleLabelValue(
    UiLanguage language,
    const SampleLabelSet& label_set,
    int code)
{
    if (code == kUnlabeledSampleLabelCode) {
        const bool use_cjk_punctuation =
            language == UiLanguage::SimplifiedChinese;
        return std::string(
                   UiText(
                       language,
                       UiTextId::UnlabeledValue)) +
               (use_cjk_punctuation ? "（" : " (") +
               std::to_string(code) +
               (use_cjk_punctuation ? "）" : ")");
    }
    return FormatSampleLabelValue(
        label_set,
        code);
}

std::string LocalizedCompactSampleLabelValue(
    UiLanguage language,
    const SampleLabelSet& label_set,
    int code)
{
    if (code == kUnlabeledSampleLabelCode) {
        return LocalizedSampleLabelValue(
            language,
            label_set,
            code);
    }
    if (const SampleLabelDefinition* label =
            FindSampleLabel(label_set, code)) {
        return label->name;
    }
    return std::to_string(code);
}

}  // namespace spectiary
