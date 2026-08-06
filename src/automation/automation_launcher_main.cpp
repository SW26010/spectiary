#include "automation/automation_named_pipe.h"
#include "automation/automation_protocol.h"
#include "automation/automation_startup.h"
#include "app/local_user_state_paths.h"
#include "app/runtime_paths.h"
#include "platform/win32_process_launcher.h"
#include "platform/win32_text.h"
#include "ui/sample_labeling_state_cache_io.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr auto kLauncherConnectTimeout = 10s;
constexpr auto kLauncherHelloResponseTimeout = 5s;
constexpr auto kLauncherCommandResponseTimeout = 10s;
constexpr auto kLauncherNormalCleanupTimeout = 5s;
constexpr auto kLauncherProcessExitTimeout = 10s;

std::string ResponseWaitFailure(
    std::string_view exchange,
    std::string_view receive_error)
{
    if (receive_error ==
            specforge::
                kAutomationNamedPipeReceiveDeadlineExpired ||
        receive_error ==
            specforge::
                kAutomationNamedPipeSendDeadlineExpired) {
        std::ostringstream message;
        message << "Automation " << exchange
                << " response timed out after "
                << kLauncherCommandResponseTimeout.count()
                << " seconds; terminal outcome was not observed.";
        return message.str();
    }
    return "Automation " + std::string(exchange) +
        " response ended before terminal outcome was observed.";
}

struct LauncherOptions {
    std::filesystem::path app_path;
    std::optional<std::filesystem::path> state_root;
    std::optional<std::filesystem::path>
        labeling_state_seed;
    std::string error_message;
};

std::wstring BuildGuiCommandLine(
    const std::filesystem::path& app_path,
    const std::wstring& pipe_name,
    std::string_view nonce,
    std::string_view instance_id,
    const std::filesystem::path& state_root)
{
    std::vector<std::wstring> arguments = {
        app_path.wstring(),
        L"--automation-pipe",
        pipe_name,
        L"--automation-nonce",
        std::wstring(nonce.begin(), nonce.end()),
        L"--automation-instance",
        std::wstring(
            instance_id.begin(),
            instance_id.end()),
        L"--automation-state-root",
        state_root.wstring(),
    };
    std::wstring command_line;
    for (const std::wstring& argument : arguments) {
        if (!command_line.empty()) {
            command_line.push_back(L' ');
        }
        command_line +=
            specforge::QuoteWindowsCommandLineArgument(argument);
    }
    return command_line;
}

std::optional<std::string> RandomHex(
    std::size_t byte_count)
{
    std::vector<unsigned char> bytes(byte_count);
    if (BCryptGenRandom(
            nullptr,
            bytes.data(),
            static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return std::nullopt;
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

bool BuildAutomationChildEnvironment(
    std::vector<wchar_t>& environment_block,
    std::string& error_message)
{
    LPWCH raw_environment =
        GetEnvironmentStringsW();
    if (raw_environment == nullptr) {
        error_message =
            "Could not read the launcher environment.";
        return false;
    }

    for (const wchar_t* cursor = raw_environment;
         *cursor != L'\0';) {
        const std::size_t length =
            std::wcslen(cursor);
        const std::wstring_view entry(
            cursor,
            length);
        const std::size_t delimiter =
            entry.find(
                L'=',
                entry.starts_with(L'=') ? 1U : 0U);
        const std::wstring_view name =
            delimiter == std::wstring_view::npos
            ? entry
            : entry.substr(0, delimiter);
        if (!specforge::
                IsIncompatibleAutomationEnvironmentVariable(
                    name)) {
            environment_block.insert(
                environment_block.end(),
                entry.begin(),
                entry.end());
            environment_block.push_back(L'\0');
        }
        cursor += length + 1U;
    }
    FreeEnvironmentStringsW(raw_environment);
    environment_block.push_back(L'\0');
    return true;
}

LauncherOptions ParseOptions(int argc, wchar_t** argv)
{
    LauncherOptions options;
    const std::filesystem::path launcher_path =
        specforge::CurrentExecutablePath();
    options.app_path =
        launcher_path.parent_path() /
        "SpecForge.exe";

    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        const auto require_value =
            [&](std::string_view name)
            -> std::optional<std::wstring> {
            if (index + 1 >= argc) {
                options.error_message =
                    "Missing value for " +
                    std::string(name) + ".";
                return std::nullopt;
            }
            ++index;
            return std::wstring(argv[index]);
        };
        if (argument == L"--app") {
            const auto value =
                require_value("--app");
            if (!value) {
                return options;
            }
            options.app_path =
                std::filesystem::path(*value);
        } else if (argument == L"--state-root") {
            const auto value =
                require_value("--state-root");
            if (!value) {
                return options;
            }
            options.state_root =
                std::filesystem::path(*value);
        } else if (
            argument ==
            L"--labeling-state-seed") {
            const auto value =
                require_value(
                    "--labeling-state-seed");
            if (!value) {
                return options;
            }
            if (options.labeling_state_seed) {
                options.error_message =
                    "Option --labeling-state-seed was provided more than once.";
                return options;
            }
            options.labeling_state_seed =
                std::filesystem::path(*value);
        } else if (argument == L"--help" ||
                   argument == L"-h") {
            options.error_message = "help";
            return options;
        } else {
            options.error_message =
                "Unknown launcher option.";
            return options;
        }
    }
    return options;
}

bool PathContainsReparsePoint(
    const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path absolute =
        std::filesystem::absolute(path, error)
            .lexically_normal();
    if (error) {
        return true;
    }
    std::filesystem::path current;
    for (const auto& component : absolute) {
        current /= component;
        const DWORD attributes =
            GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes &
             FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return true;
        }
    }
    return false;
}

bool RejectLabelingStateOutputPaths(
    const specforge::SampleLabelingStateCache& cache,
    std::string_view failure_message,
    std::string& error_message)
{
    for (const auto& [source_id, source] :
         cache.sources) {
        (void)source_id;
        for (const auto& task : source.tasks) {
            if (!task.output_path) {
                continue;
            }
            error_message =
                failure_message;
            return false;
        }
    }
    return true;
}

bool ValidateLabelingStateSeed(
    const specforge::
        AutomationReadOnlyFileLease& seed,
    const std::filesystem::path& state_root,
    const std::filesystem::path& ordinary_root,
    std::string& error_message)
{
    if (!specforge::AutomationStateRootIsIndependent(
            seed.path(),
            ordinary_root) ||
        !specforge::AutomationStateRootIsIndependent(
            seed.path(),
            state_root)) {
        error_message =
            "Labeling state seed must be independent from ordinary and automation state roots.";
        return false;
    }
    const specforge::SampleLabelingStateCacheLoadResult
        loaded =
            specforge::LoadSampleLabelingStateCache(
                seed.path(),
                {},
                specforge::
                    SampleLabelingStateCacheLoadPolicy::
                        InternalDraftsOnly);
    if (!loaded.warning.empty()) {
        error_message =
            loaded.warning ==
                    "Persistent labeling output paths are not permitted in an automation state seed."
            ? loaded.warning
            : "Labeling state seed is not a valid production sample-labeling cache.";
        return false;
    }
    return RejectLabelingStateOutputPaths(
        loaded.cache,
        "Labeling state seed must contain internal-draft tasks only; persistent output_path references are not permitted for automation.",
        error_message);
}

bool MaterializeLabelingStateSeed(
    specforge::AutomationStateRootLease&
        state_root,
    const specforge::
        AutomationReadOnlyFileLease& seed,
    std::string& error_message)
{
    const std::wstring destination_name =
        std::filesystem::path(
            specforge::local_user_state_paths::
                kSampleLabelingState)
            .wstring();
    const std::filesystem::path destination =
        state_root.path() /
        destination_name;
    if (!specforge::
            MaterializePinnedAutomationSeed(
            state_root,
            seed,
            destination_name,
            error_message)) {
        return false;
    }
    const specforge::SampleLabelingStateCacheLoadResult
        loaded =
            specforge::LoadSampleLabelingStateCache(
                destination,
                {},
                specforge::
                    SampleLabelingStateCacheLoadPolicy::
                        InternalDraftsOnly);
    if (!loaded.warning.empty()) {
        error_message =
            loaded.warning ==
                    "Persistent labeling output paths are not permitted in an automation state seed."
            ? loaded.warning
            : "Materialized labeling state failed production cache validation.";
        return false;
    }
    return RejectLabelingStateOutputPaths(
        loaded.cache,
        "Materialized labeling state contains a forbidden persistent output_path reference.",
        error_message);
}

class PrelaunchStateRootGuard {
public:
    explicit PrelaunchStateRootGuard(
        specforge::AutomationStateRootLease&
            root)
        : root_(&root)
    {
    }

    ~PrelaunchStateRootGuard()
    {
        if (!active_) {
            return;
        }
        specforge::
            RemovePinnedAutomationStateRootBeforeLaunch(
                *root_);
    }

    void Release() noexcept
    {
        active_ = false;
    }

private:
    specforge::AutomationStateRootLease*
        root_ = nullptr;
    bool active_ = true;
};

std::string Trim(std::string value)
{
    const auto whitespace =
        [](unsigned char character) {
            return std::isspace(character) != 0;
        };
    value.erase(
        value.begin(),
        std::find_if_not(
            value.begin(),
            value.end(),
            whitespace));
    value.erase(
        std::find_if_not(
            value.rbegin(),
            value.rend(),
            whitespace)
            .base(),
        value.end());
    return value;
}

enum class LauncherLineReadStatus {
    Line,
    End,
    Error,
};

LauncherLineReadStatus ReadLauncherLine(
    std::string& line,
    std::string& error_message)
{
    const HANDLE input =
        GetStdHandle(STD_INPUT_HANDLE);
    DWORD console_mode = 0;
    if (input != nullptr &&
        input != INVALID_HANDLE_VALUE &&
        GetConsoleMode(
            input,
            &console_mode) != FALSE) {
        std::wstring wide_line;
        for (;;) {
            std::array<wchar_t, 512> buffer = {};
            DWORD read = 0;
            if (ReadConsoleW(
                    input,
                    buffer.data(),
                    static_cast<DWORD>(
                        buffer.size()),
                    &read,
                    nullptr) == FALSE) {
                error_message =
                    "Could not read a Unicode command from the Windows console.";
                return LauncherLineReadStatus::Error;
            }
            if (read == 0) {
                if (wide_line.empty()) {
                    return LauncherLineReadStatus::End;
                }
                break;
            }
            const auto newline = std::find(
                buffer.begin(),
                buffer.begin() + read,
                L'\n');
            wide_line.append(
                buffer.begin(),
                newline);
            if (newline !=
                buffer.begin() + read) {
                break;
            }
        }
        line = specforge::WideToUtf8(
            wide_line);
        if (!wide_line.empty() &&
            line.empty()) {
            error_message =
                "The Windows console command could not be converted to UTF-8.";
            return LauncherLineReadStatus::Error;
        }
        return LauncherLineReadStatus::Line;
    }

    if (!std::getline(std::cin, line)) {
        if (std::cin.eof()) {
            return LauncherLineReadStatus::End;
        }
        error_message =
            "Could not read an automation command from standard input.";
        return LauncherLineReadStatus::Error;
    }
    if (!specforge::IsWellFormedUtf8(line)) {
        error_message =
            "Redirected automation commands must be encoded as UTF-8.";
        return LauncherLineReadStatus::Error;
    }
    return LauncherLineReadStatus::Line;
}

std::optional<std::size_t>
ParseNonnegativeSize(std::string_view text)
{
    if (text.empty() ||
        !std::all_of(
            text.begin(),
            text.end(),
            [](char character) {
                return character >= '0' &&
                       character <= '9';
            })) {
        return std::nullopt;
    }
    unsigned long long parsed = 0;
    const auto [end, error] =
        std::from_chars(
            text.data(),
            text.data() + text.size(),
            parsed,
            10);
    if (error != std::errc{} ||
        end != text.data() + text.size() ||
        parsed >
            (std::numeric_limits<
                std::size_t>::max)()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(
        parsed);
}

std::optional<std::int64_t>
ParseSignedInteger(std::string_view text)
{
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.front() == '+') {
        text.remove_prefix(1U);
        if (text.empty() ||
            text.front() < '0' ||
            text.front() > '9') {
            return std::nullopt;
        }
    }
    std::int64_t parsed = 0;
    const auto [end, error] =
        std::from_chars(
            text.data(),
            text.data() + text.size(),
            parsed,
            10);
    if (error != std::errc{} ||
        end != text.data() + text.size()) {
        return std::nullopt;
    }
    return parsed;
}

std::string LowerAscii(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(
                std::tolower(character));
        });
    return value;
}

struct HumanCommand {
    specforge::AutomationCommandKind kind =
        specforge::AutomationCommandKind::
            StateGet;
    specforge::AutomationCommandParameters
        parameters;
};

std::optional<HumanCommand>
CommandFromHumanLine(
    std::string line,
    std::string& error_message)
{
    line = Trim(std::move(line));
    const std::string lower = LowerAscii(line);
    if (lower == "state get" ||
        lower == "state.get") {
        return HumanCommand{
            .kind =
                specforge::AutomationCommandKind::
                    StateGet,
        };
    }
    if (lower == "wait idle" ||
        lower == "wait.idle") {
        return HumanCommand{
            .kind =
                specforge::AutomationCommandKind::
                    WaitIdle,
        };
    }
    if (lower == "profile start" ||
        lower == "profile.start") {
        return HumanCommand{
            .kind =
                specforge::AutomationCommandKind::
                    ProfileStart,
        };
    }
    if (lower == "profile stop" ||
        lower == "profile.stop") {
        return HumanCommand{
            .kind =
                specforge::AutomationCommandKind::
                    ProfileStop,
        };
    }
    if (lower == "app quit" ||
        lower == "app.quit") {
        return HumanCommand{
            .kind =
                specforge::AutomationCommandKind::
                    AppQuit,
        };
    }

    constexpr std::string_view
        setting_get_prefix = "setting get ";
    if (lower.starts_with(
            setting_get_prefix)) {
        const std::string name =
            LowerAscii(Trim(
                line.substr(
                    setting_get_prefix.size())));
        if (!name.empty()) {
            return HumanCommand{
                .kind =
                    specforge::
                        AutomationCommandKind::
                            SettingGet,
                .parameters =
                    specforge::
                        AutomationSettingGetParameters{
                            .name = name,
                        },
            };
        }
    }

    constexpr std::string_view
        setting_set_prefix = "setting set ";
    if (lower.starts_with(
            setting_set_prefix)) {
        const std::string assignment = Trim(
            line.substr(
                setting_set_prefix.size()));
        const std::size_t separator =
            assignment.find_first_of(" \t");
        if (separator != std::string::npos) {
            const std::string name =
                LowerAscii(Trim(
                    assignment.substr(
                        0,
                        separator)));
            const std::string value_text =
                Trim(assignment.substr(
                    separator + 1U));
            if (!name.empty() &&
                !value_text.empty()) {
                specforge::AutomationSettingValue
                    value = value_text;
                if (const auto integer =
                        ParseSignedInteger(
                            value_text)) {
                    value = *integer;
                } else {
                    const std::string lower_value =
                        LowerAscii(value_text);
                    if (lower_value == "true") {
                        value = true;
                    } else if (
                        lower_value == "false") {
                        value = false;
                    }
                }
                return HumanCommand{
                    .kind =
                        specforge::
                            AutomationCommandKind::
                                SettingSet,
                    .parameters =
                        specforge::
                            AutomationSettingSetParameters{
                                .name = name,
                                .value =
                                    std::move(value),
                            },
                };
            }
        }
    }

    constexpr std::string_view
        panel_get_prefix = "panel get ";
    if (lower.starts_with(panel_get_prefix)) {
        const std::string name =
            LowerAscii(Trim(line.substr(
                panel_get_prefix.size())));
        if (!name.empty()) {
            return HumanCommand{
                .kind =
                    specforge::AutomationCommandKind::
                        PanelGet,
                .parameters =
                    specforge::AutomationPanelGetParameters{
                        .name = name,
                    },
            };
        }
    }

    constexpr std::string_view
        panel_set_prefix = "panel set ";
    if (lower.starts_with(panel_set_prefix)) {
        const std::string assignment = Trim(
            line.substr(panel_set_prefix.size()));
        const std::size_t separator =
            assignment.find_first_of(" \t");
        if (separator != std::string::npos) {
            const std::string name =
                LowerAscii(Trim(assignment.substr(
                    0,
                    separator)));
            const std::string value =
                LowerAscii(Trim(assignment.substr(
                    separator + 1U)));
            if (!name.empty() &&
                (value == "true" || value == "false")) {
                return HumanCommand{
                    .kind =
                        specforge::AutomationCommandKind::
                            PanelSet,
                    .parameters =
                        specforge::AutomationPanelSetParameters{
                            .name = name,
                            .visible = value == "true",
                        },
                };
            }
        }
        error_message =
            "panel set requires a panel name followed by true or false.";
        return std::nullopt;
    }

    constexpr std::string_view
        source_prefix = "source open ";
    if (lower.starts_with(source_prefix)) {
        const std::string path = Trim(
            line.substr(source_prefix.size()));
        if (!path.empty()) {
            return HumanCommand{
                .kind =
                    specforge::
                        AutomationCommandKind::
                            SourceOpen,
                .parameters =
                    specforge::
                        AutomationSourceOpenParameters{
                            .path = path,
                        },
            };
        }
    }

    constexpr std::string_view
        spectrum_name_prefix =
            "spectrum goto name ";
    if (lower.starts_with(
            spectrum_name_prefix)) {
        const std::string name = Trim(
            line.substr(
                spectrum_name_prefix.size()));
        if (!name.empty()) {
            return HumanCommand{
                .kind =
                    specforge::
                        AutomationCommandKind::
                            SpectrumGoto,
                .parameters =
                    specforge::
                        AutomationSpectrumGotoParameters{
                            .target = {
                                .name = name,
                            },
                        },
            };
        }
    }

    constexpr std::string_view
        spectrum_index_prefix =
            "spectrum goto ";
    if (lower.starts_with(
            spectrum_index_prefix)) {
        const std::string index_text = Trim(
            line.substr(
                spectrum_index_prefix.size()));
        if (const auto index =
                ParseNonnegativeSize(
                    index_text)) {
            return HumanCommand{
                .kind =
                    specforge::
                        AutomationCommandKind::
                            SpectrumGoto,
                .parameters =
                    specforge::
                        AutomationSpectrumGotoParameters{
                            .target = {
                                .index = *index,
                            },
                },
            };
        }
        error_message =
            "spectrum goto index must be an unsigned decimal integer.";
        return std::nullopt;
    }

    constexpr std::string_view
        label_prefix = "label assign ";
    if (lower.starts_with(label_prefix)) {
        std::string assignment_text = Trim(
            line.substr(label_prefix.size()));
        std::optional<
            specforge::AutomationSpectrumTarget>
            target;
        constexpr std::string_view
            target_name_marker =
                " spectrum name ";
        constexpr std::string_view
            target_index_marker =
                " spectrum ";
        const std::string lower_assignment =
            LowerAscii(assignment_text);
        const std::size_t name_marker =
            lower_assignment.find(
                target_name_marker);
        const std::size_t index_marker =
            lower_assignment.find(
                target_index_marker);
        if (name_marker != std::string::npos) {
            const std::string name = Trim(
                assignment_text.substr(
                    name_marker +
                    target_name_marker.size()));
            assignment_text = Trim(
                assignment_text.substr(
                    0,
                    name_marker));
            if (name.empty()) {
                error_message =
                    "label assign target name must not be empty.";
                return std::nullopt;
            }
            target =
                specforge::AutomationSpectrumTarget{
                    .name = name,
                };
        } else if (
            index_marker != std::string::npos) {
            const std::string index_text = Trim(
                assignment_text.substr(
                    index_marker +
                    target_index_marker.size()));
            assignment_text = Trim(
                assignment_text.substr(
                    0,
                    index_marker));
            if (const auto index =
                    ParseNonnegativeSize(
                        index_text)) {
                target =
                    specforge::
                        AutomationSpectrumTarget{
                            .index = *index,
                        };
            } else {
                error_message =
                    "label assign target index must be an unsigned decimal integer.";
                return std::nullopt;
            }
        }
        const std::string code_text =
            assignment_text;
        try {
            std::size_t consumed = 0;
            const long long code =
                std::stoll(
                    code_text,
                    &consumed,
                    10);
            if (consumed == code_text.size() &&
                code >=
                    (std::numeric_limits<int>::min)() &&
                code <=
                    (std::numeric_limits<int>::max)()) {
                return HumanCommand{
                    .kind =
                        specforge::
                            AutomationCommandKind::
                                LabelAssign,
                    .parameters =
                        specforge::
                        AutomationLabelAssignParameters{
                            .code =
                                static_cast<int>(
                                    code),
                            .target =
                                std::move(target),
                        },
                };
            }
        } catch (const std::exception&) {
        }
    }

    constexpr std::string_view
        capture_prefix = "frame capture ";
    if (lower.starts_with(capture_prefix)) {
        const std::string path = Trim(
            line.substr(capture_prefix.size()));
        if (!path.empty()) {
            return HumanCommand{
                .kind =
                    specforge::
                        AutomationCommandKind::
                            FrameCapture,
                .parameters =
                    specforge::
                        AutomationFrameCaptureParameters{
                            .path = path,
                        },
            };
        }
    }
    error_message =
        "Unknown or malformed command. GUI input was not sent.";
    return std::nullopt;
}

bool SendCommandAndWait(
    specforge::AutomationNamedPipeClient& client,
    specforge::AutomationCommandKind command,
    const specforge::AutomationCommandParameters&
        parameters,
    std::uint64_t request_number,
    std::string& error_message)
{
    const std::string request_id =
        "request-" +
        std::to_string(request_number);
    if (!client.Send(
            specforge::
                SerializeAutomationCommandRequest(
                    request_id,
                    command,
                    parameters),
            error_message)) {
        return false;
    }

    bool accepted = false;
    const auto response_deadline =
        std::chrono::steady_clock::now() +
        kLauncherCommandResponseTimeout;
    for (;;) {
        std::string response;
        if (!client.ReceiveUntil(
                response,
                response_deadline,
                error_message)) {
            client.Close();
            error_message = ResponseWaitFailure(
                "command",
                error_message);
            return false;
        }
        std::cout << response << '\n'
                  << std::flush;
        const auto parsed =
            specforge::ParseAutomationServerMessage(
                response);
        if (!parsed.message ||
            parsed.message->request_id != request_id) {
            error_message =
                parsed.error_message.empty()
                ? "Received a response for the wrong request."
                : parsed.error_message;
            client.Close();
            error_message = ResponseWaitFailure(
                "command",
                error_message);
            return false;
        }
        if (parsed.message->status == "accepted") {
            accepted = true;
            continue;
        }
        if (!accepted) {
            error_message =
                parsed.message->error_message.empty()
                ? "Automation request failed before acceptance."
                : parsed.message->error_message;
            return false;
        }
        if (parsed.message->status != "completed") {
            error_message =
                parsed.message->error_message.empty()
                ? "Automation request did not complete."
                : parsed.message->error_message;
            return false;
        }
        return true;
    }
}

bool SendPipelineAndWait(
    specforge::AutomationNamedPipeClient& client,
    const std::vector<HumanCommand>& commands,
    std::uint64_t& request_number,
    std::string& error_message)
{
    if (commands.size() >
        specforge::kAutomationQueueCapacity) {
        error_message =
            "Pipeline accepts at most " +
            std::to_string(
                specforge::
                    kAutomationQueueCapacity) +
            " commands.";
        return false;
    }

    struct RequestState {
        bool accepted = false;
        bool terminal = false;
    };
    std::unordered_map<std::string, RequestState>
        requests;
    std::size_t sent_count = 0;
    const auto response_deadline =
        std::chrono::steady_clock::now() +
        kLauncherCommandResponseTimeout;
    for (const HumanCommand& command : commands) {
        const std::string request_id =
            "request-" +
            std::to_string(request_number++);
        const std::string request =
            specforge::SerializeAutomationCommandRequest(
                request_id,
                command.kind,
                command.parameters);
        if (!client.SendUntil(
                request,
                response_deadline,
                error_message)) {
            if (sent_count != 0) {
                client.Close();
                error_message = ResponseWaitFailure(
                    "pipeline",
                    error_message);
            }
            return false;
        }
        ++sent_count;
        requests.emplace(
            request_id,
            RequestState{});
    }

    std::size_t terminal_count = 0;
    while (terminal_count < requests.size()) {
        std::string response;
        if (!client.ReceiveUntil(
                response,
                response_deadline,
                error_message)) {
            client.Close();
            error_message = ResponseWaitFailure(
                "pipeline",
                error_message);
            return false;
        }
        std::cout << response << '\n'
                  << std::flush;
        const auto parsed =
            specforge::ParseAutomationServerMessage(
                response);
        if (!parsed.message) {
            error_message =
                parsed.error_message;
            client.Close();
            error_message = ResponseWaitFailure(
                "pipeline",
                error_message);
            return false;
        }
        auto state = requests.find(
            parsed.message->request_id);
        if (state == requests.end()) {
            error_message =
                "Received a pipeline response for an unknown request.";
            client.Close();
            error_message = ResponseWaitFailure(
                "pipeline",
                error_message);
            return false;
        }
        if (parsed.message->status == "accepted") {
            if (state->second.accepted ||
                state->second.terminal) {
                error_message =
                    "Received a duplicate or late accepted response.";
                client.Close();
                error_message = ResponseWaitFailure(
                    "pipeline",
                    error_message);
                return false;
            }
            state->second.accepted = true;
            continue;
        }
        if (!state->second.accepted ||
            state->second.terminal ||
            (parsed.message->status !=
                 "completed" &&
             parsed.message->status != "failed" &&
             parsed.message->status !=
                 "canceled")) {
            error_message =
                "Received an invalid pipeline terminal response.";
            client.Close();
            error_message = ResponseWaitFailure(
                "pipeline",
                error_message);
            return false;
        }
        state->second.terminal = true;
        ++terminal_count;
    }
    return true;
}

bool SendCommandAndDisconnectAfterAccepted(
    specforge::AutomationNamedPipeClient& client,
    const HumanCommand& command,
    std::uint64_t request_number,
    std::string& error_message)
{
    const std::string request_id =
        "request-" +
        std::to_string(request_number);
    if (!client.Send(
            specforge::
                SerializeAutomationCommandRequest(
                    request_id,
                    command.kind,
                    command.parameters),
            error_message)) {
        return false;
    }
    const auto response_deadline =
        std::chrono::steady_clock::now() +
        kLauncherCommandResponseTimeout;
    for (;;) {
        std::string response;
        if (!client.ReceiveUntil(
                response,
                response_deadline,
                error_message)) {
            client.Close();
            error_message = ResponseWaitFailure(
                "disconnect-after-accepted",
                error_message);
            return false;
        }
        std::cout << response << '\n'
                  << std::flush;
        const auto parsed =
            specforge::ParseAutomationServerMessage(
                response);
        if (!parsed.message ||
            parsed.message->request_id != request_id) {
            error_message =
                parsed.error_message.empty()
                ? "Received a response for the wrong disconnect request."
                : parsed.error_message;
            client.Close();
            error_message = ResponseWaitFailure(
                "disconnect-after-accepted",
                error_message);
            return false;
        }
        if (parsed.message->status == "accepted") {
            client.Close();
            return true;
        }
        error_message =
            "The disconnect request reached a terminal state before acceptance.";
        return false;
    }
}

class LauncherChildJobGuard {
public:
    LauncherChildJobGuard() = default;

    ~LauncherChildJobGuard()
    {
        if (job_ != nullptr) {
            CloseHandle(job_);
            job_ = nullptr;
        }
    }

    LauncherChildJobGuard(
        const LauncherChildJobGuard&) = delete;
    LauncherChildJobGuard& operator=(
        const LauncherChildJobGuard&) = delete;

    [[nodiscard]] bool Assign(
        HANDLE child_process,
        std::string& error_message)
    {
        if (child_process == nullptr) {
            error_message =
                "Could not assign an empty GUI process handle to its launcher Job Object.";
            return false;
        }
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_ == nullptr) {
            error_message =
                "Could not create the launcher GUI Job Object (Win32 error " +
                std::to_string(GetLastError()) + ").";
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(
                job_,
                JobObjectExtendedLimitInformation,
                &limits,
                sizeof(limits))) {
            error_message =
                "Could not configure the launcher GUI Job Object (Win32 error " +
                std::to_string(GetLastError()) + ").";
            CloseHandle(job_);
            job_ = nullptr;
            return false;
        }
        if (!AssignProcessToJobObject(job_, child_process)) {
            const DWORD assign_error = GetLastError();
            BOOL already_in_parent_job = FALSE;
            if (IsProcessInJob(
                    child_process,
                    nullptr,
                    &already_in_parent_job) &&
                already_in_parent_job) {
                // A CTest/runner kill-on-close Job may already own this
                // launcher. Windows does not permit an unrelated nested Job
                // on all supported hosts; the inherited parent Job still
                // gives the GUI the same forced-termination ownership.
                CloseHandle(job_);
                job_ = nullptr;
                return true;
            }
            error_message =
                "Could not assign the GUI process to the launcher Job Object (Win32 error " +
                std::to_string(assign_error) + ").";
            CloseHandle(job_);
            job_ = nullptr;
            return false;
        }
        return true;
    }

private:
    HANDLE job_ = nullptr;
};

class LauncherOwnedProcessGuard {
public:
    LauncherOwnedProcessGuard(
        HANDLE process,
        specforge::AutomationNamedPipeClient& client)
        : process_(process),
          client_(client)
    {
    }

    ~LauncherOwnedProcessGuard()
    {
        Cleanup();
    }

    LauncherOwnedProcessGuard(
        const LauncherOwnedProcessGuard&) = delete;
    LauncherOwnedProcessGuard& operator=(
        const LauncherOwnedProcessGuard&) = delete;

    void MarkHandshakeComplete() noexcept
    {
        handshake_complete_ = true;
    }

    void MarkNormalQuitRequested() noexcept
    {
        normal_quit_requested_ = true;
    }

    [[nodiscard]] bool WaitForExit(
        DWORD timeout_ms,
        DWORD& exit_code,
        std::string& error_message)
    {
        const DWORD wait_result =
            WaitForSingleObject(
                process_,
                timeout_ms);
        if (wait_result != WAIT_OBJECT_0) {
            graceful_wait_exhausted_ = true;
            error_message =
                wait_result == WAIT_TIMEOUT
                ? "SpecForge did not complete normal shutdown within " +
                    std::to_string(
                        kLauncherProcessExitTimeout.count()) +
                    " seconds."
                : "Could not wait for the SpecForge automation process.";
            return false;
        }
        if (!GetExitCodeProcess(
                process_,
                &exit_code)) {
            error_message =
                "Could not read the SpecForge automation process exit code.";
            return false;
        }
        CloseOwnedHandles();
        return true;
    }

private:
    void Cleanup() noexcept
    {
        if (process_ == nullptr) {
            return;
        }

        bool exited =
            WaitForSingleObject(
                process_,
                0) == WAIT_OBJECT_0;
        if (!exited &&
            !graceful_wait_exhausted_) {
            if (handshake_complete_ &&
                client_.connected() &&
                !normal_quit_requested_) {
                std::string ignored_error;
                (void)client_.Send(
                    specforge::
                        SerializeAutomationCommandRequest(
                            "launcher-cleanup-quit",
                            specforge::
                                AutomationCommandKind::
                                    AppQuit),
                    ignored_error);
            }
            exited =
                WaitForSingleObject(
                    process_,
                    static_cast<DWORD>(
                        kLauncherNormalCleanupTimeout.count() *
                        1000)) == WAIT_OBJECT_0;
        }

        client_.Close();
        if (!exited) {
            if (TerminateProcess(
                    process_,
                    ERROR_PROCESS_ABORTED)) {
                (void)WaitForSingleObject(
                    process_,
                    5000);
            }
        }
        CloseOwnedHandles();
    }

    void CloseOwnedHandles() noexcept
    {
        client_.Close();
        if (process_ != nullptr) {
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

    HANDLE process_ = nullptr;
    specforge::AutomationNamedPipeClient& client_;
    bool handshake_complete_ = false;
    bool normal_quit_requested_ = false;
    bool graceful_wait_exhausted_ = false;
};

void PrintUsage()
{
    std::cout
        << "Usage: SpecForgeAutomation [--app <SpecForge.exe>] [--state-root <new-absolute-directory>] [--labeling-state-seed <production-cache.json>]\n"
        << "Commands: setting get <ui.language|ui.scale>, setting set <ui.language|ui.scale> <value>, panel get <name>, panel set <name> <true|false>, source open <absolute-path>, spectrum goto <zero-based-index>, spectrum goto name <exact-name>, label assign <code> [spectrum <index>|spectrum name <exact-name>], frame capture <absolute-png-under-state-root>, profile start, profile stop, state get, wait idle, app quit, help\n"
        << "Harness controls: pipeline begin ... pipeline end; disconnect after accepted <next command>\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    const LauncherOptions options =
        ParseOptions(argc, argv);
    if (options.error_message == "help") {
        PrintUsage();
        return 0;
    }
    if (!options.error_message.empty()) {
        std::cerr << options.error_message << '\n';
        PrintUsage();
        return 2;
    }

    std::error_code path_error;
    const std::filesystem::path app_path =
        std::filesystem::absolute(
            options.app_path,
            path_error);
    if (path_error ||
        !std::filesystem::is_regular_file(
            app_path)) {
        std::cerr
            << "SpecForge GUI executable was not found.\n";
        return 2;
    }

    const std::optional<std::string> instance_id =
        RandomHex(16U);
    const std::optional<std::string> nonce =
        RandomHex(32U);
    if (!instance_id || !nonce) {
        std::cerr
            << "Could not generate automation entropy.\n";
        return 2;
    }
    const std::wstring pipe_name =
        specforge::AutomationPipeNameForInstance(
            *instance_id);

    std::filesystem::path state_root =
        options.state_root.value_or(
            std::filesystem::temp_directory_path() /
            "SpecForgeAutomation" /
            *instance_id);
    state_root =
        std::filesystem::absolute(
            state_root,
            path_error);
    if (path_error || state_root.empty()) {
        std::cerr
            << "Could not resolve the automation state root.\n";
        return 2;
    }
    std::filesystem::path ordinary_state_root;
    try {
        ordinary_state_root =
            specforge::
                OrdinaryUserStateRootForExecutable(
                    app_path);
    } catch (const std::exception& error) {
        std::cerr
            << "Could not resolve the ordinary SpecForge state root: "
            << error.what() << '\n';
        return 2;
    }
    if (!specforge::AutomationStateRootIsIndependent(
            state_root,
            ordinary_state_root)) {
        std::cerr
            << "Automation state root must be independent from the ordinary SpecForge state root.\n";
        return 2;
    }
    if (PathContainsReparsePoint(
            state_root.parent_path())) {
        std::cerr
            << "Automation state root parent path must not traverse a reparse point.\n";
        return 2;
    }
    std::string error_message;
    specforge::AutomationReadOnlyFileLease
        labeling_state_seed;
    if (options.labeling_state_seed) {
        labeling_state_seed =
            specforge::
                PinAutomationReadOnlyFile(
                    *options
                         .labeling_state_seed,
                    error_message);
        if (!labeling_state_seed.valid() ||
            !ValidateLabelingStateSeed(
                labeling_state_seed,
                state_root,
                ordinary_state_root,
                error_message)) {
            std::cerr << error_message << '\n';
            return 2;
        }
    }
    std::vector<wchar_t> child_environment;
    if (!BuildAutomationChildEnvironment(
            child_environment,
            error_message)) {
        std::cerr << error_message << '\n';
        return 2;
    }
    specforge::AutomationStateRootLease
        state_root_lease =
            specforge::
                CreatePinnedAutomationStateRoot(
                    state_root,
                    error_message);
    if (!state_root_lease.valid()) {
        std::cerr << error_message << '\n';
        return 2;
    }
    PrelaunchStateRootGuard
        prelaunch_state_root(
            state_root_lease);
    if (options.labeling_state_seed &&
        !MaterializeLabelingStateSeed(
            state_root_lease,
            labeling_state_seed,
            error_message)) {
        std::cerr << error_message << '\n';
        return 2;
    }
    labeling_state_seed =
        specforge::
            AutomationReadOnlyFileLease{};

    std::wstring command_line =
        BuildGuiCommandLine(
            app_path,
            pipe_name,
            *nonce,
            *instance_id,
            state_root);
    std::vector<wchar_t> mutable_command_line(
        command_line.begin(),
        command_line.end());
    mutable_command_line.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(
            app_path.c_str(),
            mutable_command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NEW_PROCESS_GROUP |
                CREATE_UNICODE_ENVIRONMENT,
            child_environment.data(),
            app_path.parent_path().c_str(),
            &startup,
            &process)) {
        std::cerr
            << "Could not launch SpecForge automation GUI (Win32 error "
            << GetLastError() << ").\n";
        return 2;
    }
    LauncherChildJobGuard child_job;
    if (!child_job.Assign(process.hProcess, error_message)) {
        std::cerr << error_message << '\n';
        (void)TerminateProcess(
            process.hProcess,
            ERROR_PROCESS_ABORTED);
        (void)WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 2;
    }
    CloseHandle(process.hThread);
    prelaunch_state_root.Release();

    specforge::AutomationNamedPipeClient client;
    LauncherOwnedProcessGuard child_process(
        process.hProcess,
        client);
    const auto connect_deadline =
        std::chrono::steady_clock::now() +
        kLauncherConnectTimeout;
    while (!client.connected() &&
           std::chrono::steady_clock::now() <
               connect_deadline) {
        if (WaitForSingleObject(
                process.hProcess,
                0) == WAIT_OBJECT_0) {
            break;
        }
        error_message.clear();
        (void)client.Connect(
            pipe_name,
            100ms,
            error_message);
    }
    if (!client.connected()) {
        std::cerr
            << "Could not connect to SpecForge automation: "
            << error_message << '\n';
        const std::filesystem::path startup_error =
            state_root /
            "automation-startup-error.txt";
        std::ifstream diagnostic(startup_error);
        if (diagnostic) {
            std::cerr << diagnostic.rdbuf();
        }
        return 2;
    }

    if (!client.Send(
            specforge::SerializeAutomationHelloRequest(
                "hello-1",
                *nonce),
            error_message)) {
        std::cerr << error_message << '\n';
        return 2;
    }
    std::string hello_response;
    const auto hello_response_deadline =
        std::chrono::steady_clock::now() +
        kLauncherHelloResponseTimeout;
    if (!client.ReceiveUntil(
            hello_response,
            hello_response_deadline,
            error_message)) {
        client.Close();
        if (error_message ==
            specforge::
                kAutomationNamedPipeReceiveDeadlineExpired) {
            error_message =
                "Automation hello response timed out after " +
                std::to_string(
                    kLauncherHelloResponseTimeout.count()) +
                " seconds.";
        }
        std::cerr << error_message << '\n';
        return 2;
    }
    std::cout << hello_response << '\n';
    const auto hello =
        specforge::ParseAutomationServerMessage(
            hello_response);
    if (!hello.message ||
        hello.message->status != "completed" ||
        hello.message->protocol_version !=
            specforge::kAutomationProtocolVersion ||
        hello.message->instance_id != *instance_id) {
        std::cerr
            << "Automation handshake failed.\n";
        return 2;
    }
    child_process.MarkHandshakeComplete();

    std::cout
        << "Automation state root: "
        << state_root.string() << '\n'
        << "SpecForge PID: "
        << process.dwProcessId << '\n';
    PrintUsage();

    std::uint64_t request_number = 1;
    bool app_quit_sent = false;
    bool intentional_disconnect = false;
    std::string line;
    for (;;) {
        error_message.clear();
        const LauncherLineReadStatus
            line_status =
                ReadLauncherLine(
                    line,
                    error_message);
        if (line_status ==
            LauncherLineReadStatus::End) {
            break;
        }
        if (line_status ==
            LauncherLineReadStatus::Error) {
            std::cerr << error_message << '\n';
            return 2;
        }
        const std::string command_line_text =
            Trim(std::move(line));
        if (command_line_text.empty()) {
            continue;
        }
        if (LowerAscii(command_line_text) ==
            "help") {
            PrintUsage();
            continue;
        }
        if (LowerAscii(command_line_text) ==
            "pipeline begin") {
            std::vector<HumanCommand>
                pipeline;
            bool ended = false;
            for (;;) {
                error_message.clear();
                const LauncherLineReadStatus
                    pipeline_line_status =
                        ReadLauncherLine(
                            line,
                            error_message);
                if (pipeline_line_status ==
                    LauncherLineReadStatus::End) {
                    break;
                }
                if (pipeline_line_status ==
                    LauncherLineReadStatus::Error) {
                    std::cerr
                        << error_message << '\n';
                    return 2;
                }
                const std::string pipeline_line =
                    Trim(std::move(line));
                if (LowerAscii(pipeline_line) ==
                    "pipeline end") {
                    ended = true;
                    break;
                }
                if (pipeline_line.empty()) {
                    continue;
                }
                error_message.clear();
                auto pipeline_command =
                    CommandFromHumanLine(
                        pipeline_line,
                        error_message);
                if (!pipeline_command) {
                    std::cerr
                        << error_message << '\n';
                    return 2;
                }
                pipeline.push_back(
                    std::move(*pipeline_command));
            }
            if (!ended || pipeline.empty()) {
                std::cerr
                    << "pipeline begin requires one or more commands followed by pipeline end.\n";
                return 2;
            }
            const bool pipeline_contains_app_quit =
                std::any_of(
                    pipeline.begin(),
                    pipeline.end(),
                    [](const HumanCommand& command) {
                        return command.kind ==
                            specforge::AutomationCommandKind::
                                AppQuit;
                    });
            if (!SendPipelineAndWait(
                    client,
                    pipeline,
                    request_number,
                    error_message)) {
                std::cerr
                    << error_message << '\n';
                return 2;
            }
            if (pipeline_contains_app_quit) {
                // An app.quit inside a pipeline already requests normal
                // shutdown. Do not send the fallback app.quit below after
                // the pipeline has drained; the second request races the
                // GUI shutdown and creates a spurious shutting_down result.
                app_quit_sent = true;
                child_process.MarkNormalQuitRequested();
            }
            continue;
        }
        if (LowerAscii(command_line_text) ==
            "disconnect after accepted") {
            std::string disconnected_line;
            for (;;) {
                error_message.clear();
                const LauncherLineReadStatus
                    disconnected_line_status =
                        ReadLauncherLine(
                            disconnected_line,
                            error_message);
                if (disconnected_line_status ==
                    LauncherLineReadStatus::Error) {
                    std::cerr
                        << error_message << '\n';
                    return 2;
                }
                if (disconnected_line_status ==
                        LauncherLineReadStatus::End ||
                    !Trim(
                         disconnected_line)
                         .empty()) {
                    break;
                }
            }
            error_message.clear();
            auto disconnected_command =
                CommandFromHumanLine(
                    Trim(
                        std::move(
                            disconnected_line)),
                    error_message);
            if (!disconnected_command ||
                !SendCommandAndDisconnectAfterAccepted(
                    client,
                    *disconnected_command,
                    request_number++,
                    error_message)) {
                std::cerr
                    << (error_message.empty()
                            ? "disconnect after accepted requires a valid following command."
                            : error_message)
                    << '\n';
                return 2;
            }
            intentional_disconnect = true;
            break;
        }
        error_message.clear();
        const auto command =
            CommandFromHumanLine(
                command_line_text,
                error_message);
        if (!command) {
            std::cerr << error_message << '\n';
            continue;
        }
        if (!SendCommandAndWait(
                client,
                command->kind,
                command->parameters,
                request_number++,
                error_message)) {
            std::cerr << error_message << '\n';
            return 2;
        }
        if (command->kind ==
            specforge::AutomationCommandKind::
                AppQuit) {
            app_quit_sent = true;
            child_process.MarkNormalQuitRequested();
            break;
        }
    }

    if (intentional_disconnect) {
        return 0;
    }

    std::optional<std::string> shutdown_error;
    if (!app_quit_sent && client.connected()) {
        if (SendCommandAndWait(
                client,
                specforge::AutomationCommandKind::AppQuit,
                specforge::AutomationCommandParameters{},
                request_number,
                error_message)) {
            app_quit_sent = true;
            child_process.MarkNormalQuitRequested();
        } else {
            shutdown_error = error_message;
            client.Close();
        }
    }

    DWORD exit_code = 0;
    if (!child_process.WaitForExit(
            static_cast<DWORD>(
                kLauncherProcessExitTimeout.count() *
                1000),
            exit_code,
            error_message)) {
        if (shutdown_error) {
            std::cerr << *shutdown_error << '\n';
        }
        std::cerr << error_message << '\n';
        return 2;
    }
    if (shutdown_error) {
        std::cerr << *shutdown_error << '\n';
        return 2;
    }
    return static_cast<int>(exit_code);
}
