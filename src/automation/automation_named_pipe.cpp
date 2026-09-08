#include "automation/automation_named_pipe.h"
#include "automation/automation_state.h"

#include "automation/automation_protocol.h"

#include <Windows.h>
#include <Aclapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <sstream>
#include <utility>

namespace specforge {
namespace {

constexpr DWORD kPipeBufferBytes =
    static_cast<DWORD>(kAutomationMaxMessageBytes);

std::string Win32ErrorMessage(
    std::string_view operation,
    DWORD error)
{
    std::ostringstream message;
    message << operation << " failed with Win32 error "
            << error << ".";
    return message.str();
}

struct LocalFreeDeleter {
    void operator()(void* value) const noexcept
    {
        if (value != nullptr) {
            LocalFree(value);
        }
    }
};

struct HandleCloser {
    void operator()(void* value) const noexcept
    {
        if (value != nullptr &&
            value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
};

using UniqueHandle =
    std::unique_ptr<void, HandleCloser>;
using UniqueLocalMemory =
    std::unique_ptr<void, LocalFreeDeleter>;

constexpr auto kOverlappedCancellationDrainTimeout =
    std::chrono::milliseconds(50);

struct PendingOverlappedRead {
    HANDLE completed = nullptr;
    OVERLAPPED overlapped = {};
    std::vector<char> buffer;
    DWORD bytes_read = 0;

    ~PendingOverlappedRead()
    {
        if (completed != nullptr) {
            CloseHandle(completed);
        }
    }
};

struct PendingOverlappedWrite {
    HANDLE completed = nullptr;
    OVERLAPPED overlapped = {};
    std::string buffer;

    ~PendingOverlappedWrite()
    {
        if (completed != nullptr) {
            CloseHandle(completed);
        }
    }
};

struct PipeSecurity {
    SECURITY_DESCRIPTOR descriptor = {};
    UniqueLocalMemory acl;
    std::vector<std::byte> token_user;
    SECURITY_ATTRIBUTES attributes = {};
};

bool PrepareCurrentUserPipeSecurity(
    PipeSecurity& security,
    std::string& error_message)
{
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY,
            &raw_token)) {
        error_message = Win32ErrorMessage(
            "OpenProcessToken",
            GetLastError());
        return false;
    }
    UniqueHandle token(raw_token);

    DWORD required = 0;
    (void)GetTokenInformation(
        token.get(),
        TokenUser,
        nullptr,
        0,
        &required);
    if (required == 0) {
        error_message = Win32ErrorMessage(
            "GetTokenInformation",
            GetLastError());
        return false;
    }
    security.token_user.resize(required);
    if (!GetTokenInformation(
            token.get(),
            TokenUser,
            security.token_user.data(),
            required,
            &required)) {
        error_message = Win32ErrorMessage(
            "GetTokenInformation",
            GetLastError());
        return false;
    }

    auto* token_user = reinterpret_cast<TOKEN_USER*>(
        security.token_user.data());
    EXPLICIT_ACCESSW access = {};
    access.grfAccessPermissions =
        GENERIC_READ | GENERIC_WRITE |
        READ_CONTROL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName =
        static_cast<LPWSTR>(token_user->User.Sid);

    PACL raw_acl = nullptr;
    const DWORD acl_result = SetEntriesInAclW(
        1,
        &access,
        nullptr,
        &raw_acl);
    if (acl_result != ERROR_SUCCESS) {
        error_message = Win32ErrorMessage(
            "SetEntriesInAclW",
            acl_result);
        return false;
    }
    security.acl.reset(raw_acl);

    if (!InitializeSecurityDescriptor(
            &security.descriptor,
            SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(
            &security.descriptor,
            TRUE,
            static_cast<PACL>(security.acl.get()),
            FALSE)) {
        error_message = Win32ErrorMessage(
            "SetSecurityDescriptorDacl",
            GetLastError());
        return false;
    }

    security.attributes.nLength =
        sizeof(security.attributes);
    security.attributes.lpSecurityDescriptor =
        &security.descriptor;
    security.attributes.bInheritHandle = FALSE;
    return true;
}

bool IsPipeDisconnectError(DWORD error)
{
    return error == ERROR_BROKEN_PIPE ||
           error == ERROR_NO_DATA ||
           error == ERROR_PIPE_NOT_CONNECTED ||
           error == ERROR_OPERATION_ABORTED;
}

}  // namespace

AutomationNamedPipeServer::AutomationNamedPipeServer(
    std::wstring pipe_name,
    std::string nonce,
    std::string instance_id)
    : pipe_name_(std::move(pipe_name)),
      nonce_(std::move(nonce)),
      instance_id_(std::move(instance_id))
{
}

AutomationNamedPipeServer::~AutomationNamedPipeServer()
{
    Shutdown();
}

bool AutomationNamedPipeServer::Start(
    CommandReadyCallback command_ready,
    std::string& error_message)
{
    std::lock_guard lock(mutex_);
    if (started_) {
        error_message =
            "Automation named-pipe server is already started.";
        return false;
    }

    PipeSecurity security;
    if (!PrepareCurrentUserPipeSecurity(
            security,
            error_message)) {
        return false;
    }

    pipe_ = CreateNamedPipeW(
        pipe_name_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE |
            PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        kPipeBufferBytes,
        kPipeBufferBytes,
        0,
        &security.attributes);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        error_message = Win32ErrorMessage(
            "CreateNamedPipeW",
            GetLastError());
        return false;
    }

    command_ready_ = std::move(command_ready);
    started_ = true;
    stop_requested_ = false;
    accepting_requests_ = true;
    reader_thread_ =
        std::thread([this]() { ReaderMain(); });
    writer_thread_ =
        std::thread([this]() { WriterMain(); });
    return true;
}

void AutomationNamedPipeServer::Shutdown(
    std::string_view cancellation_code)
{
    {
        std::lock_guard lock(mutex_);
        if (!started_) {
            return;
        }
        accepting_requests_ = false;
        CancelOutstandingLocked(
            cancellation_code,
            "The application is shutting down.");
    }
    NotifyCommandReady();
    WaitForResponsesDrained(
        std::chrono::milliseconds(250));

    {
        std::lock_guard lock(mutex_);
        stop_requested_ = true;
        response_ready_.notify_all();
        reader_poll_.notify_all();
    }
    if (pipe_ != INVALID_HANDLE_VALUE) {
        (void)CancelIoEx(pipe_, nullptr);
        (void)DisconnectNamedPipe(pipe_);
    }
    if (reader_thread_.joinable()) {
        (void)CancelSynchronousIo(
            reader_thread_.native_handle());
        reader_thread_.join();
    }
    if (writer_thread_.joinable()) {
        (void)CancelSynchronousIo(
            writer_thread_.native_handle());
        response_ready_.notify_all();
        writer_thread_.join();
    }

    std::lock_guard lock(mutex_);
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    started_ = false;
    client_connected_ = false;
    handshake_complete_ = false;
    accepting_requests_ = false;
    pending_commands_.clear();
    outstanding_.clear();
    responses_.clear();
}

std::vector<AutomationQueuedCommand>
AutomationNamedPipeServer::TakePendingCommands()
{
    std::lock_guard lock(mutex_);
    std::vector<AutomationQueuedCommand> commands;
    if (!client_connected_) {
        pending_commands_.clear();
        return commands;
    }
    commands.reserve(pending_commands_.size());
    while (!pending_commands_.empty()) {
        const bool dispatch_barrier =
            pending_commands_.front().command ==
            AutomationCommandKind::WaitIdle;
        commands.push_back(
            std::move(pending_commands_.front()));
        pending_commands_.pop_front();
        if (dispatch_barrier) {
            break;
        }
    }
    return commands;
}

bool AutomationNamedPipeServer::IsRequestActive(
    std::string_view request_id) const
{
    std::lock_guard lock(mutex_);
    const auto found =
        outstanding_.find(std::string(request_id));
    return found != outstanding_.end() &&
           (client_connected_ ||
            found->second.execution_claimed);
}

bool AutomationNamedPipeServer::TryClaimExecution(
    const AutomationQueuedCommand& command)
{
    std::lock_guard lock(mutex_);
    const auto found =
        outstanding_.find(command.request_id);
    if (!client_connected_ ||
        found == outstanding_.end() ||
        found->second.command != command.command ||
        found->second.sequence != command.sequence ||
        found->second.execution_claimed ||
        (command.command !=
             AutomationCommandKind::SettingSet &&
         command.command !=
             AutomationCommandKind::PanelSet &&
         command.command !=
             AutomationCommandKind::SourceOpen &&
         command.command !=
             AutomationCommandKind::SpectrumGoto &&
         command.command !=
             AutomationCommandKind::LabelAssign &&
         command.command !=
             AutomationCommandKind::ProfileStart &&
         command.command !=
             AutomationCommandKind::ProfileStop)) {
        return false;
    }
    found->second.execution_claimed = true;
    return true;
}

AutomationControlQueueSnapshot
AutomationNamedPipeServer::queue_snapshot() const
{
    std::lock_guard lock(mutex_);
    return {
        .client_connected = client_connected_,
        .handshake_complete = handshake_complete_,
        .accepting_requests = accepting_requests_,
        .pending_count = pending_commands_.size(),
        .outstanding_count = outstanding_.size(),
    };
}

void AutomationNamedPipeServer::Complete(
    const AutomationQueuedCommand& command, const AutomationCommandResult& result)
{
    CompleteSerializedBody(command, SerializeAutomationCommandResultBody(result));
}

AutomationNamedPipeServer::FrameCaptureFinalizationResult
AutomationNamedPipeServer::TryFinalizeFrameCapture(
    const AutomationQueuedCommand& command, const AutomationCommandResult& result,
    const FrameCapturePublishCallback& publish)
{
    return FinalizeSerializedFrameCapture(command,
        SerializeAutomationCommandResultBody(result), publish);
}

void AutomationNamedPipeServer::CompleteState(
    const AutomationQueuedCommand& command, const AutomationStateSnapshot& state)
{
    CompleteSerializedBody(command, SerializeAutomationStateBody(state));
}

void AutomationNamedPipeServer::CompleteSerializedBody(
    const AutomationQueuedCommand& command,
    std::string_view body_members)
{
    std::lock_guard lock(mutex_);
    const auto found =
        outstanding_.find(command.request_id);
    if (found == outstanding_.end() ||
        found->second.sequence != command.sequence) {
        return;
    }
    const bool send_response = client_connected_;
    if (!send_response &&
        !found->second.execution_claimed) {
        return;
    }
    outstanding_.erase(found);
    if (send_response) {
        EnqueueResponseLocked(
            SerializeAutomationTerminalResponse(
                command.request_id,
                command.command,
                "completed",
                body_members),
            command.request_id,
            AutomationCommandName(command.command));
    }
}

void AutomationNamedPipeServer::Fail(
    const AutomationQueuedCommand& command,
    std::string_view error_code,
    std::string_view error_message)
{
    std::lock_guard lock(mutex_);
    const auto found =
        outstanding_.find(command.request_id);
    if (found == outstanding_.end() ||
        found->second.sequence != command.sequence) {
        return;
    }
    const bool send_response = client_connected_;
    if (!send_response &&
        !found->second.execution_claimed) {
        return;
    }
    outstanding_.erase(found);
    if (send_response) {
        EnqueueResponseLocked(
            SerializeAutomationFailureResponse(
                command.request_id,
                AutomationCommandName(command.command),
                error_code,
                error_message),
            command.request_id,
            AutomationCommandName(command.command));
    }
}

AutomationNamedPipeServer::AppQuitClaimResult
AutomationNamedPipeServer::TryBeginAppQuit(
    const AutomationQueuedCommand& quit_command)
{
    std::lock_guard lock(mutex_);
    const auto found =
        outstanding_.find(quit_command.request_id);
    if (!client_connected_ ||
        found == outstanding_.end() ||
        found->second.command !=
            AutomationCommandKind::AppQuit ||
        found->second.sequence !=
            quit_command.sequence ||
        found->second.execution_claimed) {
        return AppQuitClaimResult::Inactive;
    }
    accepting_requests_ = false;
    CancelOutstandingLocked(
        "app_quit",
        "The request was canceled because app.quit began normal shutdown.",
        quit_command.request_id);
    const bool earlier_execution =
        std::any_of(
            outstanding_.begin(),
            outstanding_.end(),
            [&quit_command](const auto& entry) {
                return entry.first !=
                           quit_command.request_id &&
                       entry.second.execution_claimed &&
                       entry.second.sequence <
                           quit_command.sequence;
            });
    if (earlier_execution) {
        return AppQuitClaimResult::
            WaitingForEarlierExecution;
    }
    found->second.execution_claimed = true;
    return AppQuitClaimResult::Claimed;
}

AutomationNamedPipeServer::
    FrameCaptureFinalizationResult
AutomationNamedPipeServer::FinalizeSerializedFrameCapture(
    const AutomationQueuedCommand& command,
    std::string_view body_members,
    const FrameCapturePublishCallback& publish)
{
    std::lock_guard lock(mutex_);
    const auto found =
        outstanding_.find(command.request_id);
    if (!client_connected_ ||
        found == outstanding_.end() ||
        found->second.command !=
            AutomationCommandKind::FrameCapture ||
        found->second.sequence != command.sequence ||
        found->second.execution_claimed ||
        !publish) {
        return {};
    }

    found->second.execution_claimed = true;
    const HRESULT publish_result = publish();
    outstanding_.erase(found);
    if (FAILED(publish_result)) {
        EnqueueResponseLocked(
            SerializeAutomationFailureResponse(
                command.request_id,
                AutomationCommandName(command.command),
                "capture_failed",
                "The application-rendered PNG could not be published."),
            command.request_id,
            AutomationCommandName(command.command));
        return {
            .state =
                FrameCaptureFinalizationState::Failed,
            .result = publish_result,
        };
    }
    EnqueueResponseLocked(
        SerializeAutomationTerminalResponse(
            command.request_id,
            command.command,
            "completed",
            body_members),
        command.request_id,
        AutomationCommandName(command.command));
    return {
        .state =
            FrameCaptureFinalizationState::Completed,
        .result = S_OK,
    };
}

bool AutomationNamedPipeServer::TryCompleteIdleWaits(
    const std::vector<std::string>& request_ids)
{
    if (request_ids.empty()) {
        return true;
    }
    std::lock_guard lock(mutex_);
    if (!client_connected_) {
        return false;
    }
    std::unordered_set<std::string> completing(
        request_ids.begin(),
        request_ids.end());
    for (const std::string& request_id : request_ids) {
        const auto found = outstanding_.find(request_id);
        if (found == outstanding_.end() ||
            found->second.command !=
                AutomationCommandKind::WaitIdle) {
            return false;
        }
        const std::uint64_t wait_sequence =
            found->second.sequence;
        if (std::any_of(
                pending_commands_.begin(),
                pending_commands_.end(),
                [wait_sequence](const auto& pending) {
                    return pending.sequence <
                        wait_sequence;
                }) ||
            std::any_of(
                outstanding_.begin(),
                outstanding_.end(),
                [&completing,
                 wait_sequence](const auto& entry) {
                    return entry.second.sequence <
                               wait_sequence &&
                           !completing.contains(
                               entry.first);
                })) {
            return false;
        }
    }
    for (const std::string& request_id : request_ids) {
        outstanding_.erase(request_id);
        EnqueueResponseLocked(
            SerializeAutomationTerminalResponse(
                request_id,
                AutomationCommandKind::WaitIdle,
                "completed"),
            request_id,
            AutomationCommandName(
                AutomationCommandKind::WaitIdle));
    }
    return true;
}

void AutomationNamedPipeServer::ReaderMain()
{
    const BOOL connected =
        ConnectNamedPipe(pipe_, nullptr);
    if (!connected &&
        GetLastError() != ERROR_PIPE_CONNECTED) {
        HandleDisconnect();
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (stop_requested_) {
            return;
        }
        client_connected_ = true;
    }

    while (true) {
        {
            std::lock_guard lock(mutex_);
            if (stop_requested_ ||
                !client_connected_) {
                break;
            }
        }
        bool message_available = false;
        if (!MessageAvailable(message_available)) {
            break;
        }
        if (!message_available) {
            std::unique_lock lock(mutex_);
            (void)reader_poll_.wait_for(
                lock,
                std::chrono::milliseconds(5),
                [this]() {
                    return stop_requested_ ||
                           !client_connected_;
                });
            continue;
        }

        std::string message;
        bool too_large = false;
        {
            std::lock_guard io_lock(
                pipe_io_mutex_);
            if (ReadMessage(message, too_large)) {
                // Continue below after releasing the shared pipe I/O seam.
            } else if (!too_large) {
                break;
            }
        }
        if (message.empty() && !too_large) {
            break;
        }
        if (too_large) {
            bool close_after_response = false;
            {
                std::lock_guard lock(mutex_);
                EnqueueResponseLocked(
                    SerializeAutomationFailureResponse(
                        {},
                        {},
                        "message_too_large",
                        "Automation message exceeds max_message_bytes."),
                    {},
                    {});
                close_after_response =
                    !handshake_complete_;
            }
            if (close_after_response) {
                WaitForResponsesDrained(
                    std::chrono::milliseconds(250));
                break;
            }
            continue;
        }
        HandleClientMessage(std::move(message));
        WaitForResponsesDrained(
            std::chrono::milliseconds(1000));
    }
    HandleDisconnect();
    if (pipe_ != INVALID_HANDLE_VALUE) {
        std::lock_guard io_lock(pipe_io_mutex_);
        (void)DisconnectNamedPipe(pipe_);
    }
}

void AutomationNamedPipeServer::WriterMain()
{
    while (true) {
        std::string response;
        {
            std::unique_lock lock(mutex_);
            response_ready_.wait(lock, [this]() {
                return stop_requested_ ||
                       !responses_.empty();
            });
            if (responses_.empty()) {
                if (stop_requested_) {
                    break;
                }
                continue;
            }
            response = std::move(responses_.front());
            responses_.pop_front();
            response_write_in_progress_ = true;
        }
        bool written = false;
        {
            std::lock_guard io_lock(
                pipe_io_mutex_);
            written = WriteMessage(response);
        }
        if (!written) {
            {
                std::lock_guard lock(mutex_);
                response_write_in_progress_ = false;
                response_drained_.notify_all();
            }
            HandleDisconnect();
            break;
        }
        {
            std::lock_guard lock(mutex_);
            response_write_in_progress_ = false;
            if (responses_.empty()) {
                response_drained_.notify_all();
            }
        }
    }
}

void AutomationNamedPipeServer::HandleClientMessage(
    std::string message)
{
    const AutomationClientMessageParseResult parsed =
        ParseAutomationClientMessage(message);
    if (!parsed.message) {
        bool close_after_response = false;
        {
            std::lock_guard lock(mutex_);
            const bool reserved =
                !parsed.validated_request_id ||
                TryReserveRequestIdLocked(
                    *parsed.validated_request_id,
                    parsed.command_name);
            if (reserved) {
                EnqueueResponseLocked(
                    SerializeAutomationFailureResponse(
                        parsed.request_id,
                        parsed.command_name,
                        parsed.error_code,
                        parsed.error_message),
                    parsed.request_id,
                    parsed.command_name);
            }
            close_after_response =
                !handshake_complete_;
        }
        if (close_after_response) {
            WaitForResponsesDrained(
                std::chrono::milliseconds(250));
            std::lock_guard lock(mutex_);
            client_connected_ = false;
        }
        return;
    }

    bool notify = false;
    bool close_after_response = false;
    {
        std::lock_guard lock(mutex_);
        const AutomationClientMessage& request =
            *parsed.message;
        const std::string_view command_name =
            request.kind ==
                    AutomationClientMessage::Kind::Request
            ? AutomationCommandName(request.command)
            : std::string_view{};
        if (!TryReserveRequestIdLocked(
                request.request_id,
                command_name)) {
            return;
        }

        if (!handshake_complete_) {
            if (request.kind !=
                AutomationClientMessage::Kind::Hello) {
                EnqueueResponseLocked(
                    SerializeAutomationFailureResponse(
                        request.request_id,
                        AutomationCommandName(
                            request.command),
                        "handshake_required",
                        "A successful hello handshake is required before commands."),
                    request.request_id,
                    AutomationCommandName(
                        request.command));
                close_after_response = true;
            } else if (
                request.protocol_version !=
                kAutomationProtocolVersion) {
                EnqueueResponseLocked(
                    SerializeAutomationFailureResponse(
                        request.request_id,
                        {},
                        "version_mismatch",
                        "The requested automation protocol version is not supported."),
                    request.request_id,
                    {});
                close_after_response = true;
            } else if (request.nonce != nonce_) {
                EnqueueResponseLocked(
                    SerializeAutomationFailureResponse(
                        request.request_id,
                        {},
                        "nonce_mismatch",
                        "The launcher nonce did not match this automation instance."),
                    request.request_id,
                    {});
                close_after_response = true;
            } else {
                handshake_complete_ = true;
                EnqueueResponseLocked(
                    SerializeAutomationHelloResponse(
                        request.request_id,
                        instance_id_),
                    request.request_id,
                    {});
            }
        } else if (
            request.kind ==
            AutomationClientMessage::Kind::Hello) {
            EnqueueResponseLocked(
                SerializeAutomationFailureResponse(
                    request.request_id,
                    {},
                    "handshake_already_completed",
                    "The connection already completed its hello handshake."),
                request.request_id,
                {});
        } else if (!accepting_requests_) {
            EnqueueResponseLocked(
                SerializeAutomationFailureResponse(
                    request.request_id,
                    AutomationCommandName(
                        request.command),
                    "shutting_down",
                    "The automation instance is no longer accepting requests."),
                request.request_id,
                AutomationCommandName(
                    request.command));
        } else if (
            outstanding_.size() >=
            kAutomationQueueCapacity) {
            EnqueueResponseLocked(
                SerializeAutomationFailureResponse(
                    request.request_id,
                    AutomationCommandName(
                        request.command),
                    "queue_full",
                    "The bounded automation command queue is full."),
                request.request_id,
                AutomationCommandName(
                    request.command));
        } else {
            const std::uint64_t sequence =
                next_sequence_++;
            pending_commands_.push_back({
                .request_id = request.request_id,
                .command = request.command,
                .parameters = request.parameters,
                .sequence = sequence,
            });
            outstanding_.emplace(
                request.request_id,
                OutstandingRequest{
                    .command = request.command,
                    .sequence = sequence,
                });
            EnqueueResponseLocked(
                SerializeAutomationAcceptedResponse(
                    request.request_id,
                    request.command),
                request.request_id,
                AutomationCommandName(
                    request.command));
            notify = true;
        }
    }

    if (notify) {
        NotifyCommandReady();
    }
    if (close_after_response) {
        WaitForResponsesDrained(
            std::chrono::milliseconds(250));
        std::lock_guard lock(mutex_);
        client_connected_ = false;
    }
}

void AutomationNamedPipeServer::HandleDisconnect()
{
    bool notify = false;
    {
        std::lock_guard lock(mutex_);
        if (!client_connected_ &&
            pending_commands_.empty() &&
            outstanding_.empty()) {
            return;
        }
        client_connected_ = false;
        handshake_complete_ = false;
        accepting_requests_ = false;
        pending_commands_.clear();
        std::erase_if(
            outstanding_,
            [](const auto& entry) {
                return !entry.second
                            .execution_claimed;
            });
        responses_.clear();
        response_drained_.notify_all();
        notify = true;
    }
    if (notify) {
        NotifyCommandReady();
    }
}

void AutomationNamedPipeServer::EnqueueResponseLocked(
    std::string response,
    std::string_view request_id,
    std::string_view command_name)
{
    if (response.size() >
        kAutomationMaxMessageBytes) {
        constexpr std::string_view kErrorCode =
            "response_too_large";
        constexpr std::string_view kErrorMessage =
            "The automation response exceeded max_message_bytes.";
        response =
            SerializeAutomationFailureResponse(
                request_id,
                command_name,
                kErrorCode,
                kErrorMessage);
        if (response.size() >
            kAutomationMaxMessageBytes) {
            response =
                SerializeAutomationFailureResponse(
                    request_id,
                    {},
                    kErrorCode,
                    kErrorMessage);
        }
        if (response.size() >
            kAutomationMaxMessageBytes) {
            response =
                SerializeAutomationFailureResponse(
                    {},
                    {},
                    kErrorCode,
                    kErrorMessage);
        }
    }
    responses_.push_back(std::move(response));
    response_ready_.notify_one();
    reader_poll_.notify_one();
}

bool AutomationNamedPipeServer::TryReserveRequestIdLocked(
    std::string_view request_id,
    std::string_view command_name)
{
    if (seen_request_ids_.contains(
            std::string(request_id))) {
        EnqueueResponseLocked(
            SerializeAutomationFailureResponse(
                request_id,
                command_name,
                "duplicate_request_id",
                "request_id values must be unique for the connection."),
            request_id,
            command_name);
        return false;
    }
    if (seen_request_ids_.size() >=
        kAutomationMaxRequestsPerConnection) {
        accepting_requests_ = false;
        EnqueueResponseLocked(
            SerializeAutomationFailureResponse(
                request_id,
                command_name,
                "request_limit_reached",
                "The connection reached its bounded request limit."),
            request_id,
            command_name);
        return false;
    }
    seen_request_ids_.insert(
        std::string(request_id));
    return true;
}

void AutomationNamedPipeServer::CancelOutstandingLocked(
    std::string_view cancellation_code,
    std::string_view cancellation_message,
    std::string_view except_request_id)
{
    pending_commands_.erase(
        std::remove_if(
            pending_commands_.begin(),
            pending_commands_.end(),
            [&](const AutomationQueuedCommand& command) {
                if (command.request_id ==
                    except_request_id) {
                    return false;
                }
                const auto found =
                    outstanding_.find(
                        command.request_id);
                return found ==
                           outstanding_.end() ||
                       !found->second
                            .execution_claimed;
            }),
        pending_commands_.end());

    for (auto iterator = outstanding_.begin();
         iterator != outstanding_.end();) {
        if (iterator->first == except_request_id) {
            ++iterator;
            continue;
        }
        if (iterator->second.execution_claimed) {
            ++iterator;
            continue;
        }
        if (client_connected_) {
            EnqueueResponseLocked(
                SerializeAutomationTerminalResponse(
                    iterator->first,
                    iterator->second.command,
                    "canceled",
                    AutomationCommandResult{AutomationCancellationResult{
                        std::string(cancellation_code), std::string(cancellation_message)}}),
                iterator->first,
                AutomationCommandName(
                    iterator->second.command));
        }
        iterator = outstanding_.erase(iterator);
    }
}

bool AutomationNamedPipeServer::ReadMessage(
    std::string& message,
    bool& too_large)
{
    too_large = false;
    std::vector<char> buffer(
        kAutomationMaxMessageBytes + 1U);
    DWORD bytes_read = 0;
    const BOOL result = ReadFile(
        pipe_,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &bytes_read,
        nullptr);
    if (result) {
        if (bytes_read >
            kAutomationMaxMessageBytes) {
            too_large = true;
            return false;
        }
        message.assign(
            buffer.data(),
            static_cast<std::size_t>(bytes_read));
        return true;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_MORE_DATA) {
        too_large = true;
        std::array<char, 4096> discard = {};
        do {
            bytes_read = 0;
            if (ReadFile(
                    pipe_,
                    discard.data(),
                    static_cast<DWORD>(
                        discard.size()),
                    &bytes_read,
                    nullptr)) {
                break;
            }
        } while (GetLastError() ==
                 ERROR_MORE_DATA);
        return false;
    }
    return false;
}

bool AutomationNamedPipeServer::MessageAvailable(
    bool& available)
{
    std::lock_guard io_lock(pipe_io_mutex_);
    DWORD bytes_available = 0;
    const BOOL result = PeekNamedPipe(
        pipe_,
        nullptr,
        0,
        nullptr,
        &bytes_available,
        nullptr);
    if (!result) {
        available = false;
        return false;
    }
    available = bytes_available != 0;
    return true;
}

bool AutomationNamedPipeServer::WriteMessage(
    std::string_view message)
{
    if (message.size() >
        kAutomationMaxMessageBytes) {
        return false;
    }
    DWORD bytes_written = 0;
    const BOOL result = WriteFile(
        pipe_,
        message.data(),
        static_cast<DWORD>(message.size()),
        &bytes_written,
        nullptr);
    return result &&
           bytes_written == message.size();
}

void AutomationNamedPipeServer::NotifyCommandReady()
{
    CommandReadyCallback callback;
    {
        std::lock_guard lock(mutex_);
        callback = command_ready_;
    }
    if (callback) {
        callback();
    }
}

void AutomationNamedPipeServer::WaitForResponsesDrained(
    std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    (void)response_drained_.wait_for(
        lock,
        timeout,
        [this]() {
            return (responses_.empty() &&
                    !response_write_in_progress_) ||
                   !client_connected_;
        });
}

AutomationNamedPipeClient::~AutomationNamedPipeClient()
{
    Close();
}

bool AutomationNamedPipeClient::Connect(
    const std::wstring& pipe_name,
    std::chrono::milliseconds timeout,
    std::string& error_message)
{
    Close();
    const DWORD timeout_ms =
        timeout.count() < 0
        ? 0
        : static_cast<DWORD>(
              std::min<std::int64_t>(
                  timeout.count(),
                  MAXDWORD));
    if (!WaitNamedPipeW(
            pipe_name.c_str(),
            timeout_ms)) {
        const DWORD error = GetLastError();
        error_message =
            error == ERROR_SEM_TIMEOUT ||
                    error == ERROR_PIPE_BUSY
            ? "Automation connection was explicitly refused because the single client slot is unavailable."
            : Win32ErrorMessage(
                  "WaitNamedPipeW",
                  error);
        return false;
    }

    pipe_ = CreateFileW(
        pipe_name.c_str(),
        // READ_CONTROL is granted only to the current-user ACE and lets the
        // client/test verify that the pipe did not inherit a broad DACL.
        GENERIC_READ | GENERIC_WRITE |
            READ_CONTROL,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        error_message =
            error == ERROR_PIPE_BUSY
            ? "Automation connection was explicitly refused because another client is already connected."
            : Win32ErrorMessage(
                  "CreateFileW",
                  error);
        return false;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(
            pipe_,
            &mode,
            nullptr,
            nullptr)) {
        error_message = Win32ErrorMessage(
            "SetNamedPipeHandleState",
            GetLastError());
        Close();
        return false;
    }
    return true;
}

bool AutomationNamedPipeClient::Send(
    std::string_view message,
    std::string& error_message)
{
    return SendUntil(
        message,
        std::chrono::steady_clock::time_point::max(),
        error_message);
}

bool AutomationNamedPipeClient::SendUntil(
    std::string_view message,
    std::chrono::steady_clock::time_point deadline,
    std::string& error_message)
{
    if (!connected()) {
        error_message =
            "Automation client is not connected.";
        return false;
    }
    if (message.empty() ||
        message.size() >
            kAutomationMaxMessageBytes) {
        error_message =
            "Automation client message violates max_message_bytes.";
        return false;
    }

    const bool bounded =
        deadline !=
        std::chrono::steady_clock::time_point::max();
    if (bounded &&
        std::chrono::steady_clock::now() >= deadline) {
        error_message = std::string(
            kAutomationNamedPipeSendDeadlineExpired);
        Close();
        return false;
    }

    auto write_state =
        std::make_unique<PendingOverlappedWrite>();
    write_state->buffer.assign(
        message.data(),
        message.size());
    write_state->completed = CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr);
    if (write_state->completed == nullptr) {
        error_message = Win32ErrorMessage(
            "CreateEventW",
            GetLastError());
        Close();
        return false;
    }
    write_state->overlapped.hEvent =
        write_state->completed;
    const BOOL result = WriteFile(
        pipe_,
        write_state->buffer.data(),
        static_cast<DWORD>(write_state->buffer.size()),
        nullptr,
        &write_state->overlapped);
    const DWORD write_error =
        result ? ERROR_SUCCESS : GetLastError();
    if (!result && write_error != ERROR_IO_PENDING) {
        error_message = IsPipeDisconnectError(write_error)
            ? "Automation server disconnected."
            : Win32ErrorMessage(
                  "WriteFile",
                  write_error);
        Close();
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto remaining =
        !bounded
        ? std::chrono::milliseconds(0)
        : deadline <= now
        ? std::chrono::milliseconds(0)
        : std::chrono::duration_cast<
              std::chrono::milliseconds>(deadline - now);
    const DWORD wait_timeout =
        !bounded
        ? INFINITE
        : remaining.count() <= 0
        ? 0
        : static_cast<DWORD>(
              (std::min<std::int64_t>)(
                  remaining.count(),
                  static_cast<std::int64_t>(MAXDWORD)));
    DWORD wait_result = WAIT_OBJECT_0;
    DWORD wait_error = ERROR_SUCCESS;
    if (!result) {
        wait_result = WaitForSingleObject(
            write_state->completed,
            wait_timeout);
        wait_error =
            wait_result == WAIT_FAILED
            ? GetLastError()
            : ERROR_SUCCESS;
    }

    if (wait_result == WAIT_TIMEOUT ||
        wait_result == WAIT_FAILED) {
        (void)CancelIoEx(
            pipe_,
            &write_state->overlapped);
        Close();
        const DWORD drain_result = WaitForSingleObject(
            write_state->completed,
            static_cast<DWORD>(
                kOverlappedCancellationDrainTimeout.count()));
        if (drain_result != WAIT_OBJECT_0) {
            // The OVERLAPPED structure and its buffer must remain alive until
            // Windows completes the canceled operation. This is an abnormal
            // launcher-timeout path; retaining the single operation until
            // process exit is preferable to an unbounded wait or use-after-
            // free after the exact-child cleanup continues.
            (void)write_state.release();
        }
    }

    if (wait_result == WAIT_TIMEOUT) {
        error_message = std::string(
            kAutomationNamedPipeSendDeadlineExpired);
        return false;
    }
    if (wait_result == WAIT_FAILED) {
        error_message = Win32ErrorMessage(
            "WaitForSingleObject",
            wait_error);
        return false;
    }

    DWORD bytes_written = 0;
    if (!GetOverlappedResult(
            pipe_,
            &write_state->overlapped,
            &bytes_written,
            FALSE)) {
        const DWORD io_error = GetLastError();
        error_message = IsPipeDisconnectError(io_error)
            ? "Automation server disconnected."
            : Win32ErrorMessage(
                  "GetOverlappedResult",
                  io_error);
        Close();
        return false;
    }
    if (bytes_written != message.size()) {
        error_message =
            "Automation client write was incomplete.";
        Close();
        return false;
    }
    return true;
}

bool AutomationNamedPipeClient::Receive(
    std::string& message,
    std::string& error_message)
{
    return ReceiveUntil(
        message,
        std::chrono::steady_clock::time_point::max(),
        error_message);
}

bool AutomationNamedPipeClient::ReceiveUntil(
    std::string& message,
    std::chrono::steady_clock::time_point deadline,
    std::string& error_message)
{
    if (!connected()) {
        error_message =
            "Automation client is not connected.";
        return false;
    }

    const bool bounded =
        deadline !=
        std::chrono::steady_clock::time_point::max();
    if (bounded &&
        std::chrono::steady_clock::now() >= deadline) {
        error_message = std::string(
            kAutomationNamedPipeReceiveDeadlineExpired);
        Close();
        return false;
    }

    auto read_state =
        std::make_unique<PendingOverlappedRead>();
    read_state->buffer.resize(
        kAutomationMaxMessageBytes + 1U);
    read_state->completed = CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr);
    if (read_state->completed == nullptr) {
        error_message = Win32ErrorMessage(
            "CreateEventW",
            GetLastError());
        Close();
        return false;
    }
    read_state->overlapped.hEvent =
        read_state->completed;

    const BOOL result = ReadFile(
        pipe_,
        read_state->buffer.data(),
        static_cast<DWORD>(read_state->buffer.size()),
        nullptr,
        &read_state->overlapped);
    const DWORD read_error =
        result ? ERROR_SUCCESS : GetLastError();
    if (!result && read_error != ERROR_IO_PENDING) {
        const DWORD error = read_error;
        error_message = IsPipeDisconnectError(error)
            ? "Automation server disconnected."
            : Win32ErrorMessage(
                  "ReadFile",
                  error);
        Close();
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto remaining =
        !bounded
        ? std::chrono::milliseconds(0)
        : deadline <= now
        ? std::chrono::milliseconds(0)
        : std::chrono::duration_cast<
              std::chrono::milliseconds>(deadline - now);
    const DWORD wait_timeout =
        !bounded
        ? INFINITE
        : remaining.count() <= 0
        ? 0
        : static_cast<DWORD>(
              (std::min<std::int64_t>)(
                  remaining.count(),
                  static_cast<std::int64_t>(MAXDWORD)));
    DWORD wait_result = WAIT_OBJECT_0;
    DWORD wait_error = ERROR_SUCCESS;
    if (!result) {
        wait_result = WaitForSingleObject(
            read_state->completed,
            wait_timeout);
        wait_error =
            wait_result == WAIT_FAILED
            ? GetLastError()
            : ERROR_SUCCESS;
    }

    if (wait_result == WAIT_TIMEOUT ||
        wait_result == WAIT_FAILED) {
        (void)CancelIoEx(
            pipe_,
            &read_state->overlapped);
        Close();
        const DWORD drain_result = WaitForSingleObject(
            read_state->completed,
            static_cast<DWORD>(
                kOverlappedCancellationDrainTimeout.count()));
        if (drain_result != WAIT_OBJECT_0) {
            // The OVERLAPPED structure and its buffer must remain alive until
            // Windows completes the canceled operation. This is an abnormal
            // launcher-timeout path; retaining the single operation until
            // process exit is preferable to an unbounded wait or use-after-
            // free after the exact-child cleanup continues.
            (void)read_state.release();
        }
    }

    if (wait_result == WAIT_TIMEOUT) {
        error_message = std::string(
            kAutomationNamedPipeReceiveDeadlineExpired);
        return false;
    }
    if (wait_result == WAIT_FAILED) {
        error_message = Win32ErrorMessage(
            "WaitForSingleObject",
            wait_error);
        return false;
    }
    if (!GetOverlappedResult(
            pipe_,
            &read_state->overlapped,
            &read_state->bytes_read,
            FALSE)) {
        const DWORD error = GetLastError();
        error_message = IsPipeDisconnectError(error)
            ? "Automation server disconnected."
            : Win32ErrorMessage(
                  "GetOverlappedResult",
                  error);
        Close();
        return false;
    }
    if (read_state->bytes_read >
        kAutomationMaxMessageBytes) {
        error_message =
            "Automation server response exceeded max_message_bytes.";
        Close();
        return false;
    }
    message.assign(
        read_state->buffer.data(),
        static_cast<std::size_t>(read_state->bytes_read));
    return true;
}

void AutomationNamedPipeClient::Close()
{
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

bool AutomationNamedPipeClient::connected() const noexcept
{
    return pipe_ != INVALID_HANDLE_VALUE;
}

HANDLE AutomationNamedPipeClient::native_handle() const noexcept
{
    return pipe_;
}

}  // namespace specforge
