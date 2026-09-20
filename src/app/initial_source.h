#pragma once

#include <filesystem>
#include <optional>

namespace spectiary {

class ShellUi;

// Decided before constructing ShellUi, so explicit launches never enqueue
// another source's restore work before opening their requested source.
enum class SourceSessionRestorePolicy { Restore, Skip };

void OpenInitialSource(
    ShellUi& shell,
    const std::optional<std::filesystem::path>& initial_source);

}  // namespace spectiary
