#include "automation/automation_protocol.h"

#include "app/local_user_state_json.h"
#include "platform/win32_text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

namespace specforge {
namespace {

constexpr std::size_t kMaxRequestIdBytes = 64U;

bool IsValidRequestId(std::string_view request_id)
{
    if (request_id.empty() ||
        request_id.size() > kMaxRequestIdBytes) {
        return false;
    }
    return std::all_of(
        request_id.begin(),
        request_id.end(),
        [](unsigned char value) {
            return std::isalnum(value) != 0 ||
                   value == '-' || value == '_' ||
                   value == '.';
        });
}

bool IsValidUtf8(std::string_view value)
{
    return value.empty() || !Utf8ToWide(value).empty();
}

std::string JsonString(std::string_view value)
{
    return "\"" + JsonEscape(value) + "\"";
}

AutomationClientMessageParseResult ParseFailure(
    std::string error_code,
    std::string error_message,
    std::string request_id = {},
    std::string command_name = {},
    std::optional<std::string>
        validated_request_id = std::nullopt)
{
    return {
        .validated_request_id =
            std::move(validated_request_id),
        .request_id = std::move(request_id),
        .command_name = std::move(command_name),
        .error_code = std::move(error_code),
        .error_message = std::move(error_message),
    };
}

}  // namespace

std::string_view AutomationCommandName(
    AutomationCommandKind command) noexcept
{
    switch (command) {
    case AutomationCommandKind::StateGet:
        return "state.get";
    case AutomationCommandKind::WaitIdle:
        return "wait.idle";
    case AutomationCommandKind::AppQuit:
        return "app.quit";
    }
    return "unknown";
}

std::optional<AutomationCommandKind>
ParseAutomationCommandName(std::string_view name) noexcept
{
    if (name == "state.get") {
        return AutomationCommandKind::StateGet;
    }
    if (name == "wait.idle") {
        return AutomationCommandKind::WaitIdle;
    }
    if (name == "app.quit") {
        return AutomationCommandKind::AppQuit;
    }
    return std::nullopt;
}

AutomationClientMessageParseResult
ParseAutomationClientMessage(std::string_view json)
{
    if (json.empty()) {
        return ParseFailure(
            "invalid_json",
            "Automation messages must not be empty.");
    }
    if (!IsValidUtf8(json)) {
        return ParseFailure(
            "invalid_utf8",
            "Automation messages must be valid UTF-8.");
    }

    std::string parse_error;
    const std::optional<JsonValue> parsed =
        ParseJson(json, parse_error);
    if (!parsed || parsed->kind != JsonValue::Kind::Object) {
        return ParseFailure(
            "invalid_json",
            parse_error.empty()
                ? "Automation messages must be JSON objects."
                : parse_error);
    }

    const std::optional<std::string> type =
        ReadJsonStringMember(*parsed, "type");
    const std::optional<std::string> request_id =
        ReadJsonStringMember(*parsed, "request_id");
    const std::optional<std::string> command_name =
        ReadJsonStringMember(*parsed, "command");
    const std::string diagnostic_request_id =
        request_id.value_or("");
    const std::string diagnostic_command =
        command_name.value_or("");

    if (!request_id || !IsValidRequestId(*request_id)) {
        return ParseFailure(
            "invalid_request_id",
            "Automation request_id must contain 1-64 ASCII letters, digits, '.', '_' or '-'.",
            diagnostic_request_id,
            diagnostic_command);
    }
    const std::optional<std::string>
        validated_request_id = *request_id;
    if (!type) {
        return ParseFailure(
            "missing_type",
            "Automation messages require a string 'type'.",
            *request_id,
            diagnostic_command,
            validated_request_id);
    }

    if (*type == "hello") {
        const std::optional<int> protocol_version =
            ReadJsonIntMember(*parsed, "protocol_version");
        const std::optional<std::string> nonce =
            ReadJsonStringMember(*parsed, "nonce");
        if (!protocol_version) {
            return ParseFailure(
                "missing_protocol_version",
                "Automation hello requires an integer protocol_version.",
                *request_id,
                {},
                validated_request_id);
        }
        if (!nonce || nonce->empty()) {
            return ParseFailure(
                "missing_nonce",
                "Automation hello requires the launcher nonce.",
                *request_id,
                {},
                validated_request_id);
        }
        AutomationClientMessage message;
        message.kind = AutomationClientMessage::Kind::Hello;
        message.request_id = *request_id;
        message.protocol_version = *protocol_version;
        message.nonce = *nonce;
        return {.message = std::move(message)};
    }

    if (*type == "request") {
        if (!command_name) {
            return ParseFailure(
                "missing_command",
                "Automation requests require a string command.",
                *request_id,
                {},
                validated_request_id);
        }
        const std::optional<AutomationCommandKind> command =
            ParseAutomationCommandName(*command_name);
        if (!command) {
            return ParseFailure(
                "unsupported_command",
                "The requested automation command is not supported.",
                *request_id,
                *command_name,
                validated_request_id);
        }
        AutomationClientMessage message;
        message.kind = AutomationClientMessage::Kind::Request;
        message.request_id = *request_id;
        message.command = *command;
        return {.message = std::move(message)};
    }

    return ParseFailure(
        "unsupported_message_type",
        "Automation message type must be 'hello' or 'request'.",
        *request_id,
        diagnostic_command,
        validated_request_id);
}

AutomationServerMessageParseResult
ParseAutomationServerMessage(std::string_view json)
{
    if (json.empty() || !IsValidUtf8(json)) {
        return {
            .error_message =
                "Automation server message is not valid UTF-8 JSON.",
        };
    }
    std::string parse_error;
    const std::optional<JsonValue> parsed =
        ParseJson(json, parse_error);
    if (!parsed || parsed->kind != JsonValue::Kind::Object) {
        return {
            .error_message = parse_error.empty()
                ? "Automation server message must be a JSON object."
                : parse_error,
        };
    }

    const std::optional<std::string> type =
        ReadJsonStringMember(*parsed, "type");
    const JsonValue* request_id =
        JsonObjectMember(*parsed, "request_id");
    const std::optional<std::string> status =
        ReadJsonStringMember(*parsed, "status");
    if (!type ||
        request_id == nullptr ||
        (request_id->kind != JsonValue::Kind::String &&
         request_id->kind != JsonValue::Kind::Null) ||
        !status) {
        return {
            .error_message =
                "Automation server messages require type, string-or-null request_id and status.",
        };
    }

    AutomationServerMessage message;
    message.type = *type;
    if (request_id->kind == JsonValue::Kind::String) {
        message.request_id =
            request_id->string_value;
    }
    message.status = *status;
    message.command_name =
        ReadJsonStringMember(*parsed, "command")
            .value_or("");
    message.protocol_version =
        ReadJsonIntMember(*parsed, "protocol_version")
            .value_or(0);
    message.instance_id =
        ReadJsonStringMember(*parsed, "instance_id")
            .value_or("");

    if (const JsonValue* error =
            JsonObjectMember(*parsed, "error");
        error != nullptr &&
        error->kind == JsonValue::Kind::Object) {
        message.error_code =
            ReadJsonStringMember(*error, "code")
                .value_or("");
        message.error_message =
            ReadJsonStringMember(*error, "message")
                .value_or("");
    }
    return {.message = std::move(message)};
}

std::string SerializeAutomationHelloRequest(
    std::string_view request_id,
    std::string_view nonce)
{
    std::ostringstream output;
    output << "{\"type\":\"hello\",\"request_id\":"
           << JsonString(request_id)
           << ",\"protocol_version\":"
           << kAutomationProtocolVersion
           << ",\"nonce\":" << JsonString(nonce)
           << "}";
    return output.str();
}

std::string SerializeAutomationCommandRequest(
    std::string_view request_id,
    AutomationCommandKind command)
{
    std::ostringstream output;
    output << "{\"type\":\"request\",\"request_id\":"
           << JsonString(request_id)
           << ",\"command\":"
           << JsonString(AutomationCommandName(command))
           << "}";
    return output.str();
}

const std::vector<std::string_view>&
AutomationCapabilityNames()
{
    static const std::vector<std::string_view>
        capabilities = {
            "state.get",
            "wait.idle",
            "app.quit",
        };
    return capabilities;
}

std::string SerializeAutomationHelloResponse(
    std::string_view request_id,
    std::string_view instance_id)
{
    std::ostringstream output;
    output << "{\"type\":\"hello\",\"request_id\":"
           << JsonString(request_id)
           << ",\"status\":\"completed\""
           << ",\"protocol_version\":"
           << kAutomationProtocolVersion
           << ",\"instance_id\":"
           << JsonString(instance_id)
           << ",\"capabilities\":[";
    const auto& capabilities = AutomationCapabilityNames();
    for (std::size_t index = 0;
         index < capabilities.size();
         ++index) {
        if (index != 0) {
            output << ',';
        }
        output << JsonString(capabilities[index]);
    }
    output << "],\"max_message_bytes\":"
           << kAutomationMaxMessageBytes
           << ",\"queue_capacity\":"
           << kAutomationQueueCapacity
           << "}";
    return output.str();
}

std::string SerializeAutomationAcceptedResponse(
    std::string_view request_id,
    AutomationCommandKind command)
{
    return SerializeAutomationTerminalResponse(
        request_id,
        command,
        "accepted");
}

std::string SerializeAutomationTerminalResponse(
    std::string_view request_id,
    AutomationCommandKind command,
    std::string_view status,
    std::string_view body_members)
{
    std::ostringstream output;
    output << "{\"type\":\"response\",\"request_id\":"
           << JsonString(request_id)
           << ",\"command\":"
           << JsonString(AutomationCommandName(command))
           << ",\"status\":" << JsonString(status);
    if (!body_members.empty()) {
        output << ',' << body_members;
    }
    output << "}";
    return output.str();
}

std::string SerializeAutomationFailureResponse(
    std::string_view request_id,
    std::string_view command_name,
    std::string_view error_code,
    std::string_view error_message)
{
    std::ostringstream output;
    output << "{\"type\":\"response\",\"request_id\":";
    if (request_id.empty()) {
        output << "null";
    } else {
        output << JsonString(request_id);
    }
    if (!command_name.empty()) {
        output << ",\"command\":"
               << JsonString(command_name);
    }
    output << ",\"status\":\"failed\",\"error\":{"
           << "\"code\":" << JsonString(error_code)
           << ",\"message\":" << JsonString(error_message)
           << "}}";
    return output.str();
}

}  // namespace specforge
