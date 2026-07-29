#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

struct AutomationStartupConfiguration {
    std::wstring pipe_name;
    std::string nonce;
    std::string instance_id;
    std::filesystem::path state_root;
};

struct SpecForgeCommandLine {
    std::optional<std::filesystem::path> initial_source;
    std::optional<AutomationStartupConfiguration> automation;
    bool automation_requested = false;
    std::string error_message;
};

[[nodiscard]] SpecForgeCommandLine ParseSpecForgeCommandLine(
    std::span<const std::wstring> arguments);
[[nodiscard]] SpecForgeCommandLine
ParseCurrentProcessSpecForgeCommandLine();

[[nodiscard]] bool IsValidAutomationInstanceId(
    std::string_view value) noexcept;
[[nodiscard]] bool IsValidAutomationNonce(
    std::string_view value) noexcept;
[[nodiscard]] std::wstring AutomationPipeNameForInstance(
    std::string_view instance_id);
[[nodiscard]] bool IsIncompatibleAutomationEnvironmentVariable(
    std::wstring_view name) noexcept;
[[nodiscard]] std::optional<std::wstring>
ActiveIncompatibleAutomationEnvironmentVariable();
[[nodiscard]] std::filesystem::path
OrdinaryUserStateRootForExecutable(
    const std::filesystem::path& executable_path);
[[nodiscard]] bool AutomationStateRootIsIndependent(
    const std::filesystem::path& automation_root,
    const std::filesystem::path& ordinary_root);

}  // namespace specforge
