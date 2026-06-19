#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

enum class NpyArrayErrorKind {
    UnsupportedFormat,
    InvalidShape,
    OpenFailed,
};

class NpyArrayError : public std::runtime_error {
public:
    NpyArrayError(NpyArrayErrorKind kind, std::string message);

    [[nodiscard]] NpyArrayErrorKind kind() const noexcept;

private:
    NpyArrayErrorKind kind_;
};

enum class NpyScalarKind {
    SignedInteger,
    UnsignedInteger,
    Float,
    Bytes,
    Unicode,
};

struct NpyScalarType {
    NpyScalarKind kind = NpyScalarKind::Float;
    std::size_t item_size = 0;
    std::size_t code_units = 0;
};

struct NpyHeader {
    std::string descr;
    std::vector<std::size_t> shape;
    std::uint64_t data_offset = 0;
};

[[nodiscard]] NpyHeader ReadNpyHeader(std::istream& stream);
[[nodiscard]] std::optional<NpyScalarType> ParseNpyScalarType(std::string_view descr);
void ValidateNpyPayloadSize(
    const std::filesystem::path& path,
    const NpyHeader& header,
    std::size_t value_count,
    std::size_t item_size);
void SeekNpyData(std::istream& stream, const NpyHeader& header);
[[nodiscard]] std::string DecodeNpyString(std::string_view bytes, const NpyScalarType& scalar_type);
void WriteNpyInt32Vector(std::ostream& stream, const std::vector<int>& values);

template <typename T>
std::vector<T> ReadNpyTypedValues(
    std::istream& stream,
    std::size_t value_count,
    std::string_view truncated_message = "NPY array data is truncated")
{
    if (value_count > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) / sizeof(T)) {
        throw NpyArrayError(NpyArrayErrorKind::InvalidShape, "NPY array is too large to read");
    }

    std::vector<T> values(value_count);
    stream.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(T)));
    if (!stream) {
        throw NpyArrayError(NpyArrayErrorKind::InvalidShape, std::string(truncated_message));
    }
    return values;
}

}  // namespace specforge
