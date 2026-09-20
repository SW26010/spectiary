#pragma once

#include <Windows.h>
#include <oleidl.h>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>

namespace spectiary {

// UI-thread OLE adapter. Only copies shell paths and an opaque destination;
// import semantics belong to the ordinary in-app openers.
class Win32FileDropTarget {
public:
    // Zero means no target. Capture the destination at release, before hover
    // feedback is cleared or another drag changes the active panel.
    // all_files is affirmative shell metadata; unknown or mixed content is
    // false so file-only panels can decline before the user releases the drag.
    using HitTest = std::function<unsigned int(HWND, float, float, bool all_files)>;
    using Feedback = std::function<void(unsigned int)>;
    struct DropBatch {
        unsigned int destination = 0;
        std::vector<std::filesystem::path> paths;
    };
    static constexpr unsigned int MaximumPaths = 256;

    Win32FileDropTarget(HitTest hit_test, Feedback feedback);
    ~Win32FileDropTarget();
    Win32FileDropTarget(const Win32FileDropTarget&) = delete;
    Win32FileDropTarget& operator=(const Win32FileDropTarget&) = delete;
    void SetWindow(HWND window);
    void SetWindows(std::span<const HWND> windows);
    [[nodiscard]] DropBatch TakeDrop();

private:
    friend struct Win32FileDropTargetTestAccess;
    [[nodiscard]] IDropTarget* Receiver() const noexcept;
    struct Impl;
    Impl* impl_;
};

} // namespace spectiary
