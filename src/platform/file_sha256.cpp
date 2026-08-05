#include "platform/file_sha256.h"

#include "domain/stable_sha256.h"

#include <array>
#include <exception>
#include <fstream>
#include <string_view>
#include <utility>

namespace specforge {
namespace {

constexpr std::size_t kHashBufferSize = 64U * 1024U;

void SetError(std::string* error_message, std::string message)
{
    if (error_message != nullptr) {
        *error_message = std::move(message);
    }
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

}  // namespace

std::optional<std::string> ComputeFileSha256(
    const std::filesystem::path& path,
    std::string* error_message)
{
    if (error_message != nullptr) {
        error_message->clear();
    }

    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.good()) {
            SetError(
                error_message,
                "could not open file for hashing: " + PathToUtf8(path));
            return std::nullopt;
        }

        StableSha256 sha256;
        std::array<char, kHashBufferSize> buffer = {};
        for (;;) {
            stream.read(
                buffer.data(),
                static_cast<std::streamsize>(buffer.size()));
            const std::streamsize read_count = stream.gcount();
            if (read_count > 0) {
                sha256.Append(std::string_view(
                    buffer.data(),
                    static_cast<std::size_t>(read_count)));
            }
            if (stream.eof()) {
                break;
            }
            if (stream.fail()) {
                SetError(
                    error_message,
                    "could not read file completely: " + PathToUtf8(path));
                return std::nullopt;
            }
        }

        return sha256.FinishHex();
    } catch (const std::exception& exception) {
        SetError(
            error_message,
            "could not hash file " + PathToUtf8(path) + ": " +
                std::string(exception.what()));
        return std::nullopt;
    } catch (...) {
        SetError(
            error_message,
            "could not hash file: " + PathToUtf8(path));
        return std::nullopt;
    }
}

}  // namespace specforge
