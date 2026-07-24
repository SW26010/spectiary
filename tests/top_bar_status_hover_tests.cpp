#include "ui/top_bar_status_hover.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

class ScopedImGuiContext {
public:
    ScopedImGuiContext()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        unsigned char* font_pixels = nullptr;
        int font_width = 0;
        int font_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
        Require(
            font_pixels != nullptr && font_width > 0 && font_height > 0,
            "ImGui font atlas should build");
    }

    ~ScopedImGuiContext()
    {
        ImGui::DestroyContext();
    }

    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;
};

struct HoverObservation {
    bool geometry_hovered = false;
    bool host_hovered = false;
    bool status_hovered = false;
    bool status_clicked = false;
};

HoverObservation RenderFrame(bool cover_top_bar)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    ImGui::NewFrame();

    constexpr ImGuiWindowFlags kHostFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_MenuBar;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(400.0f, 200.0f), ImGuiCond_Always);
    ImGui::Begin("Top bar host", nullptr, kHostFlags);

    HoverObservation observation;
    if (ImGui::BeginMenuBar()) {
        const ImRect menu_bar_rect = ImGui::GetCurrentWindow()->MenuBarRect();
        const ImVec2 status_min(300.0f, menu_bar_rect.Min.y);
        const ImVec2 status_max(390.0f, menu_bar_rect.Max.y);
        observation.geometry_hovered =
            ImGui::IsMouseHoveringRect(status_min, status_max, true);
        observation.host_hovered = ImGui::IsWindowHovered();
        observation.status_hovered =
            specforge::IsTopBarStatusHoverTarget(status_min, status_max);
        observation.status_clicked =
            specforge::IsTopBarStatusLeftClickTarget(
                status_min,
                status_max);
        ImGui::EndMenuBar();
    }
    ImGui::End();

    if (cover_top_bar) {
        constexpr ImGuiWindowFlags kCoverFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize;
        ImGui::SetNextWindowPos(ImVec2(280.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(120.0f, 80.0f), ImGuiCond_Always);
        ImGui::Begin("Floating cover", nullptr, kCoverFlags);
        ImGui::End();
    }

    ImGui::EndFrame();
    return observation;
}

void TestVisibleStatusAcceptsHover()
{
    ScopedImGuiContext context;
    ImGui::GetIO().AddMousePosEvent(350.0f, 10.0f);
    (void)RenderFrame(false);
    const HoverObservation observation = RenderFrame(false);

    Require(observation.geometry_hovered, "pointer should be inside the status rectangle");
    Require(observation.host_hovered, "uncovered host should own hover");
    Require(observation.status_hovered, "visible status should accept hover");
}

void TestFloatingWindowBlocksCoveredStatusHover()
{
    ScopedImGuiContext context;
    ImGui::GetIO().AddMousePosEvent(350.0f, 10.0f);
    (void)RenderFrame(true);
    const HoverObservation observation = RenderFrame(true);

    Require(
        observation.geometry_hovered,
        "geometric hit should reproduce through the floating cover");
    Require(!observation.host_hovered, "floating cover should own window hover");
    Require(
        !observation.status_hovered,
        "covered status must not activate its tooltip target");
}

void TestVisibleStatusAcceptsLeftClick()
{
    ScopedImGuiContext context;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(350.0f, 10.0f);
    (void)RenderFrame(false);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    const HoverObservation observation =
        RenderFrame(false);

    Require(
        observation.status_clicked,
        "visible status should accept a left click");
}

void TestFloatingWindowBlocksCoveredStatusClick()
{
    ScopedImGuiContext context;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(350.0f, 10.0f);
    (void)RenderFrame(true);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    const HoverObservation observation =
        RenderFrame(true);

    Require(
        !observation.status_clicked,
        "covered status must not accept a left click");
}

}  // namespace

int main()
{
    TestVisibleStatusAcceptsHover();
    TestFloatingWindowBlocksCoveredStatusHover();
    TestVisibleStatusAcceptsLeftClick();
    TestFloatingWindowBlocksCoveredStatusClick();
    return 0;
}
