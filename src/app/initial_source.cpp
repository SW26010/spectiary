#include "app/initial_source.h"

#include "ui/shell_ui.h"

namespace specforge {

void OpenInitialSource(
    ShellUi& shell,
    const std::optional<std::filesystem::path>& initial_source)
{
    if (initial_source) {
        shell.OpenExternalSource(*initial_source);
    }
}

}  // namespace specforge
