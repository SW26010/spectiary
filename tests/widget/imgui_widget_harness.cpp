#include "imgui_widget_harness.h"

#include <stdexcept>
#include <utility>

namespace specforge::test {
WidgetHarness::WidgetHarness(std::function<void()> render)
    : previous_(ImGui::GetCurrentContext()), render_(std::move(render))
{
    context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(context_);
    context_->TestEngine = this;
    context_->TestEngineHookItems = true;
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 1000);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
}

WidgetHarness::~WidgetHarness()
{
    ImGui::DestroyContext(context_);
    ImGui::SetCurrentContext(previous_);
}

void WidgetHarness::Frames(int count)
{
    if (count < 0 || count > 120)
        throw std::runtime_error("Invalid widget frame budget");
    ImGui::SetCurrentContext(context_);
    for (int i = 0; i < count; ++i) {
        widgets_.clear();
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        render_();
        ImGui::Render();
        ++frame_count_;
    }
}

void WidgetHarness::ItemAdd(ImGuiID id, const ImRect& bounds, const ImGuiLastItemData* data)
{
    if (!id) return;
    ImRect visible = data ? data->NavRect : bounds;
    visible.ClipWith(ImGui::GetCurrentWindow()->ClipRect);
    if (visible.GetWidth() <= 0 || visible.GetHeight() <= 0) return;
    widgets_.push_back({id, ImGui::GetCurrentWindow()->Name, {}, visible,
        ((data ? data->ItemFlags : context_->CurrentItemFlags) & ImGuiItemFlags_Disabled) != 0});
}

void WidgetHarness::ItemInfo(ImGuiID id, const char* label)
{
    for (auto it = widgets_.rbegin(); it != widgets_.rend(); ++it) {
        if (it->id == id && it->window == ImGui::GetCurrentWindow()->Name) {
            it->label = label ? label : "";
            return;
        }
    }
}

Widget WidgetHarness::Find(std::string_view label, std::string_view window)
{
    ImGui::SetCurrentContext(context_);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const Widget* match = nullptr;
        for (const auto& widget : widgets_) {
            const auto suffix = widget.label.find("###");
            // BeginCombo supplies ItemAdd but no ItemInfo in this ImGui version.
            // For root-scope ### identities, derive the ID from the real window
            // seed; bounds still come exclusively from the item hook.
            const auto* owner = ImGui::FindWindowByName(widget.window.c_str());
            const bool root_identity = widget.label.empty() && owner &&
                widget.id == ImHashStr(("###" + std::string(label)).c_str(), 0, owner->ID);
            const bool matches = root_identity || widget.label == label ||
                (suffix != std::string::npos && std::string_view(widget.label).substr(suffix + 3) == label);
            if (!matches || (!window.empty() && widget.window != window)) continue;
            if (match && (match->id != widget.id || match->window != widget.window))
                throw std::runtime_error("Ambiguous widget: " + std::string(label));
            match = &widget;
        }
        if (match) return *match;
        Frames();
    }
    throw std::runtime_error("Widget not found after 8 frames: " + std::string(label));
}

void WidgetHarness::Click(std::string_view label)
{
    auto widget = Find(label);
    if (widget.disabled) throw std::runtime_error("Disabled widget: " + std::string(label));
    auto center = widget.bounds.GetCenter();
    ImGui::GetIO().AddMousePosEvent(center.x, center.y);
    Frames(2);
    // Layout can settle after a window/section opens; use its current bounds.
    widget = Find(label);
    if (widget.disabled) throw std::runtime_error("Disabled widget: " + std::string(label));
    center = widget.bounds.GetCenter();
    ImGui::GetIO().AddMousePosEvent(center.x, center.y);
    Frames();
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    Frames();
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    Frames(2);
}

void WidgetHarness::Key(ImGuiKey key)
{
    ImGui::SetCurrentContext(context_);
    ImGui::GetIO().AddKeyEvent(key, true);
    Frames();
    ImGui::GetIO().AddKeyEvent(key, false);
    Frames(2);
}

void WidgetHarness::Text(std::string_view text)
{
    ImGui::SetCurrentContext(context_);
    ImGui::GetIO().AddInputCharactersUTF8(std::string(text).c_str());
    Frames(2);
}

void WidgetHarness::Until(const std::function<bool()>& predicate,
                          std::string_view description, int max_frames)
{
    if (max_frames < 0 || max_frames > 120)
        throw std::runtime_error("Invalid widget settlement budget");
    for (int i = 0; i < max_frames && !predicate(); ++i) Frames();
    if (!predicate()) throw std::runtime_error("Widget settlement timed out: " + std::string(description));
}
} // namespace specforge::test

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb,
                               const ImGuiLastItemData* data)
{
    if (ctx->TestEngine) static_cast<specforge::test::WidgetHarness*>(ctx->TestEngine)->ItemAdd(id, bb, data);
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags)
{
    if (ctx->TestEngine) static_cast<specforge::test::WidgetHarness*>(ctx->TestEngine)->ItemInfo(id, label);
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
