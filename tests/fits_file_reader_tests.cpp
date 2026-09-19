#include "domain/fits_file_reader.h"
#include "cancellation_stage_probe.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fitsio.h>
#include <zlib.h>

namespace {

using spectiary::detail::FitsColumn;
using spectiary::detail::FitsFile;
using spectiary::detail::FitsFileError;
using spectiary::detail::FitsFileErrorCode;
using spectiary::detail::FitsHdu;
using spectiary::detail::FitsHduKind;
using spectiary::detail::FitsNumericType;
using spectiary::detail::FitsSourceEncoding;
using spectiary::tests::CancellationStageProbe;
using spectiary::tests::SourceStage;

static_assert(std::is_move_constructible_v<FitsFile>);
static_assert(std::is_move_assignable_v<FitsFile>);
static_assert(!std::is_copy_constructible_v<FitsFile>);
static_assert(!std::is_copy_assignable_v<FitsFile>);

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void TestCfitsioReentrantBuild()
{
    Require(
        fits_is_reentrant() != 0,
        "[SF-FITS-CFITSIO-REENTRANT] CFITSIO must be built reentrant for concurrent background FITS loading");
}

template <typename Operation>
void RequireFitsError(
    FitsFileErrorCode expected,
    Operation&& operation,
    std::string_view context)
{
    bool threw = false;
    try {
        std::forward<Operation>(operation)();
    } catch (const FitsFileError& error) {
        threw = true;
        Require(
            error.code() == expected,
            std::string(context) + ": unexpected FITS error category");
    }
    Require(threw, std::string(context) + ": expected a FITS error");
}

class TempDirectory {
public:
    TempDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = std::filesystem::temp_directory_path() /
            ("spectiary_fits_file_reader_tests_" +
             std::to_string(suffix));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        error.clear();
        Require(
            std::filesystem::create_directory(path_, error) && !error,
            "could not create FITS reader test directory");
    }

    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

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

void WriteFitsHeader(
    std::ofstream& stream,
    const std::vector<std::string>& cards)
{
    std::string header;
    for (const std::string& card : cards) {
        Require(card.size() == 80U, "FITS cards must be 80 bytes");
        header += card;
    }
    header += FitsCard("END");
    header.append((2880U - header.size() % 2880U) % 2880U, ' ');
    stream.write(
        header.data(),
        static_cast<std::streamsize>(header.size()));
}

void PadFitsData(std::ofstream& stream, std::size_t data_size)
{
    const std::size_t padding =
        (2880U - data_size % 2880U) % 2880U;
    const std::string bytes(padding, '\0');
    stream.write(
        bytes.data(),
        static_cast<std::streamsize>(bytes.size()));
}

void AppendBigEndianFloat(
    std::vector<unsigned char>& bytes,
    float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bytes.push_back(
        static_cast<unsigned char>((bits >> 24U) & 0xffU));
    bytes.push_back(
        static_cast<unsigned char>((bits >> 16U) & 0xffU));
    bytes.push_back(
        static_cast<unsigned char>((bits >> 8U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>(bits & 0xffU));
}

void AppendBigEndianInt32(
    std::vector<unsigned char>& bytes,
    std::int32_t value)
{
    const std::uint32_t bits = static_cast<std::uint32_t>(value);
    bytes.push_back(
        static_cast<unsigned char>((bits >> 24U) & 0xffU));
    bytes.push_back(
        static_cast<unsigned char>((bits >> 16U) & 0xffU));
    bytes.push_back(
        static_cast<unsigned char>((bits >> 8U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>(bits & 0xffU));
}

std::vector<unsigned char> ReadBytes(
    const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "could not read FITS fixture bytes");
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void WriteBytes(
    const std::filesystem::path& path,
    const std::vector<unsigned char>& bytes)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open fixture byte stream");
    if (!bytes.empty()) {
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    Require(stream.good(), "could not write fixture bytes");
}

std::vector<unsigned char> GzipBytes(
    const std::vector<unsigned char>& bytes)
{
    z_stream stream = {};
    Require(
        deflateInit2(
            &stream,
            Z_BEST_COMPRESSION,
            Z_DEFLATED,
            MAX_WBITS + 16,
            8,
            Z_DEFAULT_STRATEGY) == Z_OK,
        "could not initialize gzip fixture compression");
    stream.next_in = const_cast<Bytef*>(
        reinterpret_cast<const Bytef*>(bytes.data()));
    stream.avail_in = static_cast<uInt>(bytes.size());

    std::array<unsigned char, 64U * 1024U> buffer = {};
    std::vector<unsigned char> output;
    int result = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = deflate(&stream, Z_FINISH);
        Require(
            result == Z_OK || result == Z_STREAM_END,
            "could not compress gzip fixture bytes");
        const std::size_t produced =
            buffer.size() - stream.avail_out;
        output.insert(
            output.end(),
            buffer.begin(),
            buffer.begin() + static_cast<std::ptrdiff_t>(produced));
    } while (result != Z_STREAM_END);
    deflateEnd(&stream);
    return output;
}

void WriteEmptyPrimary(std::ofstream& stream)
{
    WriteFitsHeader(stream, {
                                FitsCard("SIMPLE", "                   T"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   0"),
                                FitsCard("EXTEND", "                   T"),
                            });
}

void WritePrimaryFloatImage(
    const std::filesystem::path& path,
    std::size_t width,
    std::size_t height)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open primary image fixture");
    WriteFitsHeader(stream, {
                                FitsCard("SIMPLE", "                   T"),
                                FitsCard("BITPIX", "                 -32"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", std::to_string(width)),
                                FitsCard("NAXIS2", std::to_string(height)),
                                FitsCard("OPTION"),
                                FitsCard("COUNT"),
                                FitsCard("SCALE"),
                            });
    std::vector<unsigned char> data;
    data.reserve(width * height * sizeof(float));
    for (std::size_t index = 0; index < width * height; ++index) {
        AppendBigEndianFloat(data, static_cast<float>(index + 1U));
    }
    stream.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write primary image fixture");
}

void WriteMultiHduFixture(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open multi-HDU FITS fixture");

    WriteFitsHeader(stream, {
                                FitsCard("SIMPLE", "                   T"),
                                FitsCard("BITPIX", "                 -32"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                   2"),
                                FitsCard("NAXIS2", "                   2"),
                                FitsCard("EXTEND", "                   T"),
                            });
    std::vector<unsigned char> primary_data;
    for (float value : {1.0F, 2.0F, 3.0F, 4.0F}) {
        AppendBigEndianFloat(primary_data, value);
    }
    stream.write(
        reinterpret_cast<const char*>(primary_data.data()),
        static_cast<std::streamsize>(primary_data.size()));
    PadFitsData(stream, primary_data.size());

    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'IMAGE   '"),
                                FitsCard("BITPIX", "                 -32"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                   3"),
                                FitsCard("NAXIS2", "                   2"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                            });
    std::vector<unsigned char> extension_data;
    for (float value : {
             11.0F, 12.0F, 13.0F,
             14.0F, 15.0F, 16.0F,
         }) {
        AppendBigEndianFloat(extension_data, value);
    }
    stream.write(
        reinterpret_cast<const char*>(extension_data.data()),
        static_cast<std::streamsize>(extension_data.size()));
    PadFitsData(stream, extension_data.size());

    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                  20"),
                                FitsCard("NAXIS2", "                   2"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   3"),
                                FitsCard("TTYPE1", "'SCALAR'"),
                                FitsCard("TFORM1", "'J'"),
                                FitsCard("TTYPE2", "'VECTOR'"),
                                FitsCard("TFORM2", "'3E'"),
                                FitsCard("TTYPE3", "'TEXT'"),
                                FitsCard("TFORM3", "'4A'"),
                            });
    std::vector<unsigned char> table_data;
    const auto append_row = [&table_data](
                                std::int32_t scalar,
                                const std::array<float, 3>& values,
                                std::string_view text) {
        AppendBigEndianInt32(table_data, scalar);
        for (float value : values) {
            AppendBigEndianFloat(table_data, value);
        }
        Require(text.size() == 4U, "table text cells must be four bytes");
        table_data.insert(table_data.end(), text.begin(), text.end());
    };
    append_row(7, {1.0F, 2.0F, 3.0F}, "ABCD");
    append_row(9, {4.0F, 5.0F, 6.0F}, "EFGH");
    stream.write(
        reinterpret_cast<const char*>(table_data.data()),
        static_cast<std::streamsize>(table_data.size()));
    PadFitsData(stream, table_data.size());
    Require(stream.good(), "could not write multi-HDU FITS fixture");
}

void WriteSingleColumnTable(
    const std::filesystem::path& path,
    std::string_view tform,
    std::size_t row_width,
    std::size_t heap_size = 0U)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open single-column table fixture");
    WriteEmptyPrimary(stream);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", std::to_string(row_width)),
                                FitsCard("NAXIS2", "                   1"),
                                FitsCard("PCOUNT", std::to_string(heap_size)),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   1"),
                                FitsCard("TTYPE1", "'VALUES'"),
                                FitsCard(
                                    "TFORM1",
                                    "'" + std::string(tform) + "'"),
                            });
    const std::vector<unsigned char> data(row_width + heap_size, 0U);
    stream.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write single-column table fixture");
}

void WriteInconsistentNumericTable(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open inconsistent table fixture");
    WriteEmptyPrimary(stream);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                  16"),
                                FitsCard("NAXIS2", "                   1"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   2"),
                                FitsCard("TTYPE1", "'FIRST'"),
                                FitsCard("TFORM1", "'2E'"),
                                FitsCard("TTYPE2", "'SECOND'"),
                                FitsCard("TFORM2", "'E'"),
                            });
    const std::vector<unsigned char> data(16U, 0U);
    stream.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
}

void WriteTruncatedHeader(const std::filesystem::path& path)
{
    const std::string bytes =
        FitsCard("SIMPLE", "                   T") +
        FitsCard("BITPIX", "                   8");
    WriteBytes(
        path,
        std::vector<unsigned char>(bytes.begin(), bytes.end()));
}

void WriteTruncatedImageData(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open truncated image fixture");
    WriteFitsHeader(stream, {
                                FitsCard("SIMPLE", "                   T"),
                                FitsCard("BITPIX", "                 -32"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", "                   4"),
                                FitsCard("NAXIS2", "                   4"),
                            });
    std::vector<unsigned char> data;
    for (float value : {1.0F, 2.0F, 3.0F, 4.0F}) {
        AppendBigEndianFloat(data, value);
    }
    stream.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
}

void WriteManyHdus(
    const std::filesystem::path& path,
    std::size_t extension_count)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open many-HDU fixture");
    WriteEmptyPrimary(stream);
    for (std::size_t index = 0; index < extension_count; ++index) {
        WriteFitsHeader(stream, {
                                    FitsCard("XTENSION", "'IMAGE   '"),
                                    FitsCard("BITPIX", "                   8"),
                                    FitsCard("NAXIS", "                   0"),
                                    FitsCard("PCOUNT", "                   0"),
                                    FitsCard("GCOUNT", "                   1"),
                                });
    }
    Require(stream.good(), "could not write many-HDU fixture");
}

void WriteManyColumnTable(
    const std::filesystem::path& path,
    std::size_t column_count)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open many-column fixture");
    WriteEmptyPrimary(stream);
    const std::size_t row_width = column_count * sizeof(float);
    std::vector<std::string> cards = {
        FitsCard("XTENSION", "'BINTABLE'"),
        FitsCard("BITPIX", "                   8"),
        FitsCard("NAXIS", "                   2"),
        FitsCard("NAXIS1", std::to_string(row_width)),
        FitsCard("NAXIS2", "                   1"),
        FitsCard("PCOUNT", "                   0"),
        FitsCard("GCOUNT", "                   1"),
        FitsCard("TFIELDS", std::to_string(column_count)),
    };
    cards.reserve(cards.size() + column_count * 2U);
    for (std::size_t index = 1; index <= column_count; ++index) {
        cards.push_back(FitsCard(
            "TTYPE" + std::to_string(index),
            "'VALUE'"));
        cards.push_back(FitsCard(
            "TFORM" + std::to_string(index),
            "'E'"));
    }
    WriteFitsHeader(stream, cards);
    const std::vector<unsigned char> data(row_width, 0U);
    stream.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write many-column fixture");
}

void WriteLargeVectorTable(
    const std::filesystem::path& path,
    std::size_t value_count)
{
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not open large-vector table fixture");
    WriteEmptyPrimary(stream);
    const std::size_t row_width = value_count * sizeof(float);
    WriteFitsHeader(stream, {
                                FitsCard("XTENSION", "'BINTABLE'"),
                                FitsCard("BITPIX", "                   8"),
                                FitsCard("NAXIS", "                   2"),
                                FitsCard("NAXIS1", std::to_string(row_width)),
                                FitsCard("NAXIS2", "                   1"),
                                FitsCard("PCOUNT", "                   0"),
                                FitsCard("GCOUNT", "                   1"),
                                FitsCard("TFIELDS", "                   1"),
                                FitsCard("TTYPE1", "'VALUES'"),
                                FitsCard(
                                    "TFORM1",
                                    "'" + std::to_string(value_count) + "E'"),
                            });
    std::vector<unsigned char> data;
    data.reserve(row_width);
    for (std::size_t index = 0; index < value_count; ++index) {
        AppendBigEndianFloat(data, static_cast<float>(index));
    }
    stream.write(
        reinterpret_cast<const char*>(data.data()),
        static_cast<std::streamsize>(data.size()));
    PadFitsData(stream, data.size());
    Require(stream.good(), "could not write large-vector table fixture");
}

void TestPlainUnicodeAndMemoryBackedOpening(
    const TempDirectory& temporary)
{
    const std::filesystem::path plain_path =
        temporary.path() / "multi_hdu.fits";
    WriteMultiHduFixture(plain_path);

    FitsFile plain = FitsFile::Open(
        plain_path,
        FitsSourceEncoding::Plain);
    Require(plain.hdus().size() == 3U, "plain FITS should expose all HDUs");

    const std::filesystem::path unicode_directory =
        temporary.path() / std::filesystem::path(L"\u5149\u8c31\u8def\u5f84");
    std::error_code error;
    Require(
        std::filesystem::create_directory(unicode_directory, error) &&
            !error,
        "could not create Unicode fixture directory");
    const std::filesystem::path unicode_path =
        unicode_directory / std::filesystem::path(L"\u8bfb\u53d6.fits");
    WritePrimaryFloatImage(unicode_path, 4U, 2U);
    FitsFile unicode = FitsFile::Open(
        unicode_path,
        FitsSourceEncoding::Plain);
    Require(
        unicode.hdus().front().image_axes ==
            std::vector<std::size_t>({4U, 2U}),
        "Unicode FITS path should preserve image dimensions");

    const std::filesystem::path gzip_path =
        temporary.path() / "memory_backed.fits.gz";
    WriteBytes(gzip_path, GzipBytes(ReadBytes(plain_path)));
    FitsFile memory_backed = FitsFile::Open(
        gzip_path,
        FitsSourceEncoding::Gzip);
    error.clear();
    Require(
        std::filesystem::remove(gzip_path, error) && !error,
        "memory-backed FITS should release its compressed source file");
    const std::vector<double> row = memory_backed.ReadImageRow(
        memory_backed.hdus().at(1),
        1U,
        3U);
    Require(
        row == std::vector<double>({14.0, 15.0, 16.0}),
        "read-only memory FITS should remain readable after source removal");
}

void TestMoveLifetimeAndFailureRaii(const TempDirectory& temporary)
{
    const std::filesystem::path path =
        temporary.path() / "repeated_open.fits";
    WritePrimaryFloatImage(path, 8U, 2U);

    for (std::size_t iteration = 0; iteration < 64U; ++iteration) {
        FitsFile original = FitsFile::Open(path, FitsSourceEncoding::Plain);
        FitsFile moved(std::move(original));
        FitsFile replacement = FitsFile::Open(
            path,
            FitsSourceEncoding::Plain);
        replacement = std::move(moved);
        Require(
            replacement.hdus().size() == 1U,
            "moved FITS handle should remain usable");
    }

    const std::filesystem::path failed_path =
        temporary.path() / "failed_open.fits";
    WriteTruncatedHeader(failed_path);
    for (std::size_t iteration = 0; iteration < 32U; ++iteration) {
        RequireFitsError(
            FitsFileErrorCode::InvalidShape,
            [&failed_path]() {
                (void)FitsFile::Open(
                    failed_path,
                    FitsSourceEncoding::Plain);
            },
            "repeated failed FITS open");
    }

    const std::filesystem::path failed_inspection_path =
        temporary.path() / "failed_inspection.fits";
    for (std::size_t iteration = 0; iteration < 32U; ++iteration) {
        WriteInconsistentNumericTable(failed_inspection_path);
        RequireFitsError(
            FitsFileErrorCode::InvalidShape,
            [&failed_inspection_path]() {
                (void)FitsFile::Open(
                    failed_inspection_path,
                    FitsSourceEncoding::Plain);
            },
            "repeated post-open FITS inspection failure");

        std::error_code removal_error;
        Require(
            std::filesystem::remove(
                failed_inspection_path,
                removal_error) &&
                !removal_error,
            "post-open inspection failure should release the CFITSIO handle");
    }

    WritePrimaryFloatImage(failed_path, 3U, 1U);
    {
        FitsFile recovered = FitsFile::Open(
            failed_path,
            FitsSourceEncoding::Plain);
        Require(
            recovered.hdus().front().image_axes.front() == 3U,
            "failed opens should not poison later RAII state");
    }

    std::error_code error;
    Require(
        std::filesystem::remove(path, error) && !error,
        "repeated open/close should release the ordinary FITS file");
    error.clear();
    Require(
        std::filesystem::remove(failed_path, error) && !error,
        "failed-open recovery should release the FITS file");
}

void TestHduAndColumnMatrix(const TempDirectory& temporary)
{
    const std::filesystem::path path =
        temporary.path() / "hdu_column_matrix.fits";
    WriteMultiHduFixture(path);
    FitsFile file = FitsFile::Open(path, FitsSourceEncoding::Plain);
    const std::vector<FitsHdu>& hdus = file.hdus();
    Require(hdus.size() == 3U, "multi-HDU FITS should expose three HDUs");
    Require(
        hdus[0].kind == FitsHduKind::Image &&
            hdus[0].image_axes == std::vector<std::size_t>({2U, 2U}) &&
            hdus[0].image_type == FitsNumericType::Float32,
        "primary image metadata should be inspected");
    Require(
        hdus[1].kind == FitsHduKind::Image &&
            hdus[1].image_axes == std::vector<std::size_t>({3U, 2U}),
        "image extension metadata should be inspected");
    Require(
        hdus[2].kind == FitsHduKind::BinaryTable &&
            hdus[2].row_count == 2U &&
            hdus[2].columns.size() == 3U,
        "binary table metadata should be inspected");

    const FitsColumn& scalar = hdus[2].columns[0];
    const FitsColumn& vector = hdus[2].columns[1];
    const FitsColumn& text = hdus[2].columns[2];
    Require(
        scalar.normalized_name == "SCALAR" && scalar.repeat == 1U &&
            scalar.numeric_type == FitsNumericType::SignedInt32,
        "fixed scalar column should preserve numeric shape");
    Require(
        vector.normalized_name == "VECTOR" && vector.repeat == 3U &&
            vector.numeric_type == FitsNumericType::Float32,
        "fixed vector column should preserve numeric shape");
    Require(
        text.normalized_name == "TEXT" && !text.numeric_type,
        "non-numeric column should remain discoverable but unsupported");

    Require(
        file.ReadImageRow(hdus[0], 1U, 2U) ==
            std::vector<double>({3.0, 4.0}),
        "primary image row should be readable");
    Require(
        file.ReadImageRow(hdus[1], 1U, 3U) ==
            std::vector<double>({14.0, 15.0, 16.0}),
        "image extension row should be readable");
    Require(
        file.ReadColumnVector(hdus[2], scalar, 0U, true) ==
            std::vector<double>({7.0, 9.0}),
        "fixed scalar column should read across rows");
    Require(
        file.ReadColumnVector(hdus[2], vector, 1U, false) ==
            std::vector<double>({4.0, 5.0, 6.0}),
        "fixed vector column should read the selected row");
    RequireFitsError(
        FitsFileErrorCode::UnsupportedFormat,
        [&file, &hdus, &text]() {
            (void)file.ReadColumnVector(hdus[2], text, 0U, false);
        },
        "non-numeric FITS column read");
}

void TestUnsupportedColumnAndShapeMatrix(
    const TempDirectory& temporary)
{
    const std::filesystem::path variable_path =
        temporary.path() / "variable_length.fits";
    WriteSingleColumnTable(variable_path, "1PE(3)", 8U, 12U);
    FitsFile variable = FitsFile::Open(
        variable_path,
        FitsSourceEncoding::Plain);
    const FitsHdu& variable_hdu = variable.hdus().at(1);
    Require(
        !variable_hdu.columns.front().numeric_type,
        "variable-length array should not be exposed as fixed numeric data");
    RequireFitsError(
        FitsFileErrorCode::UnsupportedFormat,
        [&variable, &variable_hdu]() {
            (void)variable.ReadColumnVector(
                variable_hdu,
                variable_hdu.columns.front(),
                0U,
                false);
        },
        "variable-length FITS array read");

    const std::filesystem::path text_path =
        temporary.path() / "text_column.fits";
    WriteSingleColumnTable(text_path, "8A", 8U);
    FitsFile text = FitsFile::Open(text_path, FitsSourceEncoding::Plain);
    const FitsHdu& text_hdu = text.hdus().at(1);
    Require(
        !text_hdu.columns.front().numeric_type,
        "text column should not be exposed as numeric data");
    RequireFitsError(
        FitsFileErrorCode::UnsupportedFormat,
        [&text, &text_hdu]() {
            (void)text.ReadColumnVector(
                text_hdu,
                text_hdu.columns.front(),
                0U,
                false);
        },
        "text FITS column read");

    const std::filesystem::path mismatch_path =
        temporary.path() / "inconsistent_repeat_shape.fits";
    WriteInconsistentNumericTable(mismatch_path);
    RequireFitsError(
        FitsFileErrorCode::InvalidShape,
        [&mismatch_path]() {
            (void)FitsFile::Open(
                mismatch_path,
                FitsSourceEncoding::Plain);
        },
        "inconsistent fixed-column repeat and row shape");
}

void TestOptionalUndefinedKeywords(const TempDirectory& temporary)
{
    const std::filesystem::path path =
        temporary.path() / "undefined_keywords.fits";
    WritePrimaryFloatImage(path, 2U, 1U);
    FitsFile file = FitsFile::Open(path, FitsSourceEncoding::Plain);
    const FitsHdu& hdu = file.hdus().front();
    Require(
        !file.ReadKeywordString(hdu, "OPTION"),
        "undefined string keyword should be absent");
    Require(
        !file.ReadKeywordInteger(hdu, "COUNT"),
        "undefined integer keyword should be absent");
    Require(
        !file.ReadKeywordDouble(hdu, "SCALE"),
        "undefined floating-point keyword should be absent");
}

void TestOpenAndResourceErrorMatrix(const TempDirectory& temporary)
{
    const std::filesystem::path empty_path = temporary.path() / "empty.fits";
    WriteBytes(empty_path, {});
    RequireFitsError(
        FitsFileErrorCode::InvalidShape,
        [&empty_path]() {
            (void)FitsFile::Open(empty_path, FitsSourceEncoding::Plain);
        },
        "empty FITS input");

    const std::filesystem::path non_fits_path =
        temporary.path() / "not_fits.fits";
    WriteBytes(non_fits_path, std::vector<unsigned char>(2880U, 'x'));
    RequireFitsError(
        FitsFileErrorCode::OpenFailed,
        [&non_fits_path]() {
            (void)FitsFile::Open(
                non_fits_path,
                FitsSourceEncoding::Plain);
        },
        "non-FITS input");

    const std::filesystem::path header_path =
        temporary.path() / "truncated_header.fits";
    WriteTruncatedHeader(header_path);
    RequireFitsError(
        FitsFileErrorCode::InvalidShape,
        [&header_path]() {
            (void)FitsFile::Open(
                header_path,
                FitsSourceEncoding::Plain);
        },
        "truncated FITS header");

    const std::filesystem::path data_path =
        temporary.path() / "truncated_data.fits";
    WriteTruncatedImageData(data_path);
    RequireFitsError(
        FitsFileErrorCode::InvalidShape,
        [&data_path]() {
            (void)FitsFile::Open(data_path, FitsSourceEncoding::Plain);
        },
        "truncated FITS data");

    const std::filesystem::path corrupt_gzip_path =
        temporary.path() / "corrupt.fits.gz";
    WriteBytes(corrupt_gzip_path, {'n', 'o', 't', '-', 'g', 'z', 'i', 'p'});
    RequireFitsError(
        FitsFileErrorCode::OpenFailed,
        [&corrupt_gzip_path]() {
            (void)FitsFile::Open(
                corrupt_gzip_path,
                FitsSourceEncoding::Gzip);
        },
        "corrupt gzip FITS input");

    const std::filesystem::path plain_path =
        temporary.path() / "resource_limit.fits";
    WritePrimaryFloatImage(plain_path, 256U, 4U);
    const std::vector<unsigned char> compressed =
        GzipBytes(ReadBytes(plain_path));
    const std::filesystem::path gzip_path =
        temporary.path() / "resource_limit.fits.gz";
    WriteBytes(gzip_path, compressed);
    RequireFitsError(
        FitsFileErrorCode::UnsupportedFormat,
        [&gzip_path, &compressed]() {
            (void)FitsFile::Open(
                gzip_path,
                FitsSourceEncoding::Gzip,
                compressed.size() - 1U);
        },
        "compressed FITS size limit");
    const std::size_t inflated_size = ReadBytes(plain_path).size();
    RequireFitsError(
        FitsFileErrorCode::UnsupportedFormat,
        [&gzip_path, inflated_size]() {
            (void)FitsFile::Open(
                gzip_path,
                FitsSourceEncoding::Gzip,
                spectiary::detail::kMaxSynchronousFitsFileBytes,
                inflated_size - 1U);
        },
        "inflated FITS size limit");
}

void TestMissingAddressErrorMatrix(const TempDirectory& temporary)
{
    const std::filesystem::path path =
        temporary.path() / "missing_addresses.fits";
    WriteMultiHduFixture(path);
    FitsFile file = FitsFile::Open(path, FitsSourceEncoding::Plain);
    const FitsHdu& image_hdu = file.hdus().front();
    const FitsHdu& table_hdu = file.hdus().at(2);

    FitsHdu missing_hdu = image_hdu;
    missing_hdu.index = 999U;
    RequireFitsError(
        FitsFileErrorCode::InvalidShape,
        [&file, &missing_hdu]() {
            (void)file.ReadKeywordString(missing_hdu, "SIMPLE");
        },
        "missing FITS HDU request");
    RequireFitsError(
        FitsFileErrorCode::InvalidShape,
        [&file, &table_hdu]() {
            (void)file.ReadColumnVector(
                table_hdu,
                table_hdu.columns.at(1),
                table_hdu.row_count,
                false);
        },
        "missing FITS row request");

    FitsColumn missing_column = table_hdu.columns.at(1);
    missing_column.index = 999U;
    RequireFitsError(
        FitsFileErrorCode::InvalidShape,
        [&file, &table_hdu, &missing_column]() {
            (void)file.ReadColumnVector(
                table_hdu,
                missing_column,
                0U,
                false);
        },
        "missing FITS column request");
}

void TestCfitsioErrorStackIsolation(const TempDirectory& temporary)
{
    const std::filesystem::path path =
        temporary.path() / "error_stack_isolation.fits";
    WritePrimaryFloatImage(path, 2U, 1U);
    FitsFile file = FitsFile::Open(path, FitsSourceEncoding::Plain);
    FitsHdu missing_hdu = file.hdus().front();
    missing_hdu.index = 999U;

    constexpr char kForeignStackEntry[] =
        "Spectiary foreign CFITSIO error-stack sentinel";
    fits_clear_errmsg();
    ffpmsg(kForeignStackEntry);

    bool threw = false;
    try {
        (void)file.ReadKeywordString(missing_hdu, "SIMPLE");
    } catch (const FitsFileError& error) {
        threw = true;
        Require(
            error.code() == FitsFileErrorCode::InvalidShape,
            "[SF-FITS-CFITSIO-ERROR-STACK] unexpected FITS error category");
        Require(
            std::string(error.what()).find(kForeignStackEntry) ==
                std::string::npos,
            "[SF-FITS-CFITSIO-ERROR-STACK] exception included a foreign CFITSIO error-stack entry");
    }
    Require(
        threw,
        "[SF-FITS-CFITSIO-ERROR-STACK] expected a FITS error");

    std::array<char, FLEN_ERRMSG> retained_entry = {};
    Require(
        fits_read_errmsg(retained_entry.data()) != 0 &&
            std::string(retained_entry.data()) == kForeignStackEntry,
        "[SF-FITS-CFITSIO-ERROR-STACK] exception formatting consumed the global CFITSIO error stack");
    fits_clear_errmsg();
}

void TestCancellationAcrossReaderStages(const TempDirectory& temporary)
{
    const std::filesystem::path reader_source =
        std::filesystem::path(SPECTIARY_SOURCE_DIR) /
        "src/domain/fits_file_reader.cpp";
    const std::filesystem::path large_image_path =
        temporary.path() / "large_image.fits";
    constexpr std::size_t kLargeValueCount = 200'000U;
    WritePrimaryFloatImage(large_image_path, kLargeValueCount, 1U);

    const std::filesystem::path gzip_path =
        temporary.path() / "cancel_inflate.fits.gz";
    WriteBytes(gzip_path, GzipBytes(ReadBytes(large_image_path)));
    CancellationStageProbe gzip_probe(SourceStage::Between(
        reader_source,
        "std::vector<unsigned char> InflateGzipFile(",
        "const int result = inflate(",
        "const std::size_t produced ="));
    RequireFitsError(
        FitsFileErrorCode::Canceled,
        [&gzip_path, &gzip_probe]() {
            (void)FitsFile::Open(
                gzip_path,
                FitsSourceEncoding::Gzip,
                spectiary::detail::kMaxSynchronousFitsFileBytes,
                spectiary::detail::kMaxSynchronousInflatedFitsBytes,
                [&gzip_probe]() { return gzip_probe.Poll(); });
        },
        "gzip inflate cancellation");
    Require(
        gzip_probe.stage_observed(),
        "gzip cancellation probe should reach the inflate pipeline");

    const std::filesystem::path many_hdu_path =
        temporary.path() / "cancel_hdu_enumeration.fits";
    WriteManyHdus(many_hdu_path, 256U);
    CancellationStageProbe hdu_probe(SourceStage::Between(
        reader_source,
        "std::vector<FitsHdu> InspectHdus(",
        "fits_movabs_hdu(",
        "if (status == END_OF_FILE)"));
    RequireFitsError(
        FitsFileErrorCode::Canceled,
        [&many_hdu_path, &hdu_probe]() {
            (void)FitsFile::Open(
                many_hdu_path,
                FitsSourceEncoding::Plain,
                spectiary::detail::kMaxSynchronousFitsFileBytes,
                spectiary::detail::kMaxSynchronousInflatedFitsBytes,
                [&hdu_probe]() { return hdu_probe.Poll(); });
        },
        "HDU enumeration cancellation");
    Require(
        hdu_probe.stage_observed(),
        "HDU enumeration should deliver cooperative cancellation");

    const std::filesystem::path many_column_path =
        temporary.path() / "cancel_column_enumeration.fits";
    WriteManyColumnTable(many_column_path, 256U);
    CancellationStageProbe column_discovery_probe(SourceStage::Between(
        reader_source,
        "std::vector<FitsHdu> InspectHdus(",
        "fits_get_coltypell(",
        "if (status != 0)"));
    RequireFitsError(
        FitsFileErrorCode::Canceled,
        [&many_column_path, &column_discovery_probe]() {
            (void)FitsFile::Open(
                many_column_path,
                FitsSourceEncoding::Plain,
                spectiary::detail::kMaxSynchronousFitsFileBytes,
                spectiary::detail::kMaxSynchronousInflatedFitsBytes,
                [&column_discovery_probe]() {
                    return column_discovery_probe.Poll();
                });
        },
        "large-column enumeration cancellation");
    Require(
        column_discovery_probe.stage_observed(),
        "column enumeration should deliver cooperative cancellation");

    const std::filesystem::path large_table_path =
        temporary.path() / "cancel_table_read.fits";
    WriteLargeVectorTable(large_table_path, kLargeValueCount);
    FitsFile table = FitsFile::Open(
        large_table_path,
        FitsSourceEncoding::Plain);
    const FitsHdu& table_hdu = table.hdus().at(1);
    CancellationStageProbe table_probe(SourceStage::Between(
        reader_source,
        "void ReadColumnChunk(",
        "fits_read_col(",
        "if (status != 0)"));
    RequireFitsError(
        FitsFileErrorCode::Canceled,
        [&table, &table_hdu, &table_probe]() {
            (void)table.ReadColumnVector(
                table_hdu,
                table_hdu.columns.front(),
                0U,
                false,
                [&table_probe]() { return table_probe.Poll(); });
        },
        "large table numeric read cancellation");
    Require(
        table_probe.stage_observed(),
        "large table read should deliver cooperative cancellation");

    FitsFile image = FitsFile::Open(
        large_image_path,
        FitsSourceEncoding::Plain);
    CancellationStageProbe image_probe(SourceStage::Between(
        reader_source,
        "std::vector<double> FitsFile::ReadImageRow(",
        "fits_read_img(",
        "if (status != 0)"));
    RequireFitsError(
        FitsFileErrorCode::Canceled,
        [&image, &image_probe]() {
            (void)image.ReadImageRow(
                image.hdus().front(),
                0U,
                kLargeValueCount,
                [&image_probe]() { return image_probe.Poll(); });
        },
        "large image row cancellation");
    Require(
        image_probe.stage_observed(),
        "large image row read should deliver cooperative cancellation");
}

}  // namespace

int main()
{
    try {
        TestCfitsioReentrantBuild();
        const TempDirectory temporary;
        TestPlainUnicodeAndMemoryBackedOpening(temporary);
        TestMoveLifetimeAndFailureRaii(temporary);
        TestHduAndColumnMatrix(temporary);
        TestUnsupportedColumnAndShapeMatrix(temporary);
        TestOptionalUndefinedKeywords(temporary);
        TestOpenAndResourceErrorMatrix(temporary);
        TestMissingAddressErrorMatrix(temporary);
        TestCfitsioErrorStackIsolation(temporary);
        TestCancellationAcrossReaderStages(temporary);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
