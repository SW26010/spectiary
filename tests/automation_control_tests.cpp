#include "app/local_user_state_json.h"
#include "automation/automation_named_pipe.h"
#include "automation/automation_protocol.h"
#include "automation/automation_startup.h"
#include "automation/automation_state.h"
#include "platform/win32_text.h"
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
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace specforge {

struct AutomationNamedPipeServerTestAccess {
    static void HandleDisconnect(
        AutomationNamedPipeServer& server)
    {
        server.HandleDisconnect();
    }
};

}  // namespace specforge

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
            if (mode == L"pipeline-limit") {
                if (command.command ==
                        specforge::
                            AutomationCommandKind::
                                AppQuit &&
                    server.TryBeginAppQuit(command) ==
                        specforge::
                            AutomationNamedPipeServer::
                                AppQuitClaimResult::
                                    Claimed) {
                    server.Complete(command);
                    std::ofstream marker(
                        std::filesystem::path(
                            *state_root) /
                            "launcher-cleanup-completed.txt",
                        std::ios::binary |
                            std::ios::trunc);
                    marker
                        << "pipeline limit cleanup\n";
                    marker.close();
                    server.Shutdown();
                    return marker ? 0 : 93;
                }
                std::ofstream violation(
                    std::filesystem::path(
                        *state_root) /
                        "pipeline-command-observed.txt",
                    std::ios::binary |
                        std::ios::trunc);
                violation
                    << specforge::
                           AutomationCommandName(
                           command.command)
                    << '\n';
                violation.close();
                server.Fail(
                    command,
                    "pipeline_limit_violation",
                    "Pipeline capacity validation sent a business request.");
                continue;
            }
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
                server.TryBeginAppQuit(command) ==
                    specforge::
                        AutomationNamedPipeServer::
                            AppQuitClaimResult::
                                Claimed) {
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
    const auto setting_get_request =
        specforge::ParseAutomationClientMessage(
            specforge::
                SerializeAutomationCommandRequest(
                    "setting-get-1",
                    specforge::
                        AutomationCommandKind::
                            SettingGet,
                    specforge::
                        AutomationSettingGetParameters{
                            .name = "ui.language",
                        }));
    const auto* setting_get_parameters =
        setting_get_request.message
        ? std::get_if<
              specforge::
                  AutomationSettingGetParameters>(
              &setting_get_request.message
                   ->parameters)
        : nullptr;
    const auto setting_set_request =
        specforge::ParseAutomationClientMessage(
            specforge::
                SerializeAutomationCommandRequest(
                    "setting-set-1",
                    specforge::
                        AutomationCommandKind::
                            SettingSet,
                    specforge::
                        AutomationSettingSetParameters{
                            .name = "ui.scale",
                            .value =
                                std::int64_t{125},
                        }));
    const auto* setting_set_parameters =
        setting_set_request.message
        ? std::get_if<
              specforge::
                  AutomationSettingSetParameters>(
              &setting_set_request.message
                   ->parameters)
        : nullptr;
    Require(
        setting_get_parameters != nullptr &&
            setting_get_parameters->name ==
                "ui.language" &&
            setting_set_parameters != nullptr &&
            setting_set_parameters->name ==
                "ui.scale" &&
            std::get<std::int64_t>(
                setting_set_parameters->value) ==
                125,
        "setting.get and setting.set should round-trip their bounded name and scalar value parameters");
    Require(
        specforge::ParseAutomationClientMessage(
            R"({"type":"request","request_id":"bad-setting-value","command":"setting.set","params":{"name":"ui.scale","value":{"nested":true}}})")
                .error_code == "invalid_params",
        "setting.set should reject non-scalar values before dispatch");
    const auto& capabilities =
        specforge::AutomationCapabilityNames();
    Require(
        capabilities.size() == 9 &&
            std::find(
                capabilities.begin(),
                capabilities.end(),
                "setting.get") !=
                capabilities.end() &&
            std::find(
                capabilities.begin(),
                capabilities.end(),
                "setting.set") !=
                capabilities.end(),
        "fixed capabilities should advertise both bounded setting commands");
    const auto source_request =
        specforge::ParseAutomationClientMessage(
            R"({"type":"request","request_id":"source-1","command":"source.open","params":{"path":"C:\\fixtures\\source.npy"}})");
    const auto* source_parameters =
        source_request.message
        ? std::get_if<
              specforge::
                  AutomationSourceOpenParameters>(
              &source_request.message->parameters)
        : nullptr;
    Require(
        source_parameters != nullptr &&
            source_parameters->path ==
                "C:\\fixtures\\source.npy",
        "source.open should retain its UTF-8 absolute path parameter");
    const auto label_request =
        specforge::ParseAutomationClientMessage(
            R"({"type":"request","request_id":"label-1","command":"label.assign","params":{"code":5,"target":{"name":"alpha"}}})");
    const auto* label_parameters =
        label_request.message
        ? std::get_if<
              specforge::
                  AutomationLabelAssignParameters>(
              &label_request.message->parameters)
        : nullptr;
    Require(
        label_parameters != nullptr &&
            label_parameters->code == 5 &&
            label_parameters->target &&
            label_parameters->target->name ==
                "alpha",
        "label.assign should parse a code and optional exact spectrum target");
    Require(
        specforge::ParseAutomationClientMessage(
            R"({"type":"request","request_id":"bad-target","command":"spectrum.goto","params":{"target":{"index":1,"name":"alpha"}}})")
                .error_code == "invalid_params",
        "spectrum targets should require exactly one stable identifier");

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
    const std::string unicode_command =
        specforge::WideToUtf8(
            L"source open C:\\数据\\光谱");
    Require(
        unicode_command ==
                "source open C:\\数据\\光谱" &&
            specforge::IsWellFormedUtf8(
                unicode_command) &&
            !specforge::IsWellFormedUtf8(
                std::string_view(
                    invalid_utf8.data() +
                        invalid_utf8.find(
                            static_cast<char>(
                                0xff)),
                    1)),
        "launcher text helpers should establish a strict UTF-16 console to UTF-8 protocol boundary");

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
    state.presented_source = {
        .present = true,
        .id = "presented-source-id",
        .path =
            std::filesystem::path(
                L"C:\\光谱\\presented.csv"),
    };
    state.settings = {
        .language = "zh-Hans",
        .ui_scale_percentage = 125,
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
    state.spectrum = {
        .present = true,
        .index = 2,
        .name = "alpha",
        .count = 4,
    };
    state.labeling = {
        .has_active_task = true,
        .task_id = "quality",
        .task_name = "Quality",
        .current_spectrum_code = 5,
    };
    state.capture = {
        .pending = true,
        .current_path =
            std::filesystem::path(
                L"C:\\automation\\pending.png"),
        .last_result = "failed",
        .last_path =
            std::filesystem::path(
                L"C:\\automation\\failed.png"),
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
    const auto& presented_source =
        RequireObjectMember(
            state_object,
            "presented_source");
    const auto& spectrum =
        RequireObjectMember(
            state_object,
            "spectrum");
    const auto& settings =
        RequireObjectMember(
            state_object,
            "settings");
    const auto& labeling =
        RequireObjectMember(
            state_object,
            "labeling");
    const auto& current_label =
        RequireObjectMember(
            labeling,
            "current_spectrum_label");
    const auto& capture =
        RequireObjectMember(
            state_object,
            "capture");
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
                std::string::npos &&
            specforge::ReadJsonStringMember(
                presented_source,
                "id") ==
                "presented-source-id" &&
            specforge::ReadJsonStringMember(
                presented_source,
                "path")
                ->find("presented.csv") !=
                std::string::npos &&
            specforge::ReadJsonStringMember(
                settings,
                "language") == "zh-Hans" &&
            specforge::ReadJsonIntMember(
                settings,
                "ui_scale_percentage") ==
                125 &&
            specforge::ReadJsonSizeMember(
                spectrum,
                "index") == 2U &&
            specforge::ReadJsonIntMember(
                current_label,
                "code") == 5 &&
            specforge::ReadJsonBoolMember(
                capture,
                "pending",
                false) &&
            specforge::ReadJsonStringMember(
                capture,
                "last_result") ==
                "failed" &&
            specforge::ReadJsonStringMember(
                capture,
                "current_path")
                ->find("pending.png") !=
                std::string::npos &&
            specforge::ReadJsonStringMember(
                capture,
                "last_path")
                ->find("failed.png") !=
                std::string::npos,
        "state.get body should expose the stable source, current spectrum label, capture, window and runtime contract");

    state.capture = {
        .pending = false,
        .last_result = "succeeded",
        .last_path =
            std::filesystem::path(
                L"C:\\automation\\captured.png"),
    };
    const std::string succeeded_capture_state =
        specforge::SerializeAutomationStateBody(
            state);
    state.capture.last_result = "canceled";
    const std::string canceled_capture_state =
        specforge::SerializeAutomationStateBody(
            state);
    Require(
        succeeded_capture_state.find(
            "\"pending\":false,\"last_result\":\"succeeded\"") !=
                std::string::npos &&
            canceled_capture_state.find(
                "\"last_result\":\"canceled\"") !=
                std::string::npos,
        "state.capture should distinguish stable successful and canceled terminal outcomes");
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
    const auto capture_inside =
        specforge::ValidateAutomationCapturePath(
            state_root,
            state_root / "artifacts" /
                "frame.png");
    const auto capture_outside =
        specforge::ValidateAutomationCapturePath(
            state_root,
            state_root.parent_path() /
                "outside.png");
    const auto capture_wrong_extension =
        specforge::ValidateAutomationCapturePath(
            state_root,
            state_root / "artifacts" /
                "frame.jpg");
    const std::filesystem::path existing_capture =
        state_root / "existing.png";
    {
        std::ofstream stream(
            existing_capture,
            std::ios::binary);
        stream << "preserve";
    }
    const auto capture_existing =
        specforge::ValidateAutomationCapturePath(
            state_root,
            existing_capture);
    const auto owned_inside =
        specforge::ValidateAutomationStateOwnedPath(
            state_root,
            state_root / "labels" /
                "quality.csv");
    const auto owned_root =
        specforge::ValidateAutomationStateOwnedPath(
            state_root,
            state_root);
    const auto owned_outside =
        specforge::ValidateAutomationStateOwnedPath(
            state_root,
            state_root.parent_path() /
                "ordinary" / "quality.csv");
    Require(
        capture_inside.valid &&
            !capture_outside.valid &&
            capture_outside.error_code ==
                "capture_path_outside_state_root" &&
            !capture_wrong_extension.valid &&
            capture_wrong_extension.error_code ==
                "capture_path_invalid" &&
            !capture_existing.valid &&
            capture_existing.error_code ==
                "capture_output_exists",
        "automation capture should accept only a new absolute PNG below the isolated root without overwriting");
    Require(
        owned_inside.valid &&
            !owned_root.valid &&
            !owned_outside.valid,
        "persisted automation output references should remain strictly below the isolated state root");
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

    const std::filesystem::path pinned_seed =
        state_root / "pinned-seed.json";
    {
        std::ofstream seed_stream(
            pinned_seed,
            std::ios::binary);
        seed_stream << "{\"seed\":true}";
    }
    std::string lease_error;
    auto seed_lease =
        specforge::PinAutomationReadOnlyFile(
            pinned_seed,
            lease_error);
    Require(
        seed_lease.valid(),
        lease_error);
    const std::filesystem::path moved_seed =
        state_root / "moved-seed.json";
    Require(
        MoveFileW(
            pinned_seed.c_str(),
            moved_seed.c_str()) == FALSE &&
            std::filesystem::exists(
                pinned_seed),
        "a pinned read-only seed must not be replaceable between production validation and materialization");

    const std::filesystem::path pinned_root =
        state_root / "pinned-root";
    auto root_lease =
        specforge::
            CreatePinnedAutomationStateRoot(
                pinned_root,
                lease_error);
    Require(
        root_lease.valid(),
        lease_error);
    const std::filesystem::path moved_root =
        state_root / "moved-root";
    Require(
        MoveFileW(
            pinned_root.c_str(),
            moved_root.c_str()) == FALSE &&
            std::filesystem::is_directory(
                pinned_root),
        "the launcher must retain an identity lease that prevents state-root replacement before seeding and through the child lifetime");
    Require(
        specforge::
            MaterializePinnedAutomationSeed(
                root_lease,
                seed_lease,
                L"sample-labeling.json",
                lease_error),
        lease_error);
    std::ifstream materialized_stream(
        pinned_root /
            "sample-labeling.json",
        std::ios::binary);
    const std::string materialized{
        std::istreambuf_iterator<char>(
            materialized_stream),
        std::istreambuf_iterator<char>()};
    materialized_stream.close();
    Require(
        materialized == "{\"seed\":true}",
        "seed materialization should copy the pinned source into a CREATE_NEW file relative to the pinned root handle");
    const std::filesystem::path
        renamed_materialized =
            pinned_root /
            "sample-labeling-renamed.json";
    Require(
        MoveFileW(
            (pinned_root /
             "sample-labeling.json")
                .c_str(),
            renamed_materialized.c_str()) !=
                FALSE &&
            MoveFileW(
                renamed_materialized.c_str(),
                (pinned_root /
                 "sample-labeling.json")
                    .c_str()) != FALSE,
        "the root identity lease must permit sibling cache rename operations used by production atomic persistence");
    specforge::
        RemovePinnedAutomationStateRootBeforeLaunch(
            root_lease);
    Require(
        !std::filesystem::exists(
            pinned_root),
        "a prelaunch failure should remove only the pinned automation root and its known materialized seed");
    seed_lease =
        specforge::
            AutomationReadOnlyFileLease{};
    std::filesystem::remove(
        pinned_seed,
        error);

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
    Require(
        commands.size() == 1 &&
            commands.front().command ==
                specforge::
                    AutomationCommandKind::
                        WaitIdle,
        "wait.idle should stop UI dispatch before the queued app.quit");
    Require(
        fixture.server.TryCompleteIdleWaits(
            {"wait-before-quit"}) &&
            ReceiveParsed(client).status ==
                "completed",
        "the earlier wait should terminal before later commands become dispatchable");
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
        quit != commands.end() &&
            commands.size() == 2,
        "the UI queue should expose app.quit and its later request only after the wait barrier");
    Require(
        fixture.server.TryBeginAppQuit(*quit) ==
            specforge::AutomationNamedPipeServer::
                AppQuitClaimResult::Claimed,
        "active app.quit should be claimed atomically");
    Require(
        fixture.server.TryBeginAppQuit(*quit) ==
            specforge::AutomationNamedPipeServer::
                AppQuitClaimResult::Inactive,
        "app.quit atomic claim should be one-shot");
    fixture.server.Complete(*quit);

    std::unordered_map<std::string, std::string>
        terminal_status;
    for (int index = 0; index < 2; ++index) {
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
        terminal_status["quit-1"] ==
                "completed" &&
            terminal_status[
                "state-after-quit-queued"] ==
                "canceled",
        "app.quit should cancel every other request dispatched after the completed idle barrier");

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

void TestIdleWaitIsAnEarlierOnlySequenceBarrier()
{
    RunningServer fixture;
    specforge::AutomationNamedPipeClient client;
    ConnectAndHandshake(fixture, client);

    SendRequest(
        client,
        "wait-earlier",
        specforge::AutomationCommandKind::WaitIdle);
    Require(
        ReceiveParsed(client).status ==
            "accepted",
        "the earlier idle barrier should be accepted");
    SendRequest(
        client,
        "wait-later",
        specforge::AutomationCommandKind::WaitIdle);
    Require(
        ReceiveParsed(client).status ==
            "accepted",
        "the later idle barrier should be accepted");
    auto commands =
        fixture.server.TakePendingCommands();
    Require(
        commands.size() == 1 &&
            commands.front().request_id ==
                "wait-earlier",
        "the first idle barrier should hold the later barrier in the pipe queue");

    Require(
        fixture.server.TryCompleteIdleWaits(
            {"wait-earlier"}),
        "a later accepted request must not postpone an earlier wait.idle barrier");
    const auto earlier_terminal =
        ReceiveParsed(client);
    Require(
        earlier_terminal.request_id ==
                "wait-earlier" &&
            earlier_terminal.status ==
                "completed" &&
            fixture.server.IsRequestActive(
                "wait-later"),
        "the earlier barrier should terminal independently while the later request remains active");
    commands =
        fixture.server.TakePendingCommands();
    Require(
        commands.size() == 1 &&
            commands.front().request_id ==
                "wait-later" &&
        fixture.server.TryCompleteIdleWaits(
            {"wait-later"}) &&
            ReceiveParsed(client).status ==
                "completed",
        "the later idle barrier should complete independently afterward");
}

void TestIdleWaitStopsLaterBusinessDispatch()
{
    RunningServer fixture;
    specforge::AutomationNamedPipeClient client;
    ConnectAndHandshake(fixture, client);

    const auto send_open =
        [&](std::string_view request_id,
            std::string path) {
            std::string error;
            Require(
                client.Send(
                    specforge::
                        SerializeAutomationCommandRequest(
                            request_id,
                            specforge::
                                AutomationCommandKind::
                                    SourceOpen,
                            specforge::
                                AutomationSourceOpenParameters{
                                    std::move(path)}),
                    error),
                error);
            Require(
                ReceiveParsed(client).status ==
                    "accepted",
                "source.open dispatch-barrier fixture should be accepted");
        };
    send_open(
        "open-before-wait",
        "C:\\fixtures\\source-a");
    SendRequest(
        client,
        "wait-after-open",
        specforge::AutomationCommandKind::WaitIdle);
    Require(
        ReceiveParsed(client).status ==
            "accepted",
        "the idle barrier should be accepted");
    send_open(
        "open-after-wait",
        "C:\\fixtures\\source-b");

    auto commands =
        fixture.server.TakePendingCommands();
    Require(
        commands.size() == 2 &&
            commands[0].request_id ==
                "open-before-wait" &&
            commands[1].request_id ==
                "wait-after-open" &&
            fixture.server.IsRequestActive(
                "open-after-wait"),
        "the UI owner should receive the earlier open and wait while the later open remains undispatched");
    Require(
        fixture.server.TryClaimExecution(
            commands.front()),
        "the earlier open should enter its real execution lifecycle");
    fixture.server.Complete(
        commands.front());
    Require(
        fixture.server.TryCompleteIdleWaits(
            {"wait-after-open"}),
        "the app may complete the wait after its separate Shell worker-retirement observation becomes idle");
    const auto open_terminal =
        ReceiveParsed(client);
    const auto wait_terminal =
        ReceiveParsed(client);
    Require(
        open_terminal.request_id ==
                "open-before-wait" &&
            open_terminal.status ==
                "completed" &&
            wait_terminal.request_id ==
                "wait-after-open" &&
            wait_terminal.status ==
                "completed",
        "the earlier open and wait should terminal in sequence");

    commands =
        fixture.server.TakePendingCommands();
    Require(
        commands.size() == 1 &&
            commands.front().request_id ==
                "open-after-wait" &&
            fixture.server.IsRequestActive(
                "open-after-wait"),
        "the later source.open should become dispatchable only after the wait terminal and must remain unfinished");
    fixture.server.Fail(
        commands.front(),
        "operation_canceled",
        "fixture cleanup");
    Require(
        ReceiveParsed(client).request_id ==
            "open-after-wait",
        "the later source.open should retain its independent terminal lifecycle");
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
                fixture.server.TryBeginAppQuit(
                    dequeued.front()) ==
                    specforge::
                        AutomationNamedPipeServer::
                            AppQuitClaimResult::
                                Inactive &&
                fixture.server
                    .TakePendingCommands()
                    .empty(),
            "dequeued app.quit should fail its atomic claim after client disconnect");
    }
}

void TestPreHandshakeJsonNestingIsBounded()
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

    constexpr std::size_t kDeepJsonNesting = 20'000U;
    std::string deeply_nested_json;
    deeply_nested_json.reserve(
        kDeepJsonNesting * 2U + 4U);
    deeply_nested_json.append(
        kDeepJsonNesting,
        '[');
    deeply_nested_json += "null";
    deeply_nested_json.append(
        kDeepJsonNesting,
        ']');
    Require(
        deeply_nested_json.size() <=
            specforge::kAutomationMaxMessageBytes,
        "deep JSON fixture must remain within max_message_bytes");

    DWORD bytes_written = 0;
    Require(
        WriteFile(
            client.native_handle(),
            deeply_nested_json.data(),
            static_cast<DWORD>(
                deeply_nested_json.size()),
            &bytes_written,
            nullptr) != FALSE &&
            bytes_written ==
                deeply_nested_json.size(),
        "deep raw pipe message should reach the pre-handshake parser");

    const auto response = ReceiveParsed(client);
    Require(
        response.status == "failed" &&
            response.error_code == "invalid_json",
        "over-budget JSON nesting should fail as invalid_json");

    for (int attempt = 0; attempt < 100; ++attempt) {
        if (!fixture.server
                 .queue_snapshot()
                 .client_connected) {
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    const auto snapshot =
        fixture.server.queue_snapshot();
    Require(
        !snapshot.client_connected &&
            !snapshot.handshake_complete,
        "invalid pre-handshake JSON should disconnect cleanly");
}

void TestOversizedTerminalResponseIsBounded()
{
    RunningServer fixture;
    specforge::AutomationNamedPipeClient client;
    ConnectAndHandshake(fixture, client);

    SendRequest(
        client,
        "oversized-state",
        specforge::AutomationCommandKind::StateGet);
    Require(
        ReceiveParsed(client).status == "accepted",
        "oversized response fixture should first accept state.get");
    auto commands =
        fixture.server.TakePendingCommands();
    Require(
        commands.size() == 1,
        "oversized response fixture should dequeue state.get");
    fixture.server.Complete(
        commands.front(),
        "\"state\":{\"source\":{\"path\":\"" +
            std::string(
                specforge::kAutomationMaxMessageBytes,
                'x') +
            "\"}}");

    const auto bounded = ReceiveParsed(client);
    Require(
        bounded.request_id == "oversized-state" &&
            bounded.command_name == "state.get" &&
            bounded.status == "failed" &&
            bounded.error_code ==
                "response_too_large",
        "a terminal response beyond max_message_bytes should become a bounded correlated failure");

    SendRequest(
        client,
        "after-oversized-state",
        specforge::AutomationCommandKind::StateGet);
    Require(
        ReceiveParsed(client).status == "accepted",
        "the connection should remain usable after a bounded oversized response");
    commands = fixture.server.TakePendingCommands();
    Require(
        commands.size() == 1,
        "the post-oversize request should still reach the UI queue");
    fixture.server.Complete(
        commands.front(),
        "\"state\":{}");
    const auto completed = ReceiveParsed(client);
    Require(
        completed.request_id ==
                "after-oversized-state" &&
            completed.status == "completed",
        "a normal terminal response should follow the bounded oversized failure");
}

void TestExecutionClaimsAndQuitBarrier()
{
    {
        RunningServer setting_fixture;
        specforge::AutomationNamedPipeClient
            setting_client;
        ConnectAndHandshake(
            setting_fixture,
            setting_client);
        std::string setting_send_error;
        Require(
            setting_client.Send(
                specforge::
                    SerializeAutomationCommandRequest(
                        "claimed-setting",
                        specforge::
                            AutomationCommandKind::
                                SettingSet,
                        specforge::
                            AutomationSettingSetParameters{
                                .name = "ui.scale",
                                .value =
                                    std::int64_t{125},
                            }),
                setting_send_error),
            setting_send_error);
        Require(
            ReceiveParsed(setting_client).status ==
                "accepted",
            "setting.set claim fixture should be accepted");
        const auto setting_commands =
            setting_fixture.server
                .TakePendingCommands();
        Require(
            setting_commands.size() == 1 &&
                setting_fixture.server
                    .TryClaimExecution(
                        setting_commands.front()),
            "setting.set should enter the synchronized mutation claim");
        setting_fixture.server.Complete(
            setting_commands.front(),
            "\"result\":{}");
        Require(
            ReceiveParsed(setting_client).status ==
                "completed",
            "claimed setting.set should retain its factual terminal");
    }

    RunningServer fixture;
    specforge::AutomationNamedPipeClient client;
    ConnectAndHandshake(fixture, client);

    std::string send_error;
    Require(
        client.Send(
            specforge::
                SerializeAutomationCommandRequest(
                    "claimed-label",
                    specforge::
                        AutomationCommandKind::
                            LabelAssign,
                    specforge::
                        AutomationLabelAssignParameters{
                            .code = 5}),
            send_error),
        send_error);
    Require(
        ReceiveParsed(client).status == "accepted",
        "label fixture should be accepted");
    SendRequest(
        client,
        "barrier-quit",
        specforge::AutomationCommandKind::AppQuit);
    Require(
        ReceiveParsed(client).status == "accepted",
        "quit barrier fixture should be accepted");
    SendRequest(
        client,
        "after-barrier",
        specforge::AutomationCommandKind::StateGet);
    Require(
        ReceiveParsed(client).status == "accepted",
        "later command should be accepted before the UI claims quit");

    const auto commands =
        fixture.server.TakePendingCommands();
    Require(
        commands.size() == 3,
        "execution claim fixture should dequeue one batch");
    const auto label = std::find_if(
        commands.begin(),
        commands.end(),
        [](const auto& command) {
            return command.command ==
                   specforge::AutomationCommandKind::
                       LabelAssign;
        });
    const auto quit = std::find_if(
        commands.begin(),
        commands.end(),
        [](const auto& command) {
            return command.command ==
                   specforge::AutomationCommandKind::
                       AppQuit;
        });
    Require(
        label != commands.end() &&
            quit != commands.end() &&
            fixture.server.TryClaimExecution(*label),
        "the earlier mutating command should claim its execution boundary");
    Require(
        fixture.server.TryBeginAppQuit(*quit) ==
            specforge::AutomationNamedPipeServer::
                AppQuitClaimResult::
                    WaitingForEarlierExecution,
        "app.quit should wait for an earlier claimed mutation");

    const auto later_terminal =
        ReceiveParsed(client);
    Require(
        later_terminal.request_id ==
                "after-barrier" &&
            later_terminal.status == "canceled" &&
            later_terminal.error_code ==
                "app_quit",
        "the quit barrier should cancel later unclaimed work");

    fixture.server.Complete(
        *label,
        "\"result\":{}");
    Require(
        fixture.server.TryBeginAppQuit(*quit) ==
            specforge::AutomationNamedPipeServer::
                AppQuitClaimResult::Claimed,
        "app.quit should claim after the earlier mutation reaches terminal");
    fixture.server.Complete(*quit);
    const auto label_terminal =
        ReceiveParsed(client);
    const auto quit_terminal =
        ReceiveParsed(client);
    Require(
        label_terminal.request_id ==
                "claimed-label" &&
            label_terminal.status ==
                "completed" &&
            quit_terminal.request_id ==
                "barrier-quit" &&
            quit_terminal.status ==
                "completed",
        "a claimed mutation must terminal successfully before app.quit");

    RunningServer disconnected_fixture;
    specforge::AutomationNamedPipeClient
        disconnected_client;
    ConnectAndHandshake(
        disconnected_fixture,
        disconnected_client);
    Require(
        disconnected_client.Send(
            specforge::
                SerializeAutomationCommandRequest(
                    "claimed-open",
                    specforge::
                        AutomationCommandKind::
                            SourceOpen,
                    specforge::
                        AutomationSourceOpenParameters{
                            "C:\\automation\\source"}),
            send_error),
        send_error);
    Require(
        ReceiveParsed(disconnected_client).status ==
            "accepted",
        "disconnect claim fixture should be accepted");
    const auto disconnected_commands =
        disconnected_fixture.server
            .TakePendingCommands();
    Require(
        disconnected_commands.size() == 1 &&
            disconnected_fixture.server
                .TryClaimExecution(
                    disconnected_commands.front()),
        "source.open should enter the claimed state");
    disconnected_client.Close();
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (!disconnected_fixture.server
                 .queue_snapshot()
                 .client_connected) {
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    Require(
        !disconnected_fixture.server
             .queue_snapshot()
             .client_connected &&
            disconnected_fixture.server
                .IsRequestActive("claimed-open"),
        "disconnect should retain a claimed mutation until its factual terminal");
    disconnected_fixture.server.Complete(
        disconnected_commands.front());
    Require(
        !disconnected_fixture.server
             .IsRequestActive("claimed-open"),
        "a disconnected claimed mutation should be retired by its factual terminal");
}

void TestFrameCaptureFinalizationLease()
{
    {
        RunningServer terminal_fixture;
        specforge::AutomationNamedPipeClient
            terminal_client;
        ConnectAndHandshake(
            terminal_fixture,
            terminal_client);
        std::string terminal_error;
        const auto send_capture =
            [&](std::string_view request_id) {
                Require(
                    terminal_client.Send(
                        specforge::
                            SerializeAutomationCommandRequest(
                                request_id,
                                specforge::
                                    AutomationCommandKind::
                                        FrameCapture,
                                specforge::
                                    AutomationFrameCaptureParameters{
                                        "C:\\automation\\capture.png"}),
                        terminal_error),
                    terminal_error);
                Require(
                    ReceiveParsed(terminal_client)
                            .status ==
                        "accepted",
                    "capture finalization terminal fixture should be accepted");
                const auto dequeued =
                    terminal_fixture.server
                        .TakePendingCommands();
                Require(
                    dequeued.size() == 1,
                    "capture finalization terminal fixture should dequeue one request");
                return dequeued.front();
            };

        const auto completed_command =
            send_capture("capture-completed");
        const auto completed =
            terminal_fixture.server
                .TryFinalizeFrameCapture(
                    completed_command,
                    "\"result\":{}",
                    []() {
                        return S_OK;
                    });
        const auto completed_terminal =
            ReceiveParsed(terminal_client);
        Require(
            completed.state ==
                    specforge::
                        AutomationNamedPipeServer::
                            FrameCaptureFinalizationState::
                                Completed &&
                completed_terminal.status ==
                    "completed",
            "a successful publish and completed terminal should share one finalization lease");

        const auto failed_command =
            send_capture("capture-publish-failed");
        const auto failed =
            terminal_fixture.server
                .TryFinalizeFrameCapture(
                    failed_command,
                    "\"result\":{}",
                    []() {
                        return HRESULT_FROM_WIN32(
                            ERROR_FILE_EXISTS);
                    });
        const auto failed_terminal =
            ReceiveParsed(terminal_client);
        Require(
            failed.state ==
                    specforge::
                        AutomationNamedPipeServer::
                            FrameCaptureFinalizationState::
                                Failed &&
                failed_terminal.status == "failed" &&
                failed_terminal.error_code ==
                    "capture_failed",
            "a failed no-replace publish should terminal failed inside the same finalization lease");
    }

    {
        RunningServer lease_first_fixture;
        specforge::AutomationNamedPipeClient
            lease_first_client;
        ConnectAndHandshake(
            lease_first_fixture,
            lease_first_client);

        std::string lease_first_error;
        Require(
            lease_first_client.Send(
                specforge::
                    SerializeAutomationCommandRequest(
                        "capture-lease-first",
                        specforge::
                            AutomationCommandKind::
                                FrameCapture,
                        specforge::
                            AutomationFrameCaptureParameters{
                                "C:\\automation\\capture.png"}),
                lease_first_error),
            lease_first_error);
        Require(
            ReceiveParsed(lease_first_client)
                    .status ==
                "accepted",
            "lease-first capture fixture should be accepted");
        const auto lease_first_commands =
            lease_first_fixture.server
                .TakePendingCommands();
        Require(
            lease_first_commands.size() == 1,
            "lease-first capture fixture should dequeue one request");

        std::atomic_bool publish_entered = false;
        std::atomic_bool release_publish = false;
        std::atomic_bool disconnect_started = false;
        std::atomic_bool disconnect_finished = false;
        std::atomic_uint32_t publish_count = 0;
        specforge::AutomationNamedPipeServer::
            FrameCaptureFinalizationResult
                lease_first_finalization;
        std::thread finalizer([&]() {
            lease_first_finalization =
                lease_first_fixture.server
                    .TryFinalizeFrameCapture(
                        lease_first_commands.front(),
                        "\"result\":{}",
                        [&]() {
                            ++publish_count;
                            publish_entered = true;
                            while (!release_publish.load()) {
                                std::this_thread::yield();
                            }
                            return S_OK;
                        });
        });
        const auto publish_deadline =
            std::chrono::steady_clock::now() +
            2s;
        while (!publish_entered.load() &&
               std::chrono::steady_clock::now() <
                   publish_deadline) {
            std::this_thread::yield();
        }
        Require(
            publish_entered.load(),
            "finalizer should enter the publish callback while holding its lease");

        std::thread disconnect([&]() {
            disconnect_started = true;
            specforge::
                AutomationNamedPipeServerTestAccess::
                    HandleDisconnect(
                        lease_first_fixture.server);
            disconnect_finished = true;
        });
        const auto disconnect_deadline =
            std::chrono::steady_clock::now() +
            2s;
        while (!disconnect_started.load() &&
               std::chrono::steady_clock::now() <
                   disconnect_deadline) {
            std::this_thread::yield();
        }
        Require(
            disconnect_started.load(),
            "disconnect should begin competing after the finalizer holds the lease");
        std::this_thread::sleep_for(25ms);
        Require(
            !disconnect_finished.load(),
            "disconnect must wait while the finalizer owns the publish-and-terminal lease");

        release_publish = true;
        finalizer.join();
        disconnect.join();
        const auto lease_first_snapshot =
            lease_first_fixture.server
                .queue_snapshot();
        Require(
            lease_first_finalization.state ==
                    specforge::
                        AutomationNamedPipeServer::
                            FrameCaptureFinalizationState::
                                Completed &&
                publish_count.load() == 1 &&
                disconnect_finished.load() &&
                !lease_first_snapshot
                     .client_connected &&
                lease_first_snapshot
                        .outstanding_count ==
                    0,
            "lease-first finalization must publish once and complete its terminal transition before disconnect cleanup");
        lease_first_client.Close();
    }

    RunningServer fixture;
    specforge::AutomationNamedPipeClient client;
    ConnectAndHandshake(fixture, client);

    std::string error;
    Require(
        client.Send(
            specforge::
                SerializeAutomationCommandRequest(
                    "capture-lease",
                    specforge::
                        AutomationCommandKind::
                            FrameCapture,
                    specforge::
                        AutomationFrameCaptureParameters{
                            "C:\\automation\\capture.png"}),
            error),
        error);
    Require(
        ReceiveParsed(client).status ==
            "accepted",
        "capture lease fixture should be accepted");
    const auto commands =
        fixture.server.TakePendingCommands();
    Require(
        commands.size() == 1,
        "capture lease fixture should dequeue the accepted request");

    std::atomic_bool release_finalizer = false;
    std::atomic_bool worker_ready = false;
    std::atomic_bool publish_called = false;
    specforge::AutomationNamedPipeServer::
        FrameCaptureFinalizationResult
            finalization;
    std::thread worker([&]() {
        worker_ready = true;
        while (!release_finalizer.load()) {
            std::this_thread::yield();
        }
        finalization =
            fixture.server.TryFinalizeFrameCapture(
                commands.front(),
                "\"result\":{}",
                [&]() {
                    publish_called = true;
                    return S_OK;
                });
    });
    while (!worker_ready.load()) {
        std::this_thread::yield();
    }
    client.Close();
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (!fixture.server.queue_snapshot()
                 .client_connected) {
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    Require(
        !fixture.server.queue_snapshot()
             .client_connected,
        "disconnect must acquire the server boundary before the paused finalizer is released");
    release_finalizer = true;
    worker.join();
    Require(
        finalization.state ==
                specforge::
                    AutomationNamedPipeServer::
                        FrameCaptureFinalizationState::
                            Inactive &&
            !publish_called.load(),
        "disconnect-first capture finalization must not publish output");
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
    TestIdleWaitIsAnEarlierOnlySequenceBarrier();
    TestIdleWaitStopsLaterBusinessDispatch();
    TestPreHandshakeJsonNestingIsBounded();
    TestQueueCapacityVersionAndDisconnect();
    TestOversizedTerminalResponseIsBounded();
    TestExecutionClaimsAndQuitBarrier();
    TestFrameCaptureFinalizationLease();
    TestDistinctRequestIdLimit();
    std::cout
        << "automation control tests passed\n";
    return 0;
}
