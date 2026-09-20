#pragma once

#include <Windows.h>
#include <oleidl.h>
#include <filesystem>
#include <functional>
#include <vector>

namespace spectiary {

// UI-thread OLE adapter. Only copies shell paths; source semantics belong to
// the ordinary in-app opener. No filesystem I/O is performed in COM callbacks.
class Win32FileDropTarget {
public:
    using HitTest = std::function<bool(float, float)>;
    using Feedback = std::function<void(bool)>;
    static constexpr unsigned int MaximumPaths = 256;

    Win32FileDropTarget(HitTest hit_test, Feedback feedback);
    ~Win32FileDropTarget();
    Win32FileDropTarget(const Win32FileDropTarget&) = delete;
    Win32FileDropTarget& operator=(const Win32FileDropTarget&) = delete;
    void SetWindow(HWND window);
    [[nodiscard]] std::vector<std::filesystem::path> TakePaths();

private:
    friend struct Win32FileDropTargetTestAccess;
    [[nodiscard]] IDropTarget* Receiver() const noexcept;
    struct Impl;
    Impl* impl_;
};

} // namespace spectiary
