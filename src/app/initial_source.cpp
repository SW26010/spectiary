#include "app/initial_source.h"

#include "ui/shell_ui.h"

namespace specforge {

void OpenInitialSource(
    ShellUi& shell,
    const std::optional<std::filesystem::path>& initial_source)
{
    if (initial_source) {
        // Resolve user CLI input once, before worker dispatch. Persisted
        // locators still require explicit absolute/package-relative semantics.
        std::error_code error;
        const auto absolute = std::filesystem::absolute(*initial_source, error);
        shell.OpenExternalSource(error ? *initial_source : absolute.lexically_normal());
    }
}

}  // namespace specforge
