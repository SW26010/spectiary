#include "imgui_widget_harness.h"

#include <stdexcept>
#include <utility>

namespace specforge::test {
WidgetHarness::WidgetHarness(std::function<void()> render, FrameMode mode)
    : previous_(ImGui::GetCurrentContext()), render_(std::move(render)), mode_(mode)
{
    IMGUI_CHECKVERSION();
    context_ = mode_ == FrameMode::Owned ? ImGui::CreateContext() : previous_;
    if (!context_ || context_->TestEngine)
        throw std::runtime_error("Widget harness requires an uninstrumented context");
    ImGui::SetCurrentContext(context_);
    context_->TestEngine = this;
    context_->TestEngineHookItems = true;
    if (mode_ == FrameMode::ExistingContext) return;
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
    context_->TestEngine = nullptr;
    context_->TestEngineHookItems = false;
    if (mode_ == FrameMode::Owned) ImGui::DestroyContext(context_);
    ImGui::SetCurrentContext(previous_);
}

void WidgetHarness::Frames(int count)
{
    if (count < 0 || count > 120)
        throw std::runtime_error("Invalid widget frame budget");
    ImGui::SetCurrentContext(context_);
    for (int i = 0; i < count; ++i) {
        widgets_.clear();
        if (mode_ == FrameMode::ExistingContext) {
            render_();
            ++frame_count_;
            continue;
        }
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        render_();
        ImGui::Render();
        ++frame_count_;
    }
}

void WidgetHarness::ItemAdd(ImGuiID id, const ImRect& bounds, const ImGuiLastItemData* data)
{
    if (observed_frame_ != context_->FrameCount) {
        widgets_.clear();
        observed_frame_ = context_->FrameCount;
    }
    if (!id) return;
    ImRect visible = data ? data->NavRect : bounds;
    visible.ClipWith(ImGui::GetCurrentWindow()->ClipRect);
    if (visible.GetWidth() <= 0 || visible.GetHeight() <= 0) return;
    const auto& stack = ImGui::GetCurrentWindow()->IDStack;
    widgets_.push_back({id, ImGui::GetCurrentWindow()->Name, {}, visible,
        ((data ? data->ItemFlags : context_->CurrentItemFlags) & ImGuiItemFlags_Disabled) != 0,
        std::vector<ImGuiID>(stack.begin(), stack.end())});
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

WidgetHarness& WidgetHarness::Current()
{
    auto* context = ImGui::GetCurrentContext();
    if (!context || !context->TestEngine)
        throw std::runtime_error("No widget harness attached");
    return *static_cast<WidgetHarness*>(context->TestEngine);
}

std::optional<Widget> WidgetHarness::Observe(std::string_view label, std::string_view scope, std::string_view window) const
{
    if (observed_frame_ != context_->FrameCount) return std::nullopt;
    std::optional<Widget> match;
    for (const auto& widget : widgets_) {
        // Window decorations may be reported before the root ID stack exists.
        if (widget.id_stack.empty() || (!window.empty() && widget.window != window)) continue;
        const auto suffix = widget.label.find("###");
        const auto seed = widget.id_stack.back();
        const bool identity = widget.label.empty() &&
            (widget.id == ImHashStr(std::string(label).c_str(), 0, seed) ||
             widget.id == ImHashStr(("###" + std::string(label)).c_str(), 0, seed));
        if (!(identity || widget.label == label ||
            (suffix != std::string::npos && std::string_view(widget.label).substr(suffix + 3) == label))) continue;
        if (!scope.empty()) {
            bool scoped = false;
            for (std::size_t i = 1; i < widget.id_stack.size(); ++i)
                scoped |= widget.id_stack[i] == ImHashStr(std::string(scope).c_str(), 0, widget.id_stack[i - 1]);
            if (!scoped) continue;
        }
        if (match && (match->id != widget.id || match->window != widget.window))
            throw std::runtime_error("Ambiguous widget: " + std::string(label));
        match = widget;
    }
    return match;
}

Widget WidgetHarness::Find(std::string_view label, std::string_view window)
{
    ImGui::SetCurrentContext(context_);
    for (int attempt = 0; attempt <= 8; ++attempt) {
        const auto match = Observe(label, {}, window);
        if (match) return *match;
        if (attempt < 8) Frames();
    }
    throw std::runtime_error("Widget not found after 8 frames: " + std::string(label));
}

void WidgetHarness::Click(std::string_view label, ImGuiMouseButton button)
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
    ImGui::GetIO().AddMouseButtonEvent(button, true);
    Frames();
    ImGui::GetIO().AddMouseButtonEvent(button, false);
    Frames(2);
}

void WidgetHarness::Click(std::string_view label, std::function<void()> complete_frame)
{
    if (mode_ != FrameMode::ExistingContext)
        throw std::runtime_error("Complete frame driver requires an existing context");
    auto previous_driver = std::move(render_);
    render_ = std::move(complete_frame);
    try {
        Click(label);
    } catch (...) {
        render_ = std::move(previous_driver);
        throw;
    }
    render_ = std::move(previous_driver);
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
