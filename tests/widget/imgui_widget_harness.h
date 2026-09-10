#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace specforge::test {

// Frame-local observations, never a manually maintained production rectangle map.
struct Widget {
    ImGuiID id = 0;
    std::string window;
    std::string label;
    ImRect bounds; // Visible navigation bounds for pointer input.
    ImRect raw_bounds; // Unclipped item bounds for geometry assertions.
    bool disabled = false;
    std::vector<ImGuiID> id_stack;
};

class WidgetHarness {
public:
    enum class FrameMode { Owned, ExistingContext };
    explicit WidgetHarness(std::function<void()> render, FrameMode mode = FrameMode::Owned);
    ~WidgetHarness();
    WidgetHarness(const WidgetHarness&) = delete;
    WidgetHarness& operator=(const WidgetHarness&) = delete;

    void Frames(int count = 1);
    // Exact label or ### identity suffix; optional exact window disambiguation.
    Widget Find(std::string_view label, std::string_view window = {});
    // Observe only this frame, including disabled controls. A semantic PushID
    // scope disambiguates repeated rows without relying on their screen order.
    std::optional<Widget> Observe(std::string_view label, std::string_view scope = {},
        std::string_view window = {}) const;
    static WidgetHarness& Current();
    void Click(std::string_view label, ImGuiMouseButton button = ImGuiMouseButton_Left);
    // Legacy lifecycle fixtures own NewFrame/EndFrame and observe individual
    // submissions. Borrow their frame driver only for this interaction.
    void Click(std::string_view label, std::function<void()> complete_frame);
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
    FrameMode mode_;
    int observed_frame_ = -1;
};
} // namespace specforge::test
