#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace specforge {

// Reserved inside an automation-owned state root. When present, this fixture
// supplies the concrete Windows theme returned to FollowSystem resolution;
// ordinary application launches never inspect it.
inline constexpr std::wstring_view
    kAutomationSystemThemeTestFixtureName =
        L".specforge-system-theme-test.txt";

struct AutomationStartupConfiguration {
    std::wstring pipe_name;
    std::string nonce;
    std::string instance_id;
    std::filesystem::path state_root;
    bool allow_persistent_labeling_outputs = false;
};

struct SpecForgeCommandLine {
    std::optional<std::filesystem::path> initial_source;
    std::optional<AutomationStartupConfiguration> automation;
    bool automation_requested = false;
    std::string error_message;
};

struct AutomationCapturePathValidation {
    bool valid = false;
    std::filesystem::path normalized_path;
    std::string error_code;
    std::string error_message;
};

struct AutomationStateOwnedPathValidation {
    bool valid = false;
    std::filesystem::path normalized_path;
    std::string error_message;
};

struct AutomationPreparedProfileOutput {
    std::filesystem::path path;
    std::unique_ptr<std::ostream> stream;
    std::function<void()> discard_output;
    std::string error_message;

    [[nodiscard]] bool valid() const noexcept
    {
        return stream != nullptr;
    }
};

class AutomationProfileOutputFactory {
public:
    [[nodiscard]] static AutomationPreparedProfileOutput
    Create(
        const std::filesystem::path& automation_root,
        const std::filesystem::path& output_directory);

private:
    friend struct AutomationProfileOutputFactoryTestAccess;

    using BeforeOpenCheckpoint =
        std::function<void(const std::filesystem::path&)>;

    [[nodiscard]] static AutomationPreparedProfileOutput
    CreateWithOpenCheckpoint(
        const std::filesystem::path& automation_root,
        const std::filesystem::path& output_directory,
        BeforeOpenCheckpoint before_open);
};

class AutomationStateRootLease {
public:
    AutomationStateRootLease() = default;
    ~AutomationStateRootLease();
    AutomationStateRootLease(
        AutomationStateRootLease&& other) noexcept;
    AutomationStateRootLease& operator=(
        AutomationStateRootLease&& other) noexcept;
    AutomationStateRootLease(
        const AutomationStateRootLease&) = delete;
    AutomationStateRootLease& operator=(
        const AutomationStateRootLease&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const std::filesystem::path&
    path() const noexcept;

private:
    friend AutomationStateRootLease
    CreatePinnedAutomationStateRoot(
        const std::filesystem::path&,
        std::string&);
    friend bool MaterializePinnedAutomationSeed(
        AutomationStateRootLease&,
        const class AutomationReadOnlyFileLease&,
        std::wstring_view,
        std::string&);
    friend void
    RemovePinnedAutomationStateRootBeforeLaunch(
        AutomationStateRootLease&) noexcept;

    void Close() noexcept;

    std::filesystem::path path_;
    void* parent_handle_ = nullptr;
    void* root_handle_ = nullptr;
    void* identity_lock_handle_ = nullptr;
    std::vector<std::wstring>
        materialized_entries_;
};

class AutomationReadOnlyFileLease {
public:
    AutomationReadOnlyFileLease() = default;
    ~AutomationReadOnlyFileLease();
    AutomationReadOnlyFileLease(
        AutomationReadOnlyFileLease&& other) noexcept;
    AutomationReadOnlyFileLease& operator=(
        AutomationReadOnlyFileLease&& other) noexcept;
    AutomationReadOnlyFileLease(
        const AutomationReadOnlyFileLease&) = delete;
    AutomationReadOnlyFileLease& operator=(
        const AutomationReadOnlyFileLease&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const std::filesystem::path&
    path() const noexcept;

private:
    friend AutomationReadOnlyFileLease
    PinAutomationReadOnlyFile(
        const std::filesystem::path&,
        std::string&);
    friend bool MaterializePinnedAutomationSeed(
        AutomationStateRootLease&,
        const AutomationReadOnlyFileLease&,
        std::wstring_view,
        std::string&);

    void Close() noexcept;

    std::filesystem::path path_;
    void* handle_ = nullptr;
};

[[nodiscard]] AutomationStateRootLease
CreatePinnedAutomationStateRoot(
    const std::filesystem::path& path,
    std::string& error_message);
[[nodiscard]] AutomationReadOnlyFileLease
PinAutomationReadOnlyFile(
    const std::filesystem::path& path,
    std::string& error_message);
[[nodiscard]] bool MaterializePinnedAutomationSeed(
    AutomationStateRootLease& root,
    const AutomationReadOnlyFileLease& seed,
    std::wstring_view destination_name,
    std::string& error_message);
void RemovePinnedAutomationStateRootBeforeLaunch(
    AutomationStateRootLease& root) noexcept;

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
[[nodiscard]] AutomationStateOwnedPathValidation
ValidateAutomationStateOwnedPath(
    const std::filesystem::path& automation_root,
    const std::filesystem::path& candidate_path);
[[nodiscard]] AutomationCapturePathValidation
ValidateAutomationCapturePath(
    const std::filesystem::path& automation_root,
    const std::filesystem::path& output_path);

}  // namespace specforge
