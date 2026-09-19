#include "platform/win32_process_launcher.h"

#include "platform/win32_text.h"

#include <Windows.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spectiary {
namespace {

bool ContainsCommandLineWhitespaceOrQuote(
    std::wstring_view value)
{
    return value.find_first_of(
               L" \t\n\v\f\r\"") !=
           std::wstring_view::npos;
}

std::string PathText(const std::filesystem::path& path)
{
    const std::wstring wide = path.wstring();
    if (wide.empty()) {
        return {};
    }
    const std::string utf8 = WideToUtf8(wide);
    return utf8.empty() ? "<unrepresentable path>" : utf8;
}

std::string Win32ErrorText(std::uint32_t error)
{
    if (error == 0) {
        return {};
    }

    LPWSTR raw_message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(error),
        0,
        reinterpret_cast<LPWSTR>(&raw_message),
        0,
        nullptr);
    if (length == 0 || raw_message == nullptr) {
        return {};
    }

    std::wstring message(raw_message, length);
    LocalFree(raw_message);
    while (!message.empty() &&
           (message.back() == L'\r' ||
            message.back() == L'\n' ||
            message.back() == L' ' ||
            message.back() == L'\t')) {
        message.pop_back();
    }
    return WideToUtf8(message);
}

std::string ErrorSuffix(std::uint32_t error)
{
    const std::string text = Win32ErrorText(error);
    return text.empty() ? std::string{} : ": " + text;
}

bool HasInvalidPathText(const std::wstring& value)
{
    return value.empty() ||
           value.find(L'\0') != std::wstring::npos;
}

CurrentExecutableLaunchResult InvalidSourcePathResult()
{
    return {
        .failure =
            CurrentExecutableLaunchFailure::InvalidSourcePath,
        .diagnostic =
            "Could not launch Spectiary with an empty or invalid source path.",
    };
}

CurrentExecutablePathResult ResolveCurrentExecutablePathImpl()
{
    constexpr std::size_t kInitialBufferSize = MAX_PATH;
    constexpr std::size_t kMaximumBufferSize = 32'768U;
    std::wstring buffer(kInitialBufferSize, L'\0');

    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            const DWORD error = GetLastError();
            return {
                .win32_error = error == ERROR_SUCCESS
                    ? ERROR_GEN_FAILURE
                    : error,
            };
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return {
                .path = std::filesystem::path(
                    std::move(buffer)),
            };
        }
        if (buffer.size() >= kMaximumBufferSize) {
            return {
                .win32_error = ERROR_INSUFFICIENT_BUFFER,
            };
        }
        buffer.resize(buffer.size() * 2U);
    }
}

std::string ExecutablePathUnavailableDiagnostic(
    std::uint32_t error)
{
    return "Could not resolve the current Spectiary executable path (Win32 error " +
           std::to_string(error) +
           ")" +
           ErrorSuffix(error) +
           ".";
}

std::string ProcessCreationFailureDiagnostic(
    const std::filesystem::path& executable_path,
    const std::filesystem::path& source_path,
    std::uint32_t error)
{
    return "Could not launch Spectiary executable '" +
           PathText(executable_path) +
           "' with source '" +
           PathText(source_path) +
           "' (Win32 error " +
           std::to_string(error) +
           ")" +
           ErrorSuffix(error) +
           ".";
}

CurrentExecutableLaunchResult SourcePathResolutionFailureResult(
    const std::filesystem::path& source_path,
    const std::error_code& error)
{
    const std::uint32_t native_error =
        static_cast<std::uint32_t>(error.value());
    return {
        .failure =
            CurrentExecutableLaunchFailure::InvalidSourcePath,
        .win32_error = native_error,
        .diagnostic =
            "Could not resolve source path '" +
            PathText(source_path) +
            "' before launching Spectiary (Win32 error " +
            std::to_string(native_error) +
            ")" +
            ErrorSuffix(native_error) +
            ".",
    };
}

}  // namespace

CurrentExecutablePathResult ResolveCurrentExecutablePath()
{
    return ResolveCurrentExecutablePathImpl();
}

std::wstring QuoteWindowsCommandLineArgument(
    std::wstring_view value)
{
    if (value.empty()) {
        return L"\"\"";
    }
    if (!ContainsCommandLineWhitespaceOrQuote(value)) {
        return std::wstring(value);
    }

    std::wstring quoted;
    quoted.push_back(L'\"');
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2U + 1U, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

CurrentExecutableLaunchResult LaunchExecutableWithSource(
    const std::filesystem::path& executable_path,
    const std::filesystem::path& source_path)
{
    const std::wstring requested_source_text =
        source_path.wstring();
    if (HasInvalidPathText(requested_source_text)) {
        return InvalidSourcePathResult();
    }

    std::filesystem::path launch_source_path = source_path;
    if (!launch_source_path.is_absolute()) {
        std::error_code source_path_error;
        launch_source_path = std::filesystem::absolute(
            launch_source_path,
            source_path_error);
        if (source_path_error || launch_source_path.empty()) {
            return SourcePathResolutionFailureResult(
                source_path,
                source_path_error);
        }
    }
    const std::wstring source_text =
        launch_source_path.wstring();

    const std::wstring executable_text = executable_path.wstring();
    if (HasInvalidPathText(executable_text)) {
        return {
            .failure =
                CurrentExecutableLaunchFailure::
                    ExecutablePathUnavailable,
            .diagnostic =
                "Could not launch Spectiary because the executable path is empty or invalid.",
        };
    }

    const std::wstring command_line =
        QuoteWindowsCommandLineArgument(executable_text) +
        L" --new-instance " +
        QuoteWindowsCommandLineArgument(source_text);
    std::vector<wchar_t> mutable_command_line(
        command_line.begin(),
        command_line.end());
    mutable_command_line.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(
            executable_text.c_str(),
            mutable_command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        const DWORD error = GetLastError();
        return {
            .failure =
                CurrentExecutableLaunchFailure::
                    ProcessCreationFailed,
            .win32_error = error,
            .executable_path = executable_path,
            .diagnostic = ProcessCreationFailureDiagnostic(
                executable_path,
                launch_source_path,
                error),
        };
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return {
        .executable_path = executable_path,
    };
}

CurrentExecutableLaunchResult LaunchCurrentExecutableWithSource(
    const std::filesystem::path& source_path)
{
    const std::wstring source_text = source_path.wstring();
    if (HasInvalidPathText(source_text)) {
        return InvalidSourcePathResult();
    }

    const CurrentExecutablePathResult executable =
        ResolveCurrentExecutablePath();
    if (!executable.resolved()) {
        return {
            .failure =
                CurrentExecutableLaunchFailure::
                    ExecutablePathUnavailable,
            .win32_error = executable.win32_error,
            .diagnostic = ExecutablePathUnavailableDiagnostic(
                executable.win32_error),
        };
    }
    return LaunchExecutableWithSource(
        executable.path,
        source_path);
}

}  // namespace spectiary
