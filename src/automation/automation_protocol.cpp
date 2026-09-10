#include "automation/automation_protocol.h"
#include "automation/automation_state.h"

#include "app/local_user_state_json.h"
#include "domain/utf8.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <type_traits>

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

std::string JsonString(std::string_view value)
{
    return "\"" + JsonEscape(value) + "\"";
}

std::string PathToUtf8(
    const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    return std::string(
        utf8.begin(),
        utf8.end());
}

const char* JsonBool(bool value)
{
    return value ? "true" : "false";
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

std::optional<AutomationSpectrumTarget>
ParseSpectrumTarget(
    const nlohmann::json& params,
    std::string& error_message)
{
    const nlohmann::json* target =
        JsonObjectMember(params, "target");
    if (target == nullptr ||
        target->type() != nlohmann::json::value_t::object) {
        error_message =
            "Automation spectrum targets require an object 'target'.";
        return std::nullopt;
    }

    const nlohmann::json* index =
        JsonObjectMember(*target, "index");
    const nlohmann::json* name =
        JsonObjectMember(*target, "name");
    if ((index == nullptr) == (name == nullptr)) {
        error_message =
            "Automation spectrum targets require exactly one of 'index' or 'name'.";
        return std::nullopt;
    }

    AutomationSpectrumTarget result;
    if (index != nullptr) {
        result.index =
            ReadJsonSizeMember(*target, "index");
        if (!result.index) {
            error_message =
                "Automation spectrum target index must be a non-negative integer.";
            return std::nullopt;
        }
    } else {
        result.name =
            ReadJsonStringMember(*target, "name");
        if (!result.name || result.name->empty()) {
            error_message =
                "Automation spectrum target name must be a non-empty string.";
            return std::nullopt;
        }
    }
    return result;
}

std::optional<AutomationCommandParameters>
ParseCommandParameters(
    const nlohmann::json& root,
    AutomationCommandKind command,
    std::string& error_message)
{
    const nlohmann::json* params =
        JsonObjectMember(root, "params");
    const bool requires_params =
        command == AutomationCommandKind::SettingGet ||
        command == AutomationCommandKind::SettingSet ||
        command == AutomationCommandKind::PanelGet ||
        command == AutomationCommandKind::PanelSet ||
        command == AutomationCommandKind::SourceOpen ||
        command == AutomationCommandKind::SpectrumGoto ||
        command == AutomationCommandKind::LabelAssign ||
        command == AutomationCommandKind::FrameCapture;
    if (!requires_params) {
        if (params != nullptr &&
            params->type() != nlohmann::json::value_t::object) {
            error_message =
                "Automation request params must be a JSON object.";
            return std::nullopt;
        }
        if (params != nullptr &&
            !params->get_ref<const nlohmann::json::object_t&>().empty()) {
            error_message =
                "Parameterless automation commands accept only an omitted or empty object 'params'.";
            return std::nullopt;
        }
        return AutomationCommandParameters{};
    }
    if (params == nullptr ||
        params->type() != nlohmann::json::value_t::object) {
        error_message =
            "Automation business commands require an object 'params'.";
        return std::nullopt;
    }

    switch (command) {
    case AutomationCommandKind::SettingGet: {
        const std::optional<std::string> name =
            ReadJsonStringMember(*params, "name");
        if (!name || name->empty()) {
            error_message =
                "setting.get requires a non-empty string name.";
            return std::nullopt;
        }
        return AutomationSettingGetParameters{
            .name = *name,
        };
    }
    case AutomationCommandKind::SettingSet: {
        const std::optional<std::string> name =
            ReadJsonStringMember(*params, "name");
        const nlohmann::json* value =
            JsonObjectMember(*params, "value");
        if (!name || name->empty()) {
            error_message =
                "setting.set requires a non-empty string name.";
            return std::nullopt;
        }
        if (value == nullptr) {
            error_message =
                "setting.set requires a scalar value.";
            return std::nullopt;
        }

        AutomationSettingValue setting_value;
        switch (value->type()) {
        case nlohmann::json::value_t::string:
            setting_value = value->get_ref<const std::string&>();
            break;
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
            if (!JsonIsInt64(*value)) {
                error_message = "setting.set integer is out of range.";
                return std::nullopt;
            }
            setting_value = value->get<std::int64_t>();
            break;
        case nlohmann::json::value_t::boolean:
            setting_value = value->get<bool>();
            break;
        case nlohmann::json::value_t::null:
        case nlohmann::json::value_t::object:
        case nlohmann::json::value_t::array:
        default:
            error_message =
                "setting.set value must be a string, integer or boolean.";
            return std::nullopt;
        }
        return AutomationSettingSetParameters{
            .name = *name,
            .value = std::move(setting_value),
        };
    }
    case AutomationCommandKind::PanelGet: {
        const std::optional<std::string> name =
            ReadJsonStringMember(*params, "name");
        if (!name || name->empty()) {
            error_message =
                "panel.get requires a non-empty string name.";
            return std::nullopt;
        }
        return AutomationPanelGetParameters{
            .name = *name,
        };
    }
    case AutomationCommandKind::PanelSet: {
        const std::optional<std::string> name =
            ReadJsonStringMember(*params, "name");
        const nlohmann::json* visible =
            JsonObjectMember(*params, "visible");
        if (!name || name->empty()) {
            error_message =
                "panel.set requires a non-empty string name.";
            return std::nullopt;
        }
        if (visible == nullptr ||
            visible->type() != nlohmann::json::value_t::boolean) {
            error_message =
                "panel.set requires a boolean visible value.";
            return std::nullopt;
        }
        return AutomationPanelSetParameters{
            .name = *name,
            .visible = visible->get<bool>(),
        };
    }
    case AutomationCommandKind::SourceOpen: {
        const std::optional<std::string> path =
            ReadJsonStringMember(*params, "path");
        if (!path || path->empty()) {
            error_message =
                "source.open requires a non-empty string path.";
            return std::nullopt;
        }
        return AutomationSourceOpenParameters{
            .path = *path,
        };
    }
    case AutomationCommandKind::SpectrumGoto: {
        std::optional<AutomationSpectrumTarget> target =
            ParseSpectrumTarget(*params, error_message);
        if (!target) {
            return std::nullopt;
        }
        return AutomationSpectrumGotoParameters{
            .target = std::move(*target),
        };
    }
    case AutomationCommandKind::LabelAssign: {
        const std::optional<int> code =
            ReadJsonIntMember(*params, "code");
        if (!code) {
            error_message =
                "label.assign requires an integer code.";
            return std::nullopt;
        }
        AutomationLabelAssignParameters result{
            .code = *code,
        };
        if (JsonObjectMember(*params, "target") != nullptr) {
            result.target =
                ParseSpectrumTarget(*params, error_message);
            if (!result.target) {
                return std::nullopt;
            }
        }
        return result;
    }
    case AutomationCommandKind::FrameCapture: {
        const std::optional<std::string> path =
            ReadJsonStringMember(*params, "path");
        if (!path || path->empty()) {
            error_message =
                "frame.capture requires a non-empty string path.";
            return std::nullopt;
        }
        return AutomationFrameCaptureParameters{
            .path = *path,
        };
    }
    case AutomationCommandKind::StateGet:
    case AutomationCommandKind::WaitIdle:
    case AutomationCommandKind::ProfileStart:
    case AutomationCommandKind::ProfileStop:
    case AutomationCommandKind::AppQuit:
        return AutomationCommandParameters{};
    }
    return std::nullopt;
}

void SerializeSpectrumTarget(
    std::ostringstream& output,
    const AutomationSpectrumTarget& target)
{
    output << "\"target\":{";
    if (target.index) {
        output << "\"index\":" << *target.index;
    } else {
        output << "\"name\":"
               << JsonString(target.name.value_or(""));
    }
    output << '}';
}

void SerializeSettingValue(
    std::ostringstream& output,
    const AutomationSettingValue& value)
{
    std::visit(
        [&output](const auto& scalar) {
            using Value = std::decay_t<decltype(scalar)>;
            if constexpr (std::is_same_v<Value, std::string>) {
                output << JsonString(scalar);
            } else if constexpr (std::is_same_v<Value, bool>) {
                output << (scalar ? "true" : "false");
            } else {
                output << scalar;
            }
        },
        value);
}

}  // namespace

AutomationProfileStopTerminalPolicy
AutomationProfileStopPolicy(
    ProfileSink::StopReason reason) noexcept
{
    switch (reason) {
    case ProfileSink::StopReason::Explicit:
    case ProfileSink::StopReason::DurationLimit:
    case ProfileSink::StopReason::FileSizeLimit:
        return {.succeeded = true};
    case ProfileSink::StopReason::WriteFailure:
        return {
            .error_code = "profile_write_failed",
        };
    case ProfileSink::StopReason::None:
        return {
            .error_code = "profile_stop_failed",
        };
    }
    return {
        .error_code = "profile_stop_failed",
    };
}

std::string_view AutomationCommandName(
    AutomationCommandKind command) noexcept
{
    switch (command) {
    case AutomationCommandKind::StateGet:
        return "state.get";
    case AutomationCommandKind::WaitIdle:
        return "wait.idle";
    case AutomationCommandKind::SettingGet:
        return "setting.get";
    case AutomationCommandKind::SettingSet:
        return "setting.set";
    case AutomationCommandKind::PanelGet:
        return "panel.get";
    case AutomationCommandKind::PanelSet:
        return "panel.set";
    case AutomationCommandKind::SourceOpen:
        return "source.open";
    case AutomationCommandKind::SpectrumGoto:
        return "spectrum.goto";
    case AutomationCommandKind::LabelAssign:
        return "label.assign";
    case AutomationCommandKind::FrameCapture:
        return "frame.capture";
    case AutomationCommandKind::ProfileStart:
        return "profile.start";
    case AutomationCommandKind::ProfileStop:
        return "profile.stop";
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
    if (name == "setting.get") {
        return AutomationCommandKind::SettingGet;
    }
    if (name == "setting.set") {
        return AutomationCommandKind::SettingSet;
    }
    if (name == "panel.get") {
        return AutomationCommandKind::PanelGet;
    }
    if (name == "panel.set") {
        return AutomationCommandKind::PanelSet;
    }
    if (name == "source.open") {
        return AutomationCommandKind::SourceOpen;
    }
    if (name == "spectrum.goto") {
        return AutomationCommandKind::SpectrumGoto;
    }
    if (name == "label.assign") {
        return AutomationCommandKind::LabelAssign;
    }
    if (name == "frame.capture") {
        return AutomationCommandKind::FrameCapture;
    }
    if (name == "profile.start") {
        return AutomationCommandKind::ProfileStart;
    }
    if (name == "profile.stop") {
        return AutomationCommandKind::ProfileStop;
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
    const std::optional<nlohmann::json> parsed =
        ParseJson(json, parse_error);
    if (!parsed || parsed->type() != nlohmann::json::value_t::object) {
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
        std::string parameter_error;
        std::optional<AutomationCommandParameters>
            parameters = ParseCommandParameters(
                *parsed,
                *command,
                parameter_error);
        if (!parameters) {
            return ParseFailure(
                "invalid_params",
                std::move(parameter_error),
                *request_id,
                *command_name,
                validated_request_id);
        }
        AutomationClientMessage message;
        message.kind = AutomationClientMessage::Kind::Request;
        message.request_id = *request_id;
        message.command = *command;
        message.parameters = std::move(*parameters);
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
    const std::optional<nlohmann::json> parsed =
        ParseJson(json, parse_error);
    if (!parsed || parsed->type() != nlohmann::json::value_t::object) {
        return {
            .error_message = parse_error.empty()
                ? "Automation server message must be a JSON object."
                : parse_error,
        };
    }

    const std::optional<std::string> type =
        ReadJsonStringMember(*parsed, "type");
    const nlohmann::json* request_id =
        JsonObjectMember(*parsed, "request_id");
    const std::optional<std::string> status =
        ReadJsonStringMember(*parsed, "status");
    if (!type ||
        request_id == nullptr ||
        (request_id->type() != nlohmann::json::value_t::string &&
         request_id->type() != nlohmann::json::value_t::null) ||
        !status) {
        return {
            .error_message =
                "Automation server messages require type, string-or-null request_id and status.",
        };
    }

    AutomationServerMessage message;
    message.type = *type;
    if (request_id->type() == nlohmann::json::value_t::string) {
        message.request_id =
            request_id->get_ref<const std::string&>();
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

    if (const nlohmann::json* error =
            JsonObjectMember(*parsed, "error");
        error != nullptr &&
        error->type() == nlohmann::json::value_t::object) {
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
    return SerializeAutomationCommandRequest(
        request_id,
        command,
        AutomationCommandParameters{});
}

std::string SerializeAutomationCommandRequest(
    std::string_view request_id,
    AutomationCommandKind command,
    const AutomationCommandParameters& parameters)
{
    std::ostringstream output;
    output << "{\"type\":\"request\",\"request_id\":"
           << JsonString(request_id)
           << ",\"command\":"
           << JsonString(AutomationCommandName(command));
    std::visit(
        [&output](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (!std::is_same_v<Value, std::monostate>) {
                output << ",\"params\":{";
                if constexpr (
                    std::is_same_v<
                        Value,
                        AutomationSettingGetParameters>) {
                    output << "\"name\":"
                           << JsonString(value.name);
                } else if constexpr (
                    std::is_same_v<
                        Value,
                        AutomationSettingSetParameters>) {
                    output << "\"name\":"
                           << JsonString(value.name)
                           << ",\"value\":";
                    SerializeSettingValue(
                        output,
                        value.value);
                } else if constexpr (
                    std::is_same_v<
                        Value,
                        AutomationPanelGetParameters>) {
                    output << "\"name\":"
                           << JsonString(value.name);
                } else if constexpr (
                    std::is_same_v<
                        Value,
                        AutomationPanelSetParameters>) {
                    output << "\"name\":"
                           << JsonString(value.name)
                           << ",\"visible\":"
                           << (value.visible ? "true" : "false");
                } else if constexpr (
                    std::is_same_v<
                        Value,
                        AutomationSourceOpenParameters> ||
                    std::is_same_v<
                        Value,
                        AutomationFrameCaptureParameters>) {
                    output << "\"path\":"
                           << JsonString(value.path);
                } else if constexpr (
                    std::is_same_v<
                        Value,
                        AutomationSpectrumGotoParameters>) {
                    SerializeSpectrumTarget(
                        output,
                        value.target);
                } else if constexpr (
                    std::is_same_v<
                        Value,
                        AutomationLabelAssignParameters>) {
                    output << "\"code\":" << value.code;
                    if (value.target) {
                        output << ',';
                        SerializeSpectrumTarget(
                            output,
                            *value.target);
                    }
                }
                output << '}';
            }
        },
        parameters);
    output << "}";
    return output.str();
}

const std::vector<std::string_view>&
AutomationCapabilityNames()
{
    static const std::vector<std::string_view>
        capabilities = {
            "state.get",
            "wait.idle",
            "setting.get",
            "setting.set",
            "panel.get",
            "panel.set",
            "source.open",
            "spectrum.goto",
            "label.assign",
            "frame.capture",
            "profile.start",
            "profile.stop",
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

std::string SerializeAutomationStateBody(
    const AutomationStateSnapshot& state)
{
    std::ostringstream output;
    output << "\"state\":{"
           << "\"protocol\":{"
           << "\"version\":"
           << kAutomationProtocolVersion
           << "},\"instance\":{"
           << "\"id\":" << JsonString(state.instance_id)
           << ",\"client_connected\":"
           << JsonBool(
                  state.control.client_connected)
           << ",\"handshake_complete\":"
           << JsonBool(
                  state.control.handshake_complete)
           << "},\"control\":{"
           << "\"accepting_requests\":"
           << JsonBool(
                  state.control.accepting_requests)
           << ",\"pending_count\":"
           << state.control.pending_count
           << ",\"outstanding_count\":"
           << state.control.outstanding_count
           << ",\"capacity\":"
           << state.control.capacity
           << "},\"shell\":{"
           << "\"idle\":"
           << JsonBool(state.shell.idle)
           << ",\"source_load_idle\":"
           << JsonBool(
                  state.shell.source_load_idle)
           << ",\"pending_completion_idle\":"
           << JsonBool(
                  state.shell
                      .pending_completion_idle)
           << ",\"background_retirement_idle\":"
           << JsonBool(
                  state.shell
                      .background_retirement_idle)
           << ",\"active_load_count\":"
           << state.shell.active_load_count
           << ",\"completed_load_count\":"
           << state.shell.completed_load_count
           << ",\"pending_load_count\":"
           << state.shell.pending_load_count
           << ",\"retirement_queued_count\":"
           << state.shell.retirement_queued_count
           << ",\"retirement_in_flight_count\":"
           << state.shell
                  .retirement_in_flight_count
           << "},\"source\":{";
    if (state.shell.current_source_id.empty() &&
        state.shell.current_source_path.empty()) {
        output << "\"present\":false";
    } else {
        output << "\"present\":true"
               << ",\"id\":"
               << JsonString(
                      state.shell.current_source_id)
               << ",\"path\":"
               << JsonString(
                      PathToUtf8(
                          state.shell
                              .current_source_path));
    }
    output << "},\"presented_source\":{"
           << "\"present\":"
           << JsonBool(
                  state.presented_source.present);
    if (state.presented_source.present) {
        output << ",\"id\":"
               << JsonString(
                      state.presented_source.id)
               << ",\"path\":"
               << JsonString(
                      PathToUtf8(
                          state.presented_source
                              .path));
    }
    output << "},\"settings\":{"
           << "\"language\":"
           << JsonString(state.settings.language)
           << ",\"ui_scale_percentage\":"
           << state.settings.ui_scale_percentage
           << "},\"panels\":{"
           << "\"files\":"
           << JsonBool(state.panels.files)
           << ",\"navigation\":"
           << JsonBool(state.panels.navigation)
           << ",\"annotations\":"
           << JsonBool(state.panels.annotations)
           << ",\"labeling\":"
           << JsonBool(state.panels.labeling)
           << ",\"filters\":"
           << JsonBool(state.panels.filters)
           << ",\"sorting\":"
           << JsonBool(state.panels.sorting)
           << ",\"smoothing\":"
           << JsonBool(state.panels.smoothing)
           << ",\"information\":"
           << JsonBool(state.panels.information)
           << ",\"spectral_lines\":"
           << JsonBool(state.panels.spectral_lines)
           << "},\"window\":{"
           << "\"visible\":"
           << JsonBool(state.window.visible)
           << ",\"minimized\":"
           << JsonBool(state.window.minimized)
           << ",\"client_width\":"
           << state.window.client_width
           << ",\"client_height\":"
           << state.window.client_height
           << "},\"spectrum\":{"
           << "\"present\":"
           << JsonBool(state.spectrum.present)
           << ",\"count\":"
           << state.spectrum.count;
    if (state.spectrum.present) {
        output << ",\"index\":"
               << state.spectrum.index
               << ",\"name\":"
               << JsonString(state.spectrum.name);
    }
    output << "},\"labeling\":{"
           << "\"has_active_task\":"
           << JsonBool(
                  state.labeling.has_active_task)
           << ",\"task_ids\":[";
    for (std::size_t index = 0;
         index < state.labeling.task_ids.size();
         ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << JsonString(state.labeling.task_ids[index]);
    }
    output << ']';
    if (state.labeling.has_active_task) {
        output << ",\"active_task\":{"
               << "\"id\":"
               << JsonString(state.labeling.task_id)
               << ",\"name\":"
               << JsonString(
                      state.labeling.task_name)
               << '}';
        if (state.spectrum.present) {
            output
                << ",\"current_spectrum_label\":{"
                << "\"code\":"
                << state.labeling
                       .current_spectrum_code
                << '}';
        }
    }
    output << "},\"capture\":{"
           << "\"pending\":"
           << JsonBool(state.capture.pending)
           << ",\"last_result\":"
           << JsonString(
                  state.capture.last_result);
    if (state.capture.pending &&
        !state.capture.current_path.empty()) {
        output << ",\"current_path\":"
               << JsonString(
                      PathToUtf8(
                          state.capture.current_path));
    }
    if (!state.capture.last_path.empty()) {
        output << ",\"last_path\":"
               << JsonString(
                      PathToUtf8(
                          state.capture.last_path));
    }
    output << "},\"profile\":{"
           << "\"status\":"
           << JsonString(state.profile.status)
           << ",\"stop_reason\":"
           << JsonString(
                  state.profile.stop_reason)
           << ",\"dropped_events\":"
           << state.profile.dropped_events;
    if (!state.profile.path.empty()) {
        output << ",\"path\":"
               << JsonString(
                      PathToUtf8(
                          state.profile.path));
    }
    output << "},\"runtime\":{"
           << "\"running\":"
           << JsonBool(state.runtime.running)
           << ",\"shutting_down\":"
           << JsonBool(
                  state.runtime.shutting_down)
           << ",\"frame_index\":"
           << state.runtime.frame_index
           << "}}";
    return output.str();
}

std::string SerializeAutomationCommandResultBody(const AutomationCommandResult& result)
{
    std::ostringstream out;
    const auto boolean = [](bool value) { return value ? "true" : "false"; };
    const auto spectrum = [&](const AutomationResultSpectrum& value) {
        out << "{\"index\":" << value.index << ",\"name\":" << JsonString(value.name) << '}';
    };
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return;
        } else if constexpr (std::is_same_v<T, AutomationCancellationResult>) {
            out << "\"error\":{\"code\":" << JsonString(value.code)
                << ",\"message\":" << JsonString(value.message) << '}';
        } else {
            out << "\"result\":{";
            if constexpr (std::is_same_v<T, AutomationSettingResult>) {
                out << "\"name\":" << JsonString(value.name) << ",\"value\":";
                std::visit([&](const auto& setting) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(setting)>, std::string>) out << JsonString(setting);
                    else out << setting;
                }, value.value);
                if (value.changed) out << ",\"changed\":" << boolean(*value.changed);
            } else if constexpr (std::is_same_v<T, AutomationPanelResult>) {
                out << "\"name\":" << JsonString(value.name) << ",\"visible\":" << boolean(value.visible);
                if (value.changed) out << ",\"changed\":" << boolean(*value.changed);
                if (value.frame_index) out << ",\"frame_index\":" << *value.frame_index;
            } else if constexpr (std::is_same_v<T, AutomationSourceOpenResult>) {
                out << "\"source\":{\"id\":" << JsonString(value.source_id)
                    << ",\"path\":" << JsonString(value.path)
                    << ",\"spectrum_count\":" << value.spectrum_count << "},\"current_spectrum\":";
                spectrum(value.current_spectrum);
            } else if constexpr (std::is_same_v<T, AutomationSpectrumGotoResult>) {
                out << "\"source_id\":" << JsonString(value.source_id) << ",\"spectrum\":";
                spectrum(value.spectrum);
                out << ",\"changed\":" << boolean(value.changed);
            } else if constexpr (std::is_same_v<T, AutomationLabelAssignResult>) {
                out << "\"assignment\":{\"source_id\":" << JsonString(value.source_id)
                    << ",\"task_id\":" << JsonString(value.task_id) << ",\"spectrum\":";
                spectrum(value.spectrum);
                out << ",\"previous_code\":" << value.previous_code << ",\"new_code\":" << value.new_code
                    << ",\"changed\":" << boolean(value.changed) << "},\"persistence\":{";
                const auto& p = value.persistence;
                const char* status = p.output_save_attempted
                    ? (p.output_saved ? "output_saved" : p.output_retry_scheduled ? "output_retry_scheduled" : "output_save_failed")
                    : p.state_saved ? "state_saved" : p.state_save_scheduled ? "state_save_scheduled" : "unchanged";
                out << "\"status\":" << JsonString(status)
                    << ",\"state_save_scheduled\":" << boolean(p.state_save_scheduled)
                    << ",\"state_save_attempted\":" << boolean(p.state_save_attempted)
                    << ",\"state_saved\":" << boolean(p.state_saved)
                    << ",\"output_save_attempted\":" << boolean(p.output_save_attempted)
                    << ",\"output_saved\":" << boolean(p.output_saved)
                    << ",\"output_retry_scheduled\":" << boolean(p.output_retry_scheduled)
                    << "},\"current_spectrum_after\":{\"present\":" << boolean(value.current_spectrum_after.has_value());
                if (value.current_spectrum_after) out << ",\"index\":" << value.current_spectrum_after->index
                    << ",\"name\":" << JsonString(value.current_spectrum_after->name);
                out << '}';
            } else if constexpr (std::is_same_v<T, AutomationFrameCaptureResult>) {
                out << "\"path\":" << JsonString(value.path) << ",\"format\":\"png\",\"scope\":\"main_viewport\""
                    << ",\"frame_index\":" << value.frame_index << ",\"width\":" << value.width << ",\"height\":" << value.height;
            } else if constexpr (std::is_same_v<T, AutomationProfileStartResult>) {
                out << "\"status\":\"recording\",\"path\":" << JsonString(value.path);
            } else if constexpr (std::is_same_v<T, AutomationProfileStopResult>) {
                out << "\"status\":\"succeeded\",\"path\":" << JsonString(value.path)
                    << ",\"stop_reason\":" << JsonString(ProfileSink::StopReasonName(value.reason))
                    << ",\"dropped_events\":" << value.dropped_events;
            }
            out << '}';
        }
    }, result);
    return out.str();
}

std::string SerializeAutomationTerminalResponse(
    std::string_view request_id, AutomationCommandKind command,
    std::string_view status, const AutomationCommandResult& result)
{
    return SerializeAutomationTerminalResponse(request_id, command, status,
        std::string_view(SerializeAutomationCommandResultBody(result)));
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
