#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace specforge::detail {

constexpr std::uintmax_t kMaxSynchronousFitsFileBytes =
    64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxSynchronousInflatedFitsBytes =
    64ULL * 1024ULL * 1024ULL;

enum class FitsFileErrorCode {
    OpenFailed,
    UnsupportedFormat,
    InvalidShape,
    Canceled,
};

class FitsFileError : public std::runtime_error {
public:
    FitsFileError(FitsFileErrorCode code, std::string message);

    [[nodiscard]] FitsFileErrorCode code() const noexcept;

private:
    FitsFileErrorCode code_;
};

enum class FitsSourceEncoding {
    Plain,
    Gzip,
};

enum class FitsHduKind {
    Image,
    BinaryTable,
    AsciiTable,
};

enum class FitsNumericType {
    UnsignedByte,
    SignedInt16,
    SignedInt32,
    SignedInt64,
    Float32,
    Float64,
};

struct FitsColumn {
    std::size_t index = 0;
    std::string name;
    std::string normalized_name;
    std::size_t repeat = 1;
    std::optional<FitsNumericType> numeric_type;
};

struct FitsHdu {
    std::size_t index = 0;
    FitsHduKind kind = FitsHduKind::Image;
    std::size_t row_count = 0;
    std::vector<std::size_t> image_axes;
    std::optional<FitsNumericType> image_type;
    std::vector<FitsColumn> columns;
};

class FitsFile {
public:
    static FitsFile Open(
        const std::filesystem::path& path,
        FitsSourceEncoding encoding,
        std::uintmax_t max_file_bytes = kMaxSynchronousFitsFileBytes,
        std::size_t max_inflated_bytes =
            kMaxSynchronousInflatedFitsBytes,
        const std::function<bool()>& cancellation_requested = {});

    ~FitsFile();

    FitsFile(FitsFile&&) noexcept;
    FitsFile& operator=(FitsFile&&) noexcept;

    FitsFile(const FitsFile&) = delete;
    FitsFile& operator=(const FitsFile&) = delete;

    [[nodiscard]] const std::vector<FitsHdu>& hdus() const noexcept;

    [[nodiscard]] std::optional<std::string> ReadKeywordString(
        const FitsHdu& hdu,
        std::string_view key,
        const std::function<bool()>& cancellation_requested = {}) const;
    [[nodiscard]] std::optional<std::int64_t> ReadKeywordInteger(
        const FitsHdu& hdu,
        std::string_view key,
        const std::function<bool()>& cancellation_requested = {}) const;
    [[nodiscard]] std::optional<double> ReadKeywordDouble(
        const FitsHdu& hdu,
        std::string_view key,
        const std::function<bool()>& cancellation_requested = {}) const;

    [[nodiscard]] std::vector<double> ReadColumnVector(
        const FitsHdu& hdu,
        const FitsColumn& column,
        std::size_t row_index,
        bool scalar_rows,
        const std::function<bool()>& cancellation_requested = {}) const;

    [[nodiscard]] std::vector<double> ReadImageRow(
        const FitsHdu& hdu,
        std::size_t row_index,
        std::size_t column_count,
        const std::function<bool()>& cancellation_requested = {}) const;

private:
    struct Impl;

    explicit FitsFile(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace specforge::detail
