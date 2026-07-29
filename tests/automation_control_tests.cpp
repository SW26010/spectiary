#include "app/local_user_state_json.h"
#include "automation/automation_named_pipe.h"
#include "automation/automation_protocol.h"
#include "automation/automation_startup.h"
#include "automation/automation_state.h"
#include "platform/win32_window.h"

#include <Windows.h>
#include <Aclapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace std::chrono_literals;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::string UniqueInstanceId()
{
    static std::atomic_uint32_t counter = 0;
    std::ostringstream value;
    value << std::hex << std::setfill('0')
          << std::setw(8) << GetCurrentProcessId()
          << std::setw(16) << GetTickCount64()
          << std::setw(8) << ++counter;
    return value.str();
}

struct RunningServer {
    std::string instance_id = UniqueInstanceId();
    std::string nonce = std::string(64U, 'a');
    std::wstring pipe_name =
        specforge::AutomationPipeNameForInstance(
            instance_id);
    std::atomic_uint32_t notification_count = 0;
    specforge::AutomationNamedPipeServer server{
        pipe_name,
        nonce,
        instance_id};

    RunningServer()
    {
        std::string error;
        Require(
            server.Start(
                [this]() {
                    ++notification_count;
                },
                error),
            error);
    }
};

std::optional<std::wstring> EnvironmentValue(
    const wchar_t* name)
{
    const DWORD required =
        GetEnvironmentVariableW(
            name,
            nullptr,
            0);
    if (required == 0) {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written =
        GetEnvironmentVariableW(
            name,
            value.data(),
            required);
    if (written == 0 ||
        written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    return value;
}

std::optional<std::wstring> ArgumentValue(
    int argc,
    wchar_t** argv,
    std::wstring_view name)
{
    for (int index = 1;
         index + 1 < argc;
         ++index) {
        if (std::wstring_view(argv[index]) ==
            name) {
            return std::wstring(
                argv[index + 1]);
        }
    }
    return std::nullopt;
}

std::string NarrowAscii(std::wstring_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        result.push_back(
            static_cast<char>(character));
    }
    return result;
}

int RunLauncherCleanupFixture(
    int argc,
    wchar_t** argv,
    std::wstring_view mode)
{
    const auto pipe_name =
        ArgumentValue(
            argc,
            argv,
            L"--automation-pipe");
    const auto nonce =
        ArgumentValue(
            argc,
            argv,
            L"--automation-nonce");
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
    if (!pipe_name ||
        !nonce ||
        !instance_id ||
        !state_root) {
        return 90;
    }

    specforge::AutomationNamedPipeServer server(
        *pipe_name,
        NarrowAscii(*nonce),
        NarrowAscii(*instance_id));
    std::string error;
    if (!server.Start([]() {}, error)) {
        return 91;
    }

    bool command_failed = false;
    const auto deadline =
        std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() <
           deadline) {
        for (const auto& command :
             server.TakePendingCommands()) {
            if (!command_failed) {
                if (command.command !=
                    specforge::
                        AutomationCommandKind::
                            StateGet) {
                    server.Fail(
                        command,
                        "launcher_fixture",
                        "Launcher cleanup fixture expected state.get.");
                    continue;
                }
                command_failed = true;
                if (mode == L"disconnect") {
                    server.Shutdown(
                        "launcher_fixture_disconnect");
                    std::this_thread::sleep_for(
                        30s);
                    return 92;
                }
                server.Fail(
                    command,
                    "launcher_fixture",
                    "Forced launcher command failure.");
                continue;
            }

            if (command.command ==
                    specforge::
                        AutomationCommandKind::
                            AppQuit &&
                server.TryBeginAppQuit(command)) {
                server.Complete(command);
                std::ofstream marker(
                    std::filesystem::path(
                        *state_root) /
                        "launcher-cleanup-completed.txt",
                    std::ios::binary |
                        std::ios::trunc);
                marker
                    << "normal app.quit cleanup\n";
                marker.close();
                server.Shutdown();
                return marker ? 0 : 93;
            }
            server.Fail(
                command,
                "launcher_fixture",
                "Launcher cleanup fixture expected app.quit.");
        }
        std::this_thread::sleep_for(5ms);
    }
    return 94;
}

specforge::AutomationServerMessage ReceiveParsed(
    specforge::AutomationNamedPipeClient& client)
{
    std::string message;
    std::string error;
    Require(
        client.Receive(message, error),
        error);
    const auto parsed =
        specforge::ParseAutomationServerMessage(
            message);
    Require(
        parsed.message.has_value(),
        parsed.error_message);
    return *parsed.message;
}

void ConnectAndHandshake(
    RunningServer& fixture,
    specforge::AutomationNamedPipeClient& client)
{
    std::string error;
    Require(
        client.Connect(
            fixture.pipe_name,
            1s,
        error),
        error);
    Require(
        client.Send(
            specforge::SerializeAutomationHelloRequest(
                "hello-1",
                fixture.nonce),
        error),
        error);
    const auto hello = ReceiveParsed(client);
    Require(
        hello.type == "hello" &&
            hello.status == "completed" &&
            hello.protocol_version ==
                specforge::
                    kAutomationProtocolVersion &&
            hello.instance_id ==
                fixture.instance_id,
        "hello handshake should return version, capabilities contract and instance identity");
}

void SendRequest(
    specforge::AutomationNamedPipeClient& client,
    std::string_view request_id,
    specforge::AutomationCommandKind command)
{
    std::string error;
    Require(
        client.Send(
            specforge::
                SerializeAutomationCommandRequest(
                    request_id,
                    command),
            error),
        error);
}

const specforge::JsonValue& RequireObjectMember(
    const specforge::JsonValue& object,
    std::string_view name)
{
    const specforge::JsonValue* member =
        specforge::JsonObjectMember(object, name);
    Require(
        member != nullptr &&
            member->kind ==
                specforge::JsonValue::Kind::Object,
        "expected JSON object member");
    return *member;
}

void TestProtocolAndStableState()
{
    const auto hello =
        specforge::ParseAutomationClientMessage(
            specforge::SerializeAutomationHelloRequest(
                "hello.valid-1",
                std::string(64U, 'b')));
    Require(
        hello.message &&
            hello.message->kind ==
                specforge::AutomationClientMessage::
                    Kind::Hello &&
            hello.message->protocol_version ==
                specforge::
                    kAutomationProtocolVersion,
        "hello request should round-trip through UTF-8 JSON");

    const auto request =
        specforge::ParseAutomationClientMessage(
            specforge::
                SerializeAutomationCommandRequest(
                    "request-1",
                    specforge::
                        AutomationCommandKind::
                            WaitIdle));
    Require(
        request.message &&
            request.message->command ==
                specforge::
                    AutomationCommandKind::
                        WaitIdle,
        "wait.idle should parse as a structured command");

    const auto invalid_id =
        specforge::ParseAutomationClientMessage(
            R"({"type":"request","request_id":"bad id","command":"state.get"})");
    Require(
        !invalid_id.message &&
            invalid_id.error_code ==
                "invalid_request_id",
        "request IDs outside the bounded ASCII contract should fail");
    const std::string invalid_utf8 =
        std::string(
            "{\"type\":\"hello\",\"request_id\":\"x\",\"nonce\":\"") +
        static_cast<char>(0xff) + "\"}";
    Require(
        specforge::ParseAutomationClientMessage(
            invalid_utf8)
                .error_code == "invalid_utf8",
        "invalid UTF-8 should fail before JSON dispatch");

    specforge::AutomationStateSnapshot state;
    state.instance_id =
        "0123456789abcdef0123456789abcdef";
    state.control = {
        .client_connected = true,
        .handshake_complete = true,
        .accepting_requests = true,
        .pending_count = 0,
        .outstanding_count = 1,
    };
    state.shell = {
        .idle = true,
        .source_load_idle = true,
        .pending_completion_idle = true,
        .background_retirement_idle = true,
        .current_source_id = "source-id",
        .current_source_path =
            std::filesystem::path(
                L"C:\\光谱\\source.csv"),
    };
    state.window = {
        .visible = true,
        .minimized = false,
        .client_width = 1280,
        .client_height = 820,
    };
    state.runtime = {
        .running = true,
        .frame_index = 7,
    };
    std::string parse_error;
    const auto state_json = specforge::ParseJson(
        "{" +
            specforge::
                SerializeAutomationStateBody(
                    state) +
            "}",
        parse_error);
    Require(
        state_json.has_value(),
        parse_error);
    const auto& state_object =
        RequireObjectMember(*state_json, "state");
    const auto& shell =
        RequireObjectMember(state_object, "shell");
    const auto& source =
        RequireObjectMember(state_object, "source");
    Require(
        specforge::ReadJsonBoolMember(
            shell,
            "idle",
            false) &&
            specforge::ReadJsonStringMember(
                source,
                "id") == "source-id" &&
            specforge::ReadJsonStringMember(
                source,
                "path")
                ->find("source.csv") !=
                std::string::npos,
        "state.get body should expose only the stable idle/source/window/runtime contract");
}

void TestStartupAndNoActivationContract()
{
    const std::string instance_id =
        "0123456789abcdef0123456789abcdef";
    const std::string nonce(64U, 'c');
    const std::filesystem::path state_root =
        std::filesystem::temp_directory_path() /
        ("specforge-automation-startup-" +
         std::to_string(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::create_directories(
        state_root,
        error);
    Require(!error, "startup fixture root should exist");

    const std::vector<std::wstring> arguments = {
        L"SpecForge.exe",
        L"--automation-pipe",
        specforge::AutomationPipeNameForInstance(
            instance_id),
        L"--automation-nonce",
        std::wstring(nonce.begin(), nonce.end()),
        L"--automation-instance",
        std::wstring(
            instance_id.begin(),
            instance_id.end()),
        L"--automation-state-root",
        state_root.wstring(),
    };
    const auto parsed =
        specforge::ParseSpecForgeCommandLine(
            arguments);
    Require(
        parsed.automation &&
            parsed.automation->state_root ==
                state_root,
        "complete explicit automation arguments should parse");

    std::vector<std::wstring> incomplete =
        arguments;
    incomplete.resize(incomplete.size() - 2U);
    Require(
        !specforge::ParseSpecForgeCommandLine(
             incomplete)
             .error_message.empty(),
        "partial automation startup should fail closed");

    const std::filesystem::path ordinary_root =
        state_root / "ordinary";
    const std::filesystem::path sibling_root =
        state_root / "automation-sibling";
    Require(
        !specforge::AutomationStateRootIsIndependent(
            ordinary_root,
            ordinary_root) &&
            !specforge::AutomationStateRootIsIndependent(
                ordinary_root / "child",
                ordinary_root) &&
            !specforge::AutomationStateRootIsIndependent(
                state_root,
                ordinary_root) &&
            specforge::AutomationStateRootIsIndependent(
                sibling_root,
                ordinary_root),
        "automation state root should reject same/parent/child and accept a sibling");
    Require(
        specforge::
            IsIncompatibleAutomationEnvironmentVariable(
                L"SPECFORGE_PROFILE") &&
            specforge::
                IsIncompatibleAutomationEnvironmentVariable(
                    L"specforge_profile_dir") &&
            specforge::
                IsIncompatibleAutomationEnvironmentVariable(
                    L"SPECFORGE_RUNTIME_RESOURCE_WORKLOAD") &&
            specforge::
                IsIncompatibleAutomationEnvironmentVariable(
                    L"SPECFORGE_RUNTIME_RESOURCE_STATE_DIR") &&
            !specforge::
                IsIncompatibleAutomationEnvironmentVariable(
                    L"LOCALAPPDATA"),
        "automation should identify only the incompatible legacy workload/profile environment");
    constexpr std::array<const wchar_t*, 4>
        incompatible_environment = {
            L"SPECFORGE_PROFILE",
            L"SPECFORGE_PROFILE_DIR",
            L"SPECFORGE_RUNTIME_RESOURCE_WORKLOAD",
            L"SPECFORGE_RUNTIME_RESOURCE_STATE_DIR",
        };
    std::array<std::optional<std::wstring>, 4>
        saved_environment;
    for (std::size_t index = 0;
         index < incompatible_environment.size();
         ++index) {
        const DWORD required =
            GetEnvironmentVariableW(
                incompatible_environment[index],
                nullptr,
                0);
        if (required != 0) {
            std::wstring value(required, L'\0');
            const DWORD written =
                GetEnvironmentVariableW(
                    incompatible_environment[index],
                    value.data(),
                    required);
            Require(
                written != 0 &&
                    written < required,
                "automation environment fixture should snapshot the process variable");
            value.resize(written);
            saved_environment[index] =
                std::move(value);
        }
        (void)SetEnvironmentVariableW(
            incompatible_environment[index],
            nullptr);
    }
    Require(
        SetEnvironmentVariableW(
            L"SPECFORGE_PROFILE_DIR",
            L"C:\\poisoned-profile-output") !=
            FALSE &&
            specforge::
                ActiveIncompatibleAutomationEnvironmentVariable() ==
                L"SPECFORGE_PROFILE_DIR",
        "direct automation startup should detect inherited legacy environment before App construction");
    for (std::size_t index = 0;
         index < incompatible_environment.size();
         ++index) {
        (void)SetEnvironmentVariableW(
            incompatible_environment[index],
            saved_environment[index]
                ? saved_environment[index]->c_str()
                : nullptr);
    }

    const auto no_activate =
        specforge::ResolveWin32WindowShowPlan(
            SW_SHOWDEFAULT,
            specforge::
                Win32WindowActivation::NoActivate);
    Require(
        no_activate.show_command ==
                SW_SHOWNOACTIVATE &&
            (no_activate.position_flags &
             SWP_NOACTIVATE) != 0 &&
            (no_activate.position_flags &
             SWP_SHOWWINDOW) != 0,
        "automation show plan should be explicitly non-activating");
    Require(
        specforge::ResolveWin32WindowShowPlan(
            SW_SHOWMAXIMIZED,
            specforge::
                Win32WindowActivation::Default)
                .show_command ==
            SW_SHOWMAXIMIZED,
        "ordinary show plan should preserve the caller contract");

    std::filesystem::remove_all(
        state_root,
        error);
}

void TestPipeAclIsCurrentUserOnly(HANDLE pipe)
{
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD security_result = GetSecurityInfo(
        pipe,
        SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &dacl,
        nullptr,
        &descriptor);
    Require(
        security_result == ERROR_SUCCESS &&
            descriptor != nullptr &&
            dacl != nullptr,
        "named pipe DACL should be queryable");

    ACL_SIZE_INFORMATION acl_info = {};
    Require(
        GetAclInformation(
            dacl,
            &acl_info,
            sizeof(acl_info),
            AclSizeInformation) != FALSE &&
            acl_info.AceCount == 1,
        "named pipe DACL should contain exactly one allowed principal");
    void* raw_ace = nullptr;
    Require(
        GetAce(dacl, 0, &raw_ace) != FALSE,
        "named pipe current-user ACE should exist");
    const auto* ace =
        static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
    Require(
        ace->Header.AceType ==
            ACCESS_ALLOWED_ACE_TYPE,
        "named pipe ACE should explicitly allow its principal");
    PSID pipe_sid =
        const_cast<DWORD*>(&ace->SidStart);

    HANDLE token = nullptr;
    Require(
        OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY,
            &token) != FALSE,
        "current token should open");
    DWORD required = 0;
    (void)GetTokenInformation(
        token,
        TokenUser,
        nullptr,
        0,
        &required);
    std::vector<std::byte> token_user(required);
    Require(
        required != 0 &&
            GetTokenInformation(
                token,
                TokenUser,
                token_user.data(),
                required,
                &required) != FALSE,
        "current user SID should resolve");
    CloseHandle(token);
    const auto* current_user =
        reinterpret_cast<const TOKEN_USER*>(
            token_user.data());
    Require(
        EqualSid(
            pipe_sid,
            current_user->User.Sid) != FALSE,
        "named pipe's sole ACE should match the current user SID");
    LocalFree(descriptor);
}

void TestSingleClientQueueAndLifecycle()
{
    RunningServer fixture;
    specforge::AutomationNamedPipeClient client;
    ConnectAndHandshake(fixture, client);
    TestPipeAclIsCurrentUserOnly(
        client.native_handle());

    specforge::AutomationNamedPipeClient second;
    std::string second_error;
    Require(
        !second.Connect(
            fixture.pipe_name,
            50ms,
            second_error) &&
            second_error.find(
                "single client slot") !=
                std::string::npos,
        "a second connection should be explicitly refused");

    SendRequest(
        client,
        "wait-1",
        specforge::AutomationCommandKind::WaitIdle);
    Require(
        ReceiveParsed(client).status ==
            "accepted",
        "wait.idle should be accepted before completion");
    auto commands =
        fixture.server.TakePendingCommands();
    Require(
        commands.size() == 1 &&
            commands.front().command ==
                specforge::
                    AutomationCommandKind::
                        WaitIdle,
        "UI owner should receive the structured wait command");
    Require(
        fixture.server.TryCompleteIdleWaits(
            {"wait-1"}) &&
            ReceiveParsed(client).status ==
                "completed",
        "idle wait should complete atomically only after the queue is empty");

    SendRequest(
        client,
        "duplicate-1",
        specforge::AutomationCommandKind::StateGet);
    Require(
        ReceiveParsed(client).status ==
            "accepted",
        "first request ID use should be accepted");
    SendRequest(
        client,
        "duplicate-1",
        specforge::AutomationCommandKind::StateGet);
    const auto duplicate = ReceiveParsed(client);
    Require(
        duplicate.status == "failed" &&
            duplicate.error_code ==
                "duplicate_request_id",
        "duplicate request ID should fail explicitly");
    commands =
        fixture.server.TakePendingCommands();
    Require(
        commands.size() == 1,
        "duplicate request must not enter the UI queue");
    fixture.server.Complete(
        commands.front(),
        "\"state\":{}");
    Require(
        ReceiveParsed(client).status ==
            "completed",
        "original request should retain its terminal response");

    std::string send_error;
    Require(
        client.Send(
            R"({"type":"request","request_id":"reuse-1","command":"unsupported.command"})",
            send_error),
        send_error);
    const auto unsupported =
        ReceiveParsed(client);
    Require(
        unsupported.status == "failed" &&
            unsupported.error_code ==
                "unsupported_command",
        "unsupported command should fail with its validated request ID");
    SendRequest(
        client,
        "reuse-1",
        specforge::AutomationCommandKind::StateGet);
    const auto reused =
        ReceiveParsed(client);
    Require(
        reused.status == "failed" &&
            reused.error_code ==
                "duplicate_request_id" &&
            fixture.server
                .TakePendingCommands()
                .empty(),
        "validated request ID should remain consumed after semantic parse failure");

    SendRequest(
        client,
        "wait-before-quit",
        specforge::AutomationCommandKind::WaitIdle);
    Require(
        ReceiveParsed(client).status ==
            "accepted",
        "pre-quit wait should be accepted");
    SendRequest(
        client,
        "quit-1",
        specforge::AutomationCommandKind::AppQuit);
    Require(
        ReceiveParsed(client).status ==
            "accepted",
        "app.quit should be accepted");
    SendRequest(
        client,
        "state-after-quit-queued",
        specforge::AutomationCommandKind::StateGet);
    Require(
        ReceiveParsed(client).status ==
            "accepted",
        "request queued before UI begins quit should be accepted");

    commands =
        fixture.server.TakePendingCommands();
    const auto quit = std::find_if(
        commands.begin(),
        commands.end(),
        [](const auto& command) {
            return command.command ==
                   specforge::
                       AutomationCommandKind::
                           AppQuit;
        });
    Require(
        quit != commands.end(),
        "UI queue should contain app.quit");
    Require(
        fixture.server.TryBeginAppQuit(*quit),
        "active app.quit should be claimed atomically");
    Require(
        !fixture.server.TryBeginAppQuit(*quit),
        "app.quit atomic claim should be one-shot");
    fixture.server.Complete(*quit);

    std::unordered_map<std::string, std::string>
        terminal_status;
    for (int index = 0; index < 3; ++index) {
        const auto response = ReceiveParsed(client);
        terminal_status[response.request_id] =
            response.status;
        if (response.status == "canceled") {
            Require(
                response.error_code == "app_quit",
                "quit cancellation should carry an explicit code");
        }
    }
    Require(
        terminal_status["wait-before-quit"] ==
                "canceled" &&
            terminal_status["quit-1"] ==
                "completed" &&
            terminal_status[
                "state-after-quit-queued"] ==
                "canceled",
        "app.quit should cancel every other queued or waiting request");

    SendRequest(
        client,
        "after-shutdown",
        specforge::AutomationCommandKind::StateGet);
    const auto shutting_down =
        ReceiveParsed(client);
    Require(
        shutting_down.status == "failed" &&
            shutting_down.error_code ==
                "shutting_down",
        "requests after app.quit begins should fail explicitly");
}

void TestQueueCapacityVersionAndDisconnect()
{
    {
        RunningServer fixture;
        specforge::AutomationNamedPipeClient client;
        std::string error;
        Require(
            client.Connect(
                fixture.pipe_name,
                1s,
                error),
            error);
        const std::vector<char> oversized(
            specforge::kAutomationMaxMessageBytes +
                1U,
            'x');
        DWORD bytes_written = 0;
        Require(
            WriteFile(
                client.native_handle(),
                oversized.data(),
                static_cast<DWORD>(
                    oversized.size()),
                &bytes_written,
                nullptr) != FALSE &&
                bytes_written == oversized.size(),
            "oversized raw pipe message should reach the server boundary");
        const auto too_large =
            ReceiveParsed(client);
        Require(
            too_large.status == "failed" &&
                too_large.error_code ==
                    "message_too_large",
            "message beyond max_message_bytes should fail explicitly");
    }

    {
        RunningServer fixture;
        specforge::AutomationNamedPipeClient client;
        ConnectAndHandshake(fixture, client);
        for (std::size_t index = 0;
             index <
             specforge::kAutomationQueueCapacity;
             ++index) {
            SendRequest(
                client,
                "queue-" +
                    std::to_string(index),
                specforge::
                    AutomationCommandKind::
                        WaitIdle);
            Require(
                ReceiveParsed(client).status ==
                    "accepted",
                "requests through queue capacity should be accepted");
        }
        SendRequest(
            client,
            "queue-overflow",
            specforge::AutomationCommandKind::WaitIdle);
        const auto full = ReceiveParsed(client);
        Require(
            full.status == "failed" &&
                full.error_code == "queue_full",
            "request beyond bounded queue capacity should fail");
    }

    {
        RunningServer fixture;
        specforge::AutomationNamedPipeClient client;
        std::string error;
        Require(
            client.Connect(
                fixture.pipe_name,
                1s,
                error),
            error);
        Require(
            client.Send(
                R"({"type":"hello","request_id":"hello-bad-version","protocol_version":999,"nonce":")" +
                    fixture.nonce + "\"}",
                error),
            error);
        const auto mismatch =
            ReceiveParsed(client);
        Require(
            mismatch.status == "failed" &&
                mismatch.error_code ==
                    "version_mismatch",
            "unsupported protocol version should fail the handshake");
    }

    {
        RunningServer fixture;
        specforge::AutomationNamedPipeClient client;
        ConnectAndHandshake(fixture, client);
        SendRequest(
            client,
            "disconnect-quit",
            specforge::AutomationCommandKind::AppQuit);
        Require(
            ReceiveParsed(client).status ==
                "accepted",
            "disconnect fixture request should be accepted");
        const auto dequeued =
            fixture.server.TakePendingCommands();
        Require(
            dequeued.size() == 1 &&
                fixture.server.IsRequestActive(
                    "disconnect-quit"),
            "disconnect race fixture should dequeue an active app.quit");
        client.Close();
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (!fixture.server.IsRequestActive(
                    "disconnect-quit")) {
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
        Require(
            !fixture.server.IsRequestActive(
                "disconnect-quit") &&
                !fixture.server.TryBeginAppQuit(
                    dequeued.front()) &&
                fixture.server
                    .TakePendingCommands()
                    .empty(),
            "dequeued app.quit should fail its atomic claim after client disconnect");
    }
}

void TestDistinctRequestIdLimit()
{
    RunningServer fixture;
    specforge::AutomationNamedPipeClient client;
    ConnectAndHandshake(fixture, client);

    // The successful hello already consumed one distinct request ID.
    std::size_t next_index = 1;
    while (next_index <
           specforge::kAutomationMaxRequestsPerConnection) {
        constexpr std::size_t kBatchSize = 32;
        const std::size_t batch_end =
            (std::min)(
                next_index + kBatchSize,
                specforge::
                    kAutomationMaxRequestsPerConnection);
        std::string error;
        for (std::size_t index = next_index;
             index < batch_end;
             ++index) {
            Require(
                client.Send(
                    R"({"type":"request","request_id":"limit-)" +
                        std::to_string(index) +
                        R"(","command":"unsupported.command"})",
                    error),
                error);
        }
        for (std::size_t index = next_index;
             index < batch_end;
             ++index) {
            const auto response =
                ReceiveParsed(client);
            Require(
                response.request_id ==
                        "limit-" +
                            std::to_string(index) &&
                    response.status == "failed" &&
                    response.error_code ==
                        "unsupported_command",
                "distinct valid IDs through the per-connection limit should be consumed");
        }
        next_index = batch_end;
    }

    SendRequest(
        client,
        "limit-overflow",
        specforge::AutomationCommandKind::StateGet);
    const auto first_overflow = ReceiveParsed(client);
    Require(
        first_overflow.status == "failed" &&
            first_overflow.error_code ==
                "request_limit_reached" &&
            !fixture.server.queue_snapshot()
                 .accepting_requests,
        "the first distinct ID beyond the connection limit should fail and stop acceptance");

    SendRequest(
        client,
        "limit-overflow",
        specforge::AutomationCommandKind::StateGet);
    const auto repeated_overflow = ReceiveParsed(client);
    Require(
        repeated_overflow.status == "failed" &&
            repeated_overflow.error_code ==
                "request_limit_reached",
        "an ID rejected by the request limit should remain unconsumed and fail consistently");

    SendRequest(
        client,
        "limit-1",
        specforge::AutomationCommandKind::StateGet);
    const auto duplicate = ReceiveParsed(client);
    Require(
        duplicate.status == "failed" &&
            duplicate.error_code ==
                "duplicate_request_id" &&
            fixture.server.TakePendingCommands().empty(),
        "an ID consumed before the limit should retain duplicate semantics after exhaustion");
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    const auto fixture_mode =
        EnvironmentValue(
            L"SPECFORGE_AUTOMATION_LAUNCHER_FIXTURE");
    if (fixture_mode) {
        return RunLauncherCleanupFixture(
            argc,
            argv,
            *fixture_mode);
    }

    TestProtocolAndStableState();
    TestStartupAndNoActivationContract();
    TestSingleClientQueueAndLifecycle();
    TestQueueCapacityVersionAndDisconnect();
    TestDistinctRequestIdLimit();
    std::cout
        << "automation control tests passed\n";
    return 0;
}
