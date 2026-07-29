#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

inline constexpr int kAutomationProtocolVersion = 1;
inline constexpr std::size_t kAutomationMaxMessageBytes =
    64U * 1024U;
inline constexpr std::size_t kAutomationQueueCapacity = 32U;
inline constexpr std::size_t
    kAutomationMaxRequestsPerConnection = 4096U;

enum class AutomationCommandKind {
    StateGet,
    WaitIdle,
    AppQuit,
};

[[nodiscard]] std::string_view AutomationCommandName(
    AutomationCommandKind command) noexcept;
[[nodiscard]] std::optional<AutomationCommandKind>
ParseAutomationCommandName(std::string_view name) noexcept;

struct AutomationClientMessage {
    enum class Kind {
        Hello,
        Request,
    };

    Kind kind = Kind::Hello;
    std::string request_id;
    int protocol_version = 0;
    std::string nonce;
    AutomationCommandKind command =
        AutomationCommandKind::StateGet;
};

struct AutomationClientMessageParseResult {
    std::optional<AutomationClientMessage> message;
    std::optional<std::string> validated_request_id;
    std::string request_id;
    std::string command_name;
    std::string error_code;
    std::string error_message;
};

[[nodiscard]] AutomationClientMessageParseResult
ParseAutomationClientMessage(std::string_view json);

struct AutomationServerMessage {
    std::string type;
    std::string request_id;
    std::string command_name;
    std::string status;
    std::string error_code;
    std::string error_message;
    int protocol_version = 0;
    std::string instance_id;
};

struct AutomationServerMessageParseResult {
    std::optional<AutomationServerMessage> message;
    std::string error_message;
};

[[nodiscard]] AutomationServerMessageParseResult
ParseAutomationServerMessage(std::string_view json);

[[nodiscard]] std::string SerializeAutomationHelloRequest(
    std::string_view request_id,
    std::string_view nonce);
[[nodiscard]] std::string SerializeAutomationCommandRequest(
    std::string_view request_id,
    AutomationCommandKind command);
[[nodiscard]] std::string SerializeAutomationHelloResponse(
    std::string_view request_id,
    std::string_view instance_id);
[[nodiscard]] std::string SerializeAutomationAcceptedResponse(
    std::string_view request_id,
    AutomationCommandKind command);
[[nodiscard]] std::string SerializeAutomationTerminalResponse(
    std::string_view request_id,
    AutomationCommandKind command,
    std::string_view status,
    std::string_view body_members = {});
[[nodiscard]] std::string SerializeAutomationFailureResponse(
    std::string_view request_id,
    std::string_view command_name,
    std::string_view error_code,
    std::string_view error_message);

[[nodiscard]] const std::vector<std::string_view>&
AutomationCapabilityNames();

}  // namespace specforge
