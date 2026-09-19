#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace spectiary {

enum class ImGuiLayoutLoadStatus {
    Missing,
    Loaded,
    Malformed,
    Failed,
};

struct ImGuiLayoutLoadResult {
    ImGuiLayoutLoadStatus status = ImGuiLayoutLoadStatus::Failed;
    std::string error;
};

// Shared ordinary-GUI layout persistence uses a complete-snapshot,
// last-completed-writer-wins contract. Each save writes a unique sibling
// temporary file and atomically replaces the shared target after the
// temporary stream is closed. A crashed writer therefore leaves either the
// previous complete layout or the new complete layout; a temporary sibling
// is ignored by the next reader.
class ImGuiLayoutPersistence {
public:
    explicit ImGuiLayoutPersistence(std::filesystem::path target_path);

    [[nodiscard]] const std::filesystem::path& target_path() const noexcept;

    // Must run after ImGui::CreateContext() and before the first NewFrame().
    // Missing or malformed layouts are treated as defaults so startup remains
    // usable; a later SaveNow() repairs a malformed target.
    [[nodiscard]] ImGuiLayoutLoadResult Load() const;

    // SaveIfRequested() follows ImGui's manual-save protocol. It leaves
    // WantSaveIniSettings set when the atomic write fails so a later frame or
    // shutdown can retry. SaveNow() always emits the current complete snapshot
    // and is used during orderly shutdown.
    [[nodiscard]] bool SaveIfRequested(std::string* error = nullptr) const;
    [[nodiscard]] bool SaveNow(std::string* error = nullptr) const;

    // Public for deterministic contention/interruption tests and for keeping
    // the filesystem protocol independent of ImGui's global context.
    [[nodiscard]] bool SaveSnapshot(
        std::string_view snapshot,
        std::string* error = nullptr) const;

    // ImGui's parser is intentionally permissive. Reject only snapshots that
    // are obviously binary or structurally torn before passing them to ImGui.
    [[nodiscard]] static bool IsWellFormedSnapshot(
        std::string_view snapshot) noexcept;

private:
    std::filesystem::path target_path_;
};

}  // namespace spectiary
