#pragma once

#include "profile/profile_sink.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace specforge {

inline constexpr int kAutomationProtocolVersion = 1;
inline constexpr std::size_t kAutomationMaxMessageBytes =
    64U * 1024U;
inline constexpr std::size_t kAutomationQueueCapacity = 32U;
inline constexpr std::size_t
    kAutomationMaxRequestsPerConnection = 4096U;
inline constexpr std::string_view
    kAutomationUiLanguageSettingName = "ui.language";
inline constexpr std::string_view
    kAutomationUiScaleSettingName = "ui.scale";
inline constexpr std::string_view
    kAutomationUiThemeSettingName = "ui.theme";

enum class AutomationCommandKind {
    StateGet,
    WaitIdle,
    SettingGet,
    SettingSet,
    PanelGet,
    PanelSet,
    SourceOpen,
    SpectrumGoto,
    LabelAssign,
    FrameCapture,
    ProfileStart,
    ProfileStop,
    AppQuit,
};

struct AutomationProfileStopTerminalPolicy {
    bool succeeded = false;
    std::string_view error_code;
};

[[nodiscard]] AutomationProfileStopTerminalPolicy
AutomationProfileStopPolicy(
    ProfileSink::StopReason reason) noexcept;

[[nodiscard]] std::string_view AutomationCommandName(
    AutomationCommandKind command) noexcept;
[[nodiscard]] std::optional<AutomationCommandKind>
ParseAutomationCommandName(std::string_view name) noexcept;

struct AutomationSpectrumTarget {
    std::optional<std::size_t> index;
    std::optional<std::string> name;
};

using AutomationSettingValue = std::variant<
    bool,
    std::int64_t,
    std::string>;

struct AutomationSettingGetParameters {
    std::string name;
};

struct AutomationSettingSetParameters {
    std::string name;
    AutomationSettingValue value = false;
};

struct AutomationPanelGetParameters {
    std::string name;
};

struct AutomationPanelSetParameters {
    std::string name;
    bool visible = true;
};

struct AutomationSourceOpenParameters {
    std::string path;
};

struct AutomationSpectrumGotoParameters {
    AutomationSpectrumTarget target;
};

struct AutomationLabelAssignParameters {
    int code = 0;
    std::optional<AutomationSpectrumTarget> target;
};

struct AutomationFrameCaptureParameters {
    std::string path;
};

using AutomationCommandParameters = std::variant<
    std::monostate,
    AutomationSettingGetParameters,
    AutomationSettingSetParameters,
    AutomationPanelGetParameters,
    AutomationPanelSetParameters,
    AutomationSourceOpenParameters,
    AutomationSpectrumGotoParameters,
    AutomationLabelAssignParameters,
    AutomationFrameCaptureParameters>;

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
    AutomationCommandParameters parameters;
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

struct AutomationStateSnapshot;
[[nodiscard]] std::string SerializeAutomationStateBody(
    const AutomationStateSnapshot& state);

// Computed command facts. Wire field names and optional-member rules belong
// to the protocol serializer, never to application command coordinators.
struct AutomationSettingResult {
    std::string name;
    std::variant<std::string, int> value;
    std::optional<bool> changed;
};
struct AutomationPanelResult {
    std::string name;
    bool visible = true;
    std::optional<bool> changed;
    std::optional<std::uint64_t> frame_index;
};
struct AutomationResultSpectrum {
    std::size_t index = 0;
    std::string name;
};
struct AutomationSourceOpenResult {
    std::string source_id;
    std::string path;
    std::size_t spectrum_count = 0;
    AutomationResultSpectrum current_spectrum;
};
struct AutomationSpectrumGotoResult {
    std::string source_id;
    AutomationResultSpectrum spectrum;
    bool changed = false;
};
struct AutomationPersistenceResult {
    bool state_save_scheduled = false;
    bool state_save_attempted = false;
    bool state_saved = false;
    bool output_save_attempted = false;
    bool output_saved = false;
    bool output_retry_scheduled = false;
};
struct AutomationLabelAssignResult {
    std::string source_id;
    std::string task_id;
    AutomationResultSpectrum spectrum;
    int previous_code = -1;
    int new_code = -1;
    bool changed = false;
    AutomationPersistenceResult persistence;
    std::optional<AutomationResultSpectrum> current_spectrum_after;
};
struct AutomationFrameCaptureResult {
    std::string path;
    std::uint64_t frame_index = 0;
    unsigned int width = 0;
    unsigned int height = 0;
};
struct AutomationProfileStartResult { std::string path; };
struct AutomationProfileStopResult {
    std::string path;
    ProfileSink::StopReason reason = ProfileSink::StopReason::None;
    std::uint64_t dropped_events = 0;
};
struct AutomationCancellationResult { std::string code; std::string message; };
using AutomationCommandResult = std::variant<std::monostate,
    AutomationSettingResult, AutomationPanelResult, AutomationSourceOpenResult,
    AutomationSpectrumGotoResult, AutomationLabelAssignResult,
    AutomationFrameCaptureResult, AutomationProfileStartResult,
    AutomationProfileStopResult, AutomationCancellationResult>;

[[nodiscard]] std::string SerializeAutomationCommandResultBody(
    const AutomationCommandResult& result);
[[nodiscard]] std::string SerializeAutomationTerminalResponse(
    std::string_view request_id, AutomationCommandKind command,
    std::string_view status, const AutomationCommandResult& result);

[[nodiscard]] std::string SerializeAutomationHelloRequest(
    std::string_view request_id,
    std::string_view nonce);
[[nodiscard]] std::string SerializeAutomationCommandRequest(
    std::string_view request_id,
    AutomationCommandKind command);
[[nodiscard]] std::string SerializeAutomationCommandRequest(
    std::string_view request_id,
    AutomationCommandKind command,
    const AutomationCommandParameters& parameters);
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
