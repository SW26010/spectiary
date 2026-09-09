#pragma once

#include "automation/automation_protocol.h"

#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace specforge {

inline constexpr std::string_view
    kAutomationNamedPipeReceiveDeadlineExpired =
        "Automation response deadline expired.";
inline constexpr std::string_view
    kAutomationNamedPipeSendDeadlineExpired =
        "Automation request send deadline expired.";

struct AutomationNamedPipeServerTestAccess;
struct AutomationStateSnapshot;

struct AutomationQueuedCommand {
    std::string request_id;
    AutomationCommandKind command =
        AutomationCommandKind::StateGet;
    AutomationCommandParameters parameters;
    std::uint64_t sequence = 0;
};

struct AutomationControlQueueSnapshot {
    bool client_connected = false;
    bool handshake_complete = false;
    bool accepting_requests = false;
    std::size_t pending_count = 0;
    std::size_t outstanding_count = 0;
    std::size_t capacity = kAutomationQueueCapacity;
};

class AutomationNamedPipeServer {
public:
    using CommandReadyCallback = std::function<void()>;
    using FrameCapturePublishCallback =
        std::function<HRESULT()>;

    enum class AppQuitClaimResult {
        Inactive,
        WaitingForEarlierExecution,
        Claimed,
    };

    enum class FrameCaptureFinalizationState {
        Inactive,
        Completed,
        Failed,
    };

    struct FrameCaptureFinalizationResult {
        FrameCaptureFinalizationState state =
            FrameCaptureFinalizationState::Inactive;
        HRESULT result =
            HRESULT_FROM_WIN32(ERROR_CANCELLED);
    };

    AutomationNamedPipeServer(
        std::wstring pipe_name,
        std::string nonce,
        std::string instance_id);
    ~AutomationNamedPipeServer();

    AutomationNamedPipeServer(
        const AutomationNamedPipeServer&) = delete;
    AutomationNamedPipeServer& operator=(
        const AutomationNamedPipeServer&) = delete;

    [[nodiscard]] bool Start(
        CommandReadyCallback command_ready,
        std::string& error_message);
    void Shutdown(
        std::string_view cancellation_code =
            "app_shutdown");

    [[nodiscard]] std::vector<AutomationQueuedCommand>
    TakePendingCommands();
    [[nodiscard]] bool IsRequestActive(
        std::string_view request_id) const;
    [[nodiscard]] bool TryClaimExecution(
        const AutomationQueuedCommand& command);
    [[nodiscard]] AutomationControlQueueSnapshot
    queue_snapshot() const;

    void Complete(const AutomationQueuedCommand& command,
        const AutomationCommandResult& result = {});
    [[nodiscard]] FrameCaptureFinalizationResult TryFinalizeFrameCapture(
        const AutomationQueuedCommand& command,
        const AutomationCommandResult& result,
        const FrameCapturePublishCallback& publish);
    void CompleteState(const AutomationQueuedCommand& command,
        const AutomationStateSnapshot& state);
    void Fail(
        const AutomationQueuedCommand& command,
        std::string_view error_code,
        std::string_view error_message);
    [[nodiscard]] AppQuitClaimResult TryBeginAppQuit(
        const AutomationQueuedCommand& quit_command);
    [[nodiscard]] bool TryCompleteIdleWaits(
        const std::vector<std::string>& request_ids);

private:
    void CompleteSerializedBody(const AutomationQueuedCommand& command,
        std::string_view body_members);
    [[nodiscard]] FrameCaptureFinalizationResult FinalizeSerializedFrameCapture(
        const AutomationQueuedCommand& command, std::string_view body_members,
        const FrameCapturePublishCallback& publish);
    friend struct AutomationNamedPipeServerTestAccess;

    struct OutstandingRequest {
        AutomationCommandKind command =
            AutomationCommandKind::StateGet;
        std::uint64_t sequence = 0;
        bool execution_claimed = false;
    };

    void ReaderMain();
    void WriterMain();
    void HandleClientMessage(std::string message);
    void HandleDisconnect();
    void EnqueueResponseLocked(
        std::string response,
        std::string_view request_id,
        std::string_view command_name);
    [[nodiscard]] bool TryReserveRequestIdLocked(
        std::string_view request_id,
        std::string_view command_name);
    void CancelOutstandingLocked(
        std::string_view cancellation_code,
        std::string_view cancellation_message,
        std::string_view except_request_id = {});
    [[nodiscard]] bool ReadMessage(
        std::string& message,
        bool& too_large);
    [[nodiscard]] bool MessageAvailable(
        bool& available);
    [[nodiscard]] bool WriteMessage(
        std::string_view message);
    void NotifyCommandReady();
    void WaitForResponsesDrained(
        std::chrono::milliseconds timeout);

    std::wstring pipe_name_;
    std::string nonce_;
    std::string instance_id_;

    mutable std::mutex mutex_;
    std::mutex pipe_io_mutex_;
    std::condition_variable response_ready_;
    std::condition_variable response_drained_;
    std::condition_variable reader_poll_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::thread reader_thread_;
    std::thread writer_thread_;
    CommandReadyCallback command_ready_;
    std::deque<AutomationQueuedCommand>
        pending_commands_;
    std::deque<std::string> responses_;
    std::unordered_map<std::string, OutstandingRequest>
        outstanding_;
    std::unordered_set<std::string> seen_request_ids_;
    std::uint64_t next_sequence_ = 1;
    bool started_ = false;
    bool stop_requested_ = false;
    bool client_connected_ = false;
    bool handshake_complete_ = false;
    bool accepting_requests_ = false;
    bool response_write_in_progress_ = false;
};

class AutomationNamedPipeClient {
public:
    AutomationNamedPipeClient() = default;
    ~AutomationNamedPipeClient();

    AutomationNamedPipeClient(
        const AutomationNamedPipeClient&) = delete;
    AutomationNamedPipeClient& operator=(
        const AutomationNamedPipeClient&) = delete;

    [[nodiscard]] bool Connect(
        const std::wstring& pipe_name,
        std::chrono::milliseconds timeout,
        std::string& error_message);
    [[nodiscard]] bool Send(
        std::string_view message,
        std::string& error_message);
    [[nodiscard]] bool SendUntil(
        std::string_view message,
        std::chrono::steady_clock::time_point deadline,
        std::string& error_message);
    [[nodiscard]] bool Receive(
        std::string& message,
        std::string& error_message);
    [[nodiscard]] bool ReceiveUntil(
        std::string& message,
        std::chrono::steady_clock::time_point deadline,
        std::string& error_message);
    void Close();

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] HANDLE native_handle() const noexcept;

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

}  // namespace specforge
