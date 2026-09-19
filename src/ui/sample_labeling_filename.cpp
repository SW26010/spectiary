#include "ui/sample_labeling_filename.h"

#include "domain/utf8.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace spectiary {
namespace {

constexpr std::string_view kFallbackName = "labeling-task";

[[nodiscard]] bool IsFilenameControl(std::uint32_t codepoint) noexcept
{
    return codepoint <= 0x1fU ||
        (codepoint >= 0x7fU && codepoint <= 0x9fU);
}

[[nodiscard]] bool IsWindowsIllegalAscii(std::uint32_t codepoint) noexcept
{
    switch (codepoint) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '/':
    case '\\':
    case '|':
    case '?':
    case '*':
        return true;
    default:
        return false;
    }
}

[[nodiscard]] char LowerAscii(char value) noexcept
{
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

[[nodiscard]] bool IsReservedWindowsDeviceName(
    std::string_view filename) noexcept
{
    std::string_view basename = filename.substr(0, filename.find('.'));
    while (!basename.empty() && basename.back() == ' ') {
        basename.remove_suffix(1);
    }

    std::string lowered;
    lowered.reserve(basename.size());
    for (const char value : basename) {
        lowered.push_back(LowerAscii(value));
    }

    if (lowered == "con" || lowered == "prn" ||
        lowered == "aux" || lowered == "nul") {
        return true;
    }
    if (lowered.size() != 4U ||
        (lowered.compare(0, 3, "com") != 0 &&
         lowered.compare(0, 3, "lpt") != 0)) {
        return false;
    }
    return lowered[3] >= '1' && lowered[3] <= '9';
}

[[nodiscard]] std::string_view ExtensionFor(
    SampleLabelingFilePurpose purpose) noexcept
{
    switch (purpose) {
    case SampleLabelingFilePurpose::CanonicalOutput:
        return ".asdf";
    case SampleLabelingFilePurpose::NpyExport:
        return ".npy";
    case SampleLabelingFilePurpose::CsvExport:
        return ".csv";
    }
    return ".asdf";
}

}  // namespace

std::string SuggestedSampleLabelingFilename(
    std::string_view task_name,
    SampleLabelingFilePurpose purpose)
{
    std::string safe_name;
    if (IsValidUtf8(task_name)) {
        safe_name.reserve(task_name.size());
        for (std::size_t offset = 0; offset < task_name.size();) {
            const Utf8Scalar scalar =
                DecodeValidUtf8Scalar(task_name, offset);
            if (IsFilenameControl(scalar.codepoint) ||
                IsWindowsIllegalAscii(scalar.codepoint)) {
                safe_name.push_back('_');
            } else {
                safe_name.append(task_name.substr(offset, scalar.width));
            }
            offset += scalar.width;
        }
    }

    while (!safe_name.empty() &&
           (safe_name.back() == ' ' || safe_name.back() == '.')) {
        safe_name.pop_back();
    }
    if (safe_name.empty()) {
        safe_name.assign(kFallbackName);
    }
    if (IsReservedWindowsDeviceName(safe_name)) {
        safe_name.insert(safe_name.begin(), '_');
    }

    safe_name.append(ExtensionFor(purpose));
    return safe_name;
}

}  // namespace spectiary
