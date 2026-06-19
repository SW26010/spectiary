#include "domain/spectrum_format_adapters.h"

#include "domain/npy_array_io.h"
#include "domain/sample_annotation_io.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zlib.h>

namespace specforge {
namespace {

constexpr std::size_t kLogLamGridColumns = 3909;
constexpr double kLogLamStart = 3.5682;
constexpr double kLogLamStep = 0.0001;
constexpr double kSpeedOfLightKmPerSecond = 299792.458;
constexpr std::uintmax_t kMaxSynchronousFitsFileBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxSynchronousInflatedFitsBytes = 64ULL * 1024ULL * 1024ULL;

class NpyLoadError : public std::runtime_error {
public:
    NpyLoadError(SpectrumDiagnosticCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code)
    {
    }

    [[nodiscard]] SpectrumDiagnosticCode code() const noexcept
    {
        return code_;
    }

private:
    SpectrumDiagnosticCode code_;
};

SpectrumDiagnosticCode DiagnosticCodeForNpyArrayError(NpyArrayErrorKind kind)
{
    switch (kind) {
    case NpyArrayErrorKind::UnsupportedFormat:
        return SpectrumDiagnosticCode::UnsupportedFormat;
    case NpyArrayErrorKind::OpenFailed:
        return SpectrumDiagnosticCode::OpenFailed;
    case NpyArrayErrorKind::InvalidShape:
    default:
        return SpectrumDiagnosticCode::InvalidShape;
    }
}

struct NpyRow {
    std::string dtype;
    std::size_t row_count = 0;
    std::size_t column_count = 0;
    std::size_t current_index = 0;
    std::vector<double> values;
};

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

bool IsFitsExtension(std::string_view extension)
{
    return extension == ".fits" || extension == ".fit" || extension == ".fts";
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

std::pair<std::size_t, std::size_t> MatrixShape(const NpyHeader& header)
{
    if (header.shape.size() == 1) {
        return {1, header.shape[0]};
    }
    if (header.shape.size() == 2) {
        return {header.shape[0], header.shape[1]};
    }
    throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "Only 1D or 2D NPY arrays can be opened as spectra.");
}

std::uint64_t CheckedElementCount(std::size_t row_count, std::size_t column_count)
{
    if (row_count == 0 || column_count == 0) {
        throw NpyLoadError(SpectrumDiagnosticCode::EmptyData, "NPY array has an empty shape.");
    }
    if (row_count > std::numeric_limits<std::uint64_t>::max() / column_count) {
        throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "NPY shape is too large.");
    }
    return static_cast<std::uint64_t>(row_count) * static_cast<std::uint64_t>(column_count);
}

void ValidateFileSize(
    const std::filesystem::path& path,
    const NpyHeader& header,
    std::size_t row_count,
    std::size_t column_count,
    std::size_t item_size)
{
    const std::uint64_t element_count = CheckedElementCount(row_count, column_count);
    if (element_count > std::numeric_limits<std::size_t>::max()) {
        throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "NPY shape is too large.");
    }
    try {
        ValidateNpyPayloadSize(path, header, static_cast<std::size_t>(element_count), item_size);
    } catch (const NpyArrayError& error) {
        throw NpyLoadError(DiagnosticCodeForNpyArrayError(error.kind()), error.what());
    }
}

template <typename T>
std::vector<double> ReadTypedRow(std::ifstream& stream, std::size_t column_count)
{
    std::vector<T> typed_values;
    try {
        typed_values = ReadNpyTypedValues<T>(stream, column_count, "Selected NPY row is truncated.");
    } catch (const NpyArrayError& error) {
        throw NpyLoadError(DiagnosticCodeForNpyArrayError(error.kind()), error.what());
    }

    std::vector<double> values;
    values.reserve(column_count);
    for (T value : typed_values) {
        values.push_back(static_cast<double>(value));
    }
    return values;
}

NpyRow ReadNpyRow(const std::filesystem::path& path, std::size_t requested_index)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw NpyLoadError(SpectrumDiagnosticCode::OpenFailed, "Could not open the NPY file.");
    }

    NpyHeader header;
    try {
        header = ReadNpyHeader(stream);
    } catch (const NpyArrayError& error) {
        throw NpyLoadError(DiagnosticCodeForNpyArrayError(error.kind()), error.what());
    }
    const std::optional<NpyScalarType> scalar_type = ParseNpyScalarType(header.descr);
    if (!scalar_type || scalar_type->kind != NpyScalarKind::Float || scalar_type->item_size == 0) {
        throw NpyLoadError(
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Only little-endian float32 and float64 NPY arrays are supported.");
    }
    const auto [row_count, column_count] = MatrixShape(header);
    ValidateFileSize(path, header, row_count, column_count, scalar_type->item_size);

    if (requested_index >= row_count) {
        throw NpyLoadError(
            SpectrumDiagnosticCode::InvalidShape,
            "Requested spectrum row is outside the NPY matrix.");
    }

    const std::uint64_t row_bytes = static_cast<std::uint64_t>(column_count) * scalar_type->item_size;
    const std::uint64_t row_offset = header.data_offset + static_cast<std::uint64_t>(requested_index) * row_bytes;
    if (row_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "Selected NPY row offset is too large.");
    }

    stream.seekg(static_cast<std::streamoff>(row_offset), std::ios::beg);
    if (!stream) {
        throw NpyLoadError(SpectrumDiagnosticCode::OpenFailed, "Could not seek to the selected NPY row.");
    }

    std::vector<double> values;
    switch (scalar_type->item_size) {
    case sizeof(float):
        values = ReadTypedRow<float>(stream, column_count);
        break;
    case sizeof(double):
        values = ReadTypedRow<double>(stream, column_count);
        break;
    default:
        throw NpyLoadError(SpectrumDiagnosticCode::UnsupportedFormat, "Unsupported NPY element type.");
    }

    return NpyRow{
        header.descr,
        row_count,
        column_count,
        requested_index,
        std::move(values),
    };
}

std::optional<std::string> ReadNpyNameForRow(
    const std::filesystem::path& spectrum_path,
    std::size_t expected_row_count,
    std::size_t row_index)
{
    const std::optional<std::filesystem::path> name_path = SampleCollectionCompanionNamePath(spectrum_path);
    if (!name_path) {
        return std::nullopt;
    }

    std::error_code exists_error;
    if (!std::filesystem::exists(*name_path, exists_error) || exists_error) {
        return std::nullopt;
    }

    std::ifstream stream(*name_path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }

    try {
        const NpyHeader header = ReadNpyHeader(stream);
        if (header.shape.size() != 1 || header.shape[0] != expected_row_count || row_index >= header.shape[0]) {
            return std::nullopt;
        }
        const std::optional<NpyScalarType> scalar_type = ParseNpyScalarType(header.descr);
        if (!scalar_type || (scalar_type->kind != NpyScalarKind::Bytes && scalar_type->kind != NpyScalarKind::Unicode)) {
            return std::nullopt;
        }
        const std::uint64_t row_offset =
            header.data_offset + static_cast<std::uint64_t>(row_index) * scalar_type->item_size;
        if (row_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            return std::nullopt;
        }
        stream.seekg(static_cast<std::streamoff>(row_offset), std::ios::beg);
        if (!stream) {
            return std::nullopt;
        }
        std::string bytes(scalar_type->item_size, '\0');
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            return std::nullopt;
        }
        std::string decoded = DecodeNpyString(bytes, *scalar_type);
        if (decoded.empty()) {
            return std::nullopt;
        }
        return decoded;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

SpectrumDiagnostic MakeDiagnostic(
    SpectrumDiagnosticSeverity severity,
    SpectrumDiagnosticCode code,
    std::string message,
    std::vector<SpectrumMetadataEntry> metadata = {})
{
    return SpectrumDiagnostic{severity, code, std::move(message), std::move(metadata)};
}

void AddSourceBasics(SpectrumSnapshot& snapshot, const std::filesystem::path& path, std::string_view source_type = "file")
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
    std::vector<SpectrumMetadataEntry> diagnostic_metadata = {},
    std::string source_type = "file",
    std::vector<SpectrumMetadataEntry> source_metadata = {})
{
    auto snapshot = std::make_shared<SpectrumSnapshot>();
    AddSourceBasics(*snapshot, path, source_type);
    snapshot->source.metadata.push_back({"source_type", std::move(source_type), "domain"});
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

SpectrumSnapshotHandle MakeNpyErrorSnapshot(
    const std::filesystem::path& path,
    SpectrumDiagnosticCode code,
    std::string message,
    std::vector<SpectrumMetadataEntry> diagnostic_metadata = {})
{
    return MakeErrorSnapshot(
        path,
        code,
        std::move(message),
        std::move(diagnostic_metadata),
        "file",
        {{"format", "npy", "domain"}});
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

bool IsAuxiliaryNpyArrayName(const std::filesystem::path& path)
{
    return IsSampleCollectionAuxiliaryNpyArrayName(path);
}

std::vector<double> MakeXValues(std::size_t column_count, bool has_loglam_grid)
{
    std::vector<double> x_values;
    x_values.reserve(column_count);
    for (std::size_t index = 0; index < column_count; ++index) {
        if (has_loglam_grid) {
            x_values.push_back(std::pow(10.0, kLogLamStart + static_cast<double>(index) * kLogLamStep));
        } else {
            x_values.push_back(static_cast<double>(index));
        }
    }
    return x_values;
}

void FilterFiniteValues(std::vector<double>& x_values, std::vector<double>& y_values, std::size_t& filtered_count)
{
    std::vector<double> filtered_x;
    std::vector<double> filtered_y;
    filtered_x.reserve(x_values.size());
    filtered_y.reserve(y_values.size());

    for (std::size_t index = 0; index < x_values.size(); ++index) {
        const double x_value = x_values[index];
        const double y_value = y_values[index];
        if (std::isfinite(x_value) && std::isfinite(y_value) && x_value >= 0.0) {
            filtered_x.push_back(x_value);
            filtered_y.push_back(y_value);
        }
    }

    filtered_count = x_values.size() - filtered_x.size();
    x_values = std::move(filtered_x);
    y_values = std::move(filtered_y);
}

SpectrumSnapshotHandle LoadNpySnapshot(const std::filesystem::path& path, std::size_t spectrum_index)
{
    NpyRow row;
    try {
        row = ReadNpyRow(path, spectrum_index);
    } catch (const NpyLoadError& error) {
        return MakeNpyErrorSnapshot(path, error.code(), error.what());
    } catch (const std::exception& error) {
        return MakeNpyErrorSnapshot(path, SpectrumDiagnosticCode::InvalidShape, error.what());
    }

    const bool has_loglam_grid = row.column_count == kLogLamGridColumns;
    std::vector<double> x_values = MakeXValues(row.column_count, has_loglam_grid);
    std::vector<double> y_values = std::move(row.values);

    std::size_t filtered_count = 0;
    FilterFiniteValues(x_values, y_values, filtered_count);
    if (x_values.empty()) {
        return MakeNpyErrorSnapshot(
            path,
            SpectrumDiagnosticCode::NoValidPixels,
            "Selected NPY row has no finite plottable pixels.",
            {{"row_index", std::to_string(row.current_index), "domain"}});
    }

    auto snapshot = std::make_shared<SpectrumSnapshot>();
    AddSourceBasics(*snapshot, path);
    snapshot->source.metadata.push_back({"source_type", "npy_matrix", "domain"});
    snapshot->source.metadata.push_back({"format", "npy", "domain"});
    snapshot->source.metadata.push_back({"dtype", row.dtype, "npy"});
    snapshot->source.metadata.push_back({"shape_rows", std::to_string(row.row_count), "npy"});
    snapshot->source.metadata.push_back({"shape_columns", std::to_string(row.column_count), "npy"});

    snapshot->collection.spectrum_count = row.row_count;
    snapshot->collection.current_index = row.current_index;
    snapshot->collection.can_move_previous = row.current_index > 0;
    snapshot->collection.can_move_next = row.current_index + 1 < row.row_count;

    const std::optional<std::string> sample_name = ReadNpyNameForRow(path, row.row_count, row.current_index);
    snapshot->current_spectrum.name = sample_name ? *sample_name
                                          : row.row_count > 1
                                                ? FileNameToUtf8(path) + " row " + std::to_string(row.current_index)
                                                : FileNameToUtf8(path);
    snapshot->current_spectrum.x_values = std::make_shared<const std::vector<double>>(std::move(x_values));
    snapshot->current_spectrum.y_values = std::make_shared<const std::vector<double>>(std::move(y_values));
    snapshot->current_spectrum.point_count = snapshot->current_spectrum.x_values->size();
    snapshot->current_spectrum.metadata.push_back({"row_index", std::to_string(row.current_index), "npy"});
    snapshot->current_spectrum.metadata.push_back({"source_columns", std::to_string(row.column_count), "npy"});
    if (sample_name) {
        snapshot->current_spectrum.metadata.push_back({"sample_name", *sample_name, "npy"});
        snapshot->source.metadata.push_back({"sample_names", "companion_name_npy", "npy"});
    }

    snapshot->axis.y_quantity = InferYQuantity(path);
    snapshot->axis.y_label = YLabelForQuantity(snapshot->axis.y_quantity);
    if (has_loglam_grid) {
        snapshot->axis.x_quantity = SpectrumAxisQuantity::Wavelength;
        snapshot->axis.x_unit = SpectrumAxisUnit::Angstrom;
        snapshot->axis.x_frame = SpectrumAxisFrame::Unknown;
        snapshot->axis.x_label = "wavelength";
    } else {
        snapshot->axis.x_quantity = SpectrumAxisQuantity::Pixel;
        snapshot->axis.x_unit = SpectrumAxisUnit::Pixel;
        snapshot->axis.x_frame = SpectrumAxisFrame::Unknown;
        snapshot->axis.x_label = "pixel";
    }

    snapshot->capabilities.can_plot_current_spectrum = true;
    snapshot->capabilities.can_switch_spectrum = row.row_count > 1;
    snapshot->capabilities.can_show_spectral_lines = has_loglam_grid;
    snapshot->capabilities.can_show_rest_frame_spectral_lines = false;
    snapshot->capabilities.requires_angstrom_warning = !has_loglam_grid;
    snapshot->capabilities.requires_rest_frame_warning = has_loglam_grid;

    if (has_loglam_grid) {
        snapshot->diagnostics.push_back(MakeDiagnostic(
            SpectrumDiagnosticSeverity::Warning,
            SpectrumDiagnosticCode::AxisFrameUnknown,
            "NPY row uses the 3909-point loglam wavelength grid, but no rest-frame correction status is available."));
    } else {
        snapshot->diagnostics.push_back(MakeDiagnostic(
            SpectrumDiagnosticSeverity::Warning,
            SpectrumDiagnosticCode::MissingWavelength,
            "NPY column count is not 3909, so the snapshot uses pixel index on the X axis.",
            {{"columns", std::to_string(row.column_count), "npy"}}));
    }

    if (filtered_count > 0) {
        snapshot->diagnostics.push_back(MakeDiagnostic(
            SpectrumDiagnosticSeverity::Warning,
            SpectrumDiagnosticCode::NonFiniteValuesFiltered,
            "Non-finite NPY values were filtered before plotting.",
            {{"filtered_count", std::to_string(filtered_count), "domain"}}));
    }

    return snapshot;
}

class SpectrumFileLoadError : public std::runtime_error {
public:
    SpectrumFileLoadError(SpectrumDiagnosticCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code)
    {
    }

    [[nodiscard]] SpectrumDiagnosticCode code() const noexcept
    {
        return code_;
    }

private:
    SpectrumDiagnosticCode code_;
};

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

struct FilterStats {
    std::size_t non_finite_or_non_positive_count = 0;
    std::size_t mask_filtered_count = 0;
    std::size_t ivar_filtered_count = 0;
};

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

std::vector<std::string> SplitCsvLine(std::string_view line)
{
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (character == '"') {
            if (in_quotes && index + 1 < line.size() && line[index + 1] == '"') {
                field.push_back('"');
                ++index;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (character == ',' && !in_quotes) {
            fields.push_back(TrimAscii(std::move(field)));
            field.clear();
        } else {
            field.push_back(character);
        }
    }
    fields.push_back(TrimAscii(std::move(field)));
    return fields;
}

std::optional<std::size_t> FindCsvColumn(const std::vector<std::string>& header, std::initializer_list<std::string_view> names)
{
    for (std::size_t index = 0; index < header.size(); ++index) {
        const std::string normalized = NormalizedColumnName(header[index]);
        for (std::string_view name : names) {
            if (normalized == name) {
                return index;
            }
        }
    }
    return std::nullopt;
}

SpectrumSnapshotHandle LoadCsvSnapshot(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream) {
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::OpenFailed,
            "Could not open the CSV file.",
            {},
            "file",
            {{"format", "csv", "domain"}});
    }

    std::string first_line;
    if (!std::getline(stream, first_line)) {
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::EmptyData,
            "CSV file is empty.",
            {},
            "file",
            {{"format", "csv", "domain"}});
    }

    const std::vector<std::string> first_fields = SplitCsvLine(first_line);
    std::vector<std::string> header = first_fields;
    bool first_line_is_data = false;
    if (first_fields.size() >= 2 && ParseDouble(first_fields[0]) && ParseDouble(first_fields[1])) {
        first_line_is_data = true;
        header = {"wavelength", "flux"};
    }

    const std::optional<std::size_t> wavelength_column =
        FindCsvColumn(header, {"WAV", "WAVE", "WAVELENGTH", "LAMBDA"});
    const std::optional<std::size_t> loglam_column = FindCsvColumn(header, {"LOGLAM"});
    const std::optional<std::size_t> flux_column = FindCsvColumn(header, {"FLUX", "Y", "VALUE"});
    if ((!wavelength_column && !loglam_column) || !flux_column) {
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::MissingWavelength,
            "CSV files must contain wavelength/loglam and flux columns.",
            {},
            "file",
            {{"format", "csv", "domain"}});
    }

    LoadedSpectrum loaded;
    loaded.source_type = "csv_spectrum";
    loaded.format = "csv";
    loaded.name = FileNameToUtf8(path);
    const bool uses_loglam = loglam_column && !wavelength_column;
    loaded.source_metadata.push_back({"x_column", wavelength_column ? header[*wavelength_column] : header[*loglam_column], "csv"});
    loaded.source_metadata.push_back({"flux_column", header[*flux_column], "csv"});

    FilterStats stats;
    const auto read_fields = [&](const std::vector<std::string>& fields) {
        const std::size_t x_column = wavelength_column ? *wavelength_column : *loglam_column;
        if (x_column >= fields.size() || *flux_column >= fields.size()) {
            ++stats.non_finite_or_non_positive_count;
            return;
        }
        const std::optional<double> parsed_x = ParseDouble(fields[x_column]);
        const std::optional<double> parsed_y = ParseDouble(fields[*flux_column]);
        if (!parsed_x || !parsed_y) {
            ++stats.non_finite_or_non_positive_count;
            return;
        }
        loaded.x_values.push_back(uses_loglam ? std::pow(10.0, *parsed_x) : *parsed_x);
        loaded.y_values.push_back(*parsed_y);
    };

    if (first_line_is_data) {
        read_fields(first_fields);
    }

    std::string line;
    while (std::getline(stream, line)) {
        if (TrimAscii(line).empty()) {
            continue;
        }
        read_fields(SplitCsvLine(line));
    }

    FilterSpectrumPixels(loaded.x_values, loaded.y_values, nullptr, nullptr, true, stats);
    AddFilterDiagnostics(loaded.diagnostics, stats);
    loaded.diagnostics.push_back(MakeDiagnostic(
        SpectrumDiagnosticSeverity::Warning,
        SpectrumDiagnosticCode::AxisFrameUnknown,
        "CSV wavelength values are plotted as provided; no rest-frame correction status is available."));
    return MakeLoadedSpectrumSnapshot(path, std::move(loaded));
}

std::vector<unsigned char> ReadWholeFile(const std::filesystem::path& path, std::uintmax_t max_bytes)
{
    std::error_code size_error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::OpenFailed, "Could not inspect the FITS file size.");
    }
    if (file_size > max_bytes) {
        throw SpectrumFileLoadError(
            SpectrumDiagnosticCode::UnsupportedFormat,
            "FITS file is too large for the synchronous single-spectrum loader.");
    }
    if (file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::UnsupportedFormat, "FITS file size is not addressable.");
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::OpenFailed, "Could not open the FITS file.");
    }

    std::vector<unsigned char> bytes(static_cast<std::size_t>(file_size));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream && !bytes.empty()) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::OpenFailed, "Could not read the complete FITS file.");
    }
    return bytes;
}

std::vector<unsigned char> DecompressGzip(const std::vector<unsigned char>& compressed)
{
    if (compressed.empty() || compressed.size() > std::numeric_limits<uInt>::max()) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::OpenFailed, "Compressed FITS file is empty or too large.");
    }

    z_stream stream = {};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::OpenFailed, "Could not initialize gzip decompression.");
    }

    std::vector<unsigned char> output;
    std::array<unsigned char, 64 * 1024> buffer = {};
    int result = Z_OK;
    while (result != Z_STREAM_END) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            inflateEnd(&stream);
            throw SpectrumFileLoadError(SpectrumDiagnosticCode::OpenFailed, "Could not decompress the gzip FITS file.");
        }

        const std::size_t produced = buffer.size() - stream.avail_out;
        if (produced > kMaxSynchronousInflatedFitsBytes ||
            output.size() > kMaxSynchronousInflatedFitsBytes - produced) {
            inflateEnd(&stream);
            throw SpectrumFileLoadError(
                SpectrumDiagnosticCode::UnsupportedFormat,
                "Gzipped FITS expands beyond the synchronous single-spectrum loader limit.");
        }
        output.insert(output.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(produced));
    }

    inflateEnd(&stream);
    return output;
}

struct FitsHeader {
    std::unordered_map<std::string, std::string> values;
};

struct FitsColumn {
    std::string name;
    std::string normalized_name;
    std::size_t repeat = 1;
    char code = '\0';
    std::size_t element_size = 0;
    std::size_t byte_offset = 0;
    std::size_t byte_width = 0;
};

struct FitsHdu {
    std::size_t index = 0;
    FitsHeader header;
    std::size_t data_offset = 0;
    std::size_t data_size = 0;
    std::vector<FitsColumn> columns;
};

struct FitsMetadataMatch {
    std::string key;
    std::string value;
};

std::string ParseFitsCardValue(std::string_view card)
{
    if (card.size() < 10 || card[8] != '=') {
        return {};
    }

    const std::string_view field = card.substr(10);
    std::string raw;
    bool in_quote = false;
    for (std::size_t index = 0; index < field.size(); ++index) {
        const char character = field[index];
        if (character == '\'') {
            raw.push_back(character);
            if (in_quote && index + 1 < field.size() && field[index + 1] == '\'') {
                raw.push_back(field[index + 1]);
                ++index;
            } else {
                in_quote = !in_quote;
            }
        } else if (character == '/' && !in_quote) {
            break;
        } else {
            raw.push_back(character);
        }
    }

    raw = TrimAscii(std::move(raw));
    if (raw.size() >= 2 && raw.front() == '\'' && raw.back() == '\'') {
        std::string text;
        for (std::size_t index = 1; index + 1 < raw.size(); ++index) {
            if (raw[index] == '\'' && index + 1 < raw.size() - 1 && raw[index + 1] == '\'') {
                text.push_back('\'');
                ++index;
            } else {
                text.push_back(raw[index]);
            }
        }
        return TrimAscii(std::move(text));
    }
    return raw;
}

std::optional<std::string> FitsValue(const FitsHeader& header, std::string_view key)
{
    const auto found = header.values.find(UpperAscii(std::string(key)));
    if (found == header.values.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::int64_t FitsInteger(const FitsHeader& header, std::string_view key, std::int64_t default_value = 0)
{
    const std::optional<std::string> value = FitsValue(header, key);
    if (!value) {
        return default_value;
    }
    return ParseInteger(*value).value_or(default_value);
}

std::optional<double> FitsDouble(const FitsHeader& header, std::string_view key)
{
    const std::optional<std::string> value = FitsValue(header, key);
    if (!value) {
        return std::nullopt;
    }
    return ParseDouble(std::string(*value));
}

std::size_t RoundUpFitsBlock(std::size_t value)
{
    constexpr std::size_t kFitsBlockSize = 2880;
    return ((value + kFitsBlockSize - 1U) / kFitsBlockSize) * kFitsBlockSize;
}

std::size_t FitsBitpixElementSize(std::int64_t bitpix)
{
    switch (bitpix) {
    case 8:
        return 1;
    case 16:
        return 2;
    case 32:
    case -32:
        return 4;
    case 64:
    case -64:
        return 8;
    default:
        return 0;
    }
}

std::size_t FitsTableRowDataSize(const FitsHeader& header)
{
    const std::int64_t row_width = FitsInteger(header, "NAXIS1");
    const std::int64_t row_count = FitsInteger(header, "NAXIS2");
    if (row_width < 0 || row_count < 0) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS binary table has invalid dimensions.");
    }
    const auto width = static_cast<std::size_t>(row_width);
    const auto count = static_cast<std::size_t>(row_count);
    if (width != 0 && count > std::numeric_limits<std::size_t>::max() / width) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS binary table dimensions overflow.");
    }
    return width * count;
}

std::size_t FitsDataSize(const FitsHeader& header)
{
    const std::string xtension = UpperAscii(FitsValue(header, "XTENSION").value_or({}));
    if (xtension == "BINTABLE") {
        const std::int64_t heap_bytes = FitsInteger(header, "PCOUNT");
        if (heap_bytes < 0) {
            throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS binary table has invalid dimensions.");
        }
        const auto heap = static_cast<std::size_t>(heap_bytes);
        const std::size_t table_bytes = FitsTableRowDataSize(header);
        if (heap > std::numeric_limits<std::size_t>::max() - table_bytes) {
            throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS binary table heap size overflows.");
        }
        return table_bytes + heap;
    }

    const std::int64_t axis_count = FitsInteger(header, "NAXIS");
    if (axis_count <= 0) {
        return 0;
    }
    const std::size_t element_size = FitsBitpixElementSize(FitsInteger(header, "BITPIX"));
    if (element_size == 0) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::UnsupportedFormat, "FITS image BITPIX is not supported.");
    }

    std::size_t element_count = 1;
    for (std::int64_t axis = 1; axis <= axis_count; ++axis) {
        const std::int64_t axis_size = FitsInteger(header, "NAXIS" + std::to_string(axis));
        if (axis_size < 0) {
            throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS image has invalid axis dimensions.");
        }
        if (axis_size == 0) {
            return 0;
        }
        if (element_count > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(axis_size)) {
            throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS image dimensions overflow.");
        }
        element_count *= static_cast<std::size_t>(axis_size);
    }
    if (element_count > std::numeric_limits<std::size_t>::max() / element_size) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS image byte size overflows.");
    }
    return element_count * element_size;
}

std::optional<FitsColumn> ParseFitsColumn(const FitsHeader& header, std::size_t column_index, std::size_t byte_offset)
{
    const std::optional<std::string> name = FitsValue(header, "TTYPE" + std::to_string(column_index));
    const std::optional<std::string> form = FitsValue(header, "TFORM" + std::to_string(column_index));
    if (!name || !form) {
        return std::nullopt;
    }

    const std::string tform = TrimAscii(*form);
    std::size_t cursor = 0;
    while (cursor < tform.size() && std::isdigit(static_cast<unsigned char>(tform[cursor])) != 0) {
        ++cursor;
    }
    const std::size_t repeat = cursor == 0 ? 1U : ParsePositiveSize(std::string_view(tform).substr(0, cursor)).value_or(0);
    if (repeat == 0 || cursor >= tform.size()) {
        return std::nullopt;
    }

    const char code = static_cast<char>(std::toupper(static_cast<unsigned char>(tform[cursor])));
    std::size_t element_size = 0;
    switch (code) {
    case 'A':
    case 'B':
    case 'L':
        element_size = 1;
        break;
    case 'I':
        element_size = 2;
        break;
    case 'J':
    case 'E':
        element_size = 4;
        break;
    case 'K':
    case 'D':
        element_size = 8;
        break;
    default:
        return std::nullopt;
    }

    if (repeat > std::numeric_limits<std::size_t>::max() / element_size) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS table column width overflows.");
    }

    return FitsColumn{
        TrimAscii(*name),
        NormalizedColumnName(*name),
        repeat,
        code,
        element_size,
        byte_offset,
        repeat * element_size,
    };
}

void PopulateFitsColumns(FitsHdu& hdu)
{
    if (UpperAscii(FitsValue(hdu.header, "XTENSION").value_or({})) != "BINTABLE") {
        return;
    }

    const std::int64_t field_count = FitsInteger(hdu.header, "TFIELDS");
    if (field_count <= 0) {
        return;
    }
    const std::int64_t row_width_value = FitsInteger(hdu.header, "NAXIS1");
    if (row_width_value < 0) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS binary table has invalid dimensions.");
    }
    const auto row_width = static_cast<std::size_t>(row_width_value);

    std::size_t offset = 0;
    std::vector<FitsColumn> columns;
    columns.reserve(static_cast<std::size_t>(field_count));
    for (std::int64_t index = 1; index <= field_count; ++index) {
        const std::optional<FitsColumn> column = ParseFitsColumn(hdu.header, static_cast<std::size_t>(index), offset);
        if (!column) {
            hdu.columns.clear();
            return;
        }
        if (column->byte_width > row_width || offset > row_width - column->byte_width) {
            throw SpectrumFileLoadError(
                SpectrumDiagnosticCode::InvalidShape,
                "FITS table column layout exceeds NAXIS1 row width.");
        }
        offset += column->byte_width;
        columns.push_back(*column);
    }
    if (offset != row_width) {
        throw SpectrumFileLoadError(
            SpectrumDiagnosticCode::InvalidShape,
            "FITS table column layout does not match NAXIS1 row width.");
    }
    hdu.columns = std::move(columns);
}

std::vector<FitsHdu> ParseFitsHdus(const std::vector<unsigned char>& bytes)
{
    constexpr std::size_t kFitsBlockSize = 2880;
    std::vector<FitsHdu> hdus;
    std::size_t offset = 0;

    while (offset + kFitsBlockSize <= bytes.size()) {
        FitsHdu hdu;
        hdu.index = hdus.size();
        std::size_t header_offset = offset;
        bool found_end = false;

        while (header_offset + kFitsBlockSize <= bytes.size()) {
            for (std::size_t card_offset = 0; card_offset < kFitsBlockSize; card_offset += 80U) {
                const std::size_t absolute = header_offset + card_offset;
                const std::string card(
                    reinterpret_cast<const char*>(bytes.data() + absolute),
                    reinterpret_cast<const char*>(bytes.data() + absolute + 80U));
                const std::string key = TrimAscii(card.substr(0, 8));
                if (key == "END") {
                    found_end = true;
                    break;
                }
                if (card.size() >= 10 && card[8] == '=') {
                    hdu.header.values[UpperAscii(key)] = ParseFitsCardValue(card);
                }
            }
            header_offset += kFitsBlockSize;
            if (found_end) {
                break;
            }
        }

        if (!found_end) {
            throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS header is missing END.");
        }

        hdu.data_offset = header_offset;
        hdu.data_size = FitsDataSize(hdu.header);
        if (hdu.data_offset > bytes.size() || hdu.data_size > bytes.size() - hdu.data_offset) {
            throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS HDU data is truncated.");
        }
        PopulateFitsColumns(hdu);
        hdus.push_back(std::move(hdu));

        offset = header_offset + RoundUpFitsBlock(hdus.back().data_size);
    }

    if (hdus.empty()) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::UnsupportedFormat, "FITS file has no readable HDUs.");
    }
    return hdus;
}

std::uint16_t ReadBigEndianU16(const unsigned char* bytes)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
}

std::uint32_t ReadBigEndianU32(const unsigned char* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t ReadBigEndianU64(const unsigned char* bytes)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(bytes[index]);
    }
    return value;
}

double ReadFitsNumericValue(const unsigned char* bytes, char code)
{
    switch (code) {
    case 'B':
        return static_cast<double>(bytes[0]);
    case 'I':
        return static_cast<double>(static_cast<std::int16_t>(ReadBigEndianU16(bytes)));
    case 'J':
        return static_cast<double>(static_cast<std::int32_t>(ReadBigEndianU32(bytes)));
    case 'K':
        return static_cast<double>(static_cast<std::int64_t>(ReadBigEndianU64(bytes)));
    case 'E': {
        const std::uint32_t bits = ReadBigEndianU32(bytes);
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return static_cast<double>(value);
    }
    case 'D': {
        const std::uint64_t bits = ReadBigEndianU64(bytes);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    default:
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::UnsupportedFormat, "FITS numeric column type is not supported.");
    }
}

const FitsColumn* FindFitsColumn(const FitsHdu& hdu, std::initializer_list<std::string_view> names)
{
    for (const FitsColumn& column : hdu.columns) {
        for (std::string_view name : names) {
            if (column.normalized_name == name) {
                return &column;
            }
        }
    }
    return nullptr;
}

bool IsOrMaskColumnName(std::string_view normalized_name)
{
    if (normalized_name == "ORMASK" || normalized_name == "ORMASKS") {
        return true;
    }
    constexpr std::string_view kPrefix = "ORMASK";
    if (!normalized_name.starts_with(kPrefix) || normalized_name.size() == kPrefix.size()) {
        return false;
    }
    return std::all_of(
        normalized_name.begin() + static_cast<std::ptrdiff_t>(kPrefix.size()),
        normalized_name.end(),
        [](char character) {
            return std::isdigit(static_cast<unsigned char>(character)) != 0;
        });
}

const FitsColumn* FindFitsMaskColumn(const FitsHdu& hdu)
{
    for (const FitsColumn& column : hdu.columns) {
        if (IsOrMaskColumnName(column.normalized_name)) {
            return &column;
        }
    }
    return nullptr;
}

bool IsFitsNumericColumn(const FitsColumn& column)
{
    return column.code == 'B' || column.code == 'I' || column.code == 'J' || column.code == 'K' ||
           column.code == 'E' || column.code == 'D';
}

std::vector<double> ReadFitsColumnVector(
    const std::vector<unsigned char>& bytes,
    const FitsHdu& hdu,
    const FitsColumn& column,
    std::size_t row_index,
    bool scalar_rows)
{
    const std::size_t row_width = static_cast<std::size_t>(FitsInteger(hdu.header, "NAXIS1"));
    const std::size_t row_count = static_cast<std::size_t>(FitsInteger(hdu.header, "NAXIS2"));
    const std::size_t table_data_size = FitsTableRowDataSize(hdu.header);
    if (table_data_size > hdu.data_size) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS table row data exceeds the HDU data range.");
    }
    std::vector<double> values;

    if (scalar_rows) {
        values.reserve(row_count);
        for (std::size_t row = 0; row < row_count; ++row) {
            const std::size_t relative_offset = row * row_width + column.byte_offset;
            if (relative_offset > table_data_size || column.element_size > table_data_size - relative_offset) {
                throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS table column data is truncated.");
            }
            const std::size_t offset = hdu.data_offset + relative_offset;
            values.push_back(ReadFitsNumericValue(bytes.data() + offset, column.code));
        }
        return values;
    }

    if (row_index >= row_count) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "Requested FITS table row is out of range.");
    }
    values.reserve(column.repeat);
    const std::size_t row_offset = row_index * row_width + column.byte_offset;
    for (std::size_t item = 0; item < column.repeat; ++item) {
        const std::size_t relative_offset = row_offset + item * column.element_size;
        if (relative_offset > table_data_size || column.element_size > table_data_size - relative_offset) {
            throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS table vector data is truncated.");
        }
        const std::size_t offset = hdu.data_offset + relative_offset;
        values.push_back(ReadFitsNumericValue(bytes.data() + offset, column.code));
    }
    return values;
}

std::string FormatFitsMetadataNumber(double value)
{
    std::ostringstream stream;
    stream.precision(std::numeric_limits<double>::max_digits10);
    stream << value;
    return stream.str();
}

bool HasLoadedMetadataKey(const LoadedSpectrum& loaded, std::string_view key)
{
    return std::any_of(
        loaded.source_metadata.begin(),
        loaded.source_metadata.end(),
        [key](const SpectrumMetadataEntry& entry) {
            return entry.key == key;
        });
}

std::optional<std::string_view> LoadedMetadataValue(const LoadedSpectrum& loaded, std::string_view key)
{
    for (const SpectrumMetadataEntry& entry : loaded.source_metadata) {
        if (entry.key == key) {
            return entry.value;
        }
    }
    return std::nullopt;
}

std::optional<double> LoadedMetadataDouble(const LoadedSpectrum& loaded, std::string_view key)
{
    const std::optional<std::string_view> value = LoadedMetadataValue(loaded, key);
    if (!value) {
        return std::nullopt;
    }
    return ParseDouble(std::string(*value));
}

std::optional<FitsMetadataMatch> FirstFitsHeaderValue(
    const std::vector<FitsHdu>& hdus,
    std::initializer_list<std::string_view> header_keys)
{
    for (const FitsHdu& hdu : hdus) {
        for (std::string_view key : header_keys) {
            const std::optional<std::string> value = FitsValue(hdu.header, key);
            if (value && !value->empty()) {
                return FitsMetadataMatch{std::string(key), *value};
            }
        }
    }
    return std::nullopt;
}

bool FitsMetadataTruthy(std::string_view value)
{
    const std::string normalized = UpperAscii(TrimAscii(std::string(value)));
    return normalized == "T" || normalized == "TRUE" || normalized == "1";
}

bool FitsMetadataFalsey(std::string_view value)
{
    const std::string normalized = UpperAscii(TrimAscii(std::string(value)));
    return normalized == "F" || normalized == "FALSE" || normalized == "0";
}

bool RedshiftValueIsUsable(double value)
{
    return std::isfinite(value) && value > -1.0 && value < 20.0;
}

bool RedshiftWarningIsSet(const LoadedSpectrum& loaded)
{
    const std::optional<double> warning = LoadedMetadataDouble(loaded, "redshift_warning");
    return warning && std::isfinite(*warning) && std::abs(*warning) > 0.0;
}

bool SurveyClassUsesPipelineRedshift(const LoadedSpectrum& loaded)
{
    const std::optional<std::string_view> survey_class = LoadedMetadataValue(loaded, "survey_class");
    if (!survey_class) {
        return false;
    }

    const std::string normalized = UpperAscii(TrimAscii(std::string(*survey_class)));
    return normalized == "GALAXY" || normalized == "QSO" || normalized == "AGN";
}

void AddFitsWavelengthFrameMetadata(LoadedSpectrum& loaded)
{
    if (const std::optional<std::string_view> vacuum = LoadedMetadataValue(loaded, "wavelength_vacuum")) {
        if (!HasLoadedMetadataKey(loaded, "wavelength_medium")) {
            if (FitsMetadataTruthy(*vacuum)) {
                loaded.source_metadata.push_back({"wavelength_medium", "vacuum", "fits"});
            } else if (FitsMetadataFalsey(*vacuum)) {
                loaded.source_metadata.push_back({"wavelength_medium", "air", "fits"});
            }
        }
    }
    if (!HasLoadedMetadataKey(loaded, "observer_frame_correction")) {
        const std::optional<std::string_view> heliocentric_applied =
            LoadedMetadataValue(loaded, "heliocentric_correction_applied");
        if (HasLoadedMetadataKey(loaded, "heliocentric_correction_km_s") ||
            (heliocentric_applied && FitsMetadataTruthy(*heliocentric_applied))) {
            loaded.source_metadata.push_back({"observer_frame_correction", "heliocentric", "fits"});
        }
    }
}

void AddFitsRestFrameStatusMetadata(LoadedSpectrum& loaded)
{
    if (!HasLoadedMetadataKey(loaded, "rest_frame_correction_status")) {
        loaded.source_metadata.push_back({"rest_frame_correction_status", "not_applied", "domain"});
    }
}

void AddFitsTargetRestFrameMetadata(LoadedSpectrum& loaded)
{
    if (HasLoadedMetadataKey(loaded, "target_rest_frame_status")) {
        return;
    }

    const bool prefer_pipeline_redshift = SurveyClassUsesPipelineRedshift(loaded);
    const std::optional<double> radial_velocity = LoadedMetadataDouble(loaded, "radial_velocity_km_s");
    const bool radial_velocity_usable = radial_velocity && std::isfinite(*radial_velocity);
    const std::optional<double> redshift = LoadedMetadataDouble(loaded, "redshift");
    const bool redshift_usable = redshift && RedshiftValueIsUsable(*redshift);
    const bool redshift_unreliable = RedshiftWarningIsSet(loaded);

    if (!prefer_pipeline_redshift && radial_velocity_usable) {
        const double target_redshift = *radial_velocity / kSpeedOfLightKmPerSecond;
        loaded.source_metadata.push_back({"target_redshift", FormatFitsMetadataNumber(target_redshift), "domain"});
        loaded.source_metadata.push_back({"target_redshift_source", "radial_velocity_low_speed", "domain"});
        loaded.source_metadata.push_back({"target_redshift_status", "available", "domain"});
        loaded.source_metadata.push_back({"target_rest_frame_status", "available_not_applied", "domain"});
        return;
    }

    if (redshift_usable) {
        loaded.source_metadata.push_back({"target_redshift", FormatFitsMetadataNumber(*redshift), "domain"});
        loaded.source_metadata.push_back({"target_redshift_source", "pipeline_redshift", "domain"});
        if (redshift_unreliable) {
            loaded.source_metadata.push_back({"target_redshift_status", "unreliable", "domain"});
            loaded.source_metadata.push_back({"target_redshift_warning", "zwarning_nonzero", "domain"});
            loaded.source_metadata.push_back({"target_rest_frame_status", "unreliable_not_applied", "domain"});
        } else {
            loaded.source_metadata.push_back({"target_redshift_status", "available", "domain"});
            loaded.source_metadata.push_back({"target_rest_frame_status", "available_not_applied", "domain"});
        }
        return;
    }

    if (redshift && !redshift_usable) {
        loaded.source_metadata.push_back({"target_redshift_status", "invalid", "domain"});
        loaded.source_metadata.push_back({"target_redshift_warning", "invalid_pipeline_redshift", "domain"});
    } else {
        loaded.source_metadata.push_back({"target_redshift_status", "missing", "domain"});
    }
    loaded.source_metadata.push_back({"target_rest_frame_status", "unavailable", "domain"});
}

void AddFitsHeaderMetadata(LoadedSpectrum& loaded, const std::vector<FitsHdu>& hdus)
{
    const auto add_first_value = [&](std::string_view output_key, std::initializer_list<std::string_view> header_keys) {
        if (HasLoadedMetadataKey(loaded, output_key)) {
            return;
        }
        if (const std::optional<FitsMetadataMatch> match = FirstFitsHeaderValue(hdus, header_keys)) {
            loaded.source_metadata.push_back({std::string(output_key), match->value, "fits"});
        }
    };

    if (!HasLoadedMetadataKey(loaded, "radial_velocity_km_s")) {
        if (const std::optional<FitsMetadataMatch> match = FirstFitsHeaderValue(
                hdus,
                {"RADVEL", "RAD_VEL", "RADIALV", "RADIAL_V", "RV", "VRAD", "RVEL", "1D_RV"})) {
            loaded.source_metadata.push_back({"radial_velocity_km_s", match->value, "fits"});
            loaded.source_metadata.push_back({"radial_velocity_source", "header:" + match->key, "fits"});
        }
    }
    add_first_value("telescope", {"TELESCOP"});
    add_first_value("data_release", {"DATA_V", "RUN2D"});
    add_first_value("heliocentric_correction_km_s", {"HELIO_RV"});
    add_first_value("heliocentric_correction_applied", {"HELIO"});
    add_first_value("redshift", {"Z", "1D_Z"});
    add_first_value("redshift_error", {"Z_ERR", "ZERR", "1D_Z_ERR"});
    add_first_value("redshift_warning", {"ZWARNING", "Z_WARN"});
    add_first_value("redshift_flag", {"ZFLAG"});
    add_first_value("survey_class", {"CLASS", "1D_CLASS"});
    add_first_value("survey_subclass", {"SUBCLASS", "1D_SUBCL"});
    add_first_value("wavelength_vacuum", {"VACUUM"});
    AddFitsWavelengthFrameMetadata(loaded);
    AddFitsRestFrameStatusMetadata(loaded);
    AddFitsTargetRestFrameMetadata(loaded);
}

std::optional<FitsMetadataMatch> FitsScalarColumnMetadata(
    const std::vector<unsigned char>& bytes,
    const FitsHdu& hdu,
    std::size_t row_index,
    std::initializer_list<std::string_view> column_names)
{
    const FitsColumn* column = FindFitsColumn(hdu, column_names);
    if (column == nullptr || column->repeat != 1 || !IsFitsNumericColumn(*column)) {
        return std::nullopt;
    }
    std::vector<double> values = ReadFitsColumnVector(bytes, hdu, *column, row_index, false);
    if (values.empty() || !std::isfinite(values.front())) {
        return std::nullopt;
    }
    return FitsMetadataMatch{column->name, FormatFitsMetadataNumber(values.front())};
}

void AddFitsTableMetadata(
    LoadedSpectrum& loaded,
    const std::vector<unsigned char>& bytes,
    const FitsHdu& hdu,
    std::size_t row_index,
    bool scalar_rows)
{
    if (scalar_rows) {
        return;
    }

    if (!HasLoadedMetadataKey(loaded, "radial_velocity_km_s")) {
        if (const std::optional<FitsMetadataMatch> match = FitsScalarColumnMetadata(
                bytes,
                hdu,
                row_index,
                {"RADIALVELOCITYKMS",
                 "RADIALVELOCITY",
                 "RADIALVEL",
                 "RADVEL",
                 "VRAD",
                 "RV",
                 "RVEL",
                 "1DRV"})) {
            loaded.source_metadata.push_back({"radial_velocity_km_s", match->value, "fits"});
            loaded.source_metadata.push_back({"radial_velocity_source", "table_column:" + match->key, "fits"});
        }
    }
    if (!HasLoadedMetadataKey(loaded, "redshift")) {
        if (const std::optional<FitsMetadataMatch> match =
                FitsScalarColumnMetadata(bytes, hdu, row_index, {"REDSHIFT", "Z", "ZHELIO", "1DZ"})) {
            loaded.source_metadata.push_back({"redshift", match->value, "fits"});
        }
    }
    if (!HasLoadedMetadataKey(loaded, "redshift_error")) {
        if (const std::optional<FitsMetadataMatch> match =
                FitsScalarColumnMetadata(bytes, hdu, row_index, {"ZERR", "ZERROR", "ZERRPIPE", "ZERRNOQSO", "ZERRFULL"})) {
            loaded.source_metadata.push_back({"redshift_error", match->value, "fits"});
        }
    }
    if (!HasLoadedMetadataKey(loaded, "redshift_warning")) {
        if (const std::optional<FitsMetadataMatch> match =
                FitsScalarColumnMetadata(bytes, hdu, row_index, {"ZWARNING", "ZWARN", "ZWARNINGNOQSO"})) {
            loaded.source_metadata.push_back({"redshift_warning", match->value, "fits"});
        }
    }
}

std::optional<LoadedSpectrum> TryLoadFitsTableSpectrum(
    const std::vector<unsigned char>& bytes,
    const std::vector<FitsHdu>& hdus,
    const FitsHdu& hdu,
    const std::filesystem::path& path,
    std::size_t requested_index,
    std::string format)
{
    if (hdu.columns.empty()) {
        return std::nullopt;
    }

    const FitsColumn* flux_column = FindFitsColumn(hdu, {"FLUX"});
    const FitsColumn* wavelength_column = FindFitsColumn(hdu, {"WAVELENGTH", "WAVE", "WAV", "LAMBDA"});
    const FitsColumn* loglam_column = FindFitsColumn(hdu, {"LOGLAM"});
    if (flux_column == nullptr || (wavelength_column == nullptr && loglam_column == nullptr)) {
        return std::nullopt;
    }

    const FitsColumn* x_column = wavelength_column != nullptr ? wavelength_column : loglam_column;
    const bool uses_loglam = loglam_column != nullptr && wavelength_column == nullptr;
    const bool scalar_rows = flux_column->repeat == 1 && x_column->repeat == 1;
    if (!scalar_rows && flux_column->repeat != x_column->repeat) {
        throw SpectrumFileLoadError(
            SpectrumDiagnosticCode::WavelengthFluxSizeMismatch,
            "FITS table wavelength/loglam and flux vector columns have different lengths.");
    }

    const std::size_t row_count = static_cast<std::size_t>(FitsInteger(hdu.header, "NAXIS2"));
    LoadedSpectrum loaded;
    loaded.source_type = "fits_spectrum";
    loaded.format = std::move(format);
    loaded.name = FileNameToUtf8(path);
    loaded.spectrum_count = scalar_rows ? 1U : row_count;
    loaded.current_index = scalar_rows ? 0U : requested_index;
    loaded.can_switch_spectrum = !scalar_rows && row_count > 1;
    loaded.source_metadata.push_back({"hdu_index", std::to_string(hdu.index), "fits"});
    loaded.source_metadata.push_back({"hdu_type", "bintable", "fits"});
    loaded.source_metadata.push_back({"x_column", x_column->name, "fits"});
    loaded.source_metadata.push_back({"flux_column", flux_column->name, "fits"});
    AddFitsTableMetadata(loaded, bytes, hdu, requested_index, scalar_rows);
    loaded.spectrum_metadata.push_back({"hdu_index", std::to_string(hdu.index), "fits"});
    if (!scalar_rows) {
        loaded.spectrum_metadata.push_back({"row_index", std::to_string(requested_index), "fits"});
    }

    loaded.x_values = ReadFitsColumnVector(bytes, hdu, *x_column, requested_index, scalar_rows);
    if (uses_loglam) {
        for (double& value : loaded.x_values) {
            value = std::pow(10.0, value);
        }
    }
    loaded.y_values = ReadFitsColumnVector(bytes, hdu, *flux_column, requested_index, scalar_rows);

    const FitsColumn* ivar_column = FindFitsColumn(hdu, {"IVAR", "INVERSEVARIANCE"});
    const FitsColumn* mask_column = FindFitsMaskColumn(hdu);
    std::vector<double> ivar_values;
    std::vector<double> mask_values;
    const bool use_mask = !uses_loglam && mask_column != nullptr;
    const bool use_ivar = !use_mask && ivar_column != nullptr;
    if (use_ivar) {
        ivar_values = ReadFitsColumnVector(bytes, hdu, *ivar_column, requested_index, scalar_rows);
        loaded.source_metadata.push_back({"valid_pixel_rule", "ivar_positive", "fits"});
    }
    if (use_mask) {
        mask_values = ReadFitsColumnVector(bytes, hdu, *mask_column, requested_index, scalar_rows);
        loaded.source_metadata.push_back({"valid_pixel_rule", "ormask_zero", "fits"});
    }

    FilterStats stats;
    FilterSpectrumPixels(
        loaded.x_values,
        loaded.y_values,
        use_mask ? &mask_values : nullptr,
        use_ivar ? &ivar_values : nullptr,
        true,
        stats);
    AddFilterDiagnostics(loaded.diagnostics, stats);
    loaded.diagnostics.push_back(MakeDiagnostic(
        SpectrumDiagnosticSeverity::Warning,
        SpectrumDiagnosticCode::RestFrameNotApplied,
        "FITS wavelength values are plotted as provided; no rest-frame correction was applied."));
    AddFitsHeaderMetadata(loaded, hdus);
    return loaded;
}

std::vector<double> ReadFitsImageRow(
    const std::vector<unsigned char>& bytes,
    const FitsHdu& hdu,
    std::size_t row_index,
    std::size_t column_count)
{
    const std::int64_t bitpix = FitsInteger(hdu.header, "BITPIX");
    const std::size_t element_size = FitsBitpixElementSize(bitpix);
    if ((bitpix != -32 && bitpix != -64) || element_size == 0) {
        throw SpectrumFileLoadError(
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Only float32/float64 FITS image spectra are supported.");
    }

    std::vector<double> values;
    values.reserve(column_count);
    const std::size_t row_offset = hdu.data_offset + row_index * column_count * element_size;
    for (std::size_t column = 0; column < column_count; ++column) {
        const std::size_t offset = row_offset + column * element_size;
        if (offset > bytes.size() || element_size > bytes.size() - offset) {
            throw SpectrumFileLoadError(SpectrumDiagnosticCode::InvalidShape, "FITS image row data is truncated.");
        }
        const unsigned char* value_bytes = bytes.data() + offset;
        values.push_back(ReadFitsNumericValue(value_bytes, bitpix == -32 ? 'E' : 'D'));
    }
    return values;
}

std::optional<LoadedSpectrum> TryLoadFitsImageSpectrum(
    const std::vector<unsigned char>& bytes,
    const std::vector<FitsHdu>& hdus,
    const FitsHdu& hdu,
    const std::filesystem::path& path,
    std::string format)
{
    if (UpperAscii(FitsValue(hdu.header, "XTENSION").value_or({})) == "BINTABLE") {
        return std::nullopt;
    }
    const std::int64_t axis_count = FitsInteger(hdu.header, "NAXIS");
    if (axis_count < 1) {
        return std::nullopt;
    }
    const std::optional<double> coeff0 = FitsDouble(hdu.header, "COEFF0");
    const std::optional<double> coeff1 = FitsDouble(hdu.header, "COEFF1");
    if (!coeff0 || !coeff1) {
        return std::nullopt;
    }

    const std::size_t column_count = static_cast<std::size_t>(FitsInteger(hdu.header, "NAXIS1"));
    const std::size_t row_count = axis_count >= 2 ? static_cast<std::size_t>(FitsInteger(hdu.header, "NAXIS2")) : 1U;
    if (column_count == 0 || row_count == 0) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::EmptyData, "FITS image spectrum has an empty shape.");
    }

    LoadedSpectrum loaded;
    loaded.source_type = "fits_spectrum";
    loaded.format = std::move(format);
    loaded.name = FileNameToUtf8(path);
    loaded.source_metadata.push_back({"hdu_index", std::to_string(hdu.index), "fits"});
    loaded.source_metadata.push_back({"hdu_type", "image", "fits"});
    loaded.source_metadata.push_back({"wavelength_formula", "10**(coeff0+coeff1*pixel)", "fits"});
    loaded.source_metadata.push_back({"coeff0", std::to_string(*coeff0), "fits"});
    loaded.source_metadata.push_back({"coeff1", std::to_string(*coeff1), "fits"});
    loaded.spectrum_metadata.push_back({"hdu_index", std::to_string(hdu.index), "fits"});
    loaded.y_values = ReadFitsImageRow(bytes, hdu, 0, column_count);
    loaded.x_values.reserve(column_count);
    for (std::size_t index = 0; index < column_count; ++index) {
        loaded.x_values.push_back(std::pow(10.0, *coeff0 + *coeff1 * static_cast<double>(index)));
    }

    std::vector<double> ivar_values;
    if (row_count > 1) {
        ivar_values = ReadFitsImageRow(bytes, hdu, 1, column_count);
    }
    std::vector<double> mask_values;
    if (row_count > 4) {
        mask_values = ReadFitsImageRow(bytes, hdu, 4, column_count);
    }
    if (!ivar_values.empty() && !mask_values.empty()) {
        loaded.source_metadata.push_back({"valid_pixel_rule", "ivar_positive_and_ormask_zero", "fits"});
    } else if (!ivar_values.empty()) {
        loaded.source_metadata.push_back({"valid_pixel_rule", "ivar_positive", "fits"});
    } else if (!mask_values.empty()) {
        loaded.source_metadata.push_back({"valid_pixel_rule", "ormask_zero", "fits"});
    }

    FilterStats stats;
    FilterSpectrumPixels(
        loaded.x_values,
        loaded.y_values,
        mask_values.empty() ? nullptr : &mask_values,
        ivar_values.empty() ? nullptr : &ivar_values,
        true,
        stats);
    AddFilterDiagnostics(loaded.diagnostics, stats);
    loaded.diagnostics.push_back(MakeDiagnostic(
        SpectrumDiagnosticSeverity::Warning,
        SpectrumDiagnosticCode::RestFrameNotApplied,
        "FITS wavelength values are plotted as provided; no rest-frame correction was applied."));
    AddFitsHeaderMetadata(loaded, hdus);
    return loaded;
}

SpectrumSnapshotHandle LoadFitsSnapshot(const std::filesystem::path& path, std::size_t spectrum_index)
{
    const std::string format = SourceFormatLabel(path);
    try {
        std::vector<unsigned char> bytes = ReadWholeFile(path, kMaxSynchronousFitsFileBytes);
        if (format == "fits.gz") {
            bytes = DecompressGzip(bytes);
        }
        const std::vector<FitsHdu> hdus = ParseFitsHdus(bytes);

        for (const FitsHdu& hdu : hdus) {
            std::optional<LoadedSpectrum> loaded =
                TryLoadFitsTableSpectrum(bytes, hdus, hdu, path, spectrum_index, format);
            if (loaded) {
                return MakeLoadedSpectrumSnapshot(path, std::move(*loaded));
            }
        }
        for (const FitsHdu& hdu : hdus) {
            std::optional<LoadedSpectrum> loaded = TryLoadFitsImageSpectrum(bytes, hdus, hdu, path, format);
            if (loaded) {
                return MakeLoadedSpectrumSnapshot(path, std::move(*loaded));
            }
        }

        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::CatalogNotSpectrum,
            "This FITS file looks like a catalog or unsupported FITS, not a single spectrum.",
            {{"format", format, "domain"}},
            "file",
            {{"format", format, "domain"}});
    } catch (const SpectrumFileLoadError& error) {
        return MakeErrorSnapshot(
            path,
            error.code(),
            error.what(),
            {{"format", format, "domain"}},
            "file",
            {{"format", format, "domain"}});
    } catch (const std::exception& error) {
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::InvalidShape,
            error.what(),
            {{"format", format, "domain"}},
            "file",
            {{"format", format, "domain"}});
    }
}

bool IsCsvSourceFile(const std::filesystem::path& path)
{
    return ExtensionLower(path) == ".csv";
}

bool IsFitsSourceFile(const std::filesystem::path& path)
{
    const std::string format = SourceFormatLabel(path);
    return format == "fits" || format == "fits.gz";
}

struct FolderSpectrumFile {
    std::filesystem::path path;
    std::string format;
};

struct FolderScanResult {
    std::vector<FolderSpectrumFile> spectra;
    std::size_t csv_count = 0;
    std::size_t fits_count = 0;
    std::size_t ignored_file_count = 0;
    std::size_t ignored_directory_count = 0;
    std::vector<std::string> ignored_file_examples;
    std::vector<std::string> ignored_directory_examples;
};

void PushExample(std::vector<std::string>& examples, const std::filesystem::path& path)
{
    constexpr std::size_t kMaxExamples = 3;
    if (examples.size() < kMaxExamples) {
        examples.push_back(FileNameToUtf8(path));
    }
}

std::string JoinExamples(const std::vector<std::string>& examples)
{
    std::string joined;
    for (std::size_t index = 0; index < examples.size(); ++index) {
        if (index > 0) {
            joined += ", ";
        }
        joined += examples[index];
    }
    return joined;
}

FolderScanResult ScanFolderSource(const std::filesystem::path& path)
{
    FolderScanResult scan;
    std::error_code iterator_error;
    std::filesystem::directory_iterator iterator(
        path,
        std::filesystem::directory_options::none,
        iterator_error);
    if (iterator_error) {
        throw SpectrumFileLoadError(SpectrumDiagnosticCode::OpenFailed, "Could not enumerate the input folder.");
    }

    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code type_error;
        if (entry.is_directory(type_error)) {
            ++scan.ignored_directory_count;
            PushExample(scan.ignored_directory_examples, entry.path());
            continue;
        }
        if (type_error) {
            ++scan.ignored_file_count;
            PushExample(scan.ignored_file_examples, entry.path());
            continue;
        }

        if (!entry.is_regular_file(type_error) || type_error) {
            ++scan.ignored_file_count;
            PushExample(scan.ignored_file_examples, entry.path());
            continue;
        }

        if (IsCsvSourceFile(entry.path())) {
            ++scan.csv_count;
            scan.spectra.push_back(FolderSpectrumFile{entry.path(), "csv"});
        } else if (IsFitsSourceFile(entry.path())) {
            ++scan.fits_count;
            scan.spectra.push_back(FolderSpectrumFile{entry.path(), SourceFormatLabel(entry.path())});
        } else {
            ++scan.ignored_file_count;
            PushExample(scan.ignored_file_examples, entry.path());
        }
    }

    std::stable_sort(scan.spectra.begin(), scan.spectra.end(), [](const FolderSpectrumFile& left, const FolderSpectrumFile& right) {
        return LowerAscii(FileNameToUtf8(left.path)) < LowerAscii(FileNameToUtf8(right.path));
    });
    return scan;
}

std::vector<SpectrumDiagnostic> FolderWarnings(const FolderScanResult& scan)
{
    std::vector<SpectrumDiagnostic> warnings;
    if (scan.ignored_directory_count > 0) {
        warnings.push_back(MakeDiagnostic(
            SpectrumDiagnosticSeverity::Warning,
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Folder loading is non-recursive; subfolders were ignored.",
            {{"ignored_directory_count", std::to_string(scan.ignored_directory_count), "domain"},
             {"examples", JoinExamples(scan.ignored_directory_examples), "domain"}}));
    }
    if (scan.ignored_file_count > 0) {
        warnings.push_back(MakeDiagnostic(
            SpectrumDiagnosticSeverity::Warning,
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Folder contains files that are not CSV or FITS spectra; they were ignored.",
            {{"ignored_file_count", std::to_string(scan.ignored_file_count), "domain"},
             {"examples", JoinExamples(scan.ignored_file_examples), "domain"}}));
    }
    if (scan.csv_count > 0 && scan.fits_count > 0) {
        warnings.push_back(MakeDiagnostic(
            SpectrumDiagnosticSeverity::Warning,
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Folder mixes CSV and FITS spectra; files were loaded in filename order.",
            {{"csv_file_count", std::to_string(scan.csv_count), "domain"},
             {"fits_file_count", std::to_string(scan.fits_count), "domain"}}));
    }
    return warnings;
}

std::vector<SpectrumMetadataEntry> FolderSourceMetadata(const FolderScanResult& scan, const FolderSpectrumFile* current_file)
{
    std::vector<SpectrumMetadataEntry> metadata = {
        {"format", "folder", "domain"},
        {"spectrum_file_count", std::to_string(scan.spectra.size()), "domain"},
        {"csv_file_count", std::to_string(scan.csv_count), "domain"},
        {"fits_file_count", std::to_string(scan.fits_count), "domain"},
        {"ignored_file_count", std::to_string(scan.ignored_file_count), "domain"},
        {"ignored_directory_count", std::to_string(scan.ignored_directory_count), "domain"},
    };
    if (current_file != nullptr) {
        metadata.push_back({"current_file", FileNameToUtf8(current_file->path), "domain"});
        metadata.push_back({"current_file_format", current_file->format, "domain"});
        metadata.push_back({"current_file_path", PathToUtf8(current_file->path), "domain"});
    }
    return metadata;
}

SpectrumSnapshotHandle LoadFolderSnapshot(const std::filesystem::path& path, std::size_t spectrum_index)
{
    FolderScanResult scan;
    try {
        scan = ScanFolderSource(path);
    } catch (const SpectrumFileLoadError& error) {
        return MakeErrorSnapshot(path, error.code(), error.what(), {}, "folder");
    }

    std::vector<SpectrumDiagnostic> folder_warnings = FolderWarnings(scan);
    if (scan.spectra.empty()) {
        SpectrumSnapshotHandle snapshot = MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Folder does not contain CSV or FITS spectrum files.",
            {},
            "folder",
            FolderSourceMetadata(scan, nullptr));
        auto mutable_snapshot = std::make_shared<SpectrumSnapshot>(*snapshot);
        mutable_snapshot->diagnostics.insert(
            mutable_snapshot->diagnostics.end(),
            std::make_move_iterator(folder_warnings.begin()),
            std::make_move_iterator(folder_warnings.end()));
        return mutable_snapshot;
    }

    if (spectrum_index >= scan.spectra.size()) {
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::InvalidShape,
            "Requested folder spectrum index is outside the available file list.",
            {{"requested_index", std::to_string(spectrum_index), "domain"},
             {"spectrum_file_count", std::to_string(scan.spectra.size()), "domain"}},
            "folder",
            FolderSourceMetadata(scan, nullptr));
    }

    const FolderSpectrumFile& selected = scan.spectra[spectrum_index];
    SpectrumSnapshotHandle selected_snapshot =
        selected.format == "csv" ? LoadCsvSnapshot(selected.path) : LoadFitsSnapshot(selected.path, 0);

    auto snapshot = std::make_shared<SpectrumSnapshot>(*selected_snapshot);
    AddSourceBasics(*snapshot, path, "folder");
    snapshot->source.metadata.clear();
    snapshot->source.metadata.push_back({"source_type", "folder_collection", "domain"});
    std::vector<SpectrumMetadataEntry> folder_metadata = FolderSourceMetadata(scan, &selected);
    snapshot->source.metadata.insert(
        snapshot->source.metadata.end(),
        std::make_move_iterator(folder_metadata.begin()),
        std::make_move_iterator(folder_metadata.end()));
    snapshot->source.metadata.insert(
        snapshot->source.metadata.end(),
        selected_snapshot->source.metadata.begin(),
        selected_snapshot->source.metadata.end());

    snapshot->collection.spectrum_count = scan.spectra.size();
    snapshot->collection.current_index = spectrum_index;
    snapshot->collection.can_move_previous = spectrum_index > 0;
    snapshot->collection.can_move_next = spectrum_index + 1 < scan.spectra.size();
    snapshot->capabilities.can_switch_spectrum = scan.spectra.size() > 1;

    if (snapshot->current_spectrum.name.empty()) {
        snapshot->current_spectrum.name = FileNameToUtf8(selected.path);
    }
    snapshot->current_spectrum.metadata.push_back({"folder_file_index", std::to_string(spectrum_index), "domain"});
    snapshot->current_spectrum.metadata.push_back({"folder_file_name", FileNameToUtf8(selected.path), "domain"});
    snapshot->current_spectrum.metadata.push_back({"folder_file_path", PathToUtf8(selected.path), "domain"});

    snapshot->diagnostics.insert(
        snapshot->diagnostics.begin(),
        std::make_move_iterator(folder_warnings.begin()),
        std::make_move_iterator(folder_warnings.end()));
    return snapshot;
}

}  // namespace

namespace detail {

SpectrumSnapshotHandle LoadSpectrumSnapshotFromPathImpl(const std::filesystem::path& path, std::size_t spectrum_index)
{
    if (path.empty()) {
        return MakeErrorSnapshot(path, SpectrumDiagnosticCode::OpenFailed, "No input path was provided.");
    }

    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::OpenFailed,
            exists_error ? "Could not inspect the input path." : "Input path does not exist.");
    }

    std::error_code directory_error;
    const bool is_directory = std::filesystem::is_directory(path, directory_error);
    if (directory_error) {
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::OpenFailed,
            "Could not inspect whether the input path is a file or directory.");
    }
    if (is_directory) {
        return LoadFolderSnapshot(path, spectrum_index);
    }

    const std::string extension = ExtensionLower(path);
    const std::string format = SourceFormatLabel(path);
    if (extension == ".csv") {
        return LoadCsvSnapshot(path);
    }
    if (format == "fits" || format == "fits.gz") {
        return LoadFitsSnapshot(path, spectrum_index);
    }
    if (extension != ".npy") {
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::UnsupportedFormat,
            "This file extension is not supported as a spectrum source.",
            {{"format", format, "domain"}},
            "file",
            {{"format", format, "domain"}});
    }

    if (IsAuxiliaryNpyArrayName(path)) {
        return MakeNpyErrorSnapshot(
            path,
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Auxiliary .npy arrays are not opened as spectra; choose a spectrum matrix such as *_X.npy or *_flux.npy.",
            {{"file_role", "auxiliary_npy", "domain"}});
    }

    return LoadNpySnapshot(path, spectrum_index);
}

}  // namespace detail

}  // namespace specforge
