#pragma once

#include "domain/spectrum_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace specforge::detail {

class SpectrumFileLoadError : public std::runtime_error {
public:
    SpectrumFileLoadError(SpectrumDiagnosticCode code, std::string message);

    [[nodiscard]] SpectrumDiagnosticCode code() const noexcept;

private:
    SpectrumDiagnosticCode code_;
};

std::string PathToUtf8(const std::filesystem::path& path);
std::string FileNameToUtf8(const std::filesystem::path& path);
std::string LowerAscii(std::string value);
std::string ExtensionLower(const std::filesystem::path& path);
std::string SourceFormatLabel(const std::filesystem::path& path);
std::string TrimAscii(std::string value);
std::string UpperAscii(std::string value);
std::string NormalizedColumnName(std::string value);

std::optional<std::size_t> ParsePositiveSize(std::string_view text);
std::optional<double> ParseDouble(std::string text);
std::optional<std::int64_t> ParseInteger(std::string text);

SpectrumDiagnostic MakeDiagnostic(
    SpectrumDiagnosticSeverity severity,
    SpectrumDiagnosticCode code,
    std::string message,
    std::vector<SpectrumMetadataEntry> metadata = {});

void AddSourceBasics(SpectrumSnapshot& snapshot, const std::filesystem::path& path, std::string_view source_type = "file");

SpectrumSnapshotHandle MakeErrorSnapshot(
    const std::filesystem::path& path,
    SpectrumDiagnosticCode code,
    std::string message,
    std::vector<SpectrumMetadataEntry> diagnostic_metadata = {},
    std::string source_type = "file",
    std::vector<SpectrumMetadataEntry> source_metadata = {});

SpectrumValueQuantity InferYQuantity(const std::filesystem::path& path);
std::string YLabelForQuantity(SpectrumValueQuantity quantity);

struct FilterStats {
    std::size_t non_finite_or_non_positive_count = 0;
    std::size_t mask_filtered_count = 0;
    std::size_t ivar_filtered_count = 0;
};

void FilterSpectrumPixels(
    std::vector<double>& x_values,
    std::vector<double>& y_values,
    const std::vector<double>* mask_values,
    const std::vector<double>* ivar_values,
    bool require_positive_x,
    FilterStats& stats);

void AddFilterDiagnostics(std::vector<SpectrumDiagnostic>& diagnostics, const FilterStats& stats);

struct LoadedSpectrum {
    std::string source_type;
    std::string format;
    std::string name;
    std::vector<double> x_values;
    std::vector<double> y_values;
    SpectrumValueQuantity y_quantity = SpectrumValueQuantity::Flux;
    std::size_t spectrum_count = 1;
    std::size_t current_index = 0;
    bool can_switch_spectrum = false;
    std::vector<SpectrumMetadataEntry> source_metadata;
    std::vector<SpectrumMetadataEntry> spectrum_metadata;
    std::vector<SpectrumDiagnostic> diagnostics;
};

SpectrumSnapshotHandle MakeLoadedSpectrumSnapshot(const std::filesystem::path& path, LoadedSpectrum loaded);

}  // namespace specforge::detail
