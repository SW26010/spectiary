#include "domain/fits_file_reader.h"

#include "domain/spectrum_loader_support.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <fitsio.h>
#include <zlib.h>

namespace specforge::detail {

struct FitsFile::Impl {
    fitsfile* handle = nullptr;
    std::vector<unsigned char> memory;
    void* memory_address = nullptr;
    std::size_t memory_size = 0;
    std::vector<FitsHdu> hdus;

    ~Impl()
    {
        if (handle != nullptr) {
            int status = 0;
            fits_close_file(handle, &status);
        }
    }
};

namespace {

constexpr std::size_t kTransportChunkBytes = 64U * 1024U;
constexpr std::size_t kCfitsioReadChunkElements = 8192U;

void ThrowIfCanceled(
    const std::function<bool()>& cancellation_requested)
{
    if (cancellation_requested && cancellation_requested()) {
        throw FitsFileError(
            FitsFileErrorCode::Canceled,
            "FITS loading was canceled.");
    }
}

class InflateEndGuard {
public:
    explicit InflateEndGuard(z_stream& stream)
        : stream_(&stream)
    {
    }

    ~InflateEndGuard()
    {
        inflateEnd(stream_);
    }

    InflateEndGuard(const InflateEndGuard&) = delete;
    InflateEndGuard& operator=(const InflateEndGuard&) = delete;

private:
    z_stream* stream_;
};

std::string TrimAscii(std::string value)
{
    const auto first = std::find_if_not(
        value.begin(),
        value.end(),
        [](unsigned char character) {
            return std::isspace(character) != 0;
        });
    const auto last = std::find_if_not(
        value.rbegin(),
        value.rend(),
        [](unsigned char character) {
            return std::isspace(character) != 0;
        }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string NormalizedColumnName(std::string value)
{
    value = UpperAscii(TrimAscii(std::move(value)));
    std::string normalized;
    normalized.reserve(value.size());
    for (const char character : value) {
        if (std::isalnum(
                static_cast<unsigned char>(character)) != 0) {
            normalized.push_back(character);
        }
    }
    return normalized;
}

std::string CfitsioFailureMessage(
    std::string_view stable_message,
    int status)
{
    std::array<char, FLEN_STATUS> status_text = {};
    fits_get_errstatus(status, status_text.data());

    std::string message(stable_message);
    message += " (CFITSIO status ";
    message += std::to_string(status);
    if (status_text.front() != '\0') {
        message += ": ";
        message += status_text.data();
    }

    message += ")";
    return message;
}

[[noreturn]] void ThrowCfitsioError(
    FitsFileErrorCode code,
    std::string_view message,
    int status)
{
    throw FitsFileError(
        code,
        CfitsioFailureMessage(message, status));
}

bool IsCfitsioTruncationStatus(int status)
{
    return status == END_OF_FILE ||
        status == READ_ERROR ||
        status == NO_END;
}

std::uintmax_t InspectSourceSize(
    const std::filesystem::path& path,
    std::uintmax_t max_file_bytes,
    const std::function<bool()>& cancellation_requested)
{
    ThrowIfCanceled(cancellation_requested);
    std::error_code size_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(path, size_error);
    if (size_error) {
        throw FitsFileError(
            FitsFileErrorCode::OpenFailed,
            "Could not inspect the FITS file size.");
    }
    if (file_size > max_file_bytes) {
        throw FitsFileError(
            FitsFileErrorCode::UnsupportedFormat,
            "FITS file is too large for the synchronous single-spectrum loader.");
    }
    ThrowIfCanceled(cancellation_requested);
    return file_size;
}

std::vector<unsigned char> InflateGzipFile(
    const std::filesystem::path& path,
    std::uintmax_t compressed_size,
    std::size_t max_inflated_bytes,
    const std::function<bool()>& cancellation_requested)
{
    ThrowIfCanceled(cancellation_requested);
    if (compressed_size == 0) {
        throw FitsFileError(
            FitsFileErrorCode::OpenFailed,
            "Compressed FITS file is empty.");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw FitsFileError(
            FitsFileErrorCode::OpenFailed,
            "Could not open the compressed FITS file.");
    }

    z_stream stream = {};
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
        throw FitsFileError(
            FitsFileErrorCode::OpenFailed,
            "Could not initialize gzip decompression.");
    }
    const InflateEndGuard inflate_guard(stream);

    std::array<unsigned char, kTransportChunkBytes> input_buffer = {};
    std::array<unsigned char, kTransportChunkBytes> output_buffer = {};
    std::vector<unsigned char> output;
    bool input_finished = false;
    bool member_finished = false;

    for (;;) {
        ThrowIfCanceled(cancellation_requested);
        if (stream.avail_in == 0 && !input_finished) {
            input.read(
                reinterpret_cast<char*>(input_buffer.data()),
                static_cast<std::streamsize>(input_buffer.size()));
            const std::streamsize bytes_read = input.gcount();
            if (input.bad()) {
                throw FitsFileError(
                    FitsFileErrorCode::OpenFailed,
                    "Could not read the compressed FITS file.");
            }
            if (bytes_read == 0) {
                if (!input.eof()) {
                    throw FitsFileError(
                        FitsFileErrorCode::OpenFailed,
                        "Could not read the compressed FITS file.");
                }
                input_finished = true;
            } else {
                stream.next_in = reinterpret_cast<Bytef*>(
                    input_buffer.data());
                stream.avail_in = static_cast<uInt>(bytes_read);
                input_finished = input.eof();
            }
        }

        if (member_finished) {
            if (stream.avail_in == 0 && input_finished) {
                break;
            }

            Bytef* const remaining_input = stream.next_in;
            const uInt remaining_count = stream.avail_in;
            if (inflateReset2(&stream, MAX_WBITS + 16) != Z_OK) {
                throw FitsFileError(
                    FitsFileErrorCode::OpenFailed,
                    "Could not continue gzip decompression.");
            }
            stream.next_in = remaining_input;
            stream.avail_in = remaining_count;
            member_finished = false;
        }

        if (stream.avail_in == 0 && input_finished) {
            throw FitsFileError(
                FitsFileErrorCode::OpenFailed,
                "Could not decompress the gzip FITS file.");
        }

        stream.next_out = reinterpret_cast<Bytef*>(
            output_buffer.data());
        stream.avail_out = static_cast<uInt>(output_buffer.size());
        const int result = inflate(&stream, Z_NO_FLUSH);
        ThrowIfCanceled(cancellation_requested);

        const std::size_t produced =
            output_buffer.size() - stream.avail_out;
        if (produced > max_inflated_bytes ||
            output.size() > max_inflated_bytes - produced) {
            throw FitsFileError(
                FitsFileErrorCode::UnsupportedFormat,
                "Gzipped FITS expands beyond the synchronous single-spectrum loader limit.");
        }
        output.insert(
            output.end(),
            output_buffer.begin(),
            output_buffer.begin() +
                static_cast<std::ptrdiff_t>(produced));

        if (result == Z_STREAM_END) {
            member_finished = true;
            continue;
        }
        if (result != Z_OK ||
            (input_finished && stream.avail_in == 0 && produced == 0)) {
            throw FitsFileError(
                FitsFileErrorCode::OpenFailed,
                "Could not decompress the gzip FITS file.");
        }
    }

    ThrowIfCanceled(cancellation_requested);
    if (output.empty()) {
        throw FitsFileError(
            FitsFileErrorCode::OpenFailed,
            "Compressed FITS file contains no data.");
    }
    return output;
}

std::size_t CheckedSize(
    LONGLONG value,
    std::string_view invalid_message)
{
    if (value < 0 ||
        static_cast<unsigned long long>(value) >
            static_cast<unsigned long long>(
                std::numeric_limits<std::size_t>::max())) {
        throw FitsFileError(
            FitsFileErrorCode::InvalidShape,
            std::string(invalid_message));
    }
    return static_cast<std::size_t>(value);
}

LONGLONG CheckedCfitsioIndex(
    std::size_t value,
    std::string_view invalid_message)
{
    if (value > static_cast<std::size_t>(
                    std::numeric_limits<LONGLONG>::max())) {
        throw FitsFileError(
            FitsFileErrorCode::InvalidShape,
            std::string(invalid_message));
    }
    return static_cast<LONGLONG>(value);
}

std::optional<FitsNumericType> NumericTypeForImage(int bitpix)
{
    switch (bitpix) {
    case BYTE_IMG:
        return FitsNumericType::UnsignedByte;
    case SHORT_IMG:
        return FitsNumericType::SignedInt16;
    case LONG_IMG:
        return FitsNumericType::SignedInt32;
    case LONGLONG_IMG:
        return FitsNumericType::SignedInt64;
    case FLOAT_IMG:
        return FitsNumericType::Float32;
    case DOUBLE_IMG:
        return FitsNumericType::Float64;
    default:
        return std::nullopt;
    }
}

std::optional<FitsNumericType> NumericTypeForColumn(
    int type_code)
{
    if (type_code < 0) {
        return std::nullopt;
    }
    switch (type_code) {
    case TBYTE:
        return FitsNumericType::UnsignedByte;
    case TSHORT:
        return FitsNumericType::SignedInt16;
    case TINT:
    case TLONG:
        return FitsNumericType::SignedInt32;
    case TLONGLONG:
        return FitsNumericType::SignedInt64;
    case TFLOAT:
        return FitsNumericType::Float32;
    case TDOUBLE:
        return FitsNumericType::Float64;
    default:
        return std::nullopt;
    }
}

FitsHduKind HduKindFromCfitsio(int hdu_type)
{
    switch (hdu_type) {
    case IMAGE_HDU:
        return FitsHduKind::Image;
    case BINARY_TBL:
        return FitsHduKind::BinaryTable;
    case ASCII_TBL:
        return FitsHduKind::AsciiTable;
    default:
        throw FitsFileError(
            FitsFileErrorCode::UnsupportedFormat,
            "FITS HDU type is not supported.");
    }
}

void MoveToHdu(
    fitsfile* handle,
    const FitsHdu& hdu,
    const std::function<bool()>& cancellation_requested)
{
    ThrowIfCanceled(cancellation_requested);
    if (hdu.index >= static_cast<std::size_t>(
                         std::numeric_limits<int>::max())) {
        throw FitsFileError(
            FitsFileErrorCode::InvalidShape,
            "FITS HDU index is not addressable.");
    }
    int status = 0;
    int hdu_type = 0;
    fits_movabs_hdu(
        handle,
        static_cast<int>(hdu.index + 1U),
        &hdu_type,
        &status);
    ThrowIfCanceled(cancellation_requested);
    if (status != 0) {
        ThrowCfitsioError(
            FitsFileErrorCode::InvalidShape,
            "Could not select the requested FITS HDU.",
            status);
    }
}

std::optional<std::string> ReadOptionalStringKey(
    fitsfile* handle,
    std::string_view key,
    FitsFileErrorCode failure_code,
    std::string_view failure_message,
    const std::function<bool()>& cancellation_requested)
{
    ThrowIfCanceled(cancellation_requested);
    std::array<char, FLEN_VALUE> value = {};
    const std::string key_name(key);
    int status = 0;
    fits_read_key(
        handle,
        TSTRING,
        key_name.c_str(),
        value.data(),
        nullptr,
        &status);
    ThrowIfCanceled(cancellation_requested);
    if (status == KEY_NO_EXIST || status == VALUE_UNDEFINED) {
        return std::nullopt;
    }
    if (status != 0) {
        ThrowCfitsioError(failure_code, failure_message, status);
    }
    return TrimAscii(value.data());
}

template <typename T>
std::optional<T> ReadOptionalTypedKey(
    fitsfile* handle,
    int data_type,
    std::string_view key,
    std::string_view failure_message,
    const std::function<bool()>& cancellation_requested)
{
    ThrowIfCanceled(cancellation_requested);
    T value = {};
    const std::string key_name(key);
    int status = 0;
    fits_read_key(
        handle,
        data_type,
        key_name.c_str(),
        &value,
        nullptr,
        &status);
    ThrowIfCanceled(cancellation_requested);
    if (status == KEY_NO_EXIST || status == VALUE_UNDEFINED) {
        return std::nullopt;
    }
    if (status != 0) {
        ThrowCfitsioError(
            FitsFileErrorCode::InvalidShape,
            failure_message,
            status);
    }
    return value;
}

std::uintmax_t ValidateCurrentHduRange(
    fitsfile* handle,
    std::uintmax_t source_length,
    const std::function<bool()>& cancellation_requested)
{
    ThrowIfCanceled(cancellation_requested);
    LONGLONG header_start = 0;
    LONGLONG data_start = 0;
    LONGLONG data_end = 0;
    int status = 0;
    fits_get_hduaddrll(
        handle,
        &header_start,
        &data_start,
        &data_end,
        &status);
    ThrowIfCanceled(cancellation_requested);
    if (status != 0) {
        ThrowCfitsioError(
            FitsFileErrorCode::InvalidShape,
            "Could not inspect the FITS HDU storage range.",
            status);
    }
    if (header_start < 0 || data_start < header_start ||
        data_end < data_start) {
        throw FitsFileError(
            FitsFileErrorCode::InvalidShape,
            "FITS HDU has an invalid storage range.");
    }
    const std::uintmax_t expected_end =
        static_cast<std::uintmax_t>(data_end);
    if (expected_end > source_length) {
        throw FitsFileError(
            FitsFileErrorCode::InvalidShape,
            "FITS HDU data is truncated.");
    }
    return expected_end;
}

std::vector<FitsHdu> InspectHdus(
    fitsfile* handle,
    std::uintmax_t source_length,
    const std::function<bool()>& cancellation_requested)
{
    std::vector<FitsHdu> hdus;
    std::uintmax_t last_hdu_end = 0;
    for (int hdu_number = 1;; ++hdu_number) {
        ThrowIfCanceled(cancellation_requested);
        int hdu_type = 0;
        int status = 0;
        fits_movabs_hdu(
            handle,
            hdu_number,
            &hdu_type,
            &status);
        ThrowIfCanceled(cancellation_requested);
        if (status == END_OF_FILE) {
            if (!hdus.empty() && last_hdu_end != source_length) {
                throw FitsFileError(
                    FitsFileErrorCode::InvalidShape,
                    "FITS source contains data outside its declared HDUs.");
            }
            break;
        }
        if (status != 0) {
            ThrowCfitsioError(
                FitsFileErrorCode::InvalidShape,
                "Could not inspect a FITS HDU.",
                status);
        }

        last_hdu_end = ValidateCurrentHduRange(
            handle,
            source_length,
            cancellation_requested);

        FitsHdu hdu;
        hdu.index = static_cast<std::size_t>(hdu_number - 1);
        hdu.kind = HduKindFromCfitsio(hdu_type);
        if (hdu.kind == FitsHduKind::Image) {
            int axis_count = 0;
            status = 0;
            fits_get_img_dim(handle, &axis_count, &status);
            ThrowIfCanceled(cancellation_requested);
            if (status != 0 || axis_count < 0) {
                if (status != 0) {
                    ThrowCfitsioError(
                        FitsFileErrorCode::InvalidShape,
                        "Could not inspect FITS image dimensions.",
                        status);
                }
                throw FitsFileError(
                    FitsFileErrorCode::InvalidShape,
                    "FITS image has invalid dimensions.");
            }

            std::vector<LONGLONG> axes(
                static_cast<std::size_t>(axis_count));
            if (!axes.empty()) {
                status = 0;
                fits_get_img_sizell(
                    handle,
                    axis_count,
                    axes.data(),
                    &status);
                ThrowIfCanceled(cancellation_requested);
                if (status != 0) {
                    ThrowCfitsioError(
                        FitsFileErrorCode::InvalidShape,
                        "Could not inspect FITS image dimensions.",
                        status);
                }
            }
            hdu.image_axes.reserve(axes.size());
            for (const LONGLONG axis : axes) {
                ThrowIfCanceled(cancellation_requested);
                hdu.image_axes.push_back(CheckedSize(
                    axis,
                    "FITS image has invalid dimensions."));
            }

            int bitpix = 0;
            status = 0;
            fits_get_img_type(handle, &bitpix, &status);
            ThrowIfCanceled(cancellation_requested);
            if (status != 0) {
                ThrowCfitsioError(
                    FitsFileErrorCode::InvalidShape,
                    "Could not inspect the FITS image type.",
                    status);
            }
            hdu.image_type = NumericTypeForImage(bitpix);
        } else if (hdu.kind == FitsHduKind::BinaryTable) {
            LONGLONG row_count = 0;
            status = 0;
            fits_get_num_rowsll(handle, &row_count, &status);
            ThrowIfCanceled(cancellation_requested);
            if (status != 0) {
                ThrowCfitsioError(
                    FitsFileErrorCode::InvalidShape,
                    "Could not inspect FITS table rows.",
                    status);
            }
            hdu.row_count = CheckedSize(
                row_count,
                "FITS binary table has invalid dimensions.");

            const std::optional<LONGLONG> row_width_value =
                ReadOptionalTypedKey<LONGLONG>(
                    handle,
                    TLONGLONG,
                    "NAXIS1",
                    "Could not inspect the FITS table row width.",
                    cancellation_requested);
            if (!row_width_value) {
                throw FitsFileError(
                    FitsFileErrorCode::InvalidShape,
                    "FITS binary table is missing its row width.");
            }
            const std::size_t row_width = CheckedSize(
                *row_width_value,
                "FITS binary table has an invalid row width.");

            int column_count = 0;
            status = 0;
            fits_get_num_cols(handle, &column_count, &status);
            ThrowIfCanceled(cancellation_requested);
            if (status != 0 || column_count < 0) {
                if (status != 0) {
                    ThrowCfitsioError(
                        FitsFileErrorCode::InvalidShape,
                        "Could not inspect FITS table columns.",
                        status);
                }
                throw FitsFileError(
                    FitsFileErrorCode::InvalidShape,
                    "FITS binary table has invalid columns.");
            }
            hdu.columns.reserve(
                static_cast<std::size_t>(column_count));
            std::size_t numeric_row_width = 0;
            bool all_columns_are_fixed_numeric = true;
            for (int column_number = 1;
                 column_number <= column_count;
                 ++column_number) {
                ThrowIfCanceled(cancellation_requested);
                int type_code = 0;
                LONGLONG repeat = 0;
                LONGLONG width = 0;
                status = 0;
                fits_get_coltypell(
                    handle,
                    column_number,
                    &type_code,
                    &repeat,
                    &width,
                    &status);
                ThrowIfCanceled(cancellation_requested);
                if (status != 0) {
                    ThrowCfitsioError(
                        FitsFileErrorCode::InvalidShape,
                        "Could not inspect a FITS table column.",
                        status);
                }

                const std::size_t column_repeat = CheckedSize(
                    repeat,
                    "FITS table column has invalid dimensions.");
                const std::optional<FitsNumericType> numeric_type =
                    NumericTypeForColumn(type_code);
                if (!numeric_type) {
                    all_columns_are_fixed_numeric = false;
                } else {
                    const std::size_t element_width = CheckedSize(
                        width,
                        "FITS table column has an invalid element width.");
                    if (column_repeat == 0 || element_width == 0 ||
                        column_repeat >
                            (std::numeric_limits<std::size_t>::max() -
                             numeric_row_width) /
                                element_width) {
                        throw FitsFileError(
                            FitsFileErrorCode::InvalidShape,
                            "FITS table column layout overflows.");
                    }
                    numeric_row_width += column_repeat * element_width;
                }

                const std::string key =
                    "TTYPE" + std::to_string(column_number);
                const std::string name = ReadOptionalStringKey(
                    handle,
                    key,
                    FitsFileErrorCode::InvalidShape,
                    "Could not read a FITS table column name.",
                    cancellation_requested).value_or(std::string{});
                hdu.columns.push_back(FitsColumn{
                    .index = static_cast<std::size_t>(column_number),
                    .name = name,
                    .normalized_name = NormalizedColumnName(name),
                    .repeat = column_repeat,
                    .numeric_type = numeric_type,
                });
            }
            if (all_columns_are_fixed_numeric &&
                numeric_row_width != row_width) {
                throw FitsFileError(
                    FitsFileErrorCode::InvalidShape,
                    "FITS table column layout does not match its row width.");
            }
        }
        hdus.push_back(std::move(hdu));
    }
    if (hdus.empty()) {
        throw FitsFileError(
            FitsFileErrorCode::UnsupportedFormat,
            "FITS file has no readable HDUs.");
    }
    ThrowIfCanceled(cancellation_requested);
    return hdus;
}

void ReadColumnChunk(
    fitsfile* handle,
    const FitsColumn& column,
    LONGLONG first_row,
    LONGLONG first_element,
    std::size_t count,
    double* output,
    const std::function<bool()>& cancellation_requested)
{
    ThrowIfCanceled(cancellation_requested);
    int status = 0;
    int any_null = 0;
    double null_value = std::numeric_limits<double>::quiet_NaN();
    fits_read_col(
        handle,
        TDOUBLE,
        static_cast<int>(column.index),
        first_row,
        first_element,
        CheckedCfitsioIndex(
            count,
            "FITS table read size is not addressable."),
        &null_value,
        output,
        &any_null,
        &status);
    ThrowIfCanceled(cancellation_requested);
    if (status != 0) {
        ThrowCfitsioError(
            FitsFileErrorCode::InvalidShape,
            "Could not read the requested FITS table values.",
            status);
    }
}

}  // namespace

FitsFileError::FitsFileError(
    FitsFileErrorCode code,
    std::string message)
    : std::runtime_error(std::move(message))
    , code_(code)
{
}

FitsFileErrorCode FitsFileError::code() const noexcept
{
    return code_;
}

FitsFile::FitsFile(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{
}

FitsFile::~FitsFile() = default;
FitsFile::FitsFile(FitsFile&&) noexcept = default;
FitsFile& FitsFile::operator=(FitsFile&&) noexcept = default;

FitsFile FitsFile::Open(
    const std::filesystem::path& path,
    FitsSourceEncoding encoding,
    std::uintmax_t max_file_bytes,
    std::size_t max_inflated_bytes,
    const std::function<bool()>& cancellation_requested)
{
    const std::uintmax_t file_size = InspectSourceSize(
        path,
        max_file_bytes,
        cancellation_requested);
    auto impl = std::make_unique<Impl>();

    if (fits_init_cfitsio() != 0) {
        throw FitsFileError(
            FitsFileErrorCode::OpenFailed,
            "CFITSIO runtime initialization failed.");
    }
    int status = 0;
    if (encoding == FitsSourceEncoding::Gzip) {
        impl->memory = InflateGzipFile(
            path,
            file_size,
            max_inflated_bytes,
            cancellation_requested);
        impl->memory_address = impl->memory.data();
        impl->memory_size = impl->memory.size();
        fits_open_memfile(
            &impl->handle,
            "SpecForge gzip transport",
            READONLY,
            &impl->memory_address,
            &impl->memory_size,
            0,
            nullptr,
            &status);
    } else {
        const std::string path_utf8 = PathToUtf8(path);
        fits_open_diskfile(
            &impl->handle,
            path_utf8.c_str(),
            READONLY,
            &status);
    }
    ThrowIfCanceled(cancellation_requested);
    if (status != 0 || impl->handle == nullptr) {
        if (status == 0) {
            throw FitsFileError(
                FitsFileErrorCode::OpenFailed,
                "CFITSIO could not open the FITS file.");
        }
        const bool truncated = IsCfitsioTruncationStatus(status);
        const FitsFileErrorCode error_code =
            truncated
            ? FitsFileErrorCode::InvalidShape
            : FitsFileErrorCode::OpenFailed;
        ThrowCfitsioError(
            error_code,
            truncated
                ? "The FITS input is truncated."
                : "CFITSIO could not open the FITS file.",
            status);
    }

    impl->hdus = InspectHdus(
        impl->handle,
        encoding == FitsSourceEncoding::Gzip
            ? static_cast<std::uintmax_t>(impl->memory.size())
            : file_size,
        cancellation_requested);
    return FitsFile(std::move(impl));
}

const std::vector<FitsHdu>& FitsFile::hdus() const noexcept
{
    return impl_->hdus;
}

std::optional<std::string> FitsFile::ReadKeywordString(
    const FitsHdu& hdu,
    std::string_view key,
    const std::function<bool()>& cancellation_requested) const
{
    MoveToHdu(impl_->handle, hdu, cancellation_requested);
    return ReadOptionalStringKey(
        impl_->handle,
        key,
        FitsFileErrorCode::InvalidShape,
        "Could not read a FITS keyword.",
        cancellation_requested);
}

std::optional<std::int64_t> FitsFile::ReadKeywordInteger(
    const FitsHdu& hdu,
    std::string_view key,
    const std::function<bool()>& cancellation_requested) const
{
    MoveToHdu(impl_->handle, hdu, cancellation_requested);
    const std::optional<LONGLONG> value =
        ReadOptionalTypedKey<LONGLONG>(
            impl_->handle,
            TLONGLONG,
            key,
            "Could not read an integer FITS keyword.",
            cancellation_requested);
    if (!value) {
        return std::nullopt;
    }
    if (*value < static_cast<LONGLONG>(
                     std::numeric_limits<std::int64_t>::min()) ||
        *value > static_cast<LONGLONG>(
                     std::numeric_limits<std::int64_t>::max())) {
        throw FitsFileError(
            FitsFileErrorCode::InvalidShape,
            "FITS integer keyword is out of range.");
    }
    return static_cast<std::int64_t>(*value);
}

std::optional<double> FitsFile::ReadKeywordDouble(
    const FitsHdu& hdu,
    std::string_view key,
    const std::function<bool()>& cancellation_requested) const
{
    MoveToHdu(impl_->handle, hdu, cancellation_requested);
    return ReadOptionalTypedKey<double>(
        impl_->handle,
        TDOUBLE,
        key,
        "Could not read a numeric FITS keyword.",
        cancellation_requested);
}

std::vector<double> FitsFile::ReadColumnVector(
    const FitsHdu& hdu,
    const FitsColumn& column,
    std::size_t row_index,
    bool scalar_rows,
    const std::function<bool()>& cancellation_requested) const
{
    ThrowIfCanceled(cancellation_requested);
    if (hdu.kind != FitsHduKind::BinaryTable ||
        !column.numeric_type || column.index == 0 ||
        column.index > static_cast<std::size_t>(
                           std::numeric_limits<int>::max())) {
        throw FitsFileError(
            FitsFileErrorCode::UnsupportedFormat,
            "FITS table column type is not supported.");
    }
    if (scalar_rows && column.repeat != 1U) {
        throw FitsFileError(
            FitsFileErrorCode::InvalidShape,
            "FITS scalar-row column has an invalid repeat count.");
    }
    if (!scalar_rows && row_index >= hdu.row_count) {
        throw FitsFileError(
            FitsFileErrorCode::InvalidShape,
            "Requested FITS table row is out of range.");
    }

    MoveToHdu(impl_->handle, hdu, cancellation_requested);
    const std::size_t value_count =
        scalar_rows ? hdu.row_count : column.repeat;
    std::vector<double> values(value_count);
    for (std::size_t offset = 0; offset < value_count;) {
        const std::size_t count = std::min(
            kCfitsioReadChunkElements,
            value_count - offset);
        const std::size_t row = scalar_rows ? offset : row_index;
        const std::size_t element = scalar_rows ? 0U : offset;
        ReadColumnChunk(
            impl_->handle,
            column,
            CheckedCfitsioIndex(
                row + 1U,
                "FITS table row is not addressable."),
            CheckedCfitsioIndex(
                element + 1U,
                "FITS table element is not addressable."),
            count,
            values.data() + offset,
            cancellation_requested);
        offset += count;
    }
    ThrowIfCanceled(cancellation_requested);
    return values;
}

std::vector<double> FitsFile::ReadImageRow(
    const FitsHdu& hdu,
    std::size_t row_index,
    std::size_t column_count,
    const std::function<bool()>& cancellation_requested) const
{
    ThrowIfCanceled(cancellation_requested);
    if (hdu.kind != FitsHduKind::Image ||
        (hdu.image_type != FitsNumericType::Float32 &&
         hdu.image_type != FitsNumericType::Float64)) {
        throw FitsFileError(
            FitsFileErrorCode::UnsupportedFormat,
            "Only float32/float64 FITS image spectra are supported.");
    }
    if (hdu.image_axes.empty() ||
        hdu.image_axes.front() != column_count ||
        hdu.image_axes.size() > 2U) {
        throw FitsFileError(
            FitsFileErrorCode::InvalidShape,
            "FITS image shape does not match the requested spectrum row.");
    }
    const std::size_t row_count =
        hdu.image_axes.size() >= 2U ? hdu.image_axes[1] : 1U;
    if (row_index >= row_count) {
        throw FitsFileError(
            FitsFileErrorCode::InvalidShape,
            "Requested FITS image row is out of range.");
    }
    if (column_count != 0 &&
        row_index >
            (std::numeric_limits<std::size_t>::max() - 1U) /
                column_count) {
        throw FitsFileError(
            FitsFileErrorCode::InvalidShape,
            "FITS image row offset overflows.");
    }

    MoveToHdu(impl_->handle, hdu, cancellation_requested);
    std::vector<double> values(column_count);
    const std::size_t row_offset = row_index * column_count;
    for (std::size_t offset = 0; offset < column_count;) {
        ThrowIfCanceled(cancellation_requested);
        const std::size_t count = std::min(
            kCfitsioReadChunkElements,
            column_count - offset);
        int status = 0;
        int any_null = 0;
        double null_value = std::numeric_limits<double>::quiet_NaN();
        fits_read_img(
            impl_->handle,
            TDOUBLE,
            CheckedCfitsioIndex(
                row_offset + offset + 1U,
                "FITS image offset is not addressable."),
            CheckedCfitsioIndex(
                count,
                "FITS image read size is not addressable."),
            &null_value,
            values.data() + offset,
            &any_null,
            &status);
        ThrowIfCanceled(cancellation_requested);
        if (status != 0) {
            ThrowCfitsioError(
                FitsFileErrorCode::InvalidShape,
                "Could not read the requested FITS image values.",
                status);
        }
        offset += count;
    }
    ThrowIfCanceled(cancellation_requested);
    return values;
}

}  // namespace specforge::detail
