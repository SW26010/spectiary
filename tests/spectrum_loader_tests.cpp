#include "domain/spectrum_loader.h"
#include "domain/fits_file_reader.h"
#include "domain/npy_array_io.h"
#include "domain/sample_annotation_io.h"
#include "domain/source_collection_manifest.h"
#include "domain/source_collection_identity_digest.h"
#include "domain/stable_sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
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
using specforge::SampleAnnotationKind;

constexpr double kSpeedOfLightKmPerSecond = 299792.458;

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
                                FitsCard("RV", "               -42.5"),
                                FitsCard("Z", "              0.0123"),
                                FitsCard("ZWARNING", "                   0"),
                                FitsCard("HELIO_RV", "               15.25"),
                                FitsCard("HELIO", "                   T"),
                                FitsCard("VACUUM", "                   T"),
                                FitsCard("CLASS", "'STAR'"),
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
                                FitsCard("NAXIS1", "                  40"),
                                FitsCard("NAXIS2", "                   1"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   4"),
                                FitsCard("TTYPE1", "'WAVELENGTH'"),
                                FitsCard("TFORM1", "'3E'"),
                                FitsCard("TTYPE2", "'FLUX'"),
                                FitsCard("TFORM2", "'3E'"),
                                FitsCard("TTYPE3", "'ORMASK12'"),
                                FitsCard("TFORM3", "'3E'"),
                                FitsCard("TTYPE4", "'RV'"),
                                FitsCard("TFORM4", "'E'"),
                                FitsCard("VACUUM", "                   T"),
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
    AppendBigEndianFloat(data, 124.5F);
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write vector FITS fixture");
}

void WriteFitsInvalidRedshiftTable(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open invalid-redshift FITS fixture");
    WriteFitsPrimary(stream);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                   8"),
                                FitsCard("NAXIS2", "                   2"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   2"),
                                FitsCard("TTYPE1", "'flux'"),
                                FitsCard("TFORM1", "'E'"),
                                FitsCard("TTYPE2", "'loglam'"),
                                FitsCard("TFORM2", "'E'"),
                                FitsCard("Z", "             -9999.0"),
                                FitsCard("ZWARNING", "                  64"),
                                FitsCard("VACUUM", "                   T"),
                            });

    std::vector<unsigned char> data;
    for (const std::array<float, 2> row : {
             std::array<float, 2>{10.0F, 3.0F},
             std::array<float, 2>{20.0F, 3.1F},
         }) {
        AppendBigEndianFloat(data, row[0]);
        AppendBigEndianFloat(data, row[1]);
    }
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write invalid-redshift FITS fixture");
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

std::string NpyHeaderBytes(
    unsigned char major_version,
    std::uint32_t declared_header_length,
    std::string_view header_text)
{
    Require(major_version >= 1 && major_version <= 3, "test NPY version must be v1, v2, or v3");

    constexpr std::array<unsigned char, 6> kMagic = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    std::string bytes(reinterpret_cast<const char*>(kMagic.data()), kMagic.size());
    bytes.push_back(static_cast<char>(major_version));
    bytes.push_back(0);

    const std::size_t length_byte_count = major_version == 1 ? 2 : 4;
    if (major_version == 1) {
        Require(
            declared_header_length <= std::numeric_limits<std::uint16_t>::max(),
            "test NPY v1 header length must fit in uint16");
    }
    for (std::size_t index = 0; index < length_byte_count; ++index) {
        bytes.push_back(static_cast<char>((declared_header_length >> (index * 8U)) & 0xffU));
    }
    bytes.append(header_text);
    return bytes;
}

std::string NpyHeaderBytes(unsigned char major_version, std::string header_text)
{
    const std::size_t preamble_size = major_version == 1 ? 10U : 12U;
    constexpr std::size_t kHeaderAlignment = 64;
    const std::size_t header_with_newline_size = header_text.size() + 1U;
    const std::size_t padding =
        (kHeaderAlignment - ((preamble_size + header_with_newline_size) % kHeaderAlignment)) %
        kHeaderAlignment;
    header_text.append(padding, ' ');
    header_text.push_back('\n');

    return NpyHeaderBytes(
        major_version,
        static_cast<std::uint32_t>(header_text.size()),
        header_text);
}

void TestReadsNpyV1V2V3Headers()
{
    const std::string header_text =
        "{'descr': '<f8', 'fortran_order': False, 'shape': (2, 3), }";

    for (const unsigned char major_version : std::array<unsigned char, 3>{1, 2, 3}) {
        const std::string bytes = NpyHeaderBytes(major_version, header_text);
        std::istringstream stream(bytes, std::ios::in | std::ios::binary);
        const specforge::NpyHeader header = specforge::ReadNpyHeader(stream);

        Require(header.descr == "<f8", "NPY header dtype should be preserved");
        Require(header.shape == std::vector<std::size_t>({2, 3}), "NPY header shape should be preserved");
        Require(
            header.data_offset == bytes.size(),
            "NPY data offset should account for the padded header");
        Require(header.data_offset % 64U == 0, "NPY header fixture should end on a 64-byte boundary");
        Require(bytes.back() == '\n', "NPY header fixture should retain its trailing newline");
    }
}

void TestRejectsOversizedNpyV2V3Headers()
{
    for (const unsigned char major_version : std::array<unsigned char, 2>{2, 3}) {
        const std::string bytes =
            NpyHeaderBytes(major_version, std::numeric_limits<std::uint32_t>::max(), {});
        std::istringstream stream(bytes, std::ios::in | std::ios::binary);

        bool rejected = false;
        try {
            static_cast<void>(specforge::ReadNpyHeader(stream));
        } catch (const specforge::NpyArrayError& error) {
            rejected = true;
            Require(
                error.kind() == specforge::NpyArrayErrorKind::InvalidShape,
                "oversized NPY header should use the invalid-shape error path");
            Require(
                std::string_view(error.what()).find("1 MiB limit") != std::string_view::npos,
                "oversized NPY header should explain the resource limit");
        }
        Require(rejected, "oversized NPY header must be rejected");
    }
}

void TestRejectsNpyHeaderLongerThanRemainingInput()
{
    constexpr std::uint32_t kDeclaredHeaderLength = 1024;
    for (const unsigned char major_version : std::array<unsigned char, 3>{1, 2, 3}) {
        const std::string bytes = NpyHeaderBytes(major_version, kDeclaredHeaderLength, "{'descr': '<f8'");
        std::istringstream stream(bytes, std::ios::in | std::ios::binary);

        bool rejected = false;
        try {
            static_cast<void>(specforge::ReadNpyHeader(stream));
        } catch (const specforge::NpyArrayError& error) {
            rejected = true;
            Require(
                error.kind() == specforge::NpyArrayErrorKind::InvalidShape,
                "truncated NPY header should use the invalid-shape error path");
            Require(
                std::string_view(error.what()) == "NPY header is truncated",
                "truncated NPY header should preserve the diagnostic message");
        }
        Require(rejected, "NPY header longer than the remaining input must be rejected");
    }
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

std::string_view DiagnosticMetadataValue(
    const specforge::SpectrumDiagnostic& diagnostic,
    std::string_view key)
{
    for (const specforge::SpectrumMetadataEntry& entry : diagnostic.metadata) {
        if (entry.key == key) {
            return entry.value;
        }
    }
    return {};
}

double MetadataDouble(const SpectrumSnapshotHandle& snapshot, std::string_view key)
{
    const std::string_view value = MetadataValue(snapshot, key);
    Require(!value.empty(), "expected numeric metadata value");
    return std::stod(std::string(value));
}

bool NearlyEqual(double left, double right, double tolerance)
{
    return std::abs(left - right) <= tolerance;
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

void TestLoadsNpySampleAnnotationContext()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_annotation_context.npy";
    const std::filesystem::path name_path = std::filesystem::temp_directory_path() / "specforge_annotation_context_name.npy";
    const std::filesystem::path annotation_path = std::filesystem::temp_directory_path() / "specforge_annotation_context_y.npy";
    WriteNpy(path, "<f8", {2, 3}, BytesFor<double>({1.0, 2.0, 3.0, 4.0, 5.0, 6.0}));
    WriteNpy(name_path, "<U5", {2}, UnicodeNpyBytesFor({"alpha", "beta"}, 5));
    WriteNpy(annotation_path, "<i4", {2}, BytesFor<std::int32_t>({7, -1}));

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 1);
    Require(snapshot->current_spectrum.name == "beta", "plain NPY source should use same-prefix sample name");
    const specforge::SourceCollectionIdentity identity = specforge::BuildSourceCollectionIdentity(*snapshot);
    Require(
        identity.source_name == PathToUtf8(path.filename()),
        "sample collection identity should use the source filename as its display name");
    Require(
        identity.id.find(PathToUtf8(path.parent_path())) == std::string::npos,
        "sample collection identity id should not include the absolute source directory");
    const specforge::SourceCollectionManifest context = specforge::LoadSourceCollectionManifest(*snapshot);
    Require(context.sample_names.size() == 2, "sample context should load companion sample names");
    Require(context.sample_names[0] == "alpha", "first sample name should be decoded");
    Require(context.sample_names[1] == "beta", "second sample name should be decoded");
    Require(context.annotations.size() == 1, "sample context should auto-load same-prefix y annotation");
    Require(context.annotations[0].kind == SampleAnnotationKind::CategoricalInteger, "integer y should be categorical");
    Require(context.annotations[0].values.size() == 2, "annotation should carry one value per source sample");
    Require(
        specforge::FormatSampleAnnotationValue(
            context.annotations[0],
            context.annotations[0].values[0]) == "7",
        "integer annotation should display raw code");
    Require(
        specforge::FormatSampleAnnotationValue(
            context.annotations[0],
            context.annotations[0].values[1]) == "-1",
        "integer annotation should display unlabeled sentinel raw");
}

void TestLoadsReadOnlyAnnotationDtypes()
{
    const std::filesystem::path float_path = std::filesystem::temp_directory_path() / "specforge_annotation_float_X.npy";
    const std::filesystem::path float_annotation_path =
        std::filesystem::temp_directory_path() / "specforge_annotation_float_y.npy";
    WriteNpy(float_path, "<f8", {2, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0}));
    WriteNpy(float_annotation_path, "<f4", {2}, BytesFor<float>({1.25F, -2.5F}));

    const SpectrumSnapshotHandle float_snapshot = specforge::LoadSpectrumSnapshotFromPath(float_path, 0);
    const specforge::SourceCollectionManifest float_context = specforge::LoadSourceCollectionManifest(*float_snapshot);
    Require(float_context.annotations.size() == 1, "float y annotation should be loaded");
    Require(
        float_context.annotations[0].kind == SampleAnnotationKind::ContinuousFloat,
        "floating-point y should be continuous");
    Require(
        specforge::FormatSampleAnnotationValue(
            float_context.annotations[0],
            float_context.annotations[0].values[1]) == "-2.5",
        "float annotation should display raw value");

    const std::filesystem::path string_path = std::filesystem::temp_directory_path() / "specforge_annotation_string_X.npy";
    const std::filesystem::path string_annotation_path =
        std::filesystem::temp_directory_path() / "specforge_annotation_string_y.npy";
    WriteNpy(string_path, "<f8", {2, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0}));
    WriteNpy(string_annotation_path, "<U4", {2}, UnicodeNpyBytesFor({"good", "bad"}, 4));

    const SpectrumSnapshotHandle string_snapshot = specforge::LoadSpectrumSnapshotFromPath(string_path, 0);
    const specforge::SourceCollectionManifest string_context = specforge::LoadSourceCollectionManifest(*string_snapshot);
    Require(string_context.annotations.size() == 1, "string y annotation should be loaded");
    Require(string_context.annotations[0].kind == SampleAnnotationKind::Text, "string y should stay read-only text");
    Require(
        specforge::FormatSampleAnnotationValue(
            string_context.annotations[0],
            string_context.annotations[0].values[0]) == "good",
        "string annotation should be decoded");
    Require(
        specforge::FormatSampleAnnotationValue(
            string_context.annotations[0],
            string_context.annotations[0].values[1]) == "bad",
        "string annotation should be decoded");
}

void TestAnnotationAdapterPreservesWideNumericSemantics()
{
    const std::filesystem::path signed_path =
        std::filesystem::temp_directory_path() / "specforge_annotation_adapter_i8.npy";
    const std::filesystem::path unsigned_path =
        std::filesystem::temp_directory_path() / "specforge_annotation_adapter_u8.npy";
    const std::filesystem::path floating_path =
        std::filesystem::temp_directory_path() / "specforge_annotation_adapter_f8.npy";

    WriteNpy(
        signed_path,
        "<i8",
        {2},
        BytesFor<std::int64_t>({
            std::numeric_limits<std::int64_t>::max() - 1,
            std::numeric_limits<std::int64_t>::max(),
        }));
    WriteNpy(
        unsigned_path,
        "<u8",
        {2},
        BytesFor<std::uint64_t>({
            std::numeric_limits<std::uint64_t>::max() - 1,
            std::numeric_limits<std::uint64_t>::max(),
        }));
    const double adjacent = std::nextafter(1.0, 2.0);
    WriteNpy(floating_path, "<f8", {2}, BytesFor<double>({1.0, adjacent}));

    const specforge::SampleAnnotationIoAdapter adapter;
    std::string error;
    const std::optional<specforge::SampleAnnotationResult> signed_annotation =
        adapter.Load(signed_path, 2, &error);
    Require(signed_annotation.has_value(), error.empty() ? "int64 annotation should load" : error);
    Require(
        std::get<std::int64_t>(signed_annotation->values[1].semantic) ==
            std::numeric_limits<std::int64_t>::max(),
        "int64 annotation should preserve values above exact-double range");

    const std::optional<specforge::SampleAnnotationResult> unsigned_annotation =
        adapter.Load(unsigned_path, 2, &error);
    Require(unsigned_annotation.has_value(), error.empty() ? "uint64 annotation should load" : error);
    Require(
        std::get<std::uint64_t>(unsigned_annotation->values[1].semantic) ==
            std::numeric_limits<std::uint64_t>::max(),
        "uint64 annotation should preserve its full range");

    const std::optional<specforge::SampleAnnotationResult> floating_annotation =
        adapter.Load(floating_path, 2, &error);
    Require(floating_annotation.has_value(), error.empty() ? "float64 annotation should load" : error);
    Require(
        std::get<double>(floating_annotation->values[1].semantic) == adjacent,
        "float64 annotation should retain the original adjacent value");
    Require(
        specforge::FormatSampleAnnotationValue(
            *floating_annotation,
            floating_annotation->values[0]) !=
            specforge::FormatSampleAnnotationValue(
                *floating_annotation,
                floating_annotation->values[1]),
        "adjacent float64 values should project to distinct round-trip text");

    std::error_code cleanup_error;
    std::filesystem::remove(signed_path, cleanup_error);
    std::filesystem::remove(unsigned_path, cleanup_error);
    std::filesystem::remove(floating_path, cleanup_error);
}

void TestAnnotationAdapterRejectsLabelShapeAndDtypeMismatch()
{
    const std::filesystem::path shape_path =
        std::filesystem::temp_directory_path() / "specforge_annotation_adapter_bad_shape.npy";
    const std::filesystem::path dtype_path =
        std::filesystem::temp_directory_path() / "specforge_annotation_adapter_bad_dtype.npy";
    WriteNpy(shape_path, "<i4", {2}, BytesFor<std::int32_t>({1, 2}));
    WriteNpy(dtype_path, "<f8", {2}, BytesFor<double>({1.0, 2.0}));

    const specforge::SampleAnnotationIoAdapter adapter;
    std::string error;
    Require(
        !adapter.LoadLabelResult(shape_path, 3, {}, &error).has_value(),
        "label adapter should reject a shape mismatch");
    Require(
        error.find("length") != std::string::npos,
        "shape mismatch should explain the source-count contract");

    error.clear();
    Require(
        !adapter.LoadLabelResult(dtype_path, 2, {}, &error).has_value(),
        "label adapter should reject a dtype mismatch");
    Require(
        error.find("int32") != std::string::npos,
        "dtype mismatch should explain the int32 label contract");

    std::error_code cleanup_error;
    std::filesystem::remove(shape_path, cleanup_error);
    std::filesystem::remove(dtype_path, cleanup_error);
}

void TestRejectsMismatchedSampleAnnotationLength()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_annotation_mismatch_X.npy";
    const std::filesystem::path annotation_path = std::filesystem::temp_directory_path() / "specforge_annotation_mismatch_y.npy";
    WriteNpy(path, "<f8", {2, 2}, BytesFor<double>({1.0, 2.0, 3.0, 4.0}));
    WriteNpy(annotation_path, "<i4", {1}, BytesFor<std::int32_t>({1}));

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    const specforge::SourceCollectionManifest context = specforge::LoadSourceCollectionManifest(*snapshot);
    Require(context.annotations.empty(), "mismatched annotation length must not attach to the source collection");
    Require(
        context.diagnostics.size() == 1,
        "mismatched annotation length should produce one structured diagnostic");
    Require(
        context.diagnostics.front().kind ==
                specforge::
                    SourceCollectionManifestDiagnosticKind::
                        AnnotationIgnored &&
            context.diagnostics.front().path ==
                annotation_path &&
            context.diagnostics.front().detail.find(
                "length") != std::string::npos,
        "mismatched annotation diagnostics should preserve kind, path, and technical detail");
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
    Require(MetadataValue(snapshot, "radial_velocity_km_s") == "-42.5", "FITS header RV should be retained");
    Require(MetadataValue(snapshot, "radial_velocity_source") == "header:RV", "FITS header RV source should be retained");
    Require(MetadataValue(snapshot, "redshift") == "0.0123", "FITS header redshift should be retained");
    Require(
        MetadataValue(snapshot, "heliocentric_correction_km_s") == "15.25",
        "FITS heliocentric correction should stay distinct from object RV");
    Require(MetadataValue(snapshot, "wavelength_medium") == "vacuum", "FITS VACUUM header should be normalized");
    Require(
        MetadataValue(snapshot, "observer_frame_correction") == "heliocentric",
        "FITS HELIO_RV should record the observer-frame correction");
    Require(
        NearlyEqual(MetadataDouble(snapshot, "target_redshift"), -42.5 / kSpeedOfLightKmPerSecond, 1.0e-15),
        "stellar target redshift should be derived from RV");
    Require(
        MetadataValue(snapshot, "target_redshift_source") == "radial_velocity_low_speed",
        "stellar target redshift should identify the low-speed RV approximation");
    Require(MetadataValue(snapshot, "target_redshift_status") == "available", "stellar target redshift should be available");
    Require(
        MetadataValue(snapshot, "target_rest_frame_status") == "available_not_applied",
        "available target rest-frame input should not imply an applied correction");
    Require(
        MetadataValue(snapshot, "rest_frame_correction_status") == "not_applied",
        "FITS reader should record that rest-frame correction was not applied");
    Require(snapshot->capabilities.requires_rest_frame_warning, "FITS metadata alone should not clear rest-frame warning");
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
    Require(
        MetadataValue(snapshot, "radial_velocity_km_s") == "124.5",
        "FITS vector table scalar RV column should be retained");
    Require(
        MetadataValue(snapshot, "radial_velocity_source") == "table_column:RV",
        "FITS vector table scalar RV source should be retained");
    Require(
        NearlyEqual(MetadataDouble(snapshot, "target_redshift"), 124.5 / kSpeedOfLightKmPerSecond, 1.0e-15),
        "FITS vector table target redshift should be derived from scalar RV");
    Require(HasDiagnosticCode(snapshot, SpectrumDiagnosticCode::MaskFilteredPixels), "FITS vector table should report mask filtering");
}

void TestBlocksInvalidFitsRedshiftForRestFrameInput()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "specforge_loader_invalid_redshift.fits";
    WriteFitsInvalidRedshiftTable(path);

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(snapshot->capabilities.can_plot_current_spectrum, "invalid-redshift FITS should still be plottable");
    Require(MetadataValue(snapshot, "redshift") == "-9999.0", "raw invalid FITS redshift should be visible");
    Require(MetadataValue(snapshot, "redshift_warning") == "64", "raw FITS ZWARNING should be visible");
    Require(MetadataValue(snapshot, "target_redshift").empty(), "invalid FITS redshift must not become target redshift");
    Require(
        MetadataValue(snapshot, "target_redshift_status") == "invalid",
        "invalid FITS redshift should be marked invalid");
    Require(
        MetadataValue(snapshot, "target_redshift_warning") == "invalid_pipeline_redshift",
        "invalid FITS redshift should explain why it was blocked");
    Require(
        MetadataValue(snapshot, "target_rest_frame_status") == "unavailable",
        "invalid FITS redshift should not enable target rest-frame correction");
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

void WriteFitsScalarTableWithTrailingEmptyHdus(
    const std::filesystem::path& path,
    std::size_t trailing_hdu_count)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open multi-HDU FITS fixture");
    WriteFitsPrimary(stream);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                   8"),
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
    for (std::size_t index = 0; index < trailing_hdu_count; ++index) {
        WriteFitsPrimary(stream);
    }
    Require(stream.good(), "could not write multi-HDU FITS fixture");
}

void WriteLargeFitsLoglamVectorTable(const std::filesystem::path& path, std::size_t value_count)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open large vector FITS fixture");
    WriteFitsPrimary(stream);
    const std::size_t row_width = value_count * 2U * sizeof(float);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", std::to_string(row_width)),
                                FitsCard("NAXIS2", "                   1"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   2"),
                                FitsCard("TTYPE1", "'LOGLAM'"),
                                FitsCard("TFORM1", "'" + std::to_string(value_count) + "E'"),
                                FitsCard("TTYPE2", "'FLUX'"),
                                FitsCard("TFORM2", "'" + std::to_string(value_count) + "E'"),
                            });
    std::vector<unsigned char> data;
    data.reserve(row_width);
    for (std::size_t index = 0; index < value_count; ++index) {
        AppendBigEndianFloat(data, 3.0F + static_cast<float>(index) * 0.000001F);
    }
    for (std::size_t index = 0; index < value_count; ++index) {
        AppendBigEndianFloat(data, static_cast<float>(index));
    }
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write large vector FITS fixture");
}

void TestCancelableCsvLoadStopsInsideParsingAndSorting()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_loader_cancelable.csv";
    {
        std::ofstream stream(path);
        Require(stream.good(), "could not create cancelable CSV fixture");
        stream << "wavelength,flux\n";
        for (std::size_t index = 0; index < 20'000; ++index) {
            stream << (5000 + index) << ',' << index << '\n';
        }
    }

    std::size_t cancellation_checks = 0;
    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPathCancelable(
        path,
        0,
        [&cancellation_checks]() { return ++cancellation_checks >= 8; });
    Require(snapshot == nullptr, "cancelable CSV loading should stop without publishing a snapshot");
    Require(cancellation_checks >= 8, "CSV loading should poll cancellation throughout parsing");

    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestNpyTypedValueConversionPollsCancellation()
{
    constexpr std::size_t kColumnCount = 262'144;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_loader_cancel_npy_conversion.npy";
    WriteNpy(path, "<f4", {1, kColumnCount}, BytesFor(std::vector<float>(kColumnCount, 1.0F)));

    std::size_t cancellation_checks = 0;
    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPathCancelable(
        path,
        0,
        [&cancellation_checks]() {
            ++cancellation_checks;
            return false;
        });
    Require(snapshot && snapshot->capabilities.can_plot_current_spectrum, "wide NPY fixture should load");

    constexpr std::size_t kCheckpointStride = 4096;
    const std::size_t linear_work_blocks = (kColumnCount + kCheckpointStride - 1) / kCheckpointStride;
    Require(
        cancellation_checks >= linear_work_blocks * 3,
        "NPY read, typed conversion, and plot-vector transforms should each poll cancellation");

    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestFolderSortingPollsCancellation()
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("specforge_loader_cancel_folder_sort_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    Require(std::filesystem::create_directory(path), "could not create folder sort fixture");
    constexpr std::size_t kFileCount = 64;
    for (std::size_t index = 0; index < kFileCount; ++index) {
        std::ofstream stream(path / ("sample-" + std::to_string(kFileCount - index) + ".csv"));
        stream << "fixture";
    }

    class FolderSortCancellationMarker final : public std::runtime_error {
    public:
        FolderSortCancellationMarker()
            : std::runtime_error("folder sort cancellation marker")
        {
        }
    };

    std::size_t processed_entries = 0;
    bool canceled_in_sort = false;
    try {
        (void)specforge::ScanSourceCollectionFolder(
            path,
            [&processed_entries](std::size_t processed) {
                processed_entries = processed;
            },
            [&processed_entries]() {
                if (processed_entries == kFileCount) {
                    throw FolderSortCancellationMarker();
                }
            });
    } catch (const FolderSortCancellationMarker&) {
        canceled_in_sort = true;
    }
    std::filesystem::remove_all(path, error);
    Require(processed_entries == kFileCount, "folder cancellation should arm only after enumeration");
    Require(canceled_in_sort, "folder filename sorting should poll cooperative cancellation");
}

void TestCancelableAnnotationLoadStopsInsidePayloadConversion()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_loader_cancelable_annotation.npy";
    constexpr std::size_t kValueCount = 600'000;
    WriteNpy(path, "<i4", {kValueCount}, BytesFor(std::vector<std::int32_t>(kValueCount, 7)));

    class CancellationMarker final : public std::runtime_error {
    public:
        CancellationMarker()
            : std::runtime_error("annotation cancellation marker")
        {
        }
    };

    std::size_t cancellation_checks = 0;
    bool canceled = false;
    try {
        (void)specforge::SampleAnnotationIoAdapter{}.LoadCancelable(
            path,
            kValueCount,
            [&cancellation_checks]() {
                if (++cancellation_checks >= 4) {
                    throw CancellationMarker();
                }
            });
    } catch (const CancellationMarker&) {
        canceled = true;
    }
    Require(canceled, "annotation cancellation should propagate to the queue boundary");
    Require(cancellation_checks >= 4, "annotation loading should poll beyond initial dispatch");

    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestCancelableAnnotationLoadStopsInsideMetadataPairing()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_loader_cancelable_annotation_metadata.npy";
    constexpr std::size_t kValueCount = 600'000;
    WriteNpy(path, "<i4", {kValueCount}, BytesFor(std::vector<std::int32_t>(kValueCount, 7)));

    specforge::SampleLabelingTask task =
        specforge::CreateSampleLabelingTask("metadata-cancel", "Metadata cancel", kValueCount);
    Require(
        specforge::UpsertSampleLabel(
            task.label_set,
            specforge::SampleLabelDefinition{7, "accepted", 'a'}),
        "metadata cancellation fixture label should be valid");
    std::string metadata_error;
    Require(
        specforge::SampleAnnotationIoAdapter{}.SaveLabelMetadata(
            path,
            task,
            nullptr,
            &metadata_error),
        metadata_error.empty() ? "metadata cancellation fixture should save" : metadata_error);

    class CancellationMarker final : public std::runtime_error {
    public:
        CancellationMarker()
            : std::runtime_error("annotation metadata cancellation marker")
        {
        }
    };

    constexpr std::size_t kReadChunkValues = (1024U * 1024U) / sizeof(std::int32_t);
    constexpr std::size_t kCheckpointStride = 4096;
    constexpr std::size_t kReadChunks = (kValueCount + kReadChunkValues - 1) / kReadChunkValues;
    constexpr std::size_t kPayloadConversionChecks =
        (kValueCount + kCheckpointStride - 1) / kCheckpointStride;
    constexpr std::size_t kFirstMetadataFormattingCheck =
        1 + kReadChunks + kPayloadConversionChecks + 2 + 1;
    std::size_t cancellation_checks = 0;
    bool canceled = false;
    try {
        (void)specforge::SampleAnnotationIoAdapter{}.LoadCancelable(
            path,
            kValueCount,
            [&cancellation_checks]() {
                if (++cancellation_checks >= kFirstMetadataFormattingCheck) {
                    throw CancellationMarker();
                }
            });
    } catch (const CancellationMarker&) {
        canceled = true;
    }
    Require(canceled, "annotation label metadata pairing should honor cooperative cancellation");
    Require(
        cancellation_checks >= kFirstMetadataFormattingCheck,
        "metadata cancellation should occur after payload conversion has completed");

    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(path),
        error);
}

void TestSharedAnnotationIngestionPreservesMetadataWarningsWithoutDuplicates()
{
    const std::filesystem::path annotation_path =
        std::filesystem::temp_directory_path() / "specforge_annotation_ingestion_warning.npy";
    WriteNpy(annotation_path, "<i4", {2}, BytesFor<std::int32_t>({1, 2}));
    {
        std::ofstream metadata(
            specforge::SampleAnnotationIoAdapter::MetadataPathForResult(annotation_path),
            std::ios::trunc);
        metadata << "{invalid-json";
    }

    specforge::SourceCollectionManifest manifest;
    Require(
        specforge::IngestReadOnlySampleAnnotation(manifest, annotation_path, 2),
        "shared annotation ingestion should retain the usable value array");
    Require(manifest.annotations.size() == 1, "shared ingestion should append one annotation");
    Require(
        manifest.diagnostics.size() == 1,
        "shared ingestion should preserve one structured metadata warning");
    Require(
        manifest.diagnostics.front().kind ==
                specforge::
                    SourceCollectionManifestDiagnosticKind::
                        AnnotationMetadataIgnored &&
            manifest.diagnostics.front().path ==
                specforge::SampleAnnotationIoAdapter::
                    MetadataPathForResult(
                        annotation_path) &&
            !manifest.diagnostics.front().detail.empty(),
        "metadata diagnostics should preserve kind, path, and technical detail");
    Require(
        specforge::IngestReadOnlySampleAnnotation(manifest, annotation_path, 2),
        "re-ingesting the same annotation should update it");
    Require(manifest.annotations.size() == 1, "shared ingestion should not duplicate an existing annotation path");
}

void TestCancelableGzippedFitsLoadStopsInsideTheDecoderPipeline()
{
    const std::filesystem::path fits_path =
        std::filesystem::temp_directory_path() / "specforge_loader_cancel_source.fits";
    const std::filesystem::path gzip_path =
        std::filesystem::temp_directory_path() / "specforge_loader_cancel_source.fits.gz";
    WriteFitsScalarTable(fits_path);
    WriteBytes(gzip_path, GzipBytes(ReadBytes(fits_path)));

    std::size_t cancellation_checks = 0;
    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPathCancelable(
        gzip_path,
        0,
        [&cancellation_checks]() {
            ++cancellation_checks;
            return cancellation_checks >= 4;
        });
    Require(snapshot == nullptr, "cancelable FITS.GZ loading should stop without publishing an error snapshot");
    Require(cancellation_checks >= 4, "FITS.GZ loading should poll cancellation beyond initial dispatch");
}

void TestCancelableFitsLoadStopsInsideNumericDecodeAndTransforms()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_loader_cancel_numeric_decode.fits";
    WriteLargeFitsLoglamVectorTable(path, 50'000);

    std::size_t cancellation_checks = 0;
    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPathCancelable(
        path,
        0,
        [&cancellation_checks]() {
            ++cancellation_checks;
            return cancellation_checks >= 36;
        });
    Require(snapshot == nullptr, "FITS cancellation during numeric work must not publish a snapshot");
    Require(
        cancellation_checks >= 36,
        "FITS cancellation should remain wired beyond file read and HDU parsing");

    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestFitsColumnAndImageReadersPollDuringValueConversion()
{
    constexpr std::size_t kValueCount = 20'000;
    std::vector<unsigned char> bytes;
    bytes.reserve(kValueCount * sizeof(float));
    for (std::size_t index = 0; index < kValueCount; ++index) {
        AppendBigEndianFloat(bytes, static_cast<float>(index));
    }

    specforge::detail::FitsHdu table_hdu;
    table_hdu.header.values["NAXIS1"] = std::to_string(bytes.size());
    table_hdu.header.values["NAXIS2"] = "1";
    table_hdu.data_size = bytes.size();
    specforge::detail::FitsColumn column;
    column.repeat = kValueCount;
    column.code = 'E';
    column.element_size = sizeof(float);
    column.byte_width = bytes.size();

    std::size_t column_checks = 0;
    bool column_canceled = false;
    try {
        (void)specforge::detail::ReadFitsColumnVector(
            bytes,
            table_hdu,
            column,
            0,
            false,
            [&column_checks]() { return ++column_checks >= 3; });
    } catch (const specforge::detail::FitsFileError& error) {
        column_canceled = error.code() == specforge::detail::FitsFileErrorCode::Canceled;
    }
    Require(column_canceled, "FITS table value conversion should honor cooperative cancellation");

    specforge::detail::FitsHdu image_hdu;
    image_hdu.header.values["BITPIX"] = "-32";
    image_hdu.data_size = bytes.size();
    std::size_t image_checks = 0;
    bool image_canceled = false;
    try {
        (void)specforge::detail::ReadFitsImageRow(
            bytes,
            image_hdu,
            0,
            kValueCount,
            [&image_checks]() { return ++image_checks >= 3; });
    } catch (const specforge::detail::FitsFileError& error) {
        image_canceled = error.code() == specforge::detail::FitsFileErrorCode::Canceled;
    }
    Require(image_canceled, "FITS image value conversion should honor cooperative cancellation");
}

void TestFitsColumnDiscoveryPollsDuringLargeTfieldsLoop()
{
    constexpr std::size_t kFieldCount = 999;
    std::vector<std::string> cards = {
        FitsCard("XTENSION", "'BINTABLE'"),
        FitsCard("BITPIX", "                   8"),
        FitsCard("NAXIS", "                   2"),
        FitsCard("NAXIS1", "                3996"),
        FitsCard("NAXIS2", "                   1"),
        FitsCard("PCOUNT", "                   0"),
        FitsCard("GCOUNT", "                   1"),
        FitsCard("TFIELDS", "                 999"),
    };
    cards.reserve(cards.size() + kFieldCount * 2);
    for (std::size_t index = 1; index <= kFieldCount; ++index) {
        cards.push_back(FitsCard("TTYPE" + std::to_string(index), "'VALUE'"));
        cards.push_back(FitsCard("TFORM" + std::to_string(index), "'E'"));
    }
    std::string header;
    for (const std::string& card : cards) {
        header += card;
    }
    header += FitsCard("END");
    header.append((2880 - (header.size() % 2880)) % 2880, ' ');
    std::vector<unsigned char> bytes(header.begin(), header.end());
    bytes.resize(bytes.size() + 3996, 0);
    bytes.resize(bytes.size() + ((2880 - (3996 % 2880)) % 2880), 0);

    const std::size_t header_block_count = header.size() / 2880;
    const std::size_t cancel_at = 1 + header_block_count + 2;
    std::size_t cancellation_checks = 0;
    bool canceled = false;
    try {
        (void)specforge::detail::ParseFitsHdus(
            bytes,
            [&cancellation_checks, cancel_at]() { return ++cancellation_checks >= cancel_at; });
    } catch (const specforge::detail::FitsFileError& error) {
        canceled = error.code() == specforge::detail::FitsFileErrorCode::Canceled;
    }
    Require(canceled, "FITS TFIELDS column discovery should honor cancellation inside its column loop");
}

void TestFitsHeaderMetadataScanPollsAcrossManyHdus()
{
    constexpr std::size_t kTrailingHduCount = 3'000;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_loader_cancel_many_hdu_metadata.fits";
    WriteFitsScalarTableWithTrailingEmptyHdus(path, kTrailingHduCount);

    // File read and ParseFitsHdus account for just over two checks per HDU.
    // Arming beyond that boundary targets the repeated header-metadata scans.
    const std::size_t cancel_at = 2 * (kTrailingHduCount + 2) + 100;
    std::size_t cancellation_checks = 0;
    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPathCancelable(
        path,
        0,
        [&cancellation_checks, cancel_at]() { return ++cancellation_checks >= cancel_at; });
    Require(snapshot == nullptr, "FITS metadata scan should stop without publishing a snapshot");
    Require(
        cancellation_checks >= cancel_at,
        "FITS header metadata lookup should keep polling after HDU parsing finishes");

    std::error_code error;
    std::filesystem::remove(path, error);
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
    {
        std::ofstream stream(path / "C.CSV");
        Require(stream.good(), "could not open uppercase folder CSV fixture");
        stream << "wav,flux\n5002,3\n5003,4\n";
        Require(stream.good(), "could not write uppercase folder CSV fixture");
    }
    {
        std::ofstream stream(path / "nested" / "inside.csv");
        Require(stream.good(), "could not open nested folder CSV fixture");
        stream << "wav,flux\n5004,5\n5005,6\n";
        Require(stream.good(), "could not write nested folder CSV fixture");
    }
    WriteFitsScalarTable(path / "b.fits");
    {
        std::ofstream stream(path / "notes.txt");
        Require(stream.good(), "could not open ignored folder fixture");
        stream << "not a spectrum\n";
        Require(stream.good(), "could not write ignored folder fixture");
    }
    {
        std::ofstream stream(path / "matrix.npy");
        Require(stream.good(), "could not open ignored NPY folder fixture");
        stream << "not a folder member\n";
        Require(stream.good(), "could not write ignored NPY folder fixture");
    }

    const SpectrumSnapshotHandle first = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(first->capabilities.can_plot_current_spectrum, "folder CSV/FITS collection should be plottable");
    Require(first->collection.spectrum_count == 3, "folder should count first-level CSV/FITS spectra only");
    Require(first->collection.current_index == 0, "folder should select first spectrum");
    Require(first->collection.can_move_next, "folder should allow next file navigation");
    Require(MetadataValue(first, "source_type") == "folder_collection", "folder collection source type should come from domain");
    Require(MetadataValue(first, "format") == "folder", "folder collection format should come from domain");
    const auto mixed_format_diagnostic = std::find_if(
        first->diagnostics.begin(),
        first->diagnostics.end(),
        [](const specforge::SpectrumDiagnostic& diagnostic) {
            return diagnostic.code ==
                       SpectrumDiagnosticCode::UnsupportedFormat &&
                   diagnostic.message.find(
                       "Folder mixes supported spectrum formats") !=
                       std::string::npos;
        });
    Require(
        mixed_format_diagnostic != first->diagnostics.end(),
        "folder warnings should include a distinct mixed-format diagnostic");
    Require(
        DiagnosticMetadataValue(
            *mixed_format_diagnostic,
            "csv_file_count") == "2" &&
            DiagnosticMetadataValue(
                *mixed_format_diagnostic,
                "fits_file_count") == "1",
        "mixed CSV/FITS diagnostic should retain both format counts");

    const specforge::SourceCollectionFolderListing listing = specforge::ScanSourceCollectionFolder(path);
    Require(
        listing.csv_count == 2 &&
            listing.fits_count == 1 &&
            listing.ignored_directory_count == 1 &&
            listing.ignored_file_count == 2,
        "folder scan should classify supported members, NPY, subfolders, and unrelated files");
    Require(
        std::all_of(listing.spectra.begin(), listing.spectra.end(), [](const auto& sample) {
            return !sample.stat_fingerprint.empty();
        }),
        "folder scan should retain each spectrum file stat fingerprint");
    const specforge::SourceCollectionContext context = specforge::LoadSourceCollectionContext(*first);
    Require(
        specforge::IsVersionedSha256Digest(context.identity.source_fingerprint),
        "folder source fingerprints should have a fixed-size versioned digest");
    std::vector<std::filesystem::path> legacy_fingerprint_paths;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(path)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string extension = LowerAscii(PathToUtf8(entry.path().extension()));
        if (extension == ".csv" || IsFitsSample(entry.path())) {
            legacy_fingerprint_paths.push_back(entry.path());
        }
    }
    std::stable_sort(
        legacy_fingerprint_paths.begin(),
        legacy_fingerprint_paths.end(),
        [](const std::filesystem::path& left, const std::filesystem::path& right) {
            return LowerAscii(PathToUtf8(left.filename())) < LowerAscii(PathToUtf8(right.filename()));
        });
    std::string legacy_fingerprint = "folder";
    for (const std::filesystem::path& sample_path : legacy_fingerprint_paths) {
        legacy_fingerprint += ";" + PathToUtf8(sample_path.filename()) + ":size=" +
                              std::to_string(std::filesystem::file_size(sample_path)) + ";mtime=" +
                              std::to_string(std::filesystem::last_write_time(sample_path).time_since_epoch().count());
    }
    Require(
        context.identity.source_fingerprint == specforge::VersionedSha256Digest(legacy_fingerprint),
        "streamed folder fingerprints must preserve the exact legacy identity semantics");
    const std::string legacy_identity =
        "name=" + PathToUtf8(path.filename()) + "|fingerprint=" + legacy_fingerprint + "|count=3";
    Require(
        context.identity.id == specforge::NormalizePersistedSourceCollectionIdentity(legacy_identity),
        "legacy folder cache identities should migrate to the same fixed-size key");
    Require(context.identity.id.size() == 74, "versioned SHA-256 identities should remain fixed-size");
    Require(context.manifest.sample_names.size() == 3, "source context should build folder sample names in the same pass");
    Require(context.manifest.sample_names[0] == "a.csv", "folder source context should preserve stable filename order");
    Require(context.manifest.sample_names[1] == "b.fits", "folder source context should include FITS sample names");
    Require(context.manifest.sample_names[2] == "C.CSV", "folder source context should retain uppercase CSV in stable order");
    Require(
        specforge::BuildSourceCollectionIdentity(*first).id == context.identity.id,
        "combined source context must preserve the existing folder identity format");

    const SpectrumSnapshotHandle second = specforge::LoadSpectrumSnapshotFromPath(path, 1);
    Require(second->capabilities.can_plot_current_spectrum, "folder second file should be plottable");
    Require(second->collection.current_index == 1, "folder should select second spectrum");
    Require(second->collection.can_move_previous, "folder should allow previous file navigation");
    Require(second->current_spectrum.point_count == 2, "folder FITS file should preserve selected file loader behavior");

    const SpectrumSnapshotHandle third = specforge::LoadSpectrumSnapshotFromPath(path, 2);
    Require(third->capabilities.can_plot_current_spectrum, "folder uppercase CSV should be plottable");
    Require(third->collection.current_index == 2, "folder should select the third spectrum by stable order");

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
    TestReadsNpyV1V2V3Headers();
    TestRejectsOversizedNpyV2V3Headers();
    TestRejectsNpyHeaderLongerThanRemainingInput();
    TestNpyTypedValueConversionPollsCancellation();
    TestFolderSortingPollsCancellation();
    TestLoadsSelectedNpyRow();
    TestLoadsNpySampleAnnotationContext();
    TestLoadsReadOnlyAnnotationDtypes();
    TestAnnotationAdapterPreservesWideNumericSemantics();
    TestAnnotationAdapterRejectsLabelShapeAndDtypeMismatch();
    TestSharedAnnotationIngestionPreservesMetadataWarningsWithoutDuplicates();
    TestRejectsMismatchedSampleAnnotationLength();
    TestRejectsAuxiliaryNpyArrays();
    TestClassifiesUnsupportedDtype();
    TestClassifiesEmptyShape();
    TestLoadsCsvSpectrum();
    TestCancelableCsvLoadStopsInsideParsingAndSorting();
    TestCancelableAnnotationLoadStopsInsidePayloadConversion();
    TestCancelableAnnotationLoadStopsInsideMetadataPairing();
    TestLoadsFitsScalarTableSpectrum();
    TestLoadsFitsVectorTableSpectrum();
    TestBlocksInvalidFitsRedshiftForRestFrameInput();
    TestLoadsLimitedFitsImageSpectrum();
    TestRejectsFitsImageWcsFallback();
    TestRejectsMalformedFitsTableWidth();
    TestLoadsGzippedFitsSpectrum();
    TestCancelableGzippedFitsLoadStopsInsideTheDecoderPipeline();
    TestCancelableFitsLoadStopsInsideNumericDecodeAndTransforms();
    TestFitsColumnAndImageReadersPollDuringValueConversion();
    TestFitsColumnDiscoveryPollsDuringLargeTfieldsLoop();
    TestFitsHeaderMetadataScanPollsAcrossManyHdus();
    TestRejectsCorruptGzippedFits();
    TestRejectsOversizedInflatedGzippedFits();
    TestRejectsOversizedFitsBeforeRead();
    TestLoadsFolderCollectionWithWarnings();
    TestEmptyFolderUsesDomainSnapshot();
    TestOptionalSampleDirectory();
    return 0;
}
