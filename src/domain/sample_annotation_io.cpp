#include "domain/sample_annotation_io.h"

#include "domain/npy_array_io.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace specforge {
namespace {

class NpyAnnotationError : public std::runtime_error {
public:
    explicit NpyAnnotationError(std::string message)
        : std::runtime_error(std::move(message))
    {
    }
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

std::string FormatFloatingValue(double value)
{
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value < 0.0 ? "-inf" : "inf";
    }

    std::ostringstream stream;
    stream << std::setprecision(15) << value;
    return stream.str();
}

template <typename T>
void AssignIntegralValues(SampleAnnotationResult& result, std::ifstream& stream, std::size_t value_count)
{
    const std::vector<T> typed_values = ReadNpyTypedValues<T>(stream, value_count);
    result.values.reserve(value_count);
    for (const T value : typed_values) {
        if constexpr (std::is_signed_v<T>) {
            result.values.push_back({std::to_string(static_cast<long long>(value))});
        } else {
            result.values.push_back({std::to_string(static_cast<unsigned long long>(value))});
        }
    }
}

template <typename T>
void AssignFloatingValues(SampleAnnotationResult& result, std::ifstream& stream, std::size_t value_count)
{
    const std::vector<T> typed_values = ReadNpyTypedValues<T>(stream, value_count);
    result.values.reserve(value_count);
    for (const T value : typed_values) {
        result.values.push_back({FormatFloatingValue(static_cast<double>(value))});
    }
}

void AssignStringValues(SampleAnnotationResult& result, std::ifstream& stream, const NpyScalarType& scalar_type, std::size_t value_count)
{
    result.values.reserve(value_count);
    std::string bytes(scalar_type.item_size, '\0');
    for (std::size_t index = 0; index < value_count; ++index) {
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw NpyAnnotationError("NPY string data is truncated");
        }
        result.values.push_back({DecodeNpyString(bytes, scalar_type)});
    }
}

SampleAnnotationResult ReadAnnotationNpyValues(const std::filesystem::path& path, std::size_t expected_count)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw NpyAnnotationError("could not open the NPY file");
    }

    const NpyHeader header = ReadNpyHeader(stream);
    if (header.shape.size() != 1 || header.shape[0] != expected_count) {
        throw NpyAnnotationError("NPY array length does not match the source collection");
    }
    const std::optional<NpyScalarType> scalar_type = ParseNpyScalarType(header.descr);
    if (!scalar_type) {
        throw NpyAnnotationError("NPY dtype is not supported for read-only sample annotations");
    }
    ValidateNpyPayloadSize(path, header, expected_count, scalar_type->item_size);

    if (header.data_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw NpyAnnotationError("NPY data offset is too large");
    }
    stream.seekg(static_cast<std::streamoff>(header.data_offset), std::ios::beg);
    if (!stream) {
        throw NpyAnnotationError("could not seek to the NPY data");
    }

    SampleAnnotationResult result;
    result.name = FileNameToUtf8(path.filename());
    result.path = path;
    result.dtype = header.descr;

    switch (scalar_type->kind) {
    case NpyScalarKind::SignedInteger:
        result.kind = SampleAnnotationKind::CategoricalInteger;
        switch (scalar_type->item_size) {
        case 1:
            AssignIntegralValues<std::int8_t>(result, stream, expected_count);
            break;
        case 2:
            AssignIntegralValues<std::int16_t>(result, stream, expected_count);
            break;
        case 4:
            AssignIntegralValues<std::int32_t>(result, stream, expected_count);
            break;
        case 8:
            AssignIntegralValues<std::int64_t>(result, stream, expected_count);
            break;
        default:
            throw NpyAnnotationError("signed integer NPY dtype is not supported");
        }
        break;
    case NpyScalarKind::UnsignedInteger:
        result.kind = SampleAnnotationKind::CategoricalInteger;
        switch (scalar_type->item_size) {
        case 1:
            AssignIntegralValues<std::uint8_t>(result, stream, expected_count);
            break;
        case 2:
            AssignIntegralValues<std::uint16_t>(result, stream, expected_count);
            break;
        case 4:
            AssignIntegralValues<std::uint32_t>(result, stream, expected_count);
            break;
        case 8:
            AssignIntegralValues<std::uint64_t>(result, stream, expected_count);
            break;
        default:
            throw NpyAnnotationError("unsigned integer NPY dtype is not supported");
        }
        break;
    case NpyScalarKind::Float:
        result.kind = SampleAnnotationKind::ContinuousFloat;
        if (scalar_type->item_size == 4) {
            AssignFloatingValues<float>(result, stream, expected_count);
        } else if (scalar_type->item_size == 8) {
            AssignFloatingValues<double>(result, stream, expected_count);
        } else {
            throw NpyAnnotationError("floating-point NPY dtype is not supported");
        }
        break;
    case NpyScalarKind::Bytes:
    case NpyScalarKind::Unicode:
        result.kind = SampleAnnotationKind::Text;
        AssignStringValues(result, stream, *scalar_type, expected_count);
        break;
    default:
        throw NpyAnnotationError("NPY dtype is not supported for read-only sample annotations");
    }

    return result;
}

}  // namespace

std::optional<SampleAnnotationResult> LoadSampleAnnotationResultFromPath(
    const std::filesystem::path& path,
    std::size_t expected_count,
    std::string* error_message)
{
    try {
        return ReadAnnotationNpyValues(path, expected_count);
    } catch (const std::exception& error) {
        if (error_message != nullptr) {
            *error_message = error.what();
        }
        return std::nullopt;
    }
}

std::string_view SampleAnnotationKindLabel(SampleAnnotationKind kind)
{
    switch (kind) {
    case SampleAnnotationKind::CategoricalInteger:
        return "categorical";
    case SampleAnnotationKind::Text:
        return "text";
    case SampleAnnotationKind::ContinuousFloat:
        return "continuous";
    default:
        return "unknown";
    }
}

}  // namespace specforge
