#pragma once

#include "domain/sample_labeling.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spectiary {

struct SampleLabelingCanonicalSourceDescriptor;

enum class SampleLabelExportFormat {
    Npy,
    Csv,
};

struct SampleLabelExportSnapshot {
    SampleLabelExportFormat format =
        SampleLabelExportFormat::Npy;
    std::string source_kind;
    std::vector<std::string> sample_names;
    bool uses_source_index = false;
    SampleLabelSet labels;
    std::vector<int> values;
};

inline constexpr std::string_view
    kUnlabeledSampleLabelExportText = "unlabeled";
inline constexpr char kSampleLabelExportEscapePrefix = '\\';

struct DeserializedSampleLabelExportValue {
    bool represents_unlabeled = false;
    std::string label_text;

    [[nodiscard]] bool operator==(
        const DeserializedSampleLabelExportValue&) const =
        default;
};

// Captures the active task against the source collection's canonical roster.
// Navigation, sample filtering, and sample sorting state are deliberately not
// inputs to this operation.
[[nodiscard]] std::optional<SampleLabelExportSnapshot>
BuildSampleLabelExportSnapshot(
    SampleLabelExportFormat format,
    const SampleLabelingTask& task,
    const SampleLabelingCanonicalSourceDescriptor& source,
    std::string* error_message = nullptr);

// Stable, non-localized interchange text. This is separate from
// FormatSampleLabelValue(), whose output is presentation-oriented.
[[nodiscard]] std::string SerializeSampleLabelValueForExport(
    const SampleLabelSet& labels,
    int value);

// CSV unquoting happens outside this codec. At this layer an exact
// "unlabeled" is the missing sentinel; a leading backslash escapes a labeled
// value by removing exactly one prefix character. The serializer therefore
// prefixes legal label text that equals the sentinel or already starts with a
// backslash, keeping both namespaces disjoint without restricting label names.
[[nodiscard]] std::string SerializeSampleLabelNameForExport(
    std::string_view label_name);
[[nodiscard]] DeserializedSampleLabelExportValue
DeserializeSampleLabelValueFromExport(
    std::string_view serialized);

[[nodiscard]] bool IsSampleLabelExportPath(
    const std::filesystem::path& path,
    SampleLabelExportFormat format);

// Format dispatcher for one-shot interchange exports. It consumes only the
// immutable snapshot and never adopts the target as durable task state.
[[nodiscard]] bool ExportSampleLabelSnapshot(
    const std::filesystem::path& path,
    const SampleLabelExportSnapshot& snapshot,
    std::string* error_message = nullptr);

}  // namespace spectiary
