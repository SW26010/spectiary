#include "platform/atomic_file.h"
#include "platform/atomic_file_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace specforge {
namespace {

std::atomic<std::uint64_t> temporary_file_sequence{0};

void SetError(std::string* error_message, std::string message)
{
    if (error_message != nullptr) {
        *error_message = std::move(message);
    }
}

std::string DescriptionText(std::string_view description)
{
    return description.empty() ? std::string("file") : std::string(description);
}

void RemoveTemporaryFile(const std::filesystem::path& temporary_path)
{
    std::error_code remove_error;
    std::filesystem::remove(temporary_path, remove_error);
}

bool IsTransientReplaceError(const std::error_code& error)
{
#ifdef _WIN32
    return error.category() == std::system_category() &&
           (error.value() == ERROR_SHARING_VIOLATION ||
            error.value() == ERROR_ACCESS_DENIED);
#else
    (void)error;
    return false;
#endif
}

}  // namespace

std::filesystem::path TemporarySiblingPath(const std::filesystem::path& target_path)
{
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path temporary = target_path;
    temporary += ".tmp.";
#ifdef _WIN32
    temporary += std::to_string(GetCurrentProcessId());
#else
    temporary += "pid";
#endif
    temporary += ".";
    temporary += std::to_string(timestamp);
    temporary += ".";
    temporary += std::to_string(
        temporary_file_sequence.fetch_add(
            1,
            std::memory_order_relaxed));
    return temporary;
}

bool ReplaceFileAtomicallyWithOperation(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& target_path,
    AtomicFileReplaceRetryPolicy retry_policy,
    const AtomicFileReplaceOperation& replace_operation,
    const AtomicFileRetryWait& retry_wait,
    std::string* error_message,
    std::string_view target_description)
{
    const std::size_t maximum_attempts =
        std::max<std::size_t>(
            retry_policy.maximum_attempts,
            1);
    auto retry_delay =
        std::max(
            retry_policy.initial_retry_delay,
            std::chrono::milliseconds::zero());
    const auto maximum_retry_delay =
        std::max(
            retry_policy.maximum_retry_delay,
            retry_delay);

    std::error_code replace_error;
    for (std::size_t attempt = 0;
         attempt < maximum_attempts;
         ++attempt) {
        replace_error =
            replace_operation(temporary_path, target_path);
        if (!replace_error) {
            return true;
        }
        if (!IsTransientReplaceError(replace_error) ||
            attempt + 1 >= maximum_attempts) {
            break;
        }
        if (retry_delay > std::chrono::milliseconds::zero()) {
            retry_wait(retry_delay);
        }
        retry_delay +=
            std::min(
                retry_delay,
                maximum_retry_delay - retry_delay);
    }
    SetError(
        error_message,
        "could not replace " + DescriptionText(target_description) + ": " +
            replace_error.message());
    return false;
}

bool ReplaceFileAtomically(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& target_path,
    std::string* error_message,
    std::string_view target_description,
    AtomicFileReplaceRetryPolicy retry_policy)
{
    const AtomicFileReplaceOperation replace_operation =
        [](const std::filesystem::path& source,
           const std::filesystem::path& destination) {
#ifdef _WIN32
            if (MoveFileExW(
                    source.c_str(),
                    destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING |
                        MOVEFILE_WRITE_THROUGH) != 0) {
                return std::error_code{};
            }
            return std::error_code(
                static_cast<int>(GetLastError()),
                std::system_category());
#else
            std::error_code rename_error;
            std::filesystem::rename(
                source,
                destination,
                rename_error);
            return rename_error;
#endif
        };
    const AtomicFileRetryWait retry_wait =
        [](std::chrono::milliseconds delay) {
            std::this_thread::sleep_for(delay);
        };
    return ReplaceFileAtomicallyWithOperation(
        temporary_path,
        target_path,
        retry_policy,
        replace_operation,
        retry_wait,
        error_message,
        target_description);
}

bool WriteFileAtomically(
    const std::filesystem::path& target_path,
    const AtomicFileWriteOptions& options,
    const AtomicFileWriter& writer,
    std::string* error_message)
{
    if (error_message != nullptr) {
        error_message->clear();
    }

    const std::string description = DescriptionText(options.target_description);
    if (target_path.empty()) {
        SetError(error_message, description + " path is empty");
        return false;
    }

    std::error_code filesystem_error;
    const std::filesystem::path parent = target_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            SetError(error_message, "could not create " + description + " directory: " + filesystem_error.message());
            return false;
        }
    }

    const std::filesystem::path temporary_path = TemporarySiblingPath(target_path);
    std::ofstream stream(temporary_path, options.open_mode);
    if (!stream.good()) {
        SetError(error_message, "could not open temporary " + description + " for writing: " + temporary_path.string());
        return false;
    }

    std::string writer_error;
    if (!writer(stream, writer_error)) {
        stream.close();
        RemoveTemporaryFile(temporary_path);
        SetError(
            error_message,
            writer_error.empty() ? "could not write temporary " + description + ": " + temporary_path.string()
                                 : std::move(writer_error));
        return false;
    }

    if (!stream.good()) {
        stream.close();
        RemoveTemporaryFile(temporary_path);
        SetError(error_message, "could not write temporary " + description + ": " + temporary_path.string());
        return false;
    }
    stream.close();
    if (!stream.good()) {
        RemoveTemporaryFile(temporary_path);
        SetError(error_message, "could not close temporary " + description + ": " + temporary_path.string());
        return false;
    }

    if (!ReplaceFileAtomically(
            temporary_path,
            target_path,
            error_message,
            description,
            options.replace_retry_policy)) {
        RemoveTemporaryFile(temporary_path);
        return false;
    }
    return true;
}

}  // namespace specforge
