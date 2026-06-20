#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace specforge::detail {

constexpr std::uintmax_t kMaxSynchronousFitsFileBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxSynchronousInflatedFitsBytes = 64ULL * 1024ULL * 1024ULL;

enum class FitsFileErrorCode {
    OpenFailed,
    UnsupportedFormat,
    InvalidShape,
};

class FitsFileError : public std::runtime_error {
public:
    FitsFileError(FitsFileErrorCode code, std::string message);

    [[nodiscard]] FitsFileErrorCode code() const noexcept;

private:
    FitsFileErrorCode code_;
};

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

std::vector<unsigned char> ReadFitsFileBytes(
    const std::filesystem::path& path,
    std::uintmax_t max_bytes = kMaxSynchronousFitsFileBytes);

std::vector<unsigned char> DecompressGzipFitsBytes(
    const std::vector<unsigned char>& compressed,
    std::size_t max_inflated_bytes = kMaxSynchronousInflatedFitsBytes);

std::vector<FitsHdu> ParseFitsHdus(const std::vector<unsigned char>& bytes);

std::optional<std::string> FitsValue(const FitsHeader& header, std::string_view key);
std::int64_t FitsInteger(const FitsHeader& header, std::string_view key, std::int64_t default_value = 0);
std::optional<double> FitsDouble(const FitsHeader& header, std::string_view key);

std::vector<double> ReadFitsColumnVector(
    const std::vector<unsigned char>& bytes,
    const FitsHdu& hdu,
    const FitsColumn& column,
    std::size_t row_index,
    bool scalar_rows);

std::vector<double> ReadFitsImageRow(
    const std::vector<unsigned char>& bytes,
    const FitsHdu& hdu,
    std::size_t row_index,
    std::size_t column_count);

}  // namespace specforge::detail
