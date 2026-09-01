#include "domain/spectrum_format_adapters.h"

#include "domain/fits_spectrum_loader.h"
#include "domain/npy_array_io.h"
#include "domain/source_collection_manifest.h"
#include "domain/spectrum_loader_support.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace specforge::detail {
namespace {

constexpr std::size_t kLogLamGridColumns = 3909;
constexpr double kLogLamStart = 3.5682;
constexpr double kLogLamStep = 0.0001;

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

class SpectrumLoadCanceled final : public std::exception {
};

void ThrowIfCanceled(const SpectrumLoadCancellationCheck& cancellation_requested)
{
    if (cancellation_requested && cancellation_requested()) {
        throw SpectrumLoadCanceled();
    }
}

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
std::vector<double> ReadTypedRow(
    std::ifstream& stream,
    std::size_t column_count,
    const SpectrumLoadCancellationCheck& cancellation_requested)
{
    ThrowIfCanceled(cancellation_requested);
    std::vector<T> typed_values(column_count);
    constexpr std::size_t kReadChunkBytes = 1024U * 1024U;
    const std::size_t chunk_value_count = std::max<std::size_t>(1, kReadChunkBytes / sizeof(T));
    for (std::size_t offset = 0; offset < column_count;) {
        ThrowIfCanceled(cancellation_requested);
        const std::size_t value_count = std::min(chunk_value_count, column_count - offset);
        stream.read(
            reinterpret_cast<char*>(typed_values.data() + offset),
            static_cast<std::streamsize>(value_count * sizeof(T)));
        if (!stream) {
            throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "Selected NPY row is truncated.");
        }
        offset += value_count;
    }
    ThrowIfCanceled(cancellation_requested);

    std::vector<double> values(column_count);
    for (std::size_t index = 0; index < column_count; ++index) {
        if ((index & 0xfffU) == 0U) {
            ThrowIfCanceled(cancellation_requested);
        }
        values[index] = static_cast<double>(typed_values[index]);
    }
    ThrowIfCanceled(cancellation_requested);
    return values;
}

NpyRow ReadNpyRow(
    const std::filesystem::path& path,
    std::size_t requested_index,
    const SpectrumLoadCancellationCheck& cancellation_requested)
{
    ThrowIfCanceled(cancellation_requested);
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
        values = ReadTypedRow<float>(stream, column_count, cancellation_requested);
        break;
    case sizeof(double):
        values = ReadTypedRow<double>(stream, column_count, cancellation_requested);
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

bool IsAuxiliaryNpyArrayName(const std::filesystem::path& path)
{
    return IsSourceCollectionAuxiliaryNpyArrayName(path);
}

std::vector<double> MakeXValues(
    std::size_t column_count,
    bool has_loglam_grid,
    const SpectrumLoadCancellationCheck& cancellation_requested)
{
    std::vector<double> x_values;
    x_values.reserve(column_count);
    for (std::size_t index = 0; index < column_count; ++index) {
        if ((index & 0xfffU) == 0U) {
            ThrowIfCanceled(cancellation_requested);
        }
        if (has_loglam_grid) {
            x_values.push_back(std::pow(10.0, kLogLamStart + static_cast<double>(index) * kLogLamStep));
        } else {
            x_values.push_back(static_cast<double>(index));
        }
    }
    ThrowIfCanceled(cancellation_requested);
    return x_values;
}

void FilterFiniteValues(
    std::vector<double>& x_values,
    std::vector<double>& y_values,
    std::size_t& filtered_count,
    const SpectrumLoadCancellationCheck& cancellation_requested)
{
    std::vector<double> filtered_x;
    std::vector<double> filtered_y;
    filtered_x.reserve(x_values.size());
    filtered_y.reserve(y_values.size());

    for (std::size_t index = 0; index < x_values.size(); ++index) {
        if ((index & 0xfffU) == 0U) {
            ThrowIfCanceled(cancellation_requested);
        }
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
    ThrowIfCanceled(cancellation_requested);
}

SpectrumSnapshotHandle LoadNpySnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SpectrumLoadCancellationCheck& cancellation_requested)
{
    NpyRow row;
    try {
        row = ReadNpyRow(path, spectrum_index, cancellation_requested);
    } catch (const SpectrumLoadCanceled&) {
        return nullptr;
    } catch (const NpyLoadError& error) {
        return MakeNpyErrorSnapshot(path, error.code(), error.what());
    } catch (const std::exception& error) {
        return MakeNpyErrorSnapshot(path, SpectrumDiagnosticCode::InvalidShape, error.what());
    }

    const bool has_loglam_grid = row.column_count == kLogLamGridColumns;
    std::vector<double> x_values;
    std::vector<double> y_values = std::move(row.values);
    std::size_t filtered_count = 0;
    try {
        x_values = MakeXValues(row.column_count, has_loglam_grid, cancellation_requested);
        FilterFiniteValues(x_values, y_values, filtered_count, cancellation_requested);
    } catch (const SpectrumLoadCanceled&) {
        return nullptr;
    }
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

    const std::optional<std::string> sample_name =
        LoadSourceCollectionNpySampleName(path, row.row_count, row.current_index);
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
    } else {
        snapshot->axis.x_quantity = SpectrumAxisQuantity::Pixel;
        snapshot->axis.x_unit = SpectrumAxisUnit::Pixel;
        snapshot->axis.x_frame = SpectrumAxisFrame::Unknown;
    }
    snapshot->axis.x_label = XLabelForAxis(snapshot->axis.x_quantity, snapshot->axis.x_unit);

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

std::vector<std::string> SplitCsvLine(
    std::string_view line,
    const SpectrumLoadCancellationCheck& cancellation_requested)
{
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        if ((index & 0xfffU) == 0U) {
            ThrowIfCanceled(cancellation_requested);
        }
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

SpectrumSnapshotHandle LoadCsvSnapshot(
    const std::filesystem::path& path,
    const SpectrumLoadCancellationCheck& cancellation_requested)
{
    ThrowIfCanceled(cancellation_requested);
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
    ThrowIfCanceled(cancellation_requested);

    const std::vector<std::string> first_fields = SplitCsvLine(first_line, cancellation_requested);
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
        ThrowIfCanceled(cancellation_requested);
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
        const double x_value = uses_loglam ? std::pow(10.0, *parsed_x) : *parsed_x;
        if (!std::isfinite(x_value) || x_value <= 0.0 || !std::isfinite(*parsed_y)) {
            ++stats.non_finite_or_non_positive_count;
            return;
        }
        loaded.x_values.push_back(x_value);
        loaded.y_values.push_back(*parsed_y);
    };

    if (first_line_is_data) {
        read_fields(first_fields);
    }

    std::string line;
    std::size_t line_count = 0;
    while (std::getline(stream, line)) {
        if ((line_count++ & 0xffU) == 0U) {
            ThrowIfCanceled(cancellation_requested);
        }
        if (TrimAscii(line).empty()) {
            continue;
        }
        read_fields(SplitCsvLine(line, cancellation_requested));
    }

    ThrowIfCanceled(cancellation_requested);
    AddFilterDiagnostics(loaded.diagnostics, stats);
    loaded.diagnostics.push_back(MakeDiagnostic(
        SpectrumDiagnosticSeverity::Warning,
        SpectrumDiagnosticCode::AxisFrameUnknown,
        "CSV wavelength values are plotted as provided; no rest-frame correction status is available."));
    return MakeLoadedSpectrumSnapshot(
        path,
        std::move(loaded),
        [&cancellation_requested]() { ThrowIfCanceled(cancellation_requested); });
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

std::vector<SpectrumDiagnostic> FolderWarnings(const SourceCollectionFolderListing& scan)
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
            "Folder contains files that are not supported single-file spectra; they were ignored.",
            {{"ignored_file_count", std::to_string(scan.ignored_file_count), "domain"},
             {"examples", JoinExamples(scan.ignored_file_examples), "domain"}}));
    }
    if (scan.csv_count > 0 && scan.fits_count > 0) {
        warnings.push_back(MakeDiagnostic(
            SpectrumDiagnosticSeverity::Warning,
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Folder mixes supported spectrum formats; files were loaded in filename order.",
            {{"csv_file_count", std::to_string(scan.csv_count), "domain"},
             {"fits_file_count", std::to_string(scan.fits_count), "domain"}}));
    }
    return warnings;
}

std::vector<SpectrumMetadataEntry> FolderSourceMetadata(
    const SourceCollectionFolderListing& scan,
    const SourceCollectionFolderSpectrumFile* current_file)
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

SpectrumSnapshotHandle LoadFolderSnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SourceCollectionFolderListing& scan,
    const SpectrumLoadCancellationCheck& cancellation_requested)
{
    if (cancellation_requested && cancellation_requested()) {
        return nullptr;
    }
    if (!scan.readable) {
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::OpenFailed,
            scan.error_message.empty() ? "Could not enumerate the input folder." : scan.error_message,
            {},
            "folder");
    }

    std::vector<SpectrumDiagnostic> folder_warnings = FolderWarnings(scan);
    if (scan.spectra.empty()) {
        SpectrumSnapshotHandle snapshot = MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Folder does not contain supported single-file spectrum files.",
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

    const SourceCollectionFolderSpectrumFile& selected = scan.spectra[spectrum_index];
    SpectrumSnapshotHandle selected_snapshot;
    try {
        selected_snapshot = LoadSpectrumSnapshotFromPathImplCancelable(
            selected.path,
            0,
            cancellation_requested);
    } catch (const SpectrumLoadCanceled&) {
        return nullptr;
    }
    if (!selected_snapshot || (cancellation_requested && cancellation_requested())) {
        return nullptr;
    }

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

SpectrumSnapshotHandle LoadSpectrumSnapshotFromPathImpl(const std::filesystem::path& path, std::size_t spectrum_index)
{
    return LoadSpectrumSnapshotFromPathImplCancelable(path, spectrum_index, {});
}

SpectrumSnapshotHandle LoadSpectrumSnapshotFromPathImplCancelable(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SpectrumLoadCancellationCheck& cancellation_requested)
{
    if (cancellation_requested && cancellation_requested()) {
        return nullptr;
    }
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
        return LoadFolderSnapshot(path, spectrum_index, ScanSourceCollectionFolder(path), cancellation_requested);
    }

    const std::string extension = ExtensionLower(path);
    const std::string format = SourceFormatLabel(path);
    if (extension == ".csv") {
        try {
            SpectrumSnapshotHandle snapshot = LoadCsvSnapshot(path, cancellation_requested);
            return cancellation_requested && cancellation_requested() ? nullptr : snapshot;
        } catch (const SpectrumLoadCanceled&) {
            return nullptr;
        }
    }
    if (IsFitsSourcePath(path)) {
        return LoadFitsSnapshotCancelable(path, spectrum_index, cancellation_requested);
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

    return LoadNpySnapshot(path, spectrum_index, cancellation_requested);
}

SpectrumSnapshotHandle LoadFolderSpectrumSnapshotFromListing(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SourceCollectionFolderListing& listing)
{
    return LoadFolderSnapshot(path, spectrum_index, listing, {});
}

SpectrumSnapshotHandle LoadFolderSpectrumSnapshotFromListingCancelable(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const SourceCollectionFolderListing& listing,
    const SpectrumLoadCancellationCheck& cancellation_requested)
{
    return LoadFolderSnapshot(path, spectrum_index, listing, cancellation_requested);
}

}  // namespace specforge::detail
