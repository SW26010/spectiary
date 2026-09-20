#pragma once

#include <filesystem>
#include <optional>

namespace spectiary {

class ShellUi;

// Both policies restore Files. Suppressing startup activation is transient;
// only explicit user mutations replace the durable source session.
enum class SourceSessionStartupPolicy { RestoreSavedActive, RestoreRosterWithoutActive };

void OpenInitialSource(
    ShellUi& shell,
    const std::optional<std::filesystem::path>& initial_source);

}  // namespace spectiary
