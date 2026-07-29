#include "automation/automation_named_pipe.h"
#include "automation/automation_protocol.h"
#include "automation/automation_startup.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct LauncherOptions {
    std::filesystem::path app_path;
    std::optional<std::filesystem::path> state_root;
    std::string error_message;
};

std::filesystem::path CurrentExecutablePath()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2U);
    }
}

std::wstring QuoteArgument(std::wstring_view value)
{
    if (value.empty()) {
        return L"\"\"";
    }
    const bool requires_quotes =
        value.find_first_of(L" \t\n\v\"") !=
        std::wstring_view::npos;
    if (!requires_quotes) {
        return std::wstring(value);
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2U + 1U, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

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
        command_line += QuoteArgument(argument);
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
        CurrentExecutablePath();
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

std::string TrimLower(std::string value)
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

std::optional<specforge::AutomationCommandKind>
CommandFromHumanLine(std::string_view line)
{
    if (line == "state get" ||
        line == "state.get") {
        return specforge::AutomationCommandKind::
            StateGet;
    }
    if (line == "wait idle" ||
        line == "wait.idle") {
        return specforge::AutomationCommandKind::
            WaitIdle;
    }
    if (line == "app quit" ||
        line == "app.quit") {
        return specforge::AutomationCommandKind::
            AppQuit;
    }
    return std::nullopt;
}

bool SendCommandAndWait(
    specforge::AutomationNamedPipeClient& client,
    specforge::AutomationCommandKind command,
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
                    command),
            error_message)) {
        return false;
    }

    bool accepted = false;
    for (;;) {
        std::string response;
        if (!client.Receive(
                response,
                error_message)) {
            return false;
        }
        std::cout << response << '\n';
        const auto parsed =
            specforge::ParseAutomationServerMessage(
                response);
        if (!parsed.message ||
            parsed.message->request_id != request_id) {
            error_message =
                parsed.error_message.empty()
                ? "Received a response for the wrong request."
                : parsed.error_message;
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
                ? "SpecForge did not complete normal shutdown within 10 seconds."
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
                    5000) == WAIT_OBJECT_0;
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
        << "Usage: SpecForgeAutomation [--app <SpecForge.exe>] [--state-root <new-absolute-directory>]\n"
        << "Commands: state get, wait idle, app quit, help\n";
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
    std::vector<wchar_t> child_environment;
    std::string error_message;
    if (!BuildAutomationChildEnvironment(
            child_environment,
            error_message)) {
        std::cerr << error_message << '\n';
        return 2;
    }
    if (std::filesystem::exists(state_root)) {
        std::cerr
            << "Automation state root must not already exist.\n";
        return 2;
    }
    if (!std::filesystem::create_directories(
            state_root,
            path_error) ||
        path_error) {
        std::cerr
            << "Could not create the automation state root.\n";
        return 2;
    }

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
    CloseHandle(process.hThread);

    specforge::AutomationNamedPipeClient client;
    LauncherOwnedProcessGuard child_process(
        process.hProcess,
        client);
    const auto connect_deadline =
        std::chrono::steady_clock::now() + 10s;
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
    if (!client.Receive(
            hello_response,
            error_message)) {
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
    std::string line;
    while (std::getline(std::cin, line)) {
        const std::string command_line_text =
            TrimLower(std::move(line));
        if (command_line_text.empty()) {
            continue;
        }
        if (command_line_text == "help") {
            PrintUsage();
            continue;
        }
        const auto command =
            CommandFromHumanLine(
                command_line_text);
        if (!command) {
            std::cerr
                << "Unknown command. GUI input was not sent.\n";
            continue;
        }
        if (!SendCommandAndWait(
                client,
                *command,
                request_number++,
                error_message)) {
            std::cerr << error_message << '\n';
            return 2;
        }
        if (*command ==
            specforge::AutomationCommandKind::
                AppQuit) {
            app_quit_sent = true;
            child_process.MarkNormalQuitRequested();
            break;
        }
    }

    if (!app_quit_sent && client.connected()) {
        if (SendCommandAndWait(
                client,
                specforge::AutomationCommandKind::AppQuit,
                request_number,
                error_message)) {
            app_quit_sent = true;
            child_process.MarkNormalQuitRequested();
        }
    }

    DWORD exit_code = 0;
    if (!child_process.WaitForExit(
            10000,
            exit_code,
            error_message)) {
        std::cerr << error_message << '\n';
        return 2;
    }
    return static_cast<int>(exit_code);
}
