#include "domain/spectrum_loader.h"
#include "domain/fits_file_reader.h"
#include "domain/npy_array_io.h"
#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling_asdf_codec.h"
#include "domain/sample_labeling_document.h"
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
#include <type_traits>
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

static_assert(
    std::is_move_constructible_v<specforge::detail::FitsFile>);
static_assert(
    std::is_move_assignable_v<specforge::detail::FitsFile>);
static_assert(
    !std::is_copy_constructible_v<specforge::detail::FitsFile>);
static_assert(
    !std::is_copy_assignable_v<specforge::detail::FitsFile>);

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

std::string FitsHeaderBytes(const std::vector<std::string>& cards)
{
    std::string header;
    for (const std::string& card : cards) {
        Require(card.size() == 80, "FITS test card must be 80 bytes");
        header += card;
    }
    header += FitsCard("END");
    const std::size_t padding = (2880 - (header.size() % 2880)) % 2880;
    header.append(padding, ' ');
    return header;
}

void WriteFitsHeader(std::ofstream& stream, const std::vector<std::string>& cards)
{
    const std::string header = FitsHeaderBytes(cards);
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

std::vector<unsigned char> GzipBytesWithZeroSuffix(
    const std::vector<unsigned char>& prefix,
    std::size_t zero_byte_count)
{
    z_stream stream = {};
    Require(
        deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK,
        "could not initialize gzip fixture compressor");

    std::array<unsigned char, 64 * 1024> zeros = {};
    std::array<unsigned char, 64 * 1024> buffer = {};
    std::vector<unsigned char> output;
    const auto compress = [&](const unsigned char* input, std::size_t size, int flush) {
        stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input));
        stream.avail_in = static_cast<uInt>(size);
        int result = Z_OK;
        do {
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());
            result = deflate(&stream, flush);
            Require(result == Z_OK || result == Z_STREAM_END, "could not gzip fixture bytes");
            const std::size_t produced = buffer.size() - stream.avail_out;
            output.insert(output.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(produced));
        } while (stream.avail_in > 0 || (flush == Z_FINISH && result != Z_STREAM_END));
    };

    compress(prefix.data(), prefix.size(), Z_NO_FLUSH);
    std::size_t remaining = zero_byte_count;
    while (remaining > 0) {
        const std::size_t chunk = std::min(remaining, zeros.size());
        compress(zeros.data(), chunk, Z_NO_FLUSH);
        remaining -= chunk;
    }
    compress(nullptr, 0, Z_FINISH);

    deflateEnd(&stream);
    return output;
}

std::vector<unsigned char> GzipFitsImageWithPayload(std::size_t payload_size)
{
    const std::string payload_text = std::to_string(payload_size);
    Require(payload_text.size() <= 20, "FITS payload size should fit in a numeric value field");
    const std::string header = FitsHeaderBytes({
        FitsCard("SIMPLE", "                   T"),
        FitsCard("BITPIX", "                   8"),
        FitsCard("NAXIS", "                   1"),
        FitsCard("NAXIS1", std::string(20 - payload_text.size(), ' ') + payload_text),
    });
    const std::size_t padding = (2880 - (payload_size % 2880)) % 2880;
    return GzipBytesWithZeroSuffix(
        std::vector<unsigned char>(header.begin(), header.end()),
        payload_size + padding);
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
                                FitsCard("NAXIS1", "                  44"),
                                FitsCard("NAXIS2", "                   2"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   5"),
                                FitsCard("TTYPE1", "'WAVELENGTH'"),
                                FitsCard("TFORM1", "'3E'"),
                                FitsCard("TTYPE2", "'FLUX'"),
                                FitsCard("TFORM2", "'3E'"),
                                FitsCard("TTYPE3", "'ORMASK'"),
                                FitsCard("TFORM3", "'3E'"),
                                FitsCard("TTYPE4", "'RV'"),
                                FitsCard("TFORM4", "'E'"),
                                FitsCard("TTYPE5", "'Z'"),
                                FitsCard("TFORM5", "'E'"),
                                FitsCard("VACUUM", "                   T"),
                            });

    std::vector<unsigned char> data;
    const auto append_row = [&data](
                                const std::array<float, 3>& wavelength,
                                const std::array<float, 3>& flux,
                                const std::array<float, 3>& mask,
                                float radial_velocity,
                                float redshift) {
        for (float value : wavelength) {
            AppendBigEndianFloat(data, value);
        }
        for (float value : flux) {
            AppendBigEndianFloat(data, value);
        }
        for (float value : mask) {
            AppendBigEndianFloat(data, value);
        }
        AppendBigEndianFloat(data, radial_velocity);
        AppendBigEndianFloat(data, redshift);
    };
    append_row({5000.0F, 5001.0F, 5002.0F}, {1.0F, 2.0F, 3.0F}, {0.0F, 1.0F, 0.0F}, 124.5F, 0.02F);
    append_row({6000.0F, 6001.0F, 6002.0F}, {4.0F, 5.0F, 6.0F}, {1.0F, 0.0F, 0.0F}, -50.0F, 0.03F);
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write vector FITS fixture");
}

void WriteFitsTableWithUndefinedOptionalKeywords(
    const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(
        stream.good(),
        "could not open undefined-keyword FITS table fixture");
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
                                FitsCard("TTYPE1", "'WAVELENGTH'"),
                                FitsCard("TFORM1", "'E'"),
                                FitsCard("TTYPE2", "'FLUX'"),
                                FitsCard("TFORM2", "'E'"),
                                FitsCard("RV"),
                                FitsCard("Z"),
                                FitsCard("ZWARNING"),
                                FitsCard("CLASS"),
                                FitsCard("TELESCOP"),
                                FitsCard("VACUUM"),
                            });

    std::vector<unsigned char> data;
    for (const std::array<float, 2> row : {
             std::array<float, 2>{5000.0F, 1.0F},
             std::array<float, 2>{5001.0F, 2.0F},
         }) {
        AppendBigEndianFloat(data, row[0]);
        AppendBigEndianFloat(data, row[1]);
    }
    stream.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(
        stream.good(),
        "could not write undefined-keyword FITS table fixture");
}

void WriteFitsVectorTableWithTruncatedLaterRow(
    const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(
        stream.good(),
        "could not open truncated vector FITS fixture");
    WriteFitsPrimary(stream);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                  24"),
                                FitsCard("NAXIS2", "                   2"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   2"),
                                FitsCard("TTYPE1", "'WAVELENGTH'"),
                                FitsCard("TFORM1", "'3E'"),
                                FitsCard("TTYPE2", "'FLUX'"),
                                FitsCard("TFORM2", "'3E'"),
                            });

    std::vector<unsigned char> first_row;
    for (float value : {5000.0F, 5001.0F, 5002.0F}) {
        AppendBigEndianFloat(first_row, value);
    }
    for (float value : {1.0F, 2.0F, 3.0F}) {
        AppendBigEndianFloat(first_row, value);
    }
    stream.write(
        reinterpret_cast<const char*>(first_row.data()),
        static_cast<std::streamsize>(first_row.size()));
    Require(
        stream.good(),
        "could not write the selected row of truncated vector FITS fixture");
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
        FitsCard("NAXIS1", "                   4"),
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
    for (float value : {1.0F, 2.0F, 3.0F, 4.0F}) {
        AppendBigEndianFloat(data, value);
    }
    for (float value : {1.0F, 0.0F, 1.0F, 1.0F}) {
        AppendBigEndianFloat(data, value);
    }
    for (float value : {0.0F, 0.0F, 0.0F, 0.0F}) {
        AppendBigEndianFloat(data, value);
    }
    for (float value : {0.0F, 0.0F, 0.0F, 0.0F}) {
        AppendBigEndianFloat(data, value);
    }
    for (float value : {0.0F, 0.0F, 1.0F, 0.0F}) {
        AppendBigEndianFloat(data, value);
    }
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write image FITS fixture");
}

void WriteFitsImageWithUndefinedCoefficients(
    const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(
        stream.good(),
        "could not open undefined-keyword FITS image fixture");
    WriteFitsHeader(stream, {
                                FitsCard("SIMPLE", "                   T"),
                                FitsCard("BITPIX", "                 -32"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                   3"),
                                FitsCard("NAXIS2", "                   2"),
                                FitsCard("COEFF0"),
                                FitsCard("COEFF1"),
                            });

    std::vector<unsigned char> data;
    for (float value : {1.0F, 2.0F, 3.0F, 1.0F, 1.0F, 1.0F}) {
        AppendBigEndianFloat(data, value);
    }
    stream.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(
        stream.good(),
        "could not write undefined-keyword FITS image fixture");
}

void WriteFitsImageWithTruncatedUnselectedTail(
    const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(
        stream.good(),
        "could not open truncated-tail FITS image fixture");
    WriteFitsHeader(stream, {
                                FitsCard("SIMPLE", "                   T"),
                                FitsCard("BITPIX", "                 -32"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                   4"),
                                FitsCard("NAXIS2", "                   4"),
                                FitsCard("COEFF0", "                 3.0"),
                                FitsCard("COEFF1", "               0.001"),
                            });

    std::vector<unsigned char> selected_rows;
    for (float value : {
             1.0F, 2.0F, 3.0F, 4.0F,
             1.0F, 1.0F, 1.0F, 1.0F,
         }) {
        AppendBigEndianFloat(selected_rows, value);
    }
    stream.write(
        reinterpret_cast<const char*>(selected_rows.data()),
        static_cast<std::streamsize>(selected_rows.size()));
    Require(
        stream.good(),
        "could not write selected rows of truncated-tail FITS image fixture");
}

void WriteLargeFitsImage(
    const std::filesystem::path& path,
    std::size_t column_count)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open large FITS image fixture");
    const std::string column_count_text = std::to_string(column_count);
    Require(
        column_count_text.size() <= 20U,
        "large FITS image width should fit a numeric card");
    WriteFitsHeader(stream, {
                                FitsCard("SIMPLE", "                   T"),
                                FitsCard("BITPIX", "                 -32"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard(
                                    "NAXIS1",
                                    std::string(
                                        20U - column_count_text.size(),
                                        ' ') +
                                        column_count_text),
                                FitsCard("NAXIS2", "                   1"),
                                FitsCard("COEFF0", "                 3.0"),
                                FitsCard("COEFF1", "               0.001"),
                            });

    std::vector<unsigned char> data;
    data.reserve(column_count * sizeof(float));
    for (std::size_t index = 0; index < column_count; ++index) {
        AppendBigEndianFloat(data, static_cast<float>(index));
    }
    stream.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write large FITS image fixture");
}

void WriteFitsImageThenScalarTable(const std::filesystem::path& path)
{
    WriteFitsImage(path, true);

    std::ofstream stream(path, std::ios::binary | std::ios::app);
    Require(stream.good(), "could not append table HDU to FITS image fixture");
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                   8"),
                                FitsCard("NAXIS2", "                   2"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   2"),
                                FitsCard("TTYPE1", "'WAVELENGTH'"),
                                FitsCard("TFORM1", "'E'"),
                                FitsCard("TTYPE2", "'FLUX'"),
                                FitsCard("TFORM2", "'E'"),
                            });

    std::vector<unsigned char> data;
    for (const std::array<float, 2> row : {
             std::array<float, 2>{7000.0F, 70.0F},
             std::array<float, 2>{7001.0F, 71.0F},
         }) {
        AppendBigEndianFloat(data, row[0]);
        AppendBigEndianFloat(data, row[1]);
    }
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write table HDU after FITS image");
}

void WriteTruncatedFitsHeader(const std::filesystem::path& path)
{
    const std::string bytes =
        FitsCard("SIMPLE", "                   T") + FitsCard("BITPIX", "                   8");
    WriteBytes(path, std::vector<unsigned char>(bytes.begin(), bytes.end()));
}

void WriteFitsHeaderWithMissingImageData(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open truncated image FITS fixture");
    WriteFitsHeader(stream, {
                                FitsCard("SIMPLE", "                   T"),
                                FitsCard("BITPIX", "                 -32"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                   3"),
                                FitsCard("NAXIS2", "                   2"),
                                FitsCard("COEFF0", "                 3.0"),
                                FitsCard("COEFF1", "               0.001"),
                            });
    Require(stream.good(), "could not write complete header for truncated image FITS fixture");
}

void WriteFitsTableWithDeclaredDataMismatch(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open mismatched table FITS fixture");
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
                                FitsCard("TTYPE1", "'WAVELENGTH'"),
                                FitsCard("TFORM1", "'E'"),
                                FitsCard("TTYPE2", "'FLUX'"),
                                FitsCard("TFORM2", "'E'"),
                            });
    std::vector<unsigned char> partial_row;
    AppendBigEndianFloat(partial_row, 5000.0F);
    AppendBigEndianFloat(partial_row, 1.0F);
    stream.write(
        reinterpret_cast<const char*>(partial_row.data()),
        static_cast<std::streamsize>(partial_row.size()));
    Require(stream.good(), "could not write partial table data for mismatched FITS fixture");
}

void WriteFitsTableWithUnsupportedColumnLayout(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open unsupported-column FITS fixture");
    WriteFitsPrimary(stream);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                  16"),
                                FitsCard("NAXIS2", "                   1"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   2"),
                                FitsCard("TTYPE1", "'WAVELENGTH'"),
                                FitsCard("TFORM1", "'8A'"),
                                FitsCard("TTYPE2", "'FLUX'"),
                                FitsCard("TFORM2", "'8A'"),
                            });
    const std::string data = "5000.0 1.0      ";
    Require(data.size() == 16, "unsupported-column fixture row must match NAXIS1");
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write unsupported-column FITS fixture");
}

void WriteFitsTableWithUnsupportedColumnType(
    const std::filesystem::path& path,
    std::string_view tform,
    std::size_t row_width)
{
    std::ofstream stream(path, std::ios::binary);
    Require(
        stream.good(),
        "could not open unsupported-type FITS fixture");
    WriteFitsPrimary(stream);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", std::to_string(row_width)),
                                FitsCard("NAXIS2", "                   1"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   2"),
                                FitsCard("TTYPE1", "'WAVELENGTH'"),
                                FitsCard("TFORM1", "'" + std::string(tform) + "'"),
                                FitsCard("TTYPE2", "'FLUX'"),
                                FitsCard("TFORM2", "'" + std::string(tform) + "'"),
                            });
    const std::vector<unsigned char> data(row_width, 0);
    stream.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(
        stream.good(),
        "could not write unsupported-type FITS fixture");
}

void WriteAsciiFitsTable(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open ASCII FITS fixture");
    WriteFitsPrimary(stream);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'TABLE   '"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                  16"),
                                FitsCard("NAXIS2", "                   1"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   2"),
                                FitsCard("TTYPE1", "'WAVELENGTH'"),
                                FitsCard("TBCOL1", "                   1"),
                                FitsCard("TFORM1", "'F8.2'"),
                                FitsCard("TTYPE2", "'FLUX'"),
                                FitsCard("TBCOL2", "                   9"),
                                FitsCard("TFORM2", "'F8.2'"),
                            });
    constexpr std::string_view data = " 5000.00    1.00";
    static_assert(data.size() == 16U);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write ASCII FITS fixture");
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

void RequireEquivalentMetadataEntries(
    const std::vector<specforge::SpectrumMetadataEntry>& expected,
    const std::vector<specforge::SpectrumMetadataEntry>& actual,
    std::string_view excluded_key,
    std::string_view context)
{
    std::vector<const specforge::SpectrumMetadataEntry*> expected_entries;
    std::vector<const specforge::SpectrumMetadataEntry*> actual_entries;
    for (const specforge::SpectrumMetadataEntry& entry : expected) {
        if (entry.key != excluded_key) {
            expected_entries.push_back(&entry);
        }
    }
    for (const specforge::SpectrumMetadataEntry& entry : actual) {
        if (entry.key != excluded_key) {
            actual_entries.push_back(&entry);
        }
    }

    Require(
        expected_entries.size() == actual_entries.size(),
        std::string(context) + ": metadata entry counts should match");
    for (std::size_t index = 0; index < expected_entries.size(); ++index) {
        Require(
            expected_entries[index]->key == actual_entries[index]->key &&
                expected_entries[index]->value == actual_entries[index]->value &&
                expected_entries[index]->source == actual_entries[index]->source,
            std::string(context) + ": metadata entries should match");
    }
}

void RequireEquivalentSpectrumSemantics(
    const SpectrumSnapshotHandle& expected,
    const SpectrumSnapshotHandle& actual,
    std::string_view context)
{
    Require(expected != nullptr && actual != nullptr, std::string(context) + ": expected snapshots");
    Require(expected->capabilities.can_plot_current_spectrum, std::string(context) + ": expected snapshot should be plottable");
    Require(
        expected->capabilities.can_plot_current_spectrum == actual->capabilities.can_plot_current_spectrum &&
            expected->capabilities.can_switch_spectrum == actual->capabilities.can_switch_spectrum &&
            expected->capabilities.can_show_spectral_lines == actual->capabilities.can_show_spectral_lines &&
            expected->capabilities.can_show_rest_frame_spectral_lines ==
                actual->capabilities.can_show_rest_frame_spectral_lines &&
            expected->capabilities.requires_angstrom_warning == actual->capabilities.requires_angstrom_warning &&
            expected->capabilities.requires_rest_frame_warning == actual->capabilities.requires_rest_frame_warning &&
            expected->capabilities.has_domain_error == actual->capabilities.has_domain_error,
        std::string(context) + ": public capabilities should match");
    Require(
        expected->current_spectrum.point_count == actual->current_spectrum.point_count,
        std::string(context) + ": point counts should match");
    Require(
        expected->current_spectrum.x_values != nullptr && actual->current_spectrum.x_values != nullptr &&
            expected->current_spectrum.y_values != nullptr && actual->current_spectrum.y_values != nullptr,
        std::string(context) + ": spectrum vectors should be present");
    Require(
        *expected->current_spectrum.x_values == *actual->current_spectrum.x_values,
        std::string(context) + ": wavelength values should match");
    Require(
        *expected->current_spectrum.y_values == *actual->current_spectrum.y_values,
        std::string(context) + ": flux values should match");
    Require(
        expected->axis.x_quantity == actual->axis.x_quantity &&
            expected->axis.x_unit == actual->axis.x_unit &&
            expected->axis.x_frame == actual->axis.x_frame &&
            expected->axis.y_quantity == actual->axis.y_quantity &&
            expected->axis.x_label == actual->axis.x_label &&
            expected->axis.y_label == actual->axis.y_label,
        std::string(context) + ": axis semantics should match");
    Require(
        expected->collection.spectrum_count == actual->collection.spectrum_count &&
            expected->collection.current_index == actual->collection.current_index &&
            expected->collection.can_move_previous == actual->collection.can_move_previous &&
            expected->collection.can_move_next == actual->collection.can_move_next,
        std::string(context) + ": collection semantics should match");
    Require(
        expected->diagnostics.size() == actual->diagnostics.size(),
        std::string(context) + ": diagnostics should have the same shape");
    for (std::size_t index = 0; index < expected->diagnostics.size(); ++index) {
        Require(
            expected->diagnostics[index].severity == actual->diagnostics[index].severity &&
                expected->diagnostics[index].code == actual->diagnostics[index].code,
            std::string(context) + ": diagnostic semantics should match");
        RequireEquivalentMetadataEntries(
            expected->diagnostics[index].metadata,
            actual->diagnostics[index].metadata,
            {},
            std::string(context) + ": diagnostic metadata");
    }
    RequireEquivalentMetadataEntries(
        expected->source.metadata,
        actual->source.metadata,
        "format",
        std::string(context) + ": source metadata");
    RequireEquivalentMetadataEntries(
        expected->current_spectrum.metadata,
        actual->current_spectrum.metadata,
        {},
        std::string(context) + ": current-spectrum metadata");
}

void RequireNonPlottableErrorSnapshot(const SpectrumSnapshotHandle& snapshot, std::string_view context)
{
    Require(snapshot != nullptr, std::string(context) + ": loader should return a snapshot");
    Require(!snapshot->capabilities.can_plot_current_spectrum, std::string(context) + ": snapshot should not be plottable");
    Require(snapshot->capabilities.has_domain_error, std::string(context) + ": snapshot should report a domain error");
    Require(!snapshot->diagnostics.empty(), std::string(context) + ": snapshot should include a diagnostic");
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

specforge::SampleLabelingTaskCanonicalMetadata TestCanonicalMetadata()
{
    const auto timestamp =
        specforge::ParseCanonicalTimestamp("2026-01-02T03:04:05.006Z");
    Require(timestamp.has_value(), "test canonical timestamp should parse");
    specforge::SampleLabelingTaskCanonicalMetadata metadata;
    metadata.created_at = *timestamp;
    metadata.modified_at = *timestamp;
    metadata.origin.kind = "manual";
    return metadata;
}

void TestPreservesNonCanonicalNpySampleNamesForNavigation()
{
    const auto verify_preserved = [](
                                     std::string_view suffix,
                                     std::initializer_list<std::string_view>
                                         names) {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("specforge_invalid_names_" + std::string(suffix) +
             ".npy");
        const std::filesystem::path name_path =
            std::filesystem::temp_directory_path() /
            ("specforge_invalid_names_" + std::string(suffix) +
             "_name.npy");
        WriteNpy(
            path,
            "<f8",
            {2, 2},
            BytesFor<double>({1.0, 2.0, 3.0, 4.0}));
        WriteNpy(
            name_path,
            "<U8",
            {2},
            UnicodeNpyBytesFor(names, 8));

        SpectrumSnapshotHandle snapshot =
            specforge::LoadSpectrumSnapshotFromPath(path, 0);
        const specforge::SourceCollectionManifest manifest =
            specforge::LoadSourceCollectionManifest(*snapshot);
        std::vector<std::string> expected_names;
        expected_names.reserve(names.size());
        for (const std::string_view name : names) {
            expected_names.emplace_back(name);
        }
        bool values_preserved =
            manifest.sample_names.size() ==
            expected_names.size();
        for (std::size_t index = 0;
             values_preserved && index < expected_names.size();
             ++index) {
            const auto is_blank = [](const std::string& value) {
                return std::all_of(
                    value.begin(),
                    value.end(),
                    [](unsigned char character) {
                        return std::isspace(character) != 0;
                    });
            };
            values_preserved =
                is_blank(expected_names[index])
                    ? is_blank(manifest.sample_names[index])
                    : manifest.sample_names[index] ==
                        expected_names[index];
        }
        const bool names_preserved =
            values_preserved &&
                manifest.diagnostics.size() == 1 &&
                manifest.diagnostics.front().kind ==
                    specforge::SourceCollectionManifestDiagnosticKind::
                        SampleNamesIgnored &&
                manifest.diagnostics.front().path == name_path &&
                manifest.diagnostics.front().detail.find("unique") !=
                    std::string::npos;
        snapshot.reset();
        Require(
            names_preserved,
            "blank or duplicate NPY companion names must remain available for navigation while being diagnosed as unsuitable canonical roster identity");
    };

    verify_preserved("blank", {"   ", "beta"});
    verify_preserved("duplicate", {"alpha", "alpha"});
}

specforge::SampleLabelingDocument MakeAnnotationAsdfDocument()
{
    specforge::SampleLabelingDocument document;
    document.source.base_identity = "source-base-v1";
    document.source.kind = "npy";
    document.source.name = "source_X.npy";
    document.source.fingerprint = "source-fingerprint-v1";
    document.source.sample_count = 3;
    document.source.roster.identity_kind =
        std::string{specforge::kSampleLabelingDocumentExplicitNamesRoster};
    document.source.roster.sample_names = {
        "sample-a",
        "sample-b",
        "sample-c",
    };
    document.annotation.values = {-1, 2, 7};
    document.labeling.id = "11111111-1111-4111-8111-111111111111";
    document.labeling.name = "Quality review";
    document.labeling.canonical_metadata = TestCanonicalMetadata();
    document.labeling.labels = {
        {2, "accepted", "a"},
        {7, "rejected", "r"},
    };
    return document;
}

void WriteAnnotationAsdf(
    const std::filesystem::path& path,
    const specforge::SampleLabelingDocument& document)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Require(stream.good(), "ASDF annotation fixture should open");
    const specforge::SampleLabelingAsdfWriteResult write =
        specforge::WriteSampleLabelingAsdfDocument(stream, document);
    Require(
        write.succeeded(),
        write.error.message.empty()
            ? "ASDF annotation fixture should write"
            : write.error.message);
    stream.close();
    Require(stream.good(), "ASDF annotation fixture should flush");
}

void TestAnnotationAdapterLoadsCanonicalAsdfDocumentsForSource()
{
    const std::filesystem::path explicit_path =
        std::filesystem::temp_directory_path() /
        "specforge_annotation_adapter_explicit.asdf";
    const std::filesystem::path source_index_path =
        std::filesystem::temp_directory_path() /
        "specforge_annotation_adapter_source_index.asdf";
    const std::filesystem::path npy_path =
        std::filesystem::temp_directory_path() /
        "specforge_annotation_adapter_legacy.npy";

    const specforge::SampleLabelingDocument explicit_document =
        MakeAnnotationAsdfDocument();
    WriteAnnotationAsdf(explicit_path, explicit_document);

    const std::vector<std::string> sample_names = {
        "sample-a",
        "sample-b",
        "sample-c",
    };
    const specforge::SampleAnnotationSourceCompatibility compatible_source{
        .base_identity = "source-base-v1",
        .source_name = "source_X.npy",
        .source_fingerprint = "source-fingerprint-v1",
        .sample_count = 3,
        .sample_names = sample_names,
    };

    const specforge::SampleAnnotationIoAdapter adapter;
    std::string error;
    const std::optional<specforge::SampleAnnotationResult> loaded =
        adapter.LoadForSource(explicit_path, compatible_source, &error);
    Require(
        loaded.has_value(),
        error.empty() ? "compatible ASDF annotation should load" : error);
    Require(
        loaded->kind == SampleAnnotationKind::CategoricalInteger &&
            loaded->dtype_name == "int32" && loaded->values.size() == 3,
        "ASDF annotation should project its int32 values");
    Require(
        specforge::SampleAnnotationValueAsInt(loaded->values[0]) == -1 &&
            specforge::SampleAnnotationValueAsInt(loaded->values[1]) == 2 &&
            specforge::SampleAnnotationValueAsInt(loaded->values[2]) == 7,
        "ASDF annotation values should be lossless");
    Require(
        loaded->relationship ==
                specforge::SampleAnnotationWorkflowRelationship::
                    ExternalLabelResult &&
            !loaded->label_metadata.has_value(),
        "ASDF annotation should remain distinct from legacy sidecar metadata");
    Require(
        loaded->labeling_document != nullptr &&
            loaded->labeling_document->labeling.id ==
                "11111111-1111-4111-8111-111111111111" &&
            loaded->labeling_document->labeling.name ==
                "Quality review" &&
            loaded->labeling_document->labeling.labels.size() == 2 &&
            loaded->labeling_document->labeling.labels[1].name ==
                "rejected" &&
            loaded->labeling_document->labeling.labels[1].shortcut ==
                "r" &&
            loaded->name == "Quality review" &&
            loaded->labeling_document->source.base_identity ==
                "source-base-v1" &&
            loaded->labeling_document->source.roster.sample_names ==
                sample_names,
        "ASDF canonical task, labels, source, roster, and labeling display name should remain available");
    Require(
        specforge::FormatSampleAnnotationValue(
            *loaded,
            loaded->values[2]) == "rejected (7)" &&
            specforge::FormatSampleAnnotationValue(
                *loaded,
                loaded->values[0]) == "Unlabeled (-1)",
        "ASDF canonical labels should project through annotation display semantics");

    error.clear();
    Require(
        !adapter.Load(explicit_path, 3, &error).has_value() &&
            error.find("source collection identity") != std::string::npos,
        "count-only annotation loads must not bypass ASDF source validation");

    const std::vector<std::string> reordered_names = {
        "sample-b",
        "sample-a",
        "sample-c",
    };
    const specforge::SampleAnnotationSourceCompatibility reordered_source{
        .base_identity = "source-base-v1",
        .source_name = "source_X.npy",
        .source_fingerprint = "source-fingerprint-v1",
        .sample_count = 3,
        .sample_names = reordered_names,
    };
    specforge::SourceCollectionManifest rejected_manifest;
    std::string rejection_message;
    Require(
        !specforge::IngestReadOnlySampleAnnotation(
            rejected_manifest,
            explicit_path,
            reordered_source,
            &rejection_message),
        "an ASDF roster in a different order must not attach");
    Require(
        rejected_manifest.annotations.empty() &&
            rejected_manifest.diagnostics.size() == 1 &&
            rejected_manifest.diagnostics.front().detail.find("roster") !=
                std::string::npos,
        "roster rejection should remain a structured annotation diagnostic");

    const specforge::SampleAnnotationSourceCompatibility wrong_identity{
        .base_identity = "different-source-base",
        .source_name = "source_X.npy",
        .source_fingerprint = "source-fingerprint-v1",
        .sample_count = 3,
        .sample_names = sample_names,
    };
    error.clear();
    Require(
        !adapter.LoadForSource(
             explicit_path,
             wrong_identity,
             &error)
             .has_value() &&
            error.find("source identity") != std::string::npos,
        "ASDF base source identity mismatches must be rejected");

    specforge::SampleLabelingDocument source_index_document =
        explicit_document;
    source_index_document.source.roster.identity_kind =
        std::string{specforge::kSampleLabelingDocumentSourceIndexRoster};
    source_index_document.source.roster.sample_names.clear();
    WriteAnnotationAsdf(source_index_path, source_index_document);
    error.clear();
    const std::optional<specforge::SampleAnnotationResult>
        loaded_source_index = adapter.LoadForSource(
            source_index_path,
            compatible_source,
            &error);
    Require(
        loaded_source_index.has_value() &&
            loaded_source_index->labeling_document != nullptr &&
            loaded_source_index->labeling_document->source.roster
                    .identity_kind ==
                specforge::kSampleLabelingDocumentSourceIndexRoster,
        error.empty()
            ? "source-index ASDF annotation should attach by base identity"
            : error);

    WriteNpy(
        npy_path,
        "<i4",
        {3},
        BytesFor<std::int32_t>({-1, 2, 7}));
    specforge::SampleLabelingTask legacy_task =
        specforge::CreateSampleLabelingTask(
            "legacy-task",
            "Legacy task",
            3);
    Require(
        specforge::UpsertSampleLabel(
            legacy_task.label_set,
            specforge::SampleLabelDefinition{2, "accepted", 'a'}),
        "legacy sidecar fixture label should be valid");
    std::string metadata_error;
    Require(
        adapter.SaveLabelMetadata(
            npy_path,
            legacy_task,
            nullptr,
            &metadata_error),
        metadata_error.empty()
            ? "legacy sidecar fixture should save"
            : metadata_error);
    error.clear();
    const std::optional<specforge::SampleAnnotationResult> legacy_loaded =
        adapter.LoadForSource(npy_path, compatible_source, &error);
    Require(
        legacy_loaded.has_value() &&
            legacy_loaded->label_metadata.has_value() &&
            legacy_loaded->label_metadata->task_id == "legacy-task" &&
            legacy_loaded->labeling_document == nullptr,
        error.empty()
            ? "source-aware dispatch should preserve NPY plus sidecar loading"
            : error);

    Require(
        specforge::SampleAnnotationArtifactIdentities(
            explicit_path,
            specforge::SampleLabelingOutputArtifactFormat::
                CanonicalAsdf,
            false)
                .stable_path_keys.size() == 1 &&
            specforge::SampleAnnotationArtifactIdentities(
                npy_path,
                specforge::SampleLabelingOutputArtifactFormat::
                    LegacyNpyWithSidecar,
                false)
                    .stable_path_keys.size() == 2,
        "artifact ownership should distinguish canonical ASDF from legacy NPY plus sidecar");

    std::error_code cleanup_error;
    std::filesystem::remove(explicit_path, cleanup_error);
    std::filesystem::remove(source_index_path, cleanup_error);
    std::filesystem::remove(npy_path, cleanup_error);
    std::filesystem::remove(
        specforge::SampleAnnotationIoAdapter::MetadataPathForResult(npy_path),
        cleanup_error);
}

void TestCancelableAsdfAnnotationLoadStopsInsideCodecRead()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "specforge_cancelable_annotation.asdf";
    specforge::SampleLabelingDocument document =
        MakeAnnotationAsdfDocument();
    document.source.roster.identity_kind =
        std::string{specforge::kSampleLabelingDocumentSourceIndexRoster};
    document.source.roster.sample_names.clear();
    WriteAnnotationAsdf(path, document);

    const specforge::SampleAnnotationSourceCompatibility source{
        .base_identity = document.source.base_identity,
        .source_name = document.source.name,
        .source_fingerprint = document.source.fingerprint,
        .sample_count = document.source.sample_count,
    };

    class CancellationMarker final : public std::runtime_error {
    public:
        CancellationMarker()
            : std::runtime_error("ASDF annotation cancellation marker")
        {
        }
    };

    std::size_t cancellation_checks = 0;
    bool canceled = false;
    try {
        (void)specforge::SampleAnnotationIoAdapter{}
            .LoadForSourceCancelable(
                path,
                source,
                [&cancellation_checks]() {
                    if (++cancellation_checks >= 4) {
                        throw CancellationMarker();
                    }
                });
    } catch (const CancellationMarker&) {
        canceled = true;
    }
    Require(
        canceled,
        "ASDF annotation cancellation should propagate from inside the codec reader");
    Require(
        cancellation_checks >= 4,
        "ASDF annotation loading should poll during bounded codec I/O");

    std::error_code error;
    std::filesystem::remove(path, error);
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
    Require(
        NearlyEqual(snapshot->current_spectrum.x_values->front(), std::pow(10.0, static_cast<double>(3.0F)), 1.0e-12),
        "FITS scalar table should convert the first LOGLAM value to wavelength");
    Require(
        NearlyEqual(snapshot->current_spectrum.x_values->back(), std::pow(10.0, static_cast<double>(3.2F)), 1.0e-12),
        "FITS scalar table should convert the last retained LOGLAM value to wavelength");
    Require(snapshot->current_spectrum.y_values->front() == 10.0, "FITS scalar table should retain the first flux value");
    Require(snapshot->current_spectrum.y_values->back() == 30.0, "FITS scalar table should retain the last flux value");
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

    const SpectrumSnapshotHandle first = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    const SpectrumSnapshotHandle second = specforge::LoadSpectrumSnapshotFromPathCancelable(path, 1, []() { return false; });

    Require(first->capabilities.can_plot_current_spectrum, "FITS vector table first row should be plottable");
    Require(first->current_spectrum.point_count == 2, "first-row ORMASK pixel should be filtered");
    Require(first->current_spectrum.x_values->at(0) == 5000.0, "first vector row should retain its first wavelength");
    Require(first->current_spectrum.x_values->at(1) == 5002.0, "first vector row should retain its last wavelength");
    Require(first->current_spectrum.y_values->at(0) == 1.0, "first vector row should retain its first flux");
    Require(first->current_spectrum.y_values->at(1) == 3.0, "first vector row should retain its last flux");
    Require(first->collection.spectrum_count == 2, "each vector-table row should be exposed as one spectrum");
    Require(first->collection.current_index == 0, "first vector-table selection should retain its row index");
    Require(first->collection.can_move_next, "first vector-table row should allow moving to the next spectrum");
    Require(first->capabilities.can_switch_spectrum, "multi-row vector table should support spectrum switching");
    Require(first->axis.x_quantity == SpectrumAxisQuantity::Wavelength, "FITS vector table should expose wavelength axis");
    Require(MetadataValue(first, "source_type") == "fits_spectrum", "FITS vector source type should come from domain");
    Require(MetadataValue(first, "format") == "fits", "FITS vector format should come from domain");
    Require(
        MetadataValue(first, "radial_velocity_km_s") == "124.5",
        "first vector row scalar RV column should be retained");
    Require(
        MetadataValue(first, "radial_velocity_source") == "table_column:RV",
        "FITS vector table scalar RV source should be retained");
    Require(
        NearlyEqual(MetadataDouble(first, "redshift"), 0.02, 1.0e-8),
        "first vector row scalar redshift column should be retained");
    Require(
        NearlyEqual(MetadataDouble(first, "target_redshift"), 124.5 / kSpeedOfLightKmPerSecond, 1.0e-15),
        "first vector row target redshift should be derived from scalar RV");
    Require(MetadataValue(first, "wavelength_medium") == "vacuum", "FITS vector VACUUM metadata should be normalized");
    Require(
        MetadataValue(first, "target_rest_frame_status") == "available_not_applied",
        "first vector row should expose available-but-unapplied rest-frame metadata");
    Require(
        MetadataValue(first, "rest_frame_correction_status") == "not_applied",
        "FITS vector row should record that rest-frame correction was not applied");
    Require(first->capabilities.requires_rest_frame_warning, "FITS vector row should retain the rest-frame warning");
    Require(HasDiagnosticCode(first, SpectrumDiagnosticCode::MaskFilteredPixels), "FITS vector table should report mask filtering");

    Require(second->capabilities.can_plot_current_spectrum, "FITS vector table second row should be plottable");
    Require(second->current_spectrum.point_count == 2, "second-row ORMASK pixel should be filtered");
    Require(second->current_spectrum.x_values->at(0) == 6001.0, "second vector row should retain its first unmasked wavelength");
    Require(second->current_spectrum.x_values->at(1) == 6002.0, "second vector row should retain its last wavelength");
    Require(second->current_spectrum.y_values->at(0) == 5.0, "second vector row should retain its first unmasked flux");
    Require(second->current_spectrum.y_values->at(1) == 6.0, "second vector row should retain its last flux");
    Require(second->collection.spectrum_count == 2, "second vector row should retain the table spectrum count");
    Require(second->collection.current_index == 1, "second vector-table selection should retain its row index");
    Require(second->collection.can_move_previous, "second vector-table row should allow moving to the previous spectrum");
    Require(!second->collection.can_move_next, "last vector-table row should not allow moving past the table");
    Require(MetadataValue(second, "radial_velocity_km_s") == "-50", "second vector row should use its own RV value");
    Require(
        NearlyEqual(MetadataDouble(second, "redshift"), 0.03, 1.0e-8),
        "second vector row should use its own redshift value");
    Require(
        NearlyEqual(MetadataDouble(second, "target_redshift"), -50.0 / kSpeedOfLightKmPerSecond, 1.0e-15),
        "second vector row target redshift should be derived from its own RV");
    Require(
        MetadataValue(second, "target_rest_frame_status") == "available_not_applied",
        "second vector row should retain rest-frame metadata");
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
    Require(snapshot->current_spectrum.point_count == 2, "image IVAR and ORMASK rows should filter invalid pixels");
    Require(snapshot->current_spectrum.x_values->at(0) == 1000.0, "image wavelength should use COEFF0/COEFF1");
    Require(
        NearlyEqual(snapshot->current_spectrum.x_values->at(1), std::pow(10.0, 3.003), 1.0e-12),
        "image wavelength endpoint should apply both COEFF0 and COEFF1");
    Require(snapshot->current_spectrum.y_values->at(0) == 1.0, "image row 0 should provide flux");
    Require(snapshot->current_spectrum.y_values->at(1) == 4.0, "image row 0 should provide the retained endpoint flux");
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

void TestPrefersRecognizedFitsTableOverImageHdu()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_loader_table_before_image_fallback.fits";
    WriteFitsImageThenScalarTable(path);

    const SpectrumSnapshotHandle snapshot = specforge::LoadSpectrumSnapshotFromPath(path, 0);
    Require(snapshot->capabilities.can_plot_current_spectrum, "mixed-HDU FITS should be plottable");
    Require(MetadataValue(snapshot, "hdu_type") == "bintable", "recognized table HDU should take priority over image HDU");
    Require(snapshot->current_spectrum.point_count == 2, "selected table HDU should expose both scalar rows");
    Require(snapshot->current_spectrum.x_values->at(0) == 7000.0, "mixed-HDU selection should use table wavelength values");
    Require(snapshot->current_spectrum.x_values->at(1) == 7001.0, "mixed-HDU selection should retain the table endpoint");
    Require(snapshot->current_spectrum.y_values->at(0) == 70.0, "mixed-HDU selection should use table flux values");
    Require(snapshot->current_spectrum.y_values->at(1) == 71.0, "mixed-HDU selection should retain the table flux endpoint");
}

void TestUndefinedOptionalFitsKeywordsAreAbsent()
{
    const std::filesystem::path table_path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_undefined_optional_table.fits";
    WriteFitsTableWithUndefinedOptionalKeywords(table_path);

    specforge::detail::FitsFile table_file =
        specforge::detail::FitsFile::Open(
            table_path,
            specforge::detail::FitsSourceEncoding::Plain);
    const specforge::detail::FitsHdu& table_hdu =
        table_file.hdus().at(1);
    Require(
        !table_file.ReadKeywordString(table_hdu, "CLASS"),
        "undefined string FITS keywords should be absent");
    Require(
        !table_file.ReadKeywordInteger(table_hdu, "ZWARNING"),
        "undefined integer FITS keywords should be absent");
    Require(
        !table_file.ReadKeywordDouble(table_hdu, "Z"),
        "undefined floating-point FITS keywords should be absent");

    const SpectrumSnapshotHandle table_snapshot =
        specforge::LoadSpectrumSnapshotFromPath(table_path, 0);
    Require(
        table_snapshot->capabilities.can_plot_current_spectrum,
        "undefined optional table keywords should not prevent loading");
    Require(
        MetadataValue(table_snapshot, "radial_velocity_km_s").empty() &&
            MetadataValue(table_snapshot, "redshift").empty() &&
            MetadataValue(table_snapshot, "redshift_warning").empty() &&
            MetadataValue(table_snapshot, "survey_class").empty() &&
            MetadataValue(table_snapshot, "telescope").empty(),
        "undefined optional table keywords should not publish metadata");

    const std::filesystem::path image_path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_undefined_optional_image.fits";
    WriteFitsImageWithUndefinedCoefficients(image_path);
    specforge::detail::FitsFile image_file =
        specforge::detail::FitsFile::Open(
            image_path,
            specforge::detail::FitsSourceEncoding::Plain);
    Require(
        !image_file.ReadKeywordDouble(
            image_file.hdus().front(),
            "COEFF0"),
        "undefined image coefficient keywords should be absent");

    const SpectrumSnapshotHandle image_snapshot =
        specforge::LoadSpectrumSnapshotFromPath(image_path, 0);
    RequireNonPlottableErrorSnapshot(
        image_snapshot,
        "image with undefined optional spectrum coefficients");
    Require(
        FirstDiagnosticCode(image_snapshot) ==
            SpectrumDiagnosticCode::CatalogNotSpectrum,
        "undefined image coefficients should make the image unrecognized, not malformed");

    std::error_code error;
    std::filesystem::remove(table_path, error);
    error.clear();
    std::filesystem::remove(image_path, error);
}

void TestRejectsTruncatedUnselectedFitsData()
{
    const auto require_invalid_shape = [](
                                           const std::filesystem::path& path,
                                           std::string_view context) {
        bool rejected = false;
        try {
            (void)specforge::detail::FitsFile::Open(
                path,
                specforge::detail::FitsSourceEncoding::Plain);
        } catch (const specforge::detail::FitsFileError& error) {
            rejected = error.code() ==
                specforge::detail::FitsFileErrorCode::InvalidShape;
        }
        Require(
            rejected,
            std::string(context) +
                ": reader open should reject the incomplete declared HDU");

        const SpectrumSnapshotHandle snapshot =
            specforge::LoadSpectrumSnapshotFromPath(path, 0);
        RequireNonPlottableErrorSnapshot(snapshot, context);
        Require(
            FirstDiagnosticCode(snapshot) ==
                SpectrumDiagnosticCode::InvalidShape,
            std::string(context) +
                ": truncation should map to invalid shape");
    };

    const std::filesystem::path table_path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_truncated_unselected_table_row.fits";
    WriteFitsVectorTableWithTruncatedLaterRow(table_path);
    require_invalid_shape(
        table_path,
        "FITS table truncated after the selected vector row");

    const std::filesystem::path image_path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_truncated_unselected_image_tail.fits";
    WriteFitsImageWithTruncatedUnselectedTail(image_path);
    require_invalid_shape(
        image_path,
        "FITS image truncated after the selected data and mask rows");

    std::error_code error;
    std::filesystem::remove(table_path, error);
    error.clear();
    std::filesystem::remove(image_path, error);
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

void TestMalformedFitsInputsReturnErrorSnapshots()
{
    const std::filesystem::path truncated_header =
        std::filesystem::temp_directory_path() / "specforge_loader_truncated_header.fits";
    WriteTruncatedFitsHeader(truncated_header);
    const SpectrumSnapshotHandle truncated_header_snapshot =
        specforge::LoadSpectrumSnapshotFromPath(
            truncated_header,
            0);
    RequireNonPlottableErrorSnapshot(
        truncated_header_snapshot,
        "truncated FITS header");
    Require(
        FirstDiagnosticCode(truncated_header_snapshot) ==
            SpectrumDiagnosticCode::InvalidShape,
        "truncated FITS headers should map to invalid shape");

    const std::filesystem::path missing_image_data =
        std::filesystem::temp_directory_path() / "specforge_loader_missing_image_data.fits";
    WriteFitsHeaderWithMissingImageData(missing_image_data);
    const SpectrumSnapshotHandle missing_image_data_snapshot =
        specforge::LoadSpectrumSnapshotFromPathCancelable(
            missing_image_data,
            0,
            []() { return false; });
    RequireNonPlottableErrorSnapshot(
        missing_image_data_snapshot,
        "complete FITS header with truncated data");
    Require(
        FirstDiagnosticCode(missing_image_data_snapshot) ==
            SpectrumDiagnosticCode::InvalidShape,
        "truncated FITS image data should map to invalid shape");

    const std::filesystem::path declared_data_mismatch =
        std::filesystem::temp_directory_path() / "specforge_loader_declared_data_mismatch.fits";
    WriteFitsTableWithDeclaredDataMismatch(declared_data_mismatch);
    const SpectrumSnapshotHandle declared_data_mismatch_snapshot =
        specforge::LoadSpectrumSnapshotFromPath(
            declared_data_mismatch,
            0);
    RequireNonPlottableErrorSnapshot(
        declared_data_mismatch_snapshot,
        "FITS declared dimensions larger than actual data");
    Require(
        FirstDiagnosticCode(declared_data_mismatch_snapshot) ==
            SpectrumDiagnosticCode::InvalidShape,
        "FITS declared data mismatches should map to invalid shape");

    const std::filesystem::path unsupported_columns =
        std::filesystem::temp_directory_path() / "specforge_loader_unsupported_columns.fits";
    WriteFitsTableWithUnsupportedColumnLayout(unsupported_columns);
    const SpectrumSnapshotHandle unsupported_columns_snapshot =
        specforge::LoadSpectrumSnapshotFromPathCancelable(
            unsupported_columns,
            0,
            []() { return false; });
    RequireNonPlottableErrorSnapshot(
        unsupported_columns_snapshot,
        "unsupported FITS table column layout");
    Require(
        FirstDiagnosticCode(unsupported_columns_snapshot) ==
            SpectrumDiagnosticCode::UnsupportedFormat,
        "non-numeric FITS spectrum columns should map to unsupported format");
}

void TestRejectsUnsupportedFitsStructures()
{
    struct UnsupportedColumnCase {
        std::string_view name;
        std::string_view tform;
        std::size_t row_width;
    };
    constexpr std::array cases = {
        UnsupportedColumnCase{"variable", "1PE(1)", 16U},
        UnsupportedColumnCase{"complex", "C", 16U},
        UnsupportedColumnCase{"bit", "8X", 2U},
    };

    for (const UnsupportedColumnCase& test_case : cases) {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("specforge_loader_unsupported_" +
             std::string(test_case.name) + ".fits");
        WriteFitsTableWithUnsupportedColumnType(
            path,
            test_case.tform,
            test_case.row_width);
        const SpectrumSnapshotHandle snapshot =
            specforge::LoadSpectrumSnapshotFromPathCancelable(
                path,
                0,
                []() { return false; });
        RequireNonPlottableErrorSnapshot(
            snapshot,
            "unsupported FITS column type");
        Require(
            FirstDiagnosticCode(snapshot) ==
                SpectrumDiagnosticCode::UnsupportedFormat,
            "variable, complex, and bit FITS spectrum columns should be unsupported");
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    const std::filesystem::path ascii_path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_unsupported_ascii_table.fits";
    WriteAsciiFitsTable(ascii_path);
    const SpectrumSnapshotHandle ascii_snapshot =
        specforge::LoadSpectrumSnapshotFromPath(ascii_path, 0);
    RequireNonPlottableErrorSnapshot(
        ascii_snapshot,
        "ASCII FITS table");
    Require(
        FirstDiagnosticCode(ascii_snapshot) ==
            SpectrumDiagnosticCode::UnsupportedFormat,
        "ASCII FITS tables should be unsupported");
    std::error_code error;
    std::filesystem::remove(ascii_path, error);
}

void TestLoadsGzippedFitsSpectrum()
{
    const std::filesystem::path fits_path = std::filesystem::temp_directory_path() / "specforge_loader_gzip_source.fits";
    const std::filesystem::path gzip_path = std::filesystem::temp_directory_path() / "specforge_loader_gzip_source.fits.gz";
    WriteFitsScalarTable(fits_path);
    WriteBytes(gzip_path, GzipBytes(ReadBytes(fits_path)));

    const SpectrumSnapshotHandle uncompressed = specforge::LoadSpectrumSnapshotFromPath(fits_path, 0);
    const SpectrumSnapshotHandle compressed = specforge::LoadSpectrumSnapshotFromPath(gzip_path, 0);
    RequireEquivalentSpectrumSemantics(uncompressed, compressed, "compressed and uncompressed FITS");
    Require(MetadataValue(uncompressed, "format") == "fits", "uncompressed FITS should retain its format label");
    Require(MetadataValue(compressed, "format") == "fits.gz", "gzipped FITS should retain its format label");
}

void TestLoadsConcatenatedGzipMembers()
{
    const std::filesystem::path fits_path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_concatenated_gzip_source.fits";
    const std::filesystem::path gzip_path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_concatenated_gzip_source.fits.gz";
    WriteFitsScalarTable(fits_path);
    const std::vector<unsigned char> fits_bytes = ReadBytes(fits_path);
    const std::size_t split = fits_bytes.size() / 2U;
    const std::vector<unsigned char> first_half(
        fits_bytes.begin(),
        fits_bytes.begin() + static_cast<std::ptrdiff_t>(split));
    const std::vector<unsigned char> second_half(
        fits_bytes.begin() + static_cast<std::ptrdiff_t>(split),
        fits_bytes.end());
    std::vector<unsigned char> compressed = GzipBytes(first_half);
    const std::vector<unsigned char> second_member =
        GzipBytes(second_half);
    compressed.insert(
        compressed.end(),
        second_member.begin(),
        second_member.end());
    WriteBytes(gzip_path, compressed);

    const SpectrumSnapshotHandle uncompressed =
        specforge::LoadSpectrumSnapshotFromPath(fits_path, 0);
    const SpectrumSnapshotHandle concatenated =
        specforge::LoadSpectrumSnapshotFromPath(gzip_path, 0);
    RequireEquivalentSpectrumSemantics(
        uncompressed,
        concatenated,
        "concatenated gzip members and uncompressed FITS");

    std::error_code error;
    std::filesystem::remove(fits_path, error);
    error.clear();
    std::filesystem::remove(gzip_path, error);
}

void TestRejectsDataAfterValidGzipMember()
{
    const std::filesystem::path fits_path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_gzip_trailing_source.fits";
    WriteFitsScalarTable(fits_path);
    const std::vector<unsigned char> valid_member =
        GzipBytes(ReadBytes(fits_path));

    const auto require_open_failed = [&valid_member](
                                         const std::filesystem::path& path,
                                         const std::vector<unsigned char>& suffix,
                                         std::string_view context) {
        std::vector<unsigned char> bytes = valid_member;
        bytes.insert(bytes.end(), suffix.begin(), suffix.end());
        WriteBytes(path, bytes);
        const SpectrumSnapshotHandle snapshot =
            specforge::LoadSpectrumSnapshotFromPath(path, 0);
        RequireNonPlottableErrorSnapshot(snapshot, context);
        Require(
            FirstDiagnosticCode(snapshot) ==
                SpectrumDiagnosticCode::OpenFailed,
            std::string(context) +
                ": invalid trailing compressed data should fail transport opening");
    };

    std::vector<unsigned char> broken_member =
        GzipBytes({'b', 'r', 'o', 'k', 'e', 'n'});
    Require(
        broken_member.size() > 4U,
        "broken-member fixture should include a gzip trailer");
    broken_member.resize(broken_member.size() - 4U);
    const std::filesystem::path broken_member_path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_broken_trailing_member.fits.gz";
    require_open_failed(
        broken_member_path,
        broken_member,
        "valid FITS gzip followed by a broken member");

    const std::filesystem::path garbage_path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_trailing_gzip_garbage.fits.gz";
    require_open_failed(
        garbage_path,
        {'g', 'a', 'r', 'b', 'a', 'g', 'e'},
        "valid FITS gzip followed by garbage");

    std::error_code error;
    std::filesystem::remove(fits_path, error);
    error.clear();
    std::filesystem::remove(broken_member_path, error);
    error.clear();
    std::filesystem::remove(garbage_path, error);
}

void TestLoadsFitsFromNonAsciiPath()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / std::filesystem::path(L"specforge_loader_\u5149\u8c31\u8def\u5f84");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    Require(!error, "could not create non-ASCII FITS fixture directory");
    const std::filesystem::path path = directory / std::filesystem::path(L"\u517c\u5bb9\u6d4b\u8bd5.fits");
    WriteFitsScalarTable(path);

    const SpectrumSnapshotHandle snapshot =
        specforge::LoadSpectrumSnapshotFromPathCancelable(path, 0, []() { return false; });
    Require(snapshot->capabilities.can_plot_current_spectrum, "FITS file at a non-ASCII path should be plottable");
    Require(snapshot->source.path == path, "FITS snapshot should preserve the native non-ASCII source path");
    Require(
        snapshot->current_spectrum.name == PathToUtf8(path.filename()),
        "FITS snapshot should expose the non-ASCII filename as UTF-8");
    Require(snapshot->current_spectrum.point_count == 2, "non-ASCII FITS path should preserve spectrum contents");

    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::remove(directory, error);
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
        WriteFitsHeader(stream, {
                                    FitsCard("XTENSION", "'IMAGE   '"),
                                    FitsCard("BITPIX", "                   8"),
                                    FitsCard("NAXIS", "                   0"),
                                    FitsCard("PCOUNT", "                   0"),
                                    FitsCard("GCOUNT", "                   1"),
                                });
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
            return cancellation_checks >= 5;
        });
    Require(snapshot == nullptr, "cancelable FITS.GZ loading should stop without publishing an error snapshot");
    Require(cancellation_checks >= 5, "FITS.GZ loading should poll cancellation after entering zlib");
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
    const std::filesystem::path table_path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_cancel_cfitsio_column.fits";
    WriteLargeFitsLoglamVectorTable(table_path, kValueCount);
    specforge::detail::FitsFile table_file =
        specforge::detail::FitsFile::Open(
            table_path,
            specforge::detail::FitsSourceEncoding::Plain);
    const auto table_hdu = std::find_if(
        table_file.hdus().begin(),
        table_file.hdus().end(),
        [](const specforge::detail::FitsHdu& hdu) {
            return hdu.kind ==
                specforge::detail::FitsHduKind::BinaryTable;
        });
    Require(
        table_hdu != table_file.hdus().end(),
        "large FITS table fixture should expose a binary table");
    const auto column = std::find_if(
        table_hdu->columns.begin(),
        table_hdu->columns.end(),
        [](const specforge::detail::FitsColumn& candidate) {
            return candidate.normalized_name == "FLUX";
        });
    Require(
        column != table_hdu->columns.end(),
        "large FITS table fixture should expose its flux column");

    std::size_t column_checks = 0;
    bool column_canceled = false;
    try {
        (void)table_file.ReadColumnVector(
            *table_hdu,
            *column,
            0,
            false,
            [&column_checks]() { return ++column_checks >= 5; });
    } catch (const specforge::detail::FitsFileError& error) {
        column_canceled = error.code() == specforge::detail::FitsFileErrorCode::Canceled;
    }
    Require(column_canceled, "FITS table value conversion should honor cooperative cancellation");
    Require(
        column_checks >= 5,
        "FITS table cancellation should occur after a CFITSIO read");

    const std::filesystem::path image_path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_cancel_cfitsio_image.fits";
    WriteLargeFitsImage(image_path, kValueCount);
    specforge::detail::FitsFile image_file =
        specforge::detail::FitsFile::Open(
            image_path,
            specforge::detail::FitsSourceEncoding::Plain);
    const specforge::detail::FitsHdu& image_hdu =
        image_file.hdus().front();
    std::size_t image_checks = 0;
    bool image_canceled = false;
    try {
        (void)image_file.ReadImageRow(
            image_hdu,
            0,
            kValueCount,
            [&image_checks]() { return ++image_checks >= 5; });
    } catch (const specforge::detail::FitsFileError& error) {
        image_canceled = error.code() == specforge::detail::FitsFileErrorCode::Canceled;
    }
    Require(image_canceled, "FITS image value conversion should honor cooperative cancellation");
    Require(
        image_checks >= 5,
        "FITS image cancellation should occur after a CFITSIO read");

    std::error_code error;
    std::filesystem::remove(table_path, error);
    error.clear();
    std::filesystem::remove(image_path, error);
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
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "specforge_loader_cancel_cfitsio_columns.fits";
    {
        std::ofstream stream(path, std::ios::binary);
        Require(
            stream.good(),
            "could not open large-column FITS fixture");
        WriteFitsPrimary(stream);
        WriteFitsHeader(stream, cards);
        const std::vector<unsigned char> data(3996, 0);
        stream.write(
            reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
        PadFitsData(stream, data.size());
        Require(
            stream.good(),
            "could not write large-column FITS fixture");
    }

    constexpr std::size_t kCancelInsideColumnEnumeration = 30;
    std::size_t cancellation_checks = 0;
    bool canceled = false;
    try {
        (void)specforge::detail::FitsFile::Open(
            path,
            specforge::detail::FitsSourceEncoding::Plain,
            specforge::detail::kMaxSynchronousFitsFileBytes,
            specforge::detail::kMaxSynchronousInflatedFitsBytes,
            [&cancellation_checks]() {
                return ++cancellation_checks >=
                    kCancelInsideColumnEnumeration;
            });
    } catch (const specforge::detail::FitsFileError& error) {
        canceled = error.code() == specforge::detail::FitsFileErrorCode::Canceled;
    }
    Require(canceled, "FITS TFIELDS column discovery should honor cancellation inside its column loop");

    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestFitsHeaderMetadataScanPollsAcrossManyHdus()
{
    constexpr std::size_t kTrailingHduCount = 3'000;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "specforge_loader_cancel_many_hdu_metadata.fits";
    WriteFitsScalarTableWithTrailingEmptyHdus(path, kTrailingHduCount);

    // CFITSIO enumeration accounts for four checks per empty image HDU.
    // Arming beyond that boundary targets the repeated keyword scans.
    const std::size_t cancel_at =
        4 * (kTrailingHduCount + 2) + 150;
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
    constexpr std::size_t kPayloadSize = 64ULL * 1024ULL * 1024ULL + 1ULL;
    WriteBytes(path, GzipFitsImageWithPayload(kPayloadSize));

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
    TestPreservesNonCanonicalNpySampleNamesForNavigation();
    TestLoadsReadOnlyAnnotationDtypes();
    TestAnnotationAdapterPreservesWideNumericSemantics();
    TestAnnotationAdapterLoadsCanonicalAsdfDocumentsForSource();
    TestCancelableAsdfAnnotationLoadStopsInsideCodecRead();
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
    TestPrefersRecognizedFitsTableOverImageHdu();
    TestUndefinedOptionalFitsKeywordsAreAbsent();
    TestRejectsTruncatedUnselectedFitsData();
    TestRejectsMalformedFitsTableWidth();
    TestMalformedFitsInputsReturnErrorSnapshots();
    TestRejectsUnsupportedFitsStructures();
    TestLoadsGzippedFitsSpectrum();
    TestLoadsConcatenatedGzipMembers();
    TestRejectsDataAfterValidGzipMember();
    TestLoadsFitsFromNonAsciiPath();
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
