#pragma once

#include <filesystem>
#include <optional>

namespace spectiary {

class ShellUi;

// Roster restore policy, decided before constructing ShellUi. Explicit-source
// startup restores the roster and overrides activation through the transaction.
// Skip starts an additional source-free ordinary GUI clean, without persisting
// its synthetic empty state. Explicit source mutations persist normally.
enum class SourceSessionRestorePolicy { Restore, Skip };

void OpenInitialSource(
    ShellUi& shell,
    const std::optional<std::filesystem::path>& initial_source);

}  // namespace spectiary
