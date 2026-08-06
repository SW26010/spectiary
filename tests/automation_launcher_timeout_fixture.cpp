#include "automation/automation_protocol.h"
#include "automation/automation_startup.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct HandleGuard {
    HANDLE value = INVALID_HANDLE_VALUE;

    HandleGuard() = default;

    ~HandleGuard()
    {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }

    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
};

std::optional<std::wstring> ArgumentValue(
    int argc,
    wchar_t** argv,
    std::wstring_view name)
{
    for (int index = 1;
         index + 1 < argc;
         ++index) {
        if (std::wstring_view(argv[index]) == name) {
            return std::wstring(argv[index + 1]);
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> EnvironmentValue(
    const wchar_t* name)
{
    const DWORD required =
        GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name,
        value.data(),
        required);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    return value;
}

std::string NarrowAscii(std::wstring_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

bool WriteMarker(
    const std::filesystem::path& state_root,
    std::string_view name,
    std::string_view contents)
{
    std::ofstream marker(
        state_root / std::filesystem::path(std::string(name)),
        std::ios::binary | std::ios::trunc);
    marker << contents;
    return marker.good();
}

bool ReadPipeMessage(
    HANDLE pipe,
    std::string& message)
{
    std::vector<char> buffer(
        specforge::kAutomationMaxMessageBytes + 1U);
    DWORD bytes_read = 0;
    if (!ReadFile(
            pipe,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_read,
            nullptr)) {
        return false;
    }
    if (bytes_read >
        specforge::kAutomationMaxMessageBytes) {
        return false;
    }
    message.assign(
        buffer.data(),
        static_cast<std::size_t>(bytes_read));
    return true;
}

bool WritePipeMessage(
    HANDLE pipe,
    std::string_view message)
{
    DWORD bytes_written = 0;
    return WriteFile(
               pipe,
               message.data(),
               static_cast<DWORD>(message.size()),
               &bytes_written,
               nullptr) != FALSE &&
        bytes_written == message.size();
}

int RunTimeoutFixture(
    int argc,
    wchar_t** argv,
    std::wstring_view mode)
{
    const auto pipe_name =
        ArgumentValue(argc, argv, L"--automation-pipe");
    const auto nonce =
        ArgumentValue(argc, argv, L"--automation-nonce");
    const auto instance_id =
        ArgumentValue(
            argc,
            argv,
            L"--automation-instance");
    const auto state_root =
        ArgumentValue(
            argc,
            argv,
            L"--automation-state-root");
    if (!pipe_name || !nonce || !instance_id ||
        !state_root ||
        (mode != L"hello-no-response" &&
         mode != L"accepted-no-terminal" &&
         mode != L"no-accepted-no-terminal" &&
         mode != L"pipeline-write-stall" &&
         mode != L"quit-no-terminal")) {
        return 90;
    }

    const std::filesystem::path root(*state_root);
    if (!WriteMarker(
            root,
            "timeout-fixture.pid",
            std::to_string(GetCurrentProcessId()))) {
        return 91;
    }

    HandleGuard pipe;
    pipe.value = CreateNamedPipeW(
        pipe_name->c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE |
            PIPE_READMODE_MESSAGE |
            PIPE_WAIT,
        1,
        static_cast<DWORD>(
            specforge::kAutomationMaxMessageBytes),
        static_cast<DWORD>(
            specforge::kAutomationMaxMessageBytes),
        1'000,
        nullptr);
    if (pipe.value == INVALID_HANDLE_VALUE ||
        !WriteMarker(root, "timeout-fixture-ready.txt", "ready\n")) {
        return 92;
    }

    const BOOL connected = ConnectNamedPipe(
        pipe.value,
        nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
        return 93;
    }

    std::string hello_request;
    if (!ReadPipeMessage(pipe.value, hello_request)) {
        return 94;
    }
    const auto hello =
        specforge::ParseAutomationClientMessage(hello_request);
    if (!hello.message ||
        hello.message->kind !=
            specforge::AutomationClientMessage::Kind::Hello ||
        !WriteMarker(
            root,
            "hello-request-observed.txt",
            hello_request)) {
        return 95;
    }

    if (mode == L"hello-no-response") {
        Sleep(60'000);
        return 0;
    }

    if (!WritePipeMessage(
            pipe.value,
            specforge::SerializeAutomationHelloResponse(
                hello.message->request_id,
                NarrowAscii(*instance_id)))) {
        return 96;
    }

    std::string command_request;
    if (!ReadPipeMessage(pipe.value, command_request)) {
        return 97;
    }
    const auto command =
        specforge::ParseAutomationClientMessage(command_request);
    if (!command.message ||
        command.message->kind !=
            specforge::AutomationClientMessage::Kind::Request ||
        !WriteMarker(
            root,
            mode == L"no-accepted-no-terminal"
                ? "request-observed.txt"
                : "accepted-request-observed.txt",
            command_request)) {
        return 98;
    }

    if (mode == L"no-accepted-no-terminal") {
        // The request was received, but this fixture deliberately withholds
        // both accepted and terminal responses. A successful client send is
        // still outcome-ambiguous even when accepted was not observed.
        Sleep(60'000);
        return 0;
    }

    if (!WritePipeMessage(
            pipe.value,
            specforge::SerializeAutomationAcceptedResponse(
                command.message->request_id,
                command.message->command))) {
        return 99;
    }

    if (mode == L"pipeline-write-stall") {
        if (!WriteMarker(
                root,
                "pipeline-first-accepted.txt",
                command_request)) {
            return 100;
        }

        // Stop reading after the first accepted request. The launcher must
        // apply the same absolute response deadline to every pipeline write,
        // including the later requests that fill this pipe's input buffer.
        Sleep(60'000);
        return 0;
    }

    if (mode == L"quit-no-terminal") {
        if (!WritePipeMessage(
                pipe.value,
                specforge::SerializeAutomationTerminalResponse(
                    command.message->request_id,
                    command.message->command,
                    "completed"))) {
            return 100;
        }

        std::string quit_request;
        if (!ReadPipeMessage(pipe.value, quit_request)) {
            return 101;
        }
        const auto quit =
            specforge::ParseAutomationClientMessage(quit_request);
        if (!quit.message ||
            quit.message->kind !=
                specforge::AutomationClientMessage::Kind::Request ||
            quit.message->command !=
                specforge::AutomationCommandKind::AppQuit ||
            !WriteMarker(
                root,
                "quit-request-observed.txt",
                quit_request)) {
            return 102;
        }

        // Exit successfully without writing app.quit's terminal response.
        // The launcher must preserve the response failure instead of
        // treating this GUI exit code as a successful automation run.
        return 0;
    }

    std::string extra_request;
    if (ReadPipeMessage(pipe.value, extra_request)) {
        (void)WriteMarker(
            root,
            "extra-request-after-accepted.txt",
            extra_request);
    }

    // Deliberately ignore the open pipe and the accepted request's terminal.
    // The launcher must close and then terminate this exact owned process;
    // this fixture must not make cleanup appear graceful by exiting itself.
    Sleep(60'000);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    const auto mode = EnvironmentValue(
        L"SPECFORGE_AUTOMATION_TIMEOUT_FIXTURE");
    if (!mode) {
        return 90;
    }
    return RunTimeoutFixture(argc, argv, *mode);
}
