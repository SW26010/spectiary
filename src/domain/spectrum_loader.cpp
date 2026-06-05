#include "domain/spectrum_loader.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace specforge {
namespace {

constexpr std::size_t kLogLamGridColumns = 3909;
constexpr double kLogLamStart = 3.5682;
constexpr double kLogLamStep = 0.0001;

enum class NpyElementType {
    Float32,
    Float64,
};

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

struct NpyHeader {
    std::string descr;
    NpyElementType element_type = NpyElementType::Float64;
    std::size_t element_size = 0;
    std::vector<std::size_t> shape;
    std::uint64_t data_offset = 0;
};

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

std::uint16_t ReadLittleEndianU16(const std::array<unsigned char, 2>& bytes)
{
    return static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t ReadLittleEndianU32(const std::array<unsigned char, 4>& bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::optional<std::string> RegexCapture(const std::string& text, const std::regex& expression)
{
    std::smatch match;
    if (!std::regex_search(text, match, expression) || match.size() < 2) {
        return std::nullopt;
    }
    return match[1].str();
}

std::optional<std::vector<std::size_t>> ParseShape(const std::string& shape_text)
{
    std::vector<std::size_t> shape;
    std::stringstream stream(shape_text);
    std::string token;

    while (std::getline(stream, token, ',')) {
        token = TrimAscii(token);
        if (token.empty()) {
            continue;
        }
        std::size_t parsed_offset = 0;
        unsigned long long value = 0;
        try {
            value = std::stoull(token, &parsed_offset, 10);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        if (parsed_offset != token.size() || value > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        shape.push_back(static_cast<std::size_t>(value));
    }

    if (shape.empty()) {
        return std::nullopt;
    }
    return shape;
}

std::optional<NpyElementType> ParseElementType(std::string_view descr)
{
    if (descr.size() < 3) {
        return std::nullopt;
    }

    const char endian = descr[0];
    const char kind = descr[1];
    if (endian == '>') {
        return std::nullopt;
    }
    if (endian != '<' && endian != '=' && endian != '|') {
        return std::nullopt;
    }
    if (kind != 'f') {
        return std::nullopt;
    }

    if (descr.substr(2) == "4") {
        return NpyElementType::Float32;
    }
    if (descr.substr(2) == "8") {
        return NpyElementType::Float64;
    }
    return std::nullopt;
}

std::size_t ElementSize(NpyElementType element_type)
{
    switch (element_type) {
    case NpyElementType::Float32:
        return sizeof(float);
    case NpyElementType::Float64:
        return sizeof(double);
    default:
        return 0;
    }
}

NpyHeader ReadNpyHeader(std::ifstream& stream)
{
    std::array<unsigned char, 6> magic = {};
    stream.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    constexpr std::array<unsigned char, 6> kExpectedMagic = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    if (!stream || magic != kExpectedMagic) {
        throw NpyLoadError(SpectrumDiagnosticCode::UnsupportedFormat, "File does not start with the NPY magic header.");
    }

    std::array<unsigned char, 2> version = {};
    stream.read(reinterpret_cast<char*>(version.data()), static_cast<std::streamsize>(version.size()));
    if (!stream) {
        throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "NPY version header is truncated.");
    }

    std::uint32_t header_length = 0;
    std::uint64_t data_offset = magic.size() + version.size();
    if (version[0] == 1) {
        std::array<unsigned char, 2> length_bytes = {};
        stream.read(reinterpret_cast<char*>(length_bytes.data()), static_cast<std::streamsize>(length_bytes.size()));
        if (!stream) {
            throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "NPY v1 header length is truncated.");
        }
        header_length = ReadLittleEndianU16(length_bytes);
        data_offset += length_bytes.size();
    } else if (version[0] == 2 || version[0] == 3) {
        std::array<unsigned char, 4> length_bytes = {};
        stream.read(reinterpret_cast<char*>(length_bytes.data()), static_cast<std::streamsize>(length_bytes.size()));
        if (!stream) {
            throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "NPY v2/v3 header length is truncated.");
        }
        header_length = ReadLittleEndianU32(length_bytes);
        data_offset += length_bytes.size();
    } else {
        throw NpyLoadError(SpectrumDiagnosticCode::UnsupportedFormat, "Unsupported NPY major version.");
    }

    std::string header_text(header_length, '\0');
    stream.read(header_text.data(), static_cast<std::streamsize>(header_text.size()));
    if (!stream) {
        throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "NPY header is truncated.");
    }
    data_offset += header_length;

    static const std::regex kDescrExpression("'descr'\\s*:\\s*'([^']+)'");
    static const std::regex kFortranExpression("'fortran_order'\\s*:\\s*(True|False)");
    static const std::regex kShapeExpression("'shape'\\s*:\\s*\\(([^\\)]*)\\)");

    const std::optional<std::string> descr = RegexCapture(header_text, kDescrExpression);
    const std::optional<std::string> fortran_order = RegexCapture(header_text, kFortranExpression);
    const std::optional<std::string> shape_text = RegexCapture(header_text, kShapeExpression);
    if (!descr || !fortran_order || !shape_text) {
        throw NpyLoadError(
            SpectrumDiagnosticCode::InvalidShape,
            "NPY header is missing descr, fortran_order, or shape.");
    }
    if (*fortran_order != "False") {
        throw NpyLoadError(
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Fortran-order NPY arrays are not supported yet.");
    }

    const std::optional<NpyElementType> element_type = ParseElementType(*descr);
    if (!element_type) {
        throw NpyLoadError(
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Only little-endian float32 and float64 NPY arrays are supported.");
    }

    const std::optional<std::vector<std::size_t>> shape = ParseShape(*shape_text);
    if (!shape) {
        throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "NPY shape is invalid.");
    }

    return NpyHeader{
        *descr,
        *element_type,
        ElementSize(*element_type),
        *shape,
        data_offset,
    };
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
    std::size_t column_count)
{
    const std::uint64_t element_count = CheckedElementCount(row_count, column_count);
    if (element_count > std::numeric_limits<std::uint64_t>::max() / header.element_size) {
        throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "NPY byte size overflows.");
    }
    const std::uint64_t data_bytes = element_count * static_cast<std::uint64_t>(header.element_size);
    if (header.data_offset > std::numeric_limits<std::uint64_t>::max() - data_bytes) {
        throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "NPY byte size overflows.");
    }

    std::error_code error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error) {
        throw NpyLoadError(SpectrumDiagnosticCode::OpenFailed, "Could not inspect NPY file size.");
    }
    if (file_size < header.data_offset + data_bytes) {
        throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "NPY file is smaller than the declared array data.");
    }
}

template <typename T>
std::vector<double> ReadTypedRow(std::ifstream& stream, std::size_t column_count)
{
    if (column_count > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) / sizeof(T)) {
        throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "Selected NPY row is too large to read.");
    }

    std::vector<T> typed_values(column_count);
    stream.read(
        reinterpret_cast<char*>(typed_values.data()),
        static_cast<std::streamsize>(typed_values.size() * sizeof(T)));
    if (!stream) {
        throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "Selected NPY row is truncated.");
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

    const NpyHeader header = ReadNpyHeader(stream);
    const auto [row_count, column_count] = MatrixShape(header);
    ValidateFileSize(path, header, row_count, column_count);

    if (requested_index >= row_count) {
        throw NpyLoadError(
            SpectrumDiagnosticCode::InvalidShape,
            "Requested spectrum row is outside the NPY matrix.");
    }

    const std::uint64_t row_bytes = static_cast<std::uint64_t>(column_count) * header.element_size;
    const std::uint64_t row_offset = header.data_offset + static_cast<std::uint64_t>(requested_index) * row_bytes;
    if (row_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw NpyLoadError(SpectrumDiagnosticCode::InvalidShape, "Selected NPY row offset is too large.");
    }

    stream.seekg(static_cast<std::streamoff>(row_offset), std::ios::beg);
    if (!stream) {
        throw NpyLoadError(SpectrumDiagnosticCode::OpenFailed, "Could not seek to the selected NPY row.");
    }

    std::vector<double> values;
    switch (header.element_type) {
    case NpyElementType::Float32:
        values = ReadTypedRow<float>(stream, column_count);
        break;
    case NpyElementType::Float64:
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
    const std::string filename = LowerAscii(FileNameToUtf8(path));
    static constexpr std::array<std::string_view, 9> kAuxiliaryNames = {
        "y.npy",
        "label.npy",
        "index.npy",
        "ormask.npy",
        "inverse.npy",
        "known_mask.npy",
        "ivar.npy",
        "mask.npy",
        "name.npy",
    };
    static constexpr std::array<std::string_view, 9> kAuxiliarySuffixes = {
        "_y.npy",
        "_label.npy",
        "_index.npy",
        "_ormask.npy",
        "_inverse.npy",
        "_known_mask.npy",
        "_ivar.npy",
        "_mask.npy",
        "_name.npy",
    };

    return std::any_of(kAuxiliaryNames.begin(), kAuxiliaryNames.end(), [&filename](std::string_view name) {
               return filename == name;
           }) ||
           std::any_of(kAuxiliarySuffixes.begin(), kAuxiliarySuffixes.end(), [&filename](std::string_view suffix) {
               return filename.ends_with(suffix);
           });
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

    snapshot->current_spectrum.name = row.row_count > 1
                                          ? FileNameToUtf8(path) + " row " + std::to_string(row.current_index)
                                          : FileNameToUtf8(path);
    snapshot->current_spectrum.x_values = std::make_shared<const std::vector<double>>(std::move(x_values));
    snapshot->current_spectrum.y_values = std::make_shared<const std::vector<double>>(std::move(y_values));
    snapshot->current_spectrum.point_count = snapshot->current_spectrum.x_values->size();
    snapshot->current_spectrum.metadata.push_back({"row_index", std::to_string(row.current_index), "npy"});
    snapshot->current_spectrum.metadata.push_back({"source_columns", std::to_string(row.column_count), "npy"});

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

}  // namespace

SpectrumSnapshotHandle LoadSpectrumSnapshotFromPath(const std::filesystem::path& path, std::size_t spectrum_index)
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
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Folder sources can be added to the Files list, but directory parsing is not implemented yet.",
            {},
            "folder");
    }

    const std::string extension = ExtensionLower(path);
    if (extension != ".npy") {
        const std::string format = SourceFormatLabel(path);
        return MakeErrorSnapshot(
            path,
            SpectrumDiagnosticCode::UnsupportedFormat,
            "Only .npy spectrum matrices are supported in this vertical slice.",
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

}  // namespace specforge
