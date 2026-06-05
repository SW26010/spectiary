#include "domain/spectrum_loader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <zlib.h>

namespace {

using specforge::SpectrumAxisQuantity;
using specforge::SpectrumDiagnosticCode;
using specforge::SpectrumDiagnosticSeverity;
using specforge::SpectrumSnapshotHandle;
using specforge::SpectrumValueQuantity;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string ShapeText(const std::vector<std::size_t>& shape)
{
    std::ostringstream stream;
    stream << '(';
    for (std::size_t index = 0; index < shape.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << shape[index];
    }
    if (shape.size() == 1) {
        stream << ',';
    }
    stream << ')';
    return stream.str();
}

template <typename T>
std::vector<unsigned char> BytesFor(const std::vector<T>& values)
{
    std::vector<unsigned char> bytes(values.size() * sizeof(T));
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), values.data(), bytes.size());
    }
    return bytes;
}

std::vector<unsigned char> UnicodeNpyBytesFor(std::initializer_list<std::string_view> values, std::size_t code_units)
{
    std::vector<unsigned char> bytes;
    bytes.reserve(values.size() * code_units * sizeof(std::uint32_t));
    for (std::string_view value : values) {
        for (std::size_t index = 0; index < code_units; ++index) {
            const std::uint32_t code_point = index < value.size() ? static_cast<unsigned char>(value[index]) : 0U;
            bytes.push_back(static_cast<unsigned char>(code_point & 0xffU));
            bytes.push_back(static_cast<unsigned char>((code_point >> 8U) & 0xffU));
            bytes.push_back(static_cast<unsigned char>((code_point >> 16U) & 0xffU));
            bytes.push_back(static_cast<unsigned char>((code_point >> 24U) & 0xffU));
        }
    }
    return bytes;
}

std::string FitsCard(std::string_view key, std::string_view value = {})
{
    std::string card;
    if (key == "END") {
        card = "END";
    } else {
        card = std::string(key);
        card.resize(8, ' ');
        card += "= ";
        card += value;
    }
    card.resize(80, ' ');
    return card;
}

void WriteFitsHeader(std::ofstream& stream, const std::vector<std::string>& cards)
{
    std::string header;
    for (const std::string& card : cards) {
        Require(card.size() == 80, "FITS test card must be 80 bytes");
        header += card;
    }
    header += FitsCard("END");
    const std::size_t padding = (2880 - (header.size() % 2880)) % 2880;
    header.append(padding, ' ');
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));
}

void PadFitsData(std::ofstream& stream, std::size_t data_size)
{
    const std::size_t padding = (2880 - (data_size % 2880)) % 2880;
    const std::string bytes(padding, '\0');
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open fixture for reading");
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void WriteBytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open fixture for writing");
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    Require(stream.good(), "could not write fixture bytes");
}

std::vector<unsigned char> GzipRepeatedBytes(std::size_t byte_count)
{
    z_stream stream = {};
    Require(
        deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK,
        "could not initialize gzip fixture compressor");

    std::array<unsigned char, 64 * 1024> input = {};
    std::array<unsigned char, 64 * 1024> buffer = {};
    std::vector<unsigned char> output;
    int result = Z_OK;
    std::size_t remaining = byte_count;
    do {
        if (stream.avail_in == 0 && remaining > 0) {
            const std::size_t chunk = std::min(remaining, input.size());
            stream.next_in = reinterpret_cast<Bytef*>(input.data());
            stream.avail_in = static_cast<uInt>(chunk);
            remaining -= chunk;
        }

        const int flush = remaining == 0 ? Z_FINISH : Z_NO_FLUSH;
        do {
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());
            result = deflate(&stream, flush);
            Require(result == Z_OK || result == Z_STREAM_END, "could not gzip fixture bytes");
            const std::size_t produced = buffer.size() - stream.avail_out;
            output.insert(output.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(produced));
        } while (stream.avail_out == 0);
    } while (result != Z_STREAM_END);

    deflateEnd(&stream);
    return output;
}

std::vector<unsigned char> GzipBytes(const std::vector<unsigned char>& bytes)
{
    z_stream stream = {};
    Require(
        deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK,
        "could not initialize gzip fixture compressor");
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(bytes.data()));
    stream.avail_in = static_cast<uInt>(bytes.size());

    std::array<unsigned char, 64 * 1024> buffer = {};
    std::vector<unsigned char> output;
    int result = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = deflate(&stream, Z_FINISH);
        Require(result == Z_OK || result == Z_STREAM_END, "could not gzip fixture bytes");
        const std::size_t produced = buffer.size() - stream.avail_out;
        output.insert(output.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(produced));
    } while (result != Z_STREAM_END);

    deflateEnd(&stream);
    return output;
}

void AppendBigEndianFloat(std::vector<unsigned char>& bytes, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bytes.push_back(static_cast<unsigned char>((bits >> 24U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>((bits >> 16U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>((bits >> 8U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>(bits & 0xffU));
}

void WriteFitsPrimary(std::ofstream& stream)
{
    WriteFitsHeader(stream, {
                                FitsCard("SIMPLE", "                   T"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   0"),
                                FitsCard("EXTEND", "                   T"),
                            });
}

void WriteFitsScalarTable(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open scalar FITS fixture");
    WriteFitsPrimary(stream);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                  12"),
                                FitsCard("NAXIS2", "                   3"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   3"),
                                FitsCard("TTYPE1", "'flux'"),
                                FitsCard("TFORM1", "'E'"),
                                FitsCard("TTYPE2", "'loglam'"),
                                FitsCard("TFORM2", "'E'"),
                                FitsCard("TTYPE3", "'ivar'"),
                                FitsCard("TFORM3", "'E'"),
                            });

    std::vector<unsigned char> data;
    for (const std::array<float, 3> row : {
             std::array<float, 3>{10.0F, 3.0F, 1.0F},
             std::array<float, 3>{20.0F, 3.1F, 0.0F},
             std::array<float, 3>{30.0F, 3.2F, 2.0F},
         }) {
        AppendBigEndianFloat(data, row[0]);
        AppendBigEndianFloat(data, row[1]);
        AppendBigEndianFloat(data, row[2]);
    }
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write scalar FITS fixture");
}

void WriteFitsVectorTable(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open vector FITS fixture");
    WriteFitsPrimary(stream);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                  36"),
                                FitsCard("NAXIS2", "                   1"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   3"),
                                FitsCard("TTYPE1", "'WAVELENGTH'"),
                                FitsCard("TFORM1", "'3E'"),
                                FitsCard("TTYPE2", "'FLUX'"),
                                FitsCard("TFORM2", "'3E'"),
                                FitsCard("TTYPE3", "'ORMASK12'"),
                                FitsCard("TFORM3", "'3E'"),
                            });

    std::vector<unsigned char> data;
    for (float value : {5000.0F, 5001.0F, 5002.0F}) {
        AppendBigEndianFloat(data, value);
    }
    for (float value : {1.0F, 2.0F, 3.0F}) {
        AppendBigEndianFloat(data, value);
    }
    for (float value : {0.0F, 1.0F, 0.0F}) {
        AppendBigEndianFloat(data, value);
    }
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write vector FITS fixture");
}

void WriteMalformedFitsTableWidth(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open malformed FITS fixture");
    WriteFitsPrimary(stream);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                   4"),
                                FitsCard("NAXIS2", "                   1"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   2"),
                                FitsCard("TTYPE1", "'FLUX'"),
                                FitsCard("TFORM1", "'E'"),
                                FitsCard("TTYPE2", "'LOGLAM'"),
                                FitsCard("TFORM2", "'E'"),
                            });

    std::vector<unsigned char> data;
    AppendBigEndianFloat(data, 1.0F);
    AppendBigEndianFloat(data, 3.0F);
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write malformed FITS fixture");
}

void WriteFitsImage(const std::filesystem::path& path, bool use_coeff_wavelength)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open image FITS fixture");

    std::vector<std::string> cards = {
        FitsCard("SIMPLE", "                   T"),
        FitsCard("BITPIX", "                 -32"),
        FitsCard("NAXIS", "                   2"),
        FitsCard("NAXIS1", "                   3"),
        FitsCard("NAXIS2", "                   5"),
    };
    if (use_coeff_wavelength) {
        cards.push_back(FitsCard("COEFF0", "                 3.0"));
        cards.push_back(FitsCard("COEFF1", "               0.001"));
    } else {
        cards.push_back(FitsCard("CRVAL1", "                 3.0"));
        cards.push_back(FitsCard("CD1_1", "               0.001"));
    }
    WriteFitsHeader(stream, cards);

    std::vector<unsigned char> data;
    for (float value : {1.0F, 2.0F, 3.0F}) {
        AppendBigEndianFloat(data, value);
    }
    for (float value : {1.0F, 0.0F, 1.0F}) {
        AppendBigEndianFloat(data, value);
    }
    for (float value : {0.0F, 0.0F, 0.0F}) {
        AppendBigEndianFloat(data, value);
    }
    for (float value : {0.0F, 0.0F, 0.0F}) {
        AppendBigEndianFloat(data, value);
    }
    for (float value : {0.0F, 0.0F, 1.0F}) {
        AppendBigEndianFloat(data, value);
    }
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write image FITS fixture");
}

void WriteNpy(
    const std::filesystem::path& path,
    std::string_view descr,
    const std::vector<std::size_t>& shape,
    const std::vector<unsigned char>& payload)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open test fixture for writing");

    std::string header = "{'descr': '";
    header += descr;
    header += "', 'fortran_order': False, 'shape': ";
    header += ShapeText(shape);
    header += ", }";

    constexpr std::size_t kPreambleSize = 10;
    const std::size_t header_with_newline = header.size() + 1;
    const std::size_t padding = (16 - ((kPreambleSize + header_with_newline) % 16)) % 16;
    header.append(padding, ' ');
    header.push_back('\n');
    Require(header.size() <= std::numeric_limits<std::uint16_t>::max(), "test NPY header is too large");

    constexpr unsigned char kMagic[] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    stream.write(reinterpret_cast<const char*>(kMagic), static_cast<std::streamsize>(sizeof(kMagic)));
    constexpr char kVersion[] = {1, 0};
    stream.write(kVersion, static_cast<std::streamsize>(sizeof(kVersion)));

    const auto header_length = static_cast<std::uint16_t>(header.size());
    const char length_bytes[] = {
        static_cast<char>(header_length & 0xffU),
        static_cast<char>((header_length >> 8U) & 0xffU),
    };
    stream.write(length_bytes, static_cast<std::streamsize>(sizeof(length_bytes)));
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!payload.empty()) {
        stream.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }
    Require(stream.good(), "could not write test NPY fixture");
}

SpectrumDiagnosticCode FirstDiagnosticCode(const SpectrumSnapshotHandle& snapshot)
{
    Require(snapshot != nullptr, "expected a snapshot");
    Require(!snapshot->diagnostics.empty(), "expected a diagnostic");
    return snapshot->diagnostics.front().code;
}

bool HasDiagnosticCode(const SpectrumSnapshotHandle& snapshot, SpectrumDiagnosticCode code)
{
    Require(snapshot != nullptr, "expected a snapshot");
    for (const specforge::SpectrumDiagnostic& diagnostic : snapshot->diagnostics) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

std::string_view MetadataValue(const SpectrumSnapshotHandle& snapshot, std::string_view key)
{
    Require(snapshot != nullptr, "expected a snapshot");
    for (const specforge::SpectrumMetadataEntry& entry : snapshot->source.metadata) {
        if (entry.key == key) {
            return entry.value;
        }
    }
    return {};
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool IsFitsSample(const std::filesystem::path& path)
{
    const std::string extension = LowerAscii(PathToUtf8(path.extension()));
    if (extension == ".fits" || extension == ".fit" || extension == ".fts") {
        return true;
    }
    return extension == ".gz" && IsFitsSample(path.stem());
}

bool IsAuxiliaryNpySample(const std::filesystem::path& path)
{
    const std::string filename = LowerAscii(PathToUtf8(path.filename()));
    return filename.ends_with("_name.npy") || filename.ends_with("_y.npy") || filename.ends_with("_label.npy") ||
           filename.ends_with("_index.npy") || filename.ends_with("_ormask.npy") || filename.ends_with("_inverse.npy") ||
           filename.ends_with("_known_mask.npy");
}

bool DirectoryContainsCsvOrFitsSamples(const std::filesystem::path& path)
{
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(path)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string extension = LowerAscii(PathToUtf8(entry.path().extension()));
        if (extension == ".csv" || IsFitsSample(entry.path())) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> EnvironmentVariable(std::string_view name)
{
#if defined(_MSC_VER)
    std::string name_text(name);
    char* value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&value, &value_size, name_text.c_str()) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    return result.empty() ? std::nullopt : std::optional<std::string>{std::move(result)};
#else
    std::string name_text(name);
    const char* value = std::getenv(name_text.c_str());
    if (value == nullptr || std::string_view(value).empty()) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

void TestLoadsSelectedNpyRow()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_row_X.npy";
    const std::filesystem::path name_path = std::filesystem::temp_directory_path() / "specforge_loader_row_name.npy";
    WriteNpy(
        path,
        "<f8",
        {2, 3},
        BytesFor<double>({
            1.0,
            2.0,
            std::numeric_limits<double>::quiet_NaN(),
            4.0,
            5.0,
            6.0,
        }));
    WriteNpy(name_path, "<U5", {2}, UnicodeNpyBytesFor({"alpha", "beta"}, 5));

    const SpectrumSnapshotHandle first = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(first->capabilities.can_plot_current_spectrum, "first row should be plottable");
    Require(first->collection.spectrum_count == 2, "row count should be preserved");
    Require(first->collection.current_index == 0, "first row index should be selected");
    Require(first->collection.can_move_next, "first row should allow next navigation");
    Require(first->current_spectrum.point_count == 2, "non-finite pixels should be filtered");
    Require(first->current_spectrum.name == "alpha", "first row should use companion sample name");
    Require(first->axis.x_quantity == SpectrumAxisQuantity::Pixel, "short matrix should use pixel axis");
    Require(first->axis.y_quantity == SpectrumValueQuantity::FeatureValue, "X.npy should be labeled as feature data");
    Require(
        HasDiagnosticCode(first, SpectrumDiagnosticCode::NonFiniteValuesFiltered),
        "filtered row should report non-finite filtering");

    const SpectrumSnapshotHandle second = specforge::LoadSpectrumSnapshotFromPath(path, 1);
    Require(second->capabilities.can_plot_current_spectrum, "second row should be plottable");
    Require(second->collection.current_index == 1, "second row index should be selected");
    Require(second->collection.can_move_previous, "second row should allow previous navigation");
    Require(second->current_spectrum.point_count == 3, "finite second row should keep all points");
    Require(second->current_spectrum.name == "beta", "second row should use companion sample name");
}

void TestRejectsAuxiliaryNpyArrays()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_label.npy";
    WriteNpy(path, "<f8", {1, 2}, BytesFor<double>({1.0, 2.0}));

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(!snapshot->capabilities.can_plot_current_spectrum, "auxiliary arrays should not be plottable");
    Require(
        FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::UnsupportedFormat,
        "auxiliary arrays should be classified as unsupported format");
}

void TestClassifiesUnsupportedDtype()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_flux.npy";
    WriteNpy(path, "<i4", {1, 3}, BytesFor<std::int32_t>({1, 2, 3}));

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(
        FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::UnsupportedFormat,
        "integer NPY arrays should be unsupported, not invalid shape");
}

void TestClassifiesEmptyShape()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_empty_flux.npy";
    WriteNpy(path, "<f8", {0, 3}, {});

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::EmptyData, "empty NPY shape should be empty data");
}

void TestLoadsCsvSpectrum()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_wavelength_flux.csv";
    {
        std::ofstream stream(path);
        Require(stream.good(), "could not open CSV test fixture for writing");
        stream << "wav,loglam,flux\n5001,3.1,2\n5000,3.0,1\nnot-a-number,3.2,3\n";
        Require(stream.good(), "could not write CSV test fixture");
    }

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(snapshot->capabilities.can_plot_current_spectrum, "CSV wavelength/flux should be plottable");
    Require(!snapshot->capabilities.has_domain_error, "loaded CSV should not be a domain error snapshot");
    Require(snapshot->current_spectrum.point_count == 2, "invalid CSV rows should be filtered");
    Require(snapshot->current_spectrum.x_values->at(0) == 5000.0, "CSV wav column should not be treated as loglam");
    Require(snapshot->current_spectrum.x_values->at(1) == 5001.0, "CSV wav column should take precedence over loglam");
    Require(snapshot->axis.x_quantity == SpectrumAxisQuantity::Wavelength, "CSV should expose wavelength axis");
    Require(snapshot->axis.y_quantity == SpectrumValueQuantity::Flux, "CSV should expose flux values");
    Require(MetadataValue(snapshot, "source_type") == "csv_spectrum", "CSV source type should come from domain");
    Require(MetadataValue(snapshot, "format") == "csv", "CSV format should come from domain");
    Require(
        HasDiagnosticCode(snapshot, SpectrumDiagnosticCode::NonFiniteValuesFiltered),
        "invalid CSV rows should report filtering");
}

void TestLoadsFitsScalarTableSpectrum()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_sdss_table.fits";
    WriteFitsScalarTable(path);

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(snapshot->capabilities.can_plot_current_spectrum, "FITS scalar table should be plottable");
    Require(snapshot->current_spectrum.point_count == 2, "IVAR zero row should be filtered");
    Require(snapshot->axis.x_quantity == SpectrumAxisQuantity::Wavelength, "FITS table should expose wavelength axis");
    Require(snapshot->axis.y_quantity == SpectrumValueQuantity::Flux, "FITS table should expose flux axis");
    Require(MetadataValue(snapshot, "source_type") == "fits_spectrum", "FITS source type should come from domain");
    Require(MetadataValue(snapshot, "format") == "fits", "FITS format should come from domain");
    Require(HasDiagnosticCode(snapshot, SpectrumDiagnosticCode::IvarFilteredPixels), "FITS table should report IVAR filtering");
}

void TestLoadsFitsVectorTableSpectrum()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_lamost_vector.fits";
    WriteFitsVectorTable(path);

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(snapshot->capabilities.can_plot_current_spectrum, "FITS vector table should be plottable");
    Require(snapshot->current_spectrum.point_count == 2, "ORMASK nonzero pixel should be filtered");
    Require(snapshot->axis.x_quantity == SpectrumAxisQuantity::Wavelength, "FITS vector table should expose wavelength axis");
    Require(MetadataValue(snapshot, "source_type") == "fits_spectrum", "FITS vector source type should come from domain");
    Require(MetadataValue(snapshot, "format") == "fits", "FITS vector format should come from domain");
    Require(HasDiagnosticCode(snapshot, SpectrumDiagnosticCode::MaskFilteredPixels), "FITS vector table should report mask filtering");
}

void TestLoadsLimitedFitsImageSpectrum()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_limited_image.fits";
    WriteFitsImage(path, true);

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(snapshot->capabilities.can_plot_current_spectrum, "limited FITS image should be plottable");
    Require(snapshot->current_spectrum.point_count == 1, "image IVAR and ORMASK rows should filter invalid pixels");
    Require(snapshot->current_spectrum.x_values->at(0) == 1000.0, "image wavelength should use COEFF0/COEFF1");
    Require(snapshot->current_spectrum.y_values->at(0) == 1.0, "image row 0 should provide flux");
    Require(MetadataValue(snapshot, "hdu_type") == "image", "image HDU metadata should be retained");
    Require(
        MetadataValue(snapshot, "valid_pixel_rule") == "ivar_positive_and_ormask_zero",
        "image fallback should record combined IVAR and ORMASK filtering");
    Require(HasDiagnosticCode(snapshot, SpectrumDiagnosticCode::IvarFilteredPixels), "image should report IVAR filtering");
    Require(HasDiagnosticCode(snapshot, SpectrumDiagnosticCode::MaskFilteredPixels), "image should report mask filtering");
}

void TestRejectsFitsImageWcsFallback()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_wcs_image.fits";
    WriteFitsImage(path, false);

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(!snapshot->capabilities.can_plot_current_spectrum, "CRVAL1/CD1_1-only FITS image should not be plottable");
    Require(
        FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::CatalogNotSpectrum,
        "CRVAL1/CD1_1-only image should not be treated as a single spectrum");
}

void TestRejectsMalformedFitsTableWidth()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_bad_table_width.fits";
    WriteMalformedFitsTableWidth(path);

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(!snapshot->capabilities.can_plot_current_spectrum, "malformed FITS table should not be plottable");
    Require(
        FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::InvalidShape,
        "malformed FITS table width should be a domain invalid-shape snapshot");
}

void TestLoadsGzippedFitsSpectrum()
{
    const std::filesystem::path fits_path = std::filesystem::temp_directory_path() / "specforge_loader_gzip_source.fits";
    const std::filesystem::path gzip_path = std::filesystem::temp_directory_path() / "specforge_loader_gzip_source.fits.gz";
    WriteFitsScalarTable(fits_path);
    WriteBytes(gzip_path, GzipBytes(ReadBytes(fits_path)));

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(gzip_path, 0);
    Require(snapshot->capabilities.can_plot_current_spectrum, "gzipped FITS table should be plottable");
    Require(snapshot->current_spectrum.point_count == 2, "gzipped FITS should preserve table filtering behavior");
    Require(MetadataValue(snapshot, "format") == "fits.gz", "gzipped FITS format should come from domain");
}

void TestRejectsCorruptGzippedFits()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_corrupt.fits.gz";
    WriteBytes(path, {'n', 'o', 't', '-', 'g', 'z', 'i', 'p'});

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(!snapshot->capabilities.can_plot_current_spectrum, "corrupt gzip FITS should not be plottable");
    Require(
        FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::OpenFailed,
        "corrupt gzip FITS should be an open/decompression error snapshot");
}

void TestRejectsOversizedInflatedGzippedFits()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_inflated_too_large.fits.gz";
    WriteBytes(path, GzipRepeatedBytes(64ULL * 1024ULL * 1024ULL + 1ULL));

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(!snapshot->capabilities.can_plot_current_spectrum, "over-inflated gzip FITS should not be plottable");
    Require(
        FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::UnsupportedFormat,
        "over-inflated gzip FITS should trip the synchronous inflated-size guard");
}

void TestRejectsOversizedFitsBeforeRead()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_large_catalog.fits";
    {
        std::ofstream stream(path, std::ios::binary);
        Require(stream.good(), "could not open oversized FITS fixture");
        stream.seekp(65LL * 1024LL * 1024LL);
        stream.write("", 1);
        Require(stream.good(), "could not write oversized FITS fixture");
    }

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(!snapshot->capabilities.can_plot_current_spectrum, "oversized FITS should not be plottable");
    Require(
        FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::UnsupportedFormat,
        "oversized FITS should be rejected by the synchronous loader guard");
    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestLoadsFolderCollectionWithWarnings()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_folder_source";
    std::error_code error;
    std::filesystem::remove_all(path, error);
    Require(std::filesystem::create_directory(path), "could not create folder source test fixture");
    Require(std::filesystem::create_directory(path / "nested"), "could not create nested folder source test fixture");
    {
        std::ofstream stream(path / "a.csv");
        Require(stream.good(), "could not open folder CSV fixture");
        stream << "wav,flux\n5000,1\n5001,2\n";
        Require(stream.good(), "could not write folder CSV fixture");
    }
    WriteFitsScalarTable(path / "b.fits");
    {
        std::ofstream stream(path / "notes.txt");
        Require(stream.good(), "could not open ignored folder fixture");
        stream << "not a spectrum\n";
        Require(stream.good(), "could not write ignored folder fixture");
    }

    const SpectrumSnapshotHandle first = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(first->capabilities.can_plot_current_spectrum, "folder CSV/FITS collection should be plottable");
    Require(first->collection.spectrum_count == 2, "folder should count CSV/FITS spectra only");
    Require(first->collection.current_index == 0, "folder should select first spectrum");
    Require(first->collection.can_move_next, "folder should allow next file navigation");
    Require(MetadataValue(first, "source_type") == "folder_collection", "folder collection source type should come from domain");
    Require(MetadataValue(first, "format") == "folder", "folder collection format should come from domain");
    Require(HasDiagnosticCode(first, SpectrumDiagnosticCode::UnsupportedFormat), "folder warnings should be reported");

    const SpectrumSnapshotHandle second = specforge::LoadSpectrumSnapshotFromPath(path, 1);
    Require(second->capabilities.can_plot_current_spectrum, "folder second file should be plottable");
    Require(second->collection.current_index == 1, "folder should select second spectrum");
    Require(second->collection.can_move_previous, "folder should allow previous file navigation");
    Require(second->current_spectrum.point_count == 2, "folder FITS file should preserve selected file loader behavior");

    std::filesystem::remove_all(path, error);
}

void TestEmptyFolderUsesDomainSnapshot()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_empty_folder_source";
    std::error_code error;
    std::filesystem::remove_all(path, error);
    Require(std::filesystem::create_directory(path), "could not create empty folder source test fixture");

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(!snapshot->capabilities.can_plot_current_spectrum, "empty folder source should not be plottable");
    Require(snapshot->capabilities.has_domain_error, "empty folder source should be a domain error snapshot");
    Require(FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::UnsupportedFormat, "empty folder should be unsupported");
    Require(snapshot->diagnostics.front().severity == SpectrumDiagnosticSeverity::Error, "empty folder diagnostic should be an error");
    Require(MetadataValue(snapshot, "source_type") == "folder", "empty folder source type should come from domain");

    std::filesystem::remove_all(path, error);
}

void TestOptionalSampleDirectory()
{
    const std::optional<std::string> sample_directory = EnvironmentVariable("SPECFORGE_SAMPLE_DIR");
    if (!sample_directory) {
        return;
    }

    const std::filesystem::path root(*sample_directory);
    Require(std::filesystem::exists(root), "SPECFORGE_SAMPLE_DIR does not exist");

    std::size_t checked_count = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const std::filesystem::path path = entry.path();
        const std::string extension = LowerAscii(PathToUtf8(path.extension()));
        const bool is_npy = extension == ".npy";
        const bool is_csv = extension == ".csv";
        const bool is_fits = IsFitsSample(path);
        if (!is_npy && !is_csv && !is_fits) {
            continue;
        }

        const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
        const std::string path_text = PathToUtf8(path);
        if (is_npy && IsAuxiliaryNpySample(path)) {
            Require(
                !snapshot->capabilities.can_plot_current_spectrum,
                "auxiliary sample NPY should not be plottable: " + path_text);
        } else if (is_fits && !snapshot->capabilities.can_plot_current_spectrum) {
            Require(snapshot->capabilities.has_domain_error, "non-plottable FITS should be a domain error: " + path_text);
            Require(
                FirstDiagnosticCode(snapshot) == SpectrumDiagnosticCode::CatalogNotSpectrum,
                "sample catalog FITS should be allowed as a catalog error snapshot: " + path_text);
        } else {
            Require(snapshot->capabilities.can_plot_current_spectrum, "sample should be plottable: " + path_text);
            Require(snapshot->current_spectrum.point_count > 0, "sample should have plotted points: " + path_text);
        }
        ++checked_count;
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory() || !DirectoryContainsCsvOrFitsSamples(entry.path())) {
            continue;
        }

        const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(entry.path(), 0);
        const std::string path_text = PathToUtf8(entry.path());
        Require(snapshot->capabilities.can_plot_current_spectrum, "sample folder should be plottable: " + path_text);
        Require(snapshot->collection.spectrum_count > 0, "sample folder should expose a collection: " + path_text);
        ++checked_count;
    }

    Require(checked_count > 0, "SPECFORGE_SAMPLE_DIR did not contain supported sample files");
}

}  // namespace

int main()
{
    TestLoadsSelectedNpyRow();
    TestRejectsAuxiliaryNpyArrays();
    TestClassifiesUnsupportedDtype();
    TestClassifiesEmptyShape();
    TestLoadsCsvSpectrum();
    TestLoadsFitsScalarTableSpectrum();
    TestLoadsFitsVectorTableSpectrum();
    TestLoadsLimitedFitsImageSpectrum();
    TestRejectsFitsImageWcsFallback();
    TestRejectsMalformedFitsTableWidth();
    TestLoadsGzippedFitsSpectrum();
    TestRejectsCorruptGzippedFits();
    TestRejectsOversizedInflatedGzippedFits();
    TestRejectsOversizedFitsBeforeRead();
    TestLoadsFolderCollectionWithWarnings();
    TestEmptyFolderUsesDomainSnapshot();
    TestOptionalSampleDirectory();
    return 0;
}
