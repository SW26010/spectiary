#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace specforge::test {

// Frame-local observations, never a manually maintained production rectangle map.
struct Widget {
    ImGuiID id = 0;
    std::string window;
    std::string label;
    ImRect bounds;
    bool disabled = false;
};

class WidgetHarness {
public:
    explicit WidgetHarness(std::function<void()> render);
    ~WidgetHarness();
    WidgetHarness(const WidgetHarness&) = delete;
    WidgetHarness& operator=(const WidgetHarness&) = delete;

    void Frames(int count = 1);
    // Exact label or ### identity suffix; optional exact window disambiguation.
    Widget Find(std::string_view label, std::string_view window = {});
    void Click(std::string_view label);
    void Key(ImGuiKey key);
    void Text(std::string_view text);
    void Until(const std::function<bool()>& predicate, std::string_view description,
               int max_frames = 12);
    int frame_count() const { return frame_count_; }

    void ItemAdd(ImGuiID id, const ImRect& bounds, const ImGuiLastItemData* data);
    void ItemInfo(ImGuiID id, const char* label);

private:
    ImGuiContext* previous_ = nullptr;
    ImGuiContext* context_ = nullptr;
    std::function<void()> render_;
    std::vector<Widget> widgets_;
    int frame_count_ = 0;
};
} // namespace specforge::test
