#include "domain/spectrum_loader_support.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>

namespace specforge::detail {
namespace {

bool IsFitsExtension(std::string_view extension)
{
    return extension == ".fits" || extension == ".fit" || extension == ".fts";
}

void SortByX(std::vector<double>& x_values, std::vector<double>& y_values)
{
    std::vector<std::pair<double, double>> pairs;
    pairs.reserve(x_values.size());
    for (std::size_t index = 0; index < x_values.size(); ++index) {
        pairs.emplace_back(x_values[index], y_values[index]);
    }
    std::stable_sort(pairs.begin(), pairs.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    for (std::size_t index = 0; index < pairs.size(); ++index) {
        x_values[index] = pairs[index].first;
        y_values[index] = pairs[index].second;
    }
}

}  // namespace

SpectrumFileLoadError::SpectrumFileLoadError(SpectrumDiagnosticCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code)
{
}

SpectrumDiagnosticCode SpectrumFileLoadError::code() const noexcept
{
    return code_;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string FileNameToUtf8(const std::filesystem::path& path)
{
    const std::filesystem::path filename = path.filename();
    return filename.empty() ? PathToUtf8(path) : PathToUtf8(filename);
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string ExtensionLower(const std::filesystem::path& path)
{
    return LowerAscii(PathToUtf8(path.extension()));
}

std::string SourceFormatLabel(const std::filesystem::path& path)
{
    const std::string extension = ExtensionLower(path);
    if (extension == ".npy") {
        return "npy";
    }
    if (extension == ".csv") {
        return "csv";
    }
    if (IsFitsExtension(extension)) {
        return "fits";
    }
    if (extension == ".gz" && IsFitsExtension(ExtensionLower(path.stem()))) {
        return "fits.gz";
    }
    if (!extension.empty() && extension.front() == '.') {
        return extension.substr(1);
    }
    return "file";
}

std::string TrimAscii(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();

    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string UpperAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

std::string NormalizedColumnName(std::string value)
{
    value = UpperAscii(TrimAscii(std::move(value)));
    std::string normalized;
    normalized.reserve(value.size());
    for (const char character : value) {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0) {
            normalized.push_back(character);
        }
    }
    return normalized;
}

std::optional<std::size_t> ParsePositiveSize(std::string_view text)
{
    if (text.empty()) {
        return std::nullopt;
    }

    std::size_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const std::size_t digit = static_cast<std::size_t>(character - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            return std::nullopt;
        }
        value = value * 10U + digit;
    }
    return value == 0 ? std::nullopt : std::optional<std::size_t>{value};
}

std::optional<double> ParseDouble(std::string text)
{
    text = TrimAscii(std::move(text));
    if (text.empty()) {
        return std::nullopt;
    }
    std::replace(text.begin(), text.end(), 'D', 'E');
    std::replace(text.begin(), text.end(), 'd', 'e');

    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || end == nullptr || *end != '\0') {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> ParseInteger(std::string text)
{
    text = TrimAscii(std::move(text));
    if (text.empty()) {
        return std::nullopt;
    }

    std::size_t parsed_offset = 0;
    try {
        const long long value = std::stoll(text, &parsed_offset, 10);
        if (parsed_offset != text.size()) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

SpectrumDiagnostic MakeDiagnostic(
    SpectrumDiagnosticSeverity severity,
    SpectrumDiagnosticCode code,
    std::string message,
    std::vector<SpectrumMetadataEntry> metadata)
{
    return SpectrumDiagnostic{severity, code, std::move(message), std::move(metadata)};
}

void AddSourceBasics(SpectrumSnapshot& snapshot, const std::filesystem::path& path, std::string_view source_type)
{
    const std::string path_text = PathToUtf8(path);
    const std::string_view source_prefix = source_type == "folder" ? "folder:" : "file:";
    snapshot.source.id = path_text.empty() ? std::string(source_prefix) : std::string(source_prefix) + path_text;
    snapshot.source.display_name = FileNameToUtf8(path);
    snapshot.source.path = path;
    snapshot.source.uri = path_text.empty() ? std::string{} : "file://" + path_text;
}

SpectrumSnapshotHandle MakeErrorSnapshot(
    const std::filesystem::path& path,
    SpectrumDiagnosticCode code,
    std::string message,
    std::vector<SpectrumMetadataEntry> diagnostic_metadata,
    std::string source_type,
    std::vector<SpectrumMetadataEntry> source_metadata)
{
    auto snapshot = std::make_shared<SpectrumSnapshot>();
    AddSourceBasics(*snapshot, path, source_type);
    snapshot->source.metadata.push_back({"source_type", source_type, "domain"});
    snapshot->source.metadata.insert(
        snapshot->source.metadata.end(),
        std::make_move_iterator(source_metadata.begin()),
        std::make_move_iterator(source_metadata.end()));
    snapshot->collection.spectrum_count = 0;
    snapshot->capabilities.has_domain_error = true;
    snapshot->diagnostics.push_back(MakeDiagnostic(
        SpectrumDiagnosticSeverity::Error,
        code,
        std::move(message),
        std::move(diagnostic_metadata)));
    return snapshot;
}

SpectrumValueQuantity InferYQuantity(const std::filesystem::path& path)
{
    const std::string filename = LowerAscii(FileNameToUtf8(path));
    if (filename.ends_with("_x.npy") || filename.ends_with("-x.npy") || filename == "x.npy") {
        return SpectrumValueQuantity::FeatureValue;
    }
    if (filename.ends_with("_flux.npy") || filename.ends_with("-flux.npy") || filename == "flux.npy") {
        return SpectrumValueQuantity::Flux;
    }
    return SpectrumValueQuantity::NormalizedFlux;
}

std::string YLabelForQuantity(SpectrumValueQuantity quantity)
{
    switch (quantity) {
    case SpectrumValueQuantity::Flux:
        return "flux";
    case SpectrumValueQuantity::NormalizedFlux:
        return "normalized flux";
    case SpectrumValueQuantity::FeatureValue:
        return "feature value";
    case SpectrumValueQuantity::Unknown:
    default:
        return "value";
    }
}

void FilterSpectrumPixels(
    std::vector<double>& x_values,
    std::vector<double>& y_values,
    const std::vector<double>* mask_values,
    const std::vector<double>* ivar_values,
    bool require_positive_x,
    FilterStats& stats)
{
    if (x_values.size() != y_values.size()) {
        throw SpectrumFileLoadError(
            SpectrumDiagnosticCode::WavelengthFluxSizeMismatch,
            "Wavelength and flux arrays do not have the same length.");
    }
    if ((mask_values != nullptr && mask_values->size() != x_values.size()) ||
        (ivar_values != nullptr && ivar_values->size() != x_values.size())) {
        throw SpectrumFileLoadError(
            SpectrumDiagnosticCode::WavelengthFluxSizeMismatch,
            "Mask or inverse-variance arrays do not match the flux array length.");
    }

    std::vector<double> filtered_x;
    std::vector<double> filtered_y;
    filtered_x.reserve(x_values.size());
    filtered_y.reserve(y_values.size());

    for (std::size_t index = 0; index < x_values.size(); ++index) {
        const double x_value = x_values[index];
        const double y_value = y_values[index];
        if (!std::isfinite(x_value) || !std::isfinite(y_value) || (require_positive_x && x_value <= 0.0)) {
            ++stats.non_finite_or_non_positive_count;
            continue;
        }

        if (ivar_values != nullptr) {
            const double ivar = (*ivar_values)[index];
            if (!std::isfinite(ivar) || ivar <= 0.0) {
                ++stats.ivar_filtered_count;
                continue;
            }
        }

        if (mask_values != nullptr) {
            const double mask = (*mask_values)[index];
            if (!std::isfinite(mask) || mask != 0.0) {
                ++stats.mask_filtered_count;
                continue;
            }
        }

        filtered_x.push_back(x_value);
        filtered_y.push_back(y_value);
    }

    x_values = std::move(filtered_x);
    y_values = std::move(filtered_y);
}

void AddFilterDiagnostics(std::vector<SpectrumDiagnostic>& diagnostics, const FilterStats& stats)
{
    if (stats.non_finite_or_non_positive_count > 0) {
        diagnostics.push_back(MakeDiagnostic(
            SpectrumDiagnosticSeverity::Warning,
            SpectrumDiagnosticCode::NonFiniteValuesFiltered,
            "Non-finite or non-positive coordinate pixels were filtered before plotting.",
            {{"filtered_count", std::to_string(stats.non_finite_or_non_positive_count), "domain"}}));
    }
    if (stats.ivar_filtered_count > 0) {
        diagnostics.push_back(MakeDiagnostic(
            SpectrumDiagnosticSeverity::Warning,
            SpectrumDiagnosticCode::IvarFilteredPixels,
            "Pixels with missing or non-positive inverse variance were filtered before plotting.",
            {{"filtered_count", std::to_string(stats.ivar_filtered_count), "domain"}}));
    }
    if (stats.mask_filtered_count > 0) {
        diagnostics.push_back(MakeDiagnostic(
            SpectrumDiagnosticSeverity::Warning,
            SpectrumDiagnosticCode::MaskFilteredPixels,
            "Masked pixels were filtered before plotting.",
            {{"filtered_count", std::to_string(stats.mask_filtered_count), "domain"}}));
    }
}

SpectrumSnapshotHandle MakeLoadedSpectrumSnapshot(const std::filesystem::path& path, LoadedSpectrum loaded)
{
    if (loaded.x_values.empty() || loaded.y_values.empty()) {
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::NoValidPixels,
            "The source did not contain any valid plottable pixels.",
            {},
            "file",
            {{"format", loaded.format, "domain"}});
    }

    SortByX(loaded.x_values, loaded.y_values);

    auto snapshot = std::make_shared<SpectrumSnapshot>();
    AddSourceBasics(*snapshot, path);
    snapshot->source.metadata.push_back({"source_type", loaded.source_type, "domain"});
    snapshot->source.metadata.push_back({"format", loaded.format, "domain"});
    snapshot->source.metadata.insert(
        snapshot->source.metadata.end(),
        std::make_move_iterator(loaded.source_metadata.begin()),
        std::make_move_iterator(loaded.source_metadata.end()));

    snapshot->collection.spectrum_count = loaded.spectrum_count;
    snapshot->collection.current_index = loaded.current_index;
    snapshot->collection.can_move_previous = loaded.current_index > 0;
    snapshot->collection.can_move_next = loaded.current_index + 1 < loaded.spectrum_count;

    snapshot->current_spectrum.name = loaded.name.empty() ? FileNameToUtf8(path) : std::move(loaded.name);
    snapshot->current_spectrum.x_values = std::make_shared<const std::vector<double>>(std::move(loaded.x_values));
    snapshot->current_spectrum.y_values = std::make_shared<const std::vector<double>>(std::move(loaded.y_values));
    snapshot->current_spectrum.point_count = snapshot->current_spectrum.x_values->size();
    snapshot->current_spectrum.metadata = std::move(loaded.spectrum_metadata);

    snapshot->axis.x_quantity = SpectrumAxisQuantity::Wavelength;
    snapshot->axis.x_unit = SpectrumAxisUnit::Angstrom;
    snapshot->axis.x_frame = SpectrumAxisFrame::Unknown;
    snapshot->axis.y_quantity = loaded.y_quantity;
    snapshot->axis.x_label = "wavelength";
    snapshot->axis.y_label = YLabelForQuantity(loaded.y_quantity);

    snapshot->capabilities.can_plot_current_spectrum = true;
    snapshot->capabilities.can_switch_spectrum = loaded.can_switch_spectrum;
    snapshot->capabilities.can_show_spectral_lines = true;
    snapshot->capabilities.can_show_rest_frame_spectral_lines = false;
    snapshot->capabilities.requires_angstrom_warning = false;
    snapshot->capabilities.requires_rest_frame_warning = true;

    snapshot->diagnostics = std::move(loaded.diagnostics);
    return snapshot;
}

}  // namespace specforge::detail
