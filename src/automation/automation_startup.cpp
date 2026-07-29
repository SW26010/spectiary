#include "automation/automation_startup.h"

#include "app/runtime_paths.h"
#include "platform/win32_text.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

constexpr std::wstring_view kAutomationPipeOption =
    L"--automation-pipe";
constexpr std::wstring_view kAutomationNonceOption =
    L"--automation-nonce";
constexpr std::wstring_view kAutomationInstanceOption =
    L"--automation-instance";
constexpr std::wstring_view kAutomationStateRootOption =
    L"--automation-state-root";
constexpr std::array<std::wstring_view, 4>
    kIncompatibleAutomationEnvironmentVariables = {
        L"SPECFORGE_PROFILE",
        L"SPECFORGE_PROFILE_DIR",
        L"SPECFORGE_RUNTIME_RESOURCE_WORKLOAD",
        L"SPECFORGE_RUNTIME_RESOURCE_STATE_DIR",
    };

bool IsLowerHex(std::string_view value)
{
    return std::all_of(
        value.begin(),
        value.end(),
        [](char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        });
}

std::string NarrowAscii(std::wstring_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < 0 || character > 0x7f) {
            return {};
        }
        result.push_back(
            static_cast<char>(character));
    }
    return result;
}

std::optional<std::wstring> TakeOptionValue(
    std::span<const std::wstring> arguments,
    std::size_t& index,
    std::string& error_message)
{
    if (index + 1 >= arguments.size()) {
        error_message =
            "Automation startup option is missing its value.";
        return std::nullopt;
    }
    ++index;
    if (arguments[index].empty()) {
        error_message =
            "Automation startup option values must not be empty.";
        return std::nullopt;
    }
    return arguments[index];
}

std::filesystem::path NormalizedAbsolutePath(
    const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path normalized =
        std::filesystem::weakly_canonical(
            path,
            error);
    if (!error) {
        return normalized;
    }
    error.clear();
    normalized =
        std::filesystem::absolute(path, error);
    return (error ? path : normalized)
        .lexically_normal();
}

bool PathComponentEqual(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    const std::wstring left_text = left.wstring();
    const std::wstring right_text = right.wstring();
    return CompareStringOrdinal(
               left_text.c_str(),
               static_cast<int>(left_text.size()),
               right_text.c_str(),
               static_cast<int>(right_text.size()),
               TRUE) == CSTR_EQUAL;
}

bool PathContains(
    const std::filesystem::path& parent,
    const std::filesystem::path& child)
{
    const std::filesystem::path normalized_parent =
        NormalizedAbsolutePath(parent);
    const std::filesystem::path normalized_child =
        NormalizedAbsolutePath(child);
    auto parent_iterator = normalized_parent.begin();
    auto child_iterator = normalized_child.begin();
    for (; parent_iterator != normalized_parent.end();
         ++parent_iterator, ++child_iterator) {
        if (child_iterator == normalized_child.end() ||
            !PathComponentEqual(
                *parent_iterator,
                *child_iterator)) {
            return false;
        }
    }
    return true;
}

template <typename T>
bool SetOnce(
    std::optional<T>& target,
    T value,
    std::string_view option_name,
    std::string& error_message)
{
    if (target) {
        error_message =
            "Automation startup option '" +
            std::string(option_name) +
            "' was provided more than once.";
        return false;
    }
    target = std::move(value);
    return true;
}

}  // namespace

bool IsValidAutomationInstanceId(
    std::string_view value) noexcept
{
    return value.size() == 32U && IsLowerHex(value);
}

bool IsValidAutomationNonce(
    std::string_view value) noexcept
{
    return value.size() == 64U && IsLowerHex(value);
}

std::wstring AutomationPipeNameForInstance(
    std::string_view instance_id)
{
    const std::wstring wide_instance =
        Utf8ToWide(instance_id);
    return L"\\\\.\\pipe\\SpecForge.Automation." +
           wide_instance;
}

bool IsIncompatibleAutomationEnvironmentVariable(
    std::wstring_view name) noexcept
{
    return std::any_of(
        kIncompatibleAutomationEnvironmentVariables.begin(),
        kIncompatibleAutomationEnvironmentVariables.end(),
        [name](std::wstring_view candidate) {
            return CompareStringOrdinal(
                       name.data(),
                       static_cast<int>(name.size()),
                       candidate.data(),
                       static_cast<int>(candidate.size()),
                       TRUE) == CSTR_EQUAL;
        });
}

std::optional<std::wstring>
ActiveIncompatibleAutomationEnvironmentVariable()
{
    for (const std::wstring_view name :
         kIncompatibleAutomationEnvironmentVariables) {
        if (GetEnvironmentVariableW(
                std::wstring(name).c_str(),
                nullptr,
                0) != 0) {
            return std::wstring(name);
        }
    }
    return std::nullopt;
}

std::filesystem::path
OrdinaryUserStateRootForExecutable(
    const std::filesystem::path& executable_path)
{
    RuntimePathInputs inputs =
        CurrentProcessRuntimePathInputs(
            executable_path);
    inputs.local_user_state_root_override.reset();
    const SpecForgeStartup startup =
        PrepareSpecForgeStartup(
            std::move(inputs));
    return startup.runtime_paths()
        .local_user_state_root;
}

bool AutomationStateRootIsIndependent(
    const std::filesystem::path& automation_root,
    const std::filesystem::path& ordinary_root)
{
    return !PathContains(
               automation_root,
               ordinary_root) &&
           !PathContains(
               ordinary_root,
               automation_root);
}

SpecForgeCommandLine ParseSpecForgeCommandLine(
    std::span<const std::wstring> arguments)
{
    SpecForgeCommandLine result;
    std::optional<std::wstring> pipe_name;
    std::optional<std::wstring> nonce;
    std::optional<std::wstring> instance_id;
    std::optional<std::filesystem::path> state_root;

    for (std::size_t index = 1;
         index < arguments.size();
         ++index) {
        const std::wstring_view argument =
            arguments[index];
        const bool automation_option =
            argument == kAutomationPipeOption ||
            argument == kAutomationNonceOption ||
            argument == kAutomationInstanceOption ||
            argument == kAutomationStateRootOption;
        if (automation_option) {
            result.automation_requested = true;
            std::optional<std::wstring> value =
                TakeOptionValue(
                    arguments,
                    index,
                    result.error_message);
            if (!value) {
                return result;
            }
            if (argument == kAutomationPipeOption) {
                if (!SetOnce(
                        pipe_name,
                        std::move(*value),
                        "--automation-pipe",
                        result.error_message)) {
                    return result;
                }
            } else if (argument ==
                       kAutomationNonceOption) {
                if (!SetOnce(
                        nonce,
                        std::move(*value),
                        "--automation-nonce",
                        result.error_message)) {
                    return result;
                }
            } else if (argument ==
                       kAutomationInstanceOption) {
                if (!SetOnce(
                        instance_id,
                        std::move(*value),
                        "--automation-instance",
                        result.error_message)) {
                    return result;
                }
            } else {
                const std::filesystem::path path(*value);
                if (!SetOnce(
                        state_root,
                        path,
                        "--automation-state-root",
                        result.error_message)) {
                    return result;
                }
            }
            continue;
        }

        if (!result.initial_source &&
            !argument.empty() &&
            argument.front() != L'-') {
            result.initial_source =
                std::filesystem::path(argument);
        }
    }

    if (!result.automation_requested) {
        return result;
    }
    if (!pipe_name || !nonce || !instance_id ||
        !state_root) {
        result.error_message =
            "Automation startup requires --automation-pipe, --automation-nonce, --automation-instance and --automation-state-root.";
        return result;
    }

    const std::string nonce_utf8 =
        NarrowAscii(*nonce);
    const std::string instance_utf8 =
        NarrowAscii(*instance_id);
    if (!IsValidAutomationNonce(nonce_utf8)) {
        result.error_message =
            "Automation nonce must be 64 lowercase hexadecimal characters.";
        return result;
    }
    if (!IsValidAutomationInstanceId(instance_utf8)) {
        result.error_message =
            "Automation instance ID must be 32 lowercase hexadecimal characters.";
        return result;
    }
    if (*pipe_name !=
        AutomationPipeNameForInstance(instance_utf8)) {
        result.error_message =
            "Automation pipe name does not match the launcher instance ID.";
        return result;
    }
    if (!state_root->is_absolute()) {
        result.error_message =
            "Automation state root must be an absolute path.";
        return result;
    }
    std::error_code state_root_error;
    if (!std::filesystem::is_directory(
            *state_root,
            state_root_error) ||
        state_root_error) {
        result.error_message =
            "Automation state root must already exist as a directory created by the launcher.";
        return result;
    }

    result.automation = AutomationStartupConfiguration{
        .pipe_name = std::move(*pipe_name),
        .nonce = nonce_utf8,
        .instance_id = instance_utf8,
        .state_root = std::move(*state_root),
    };
    return result;
}

SpecForgeCommandLine
ParseCurrentProcessSpecForgeCommandLine()
{
    int argument_count = 0;
    LPWSTR* raw_arguments =
        CommandLineToArgvW(
            GetCommandLineW(),
            &argument_count);
    if (raw_arguments == nullptr) {
        return {
            .error_message =
                "Could not parse the SpecForge process command line.",
        };
    }

    std::vector<std::wstring> arguments;
    arguments.reserve(
        static_cast<std::size_t>(argument_count));
    for (int index = 0;
         index < argument_count;
         ++index) {
        arguments.emplace_back(
            raw_arguments[index] != nullptr
                ? raw_arguments[index]
                : L"");
    }
    LocalFree(raw_arguments);
    return ParseSpecForgeCommandLine(arguments);
}

}  // namespace specforge
