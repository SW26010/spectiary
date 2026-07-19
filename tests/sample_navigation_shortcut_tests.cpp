#include "ui/sample_navigation_shortcut.h"

#include <imgui.h>

#include <cstddef>
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

using specforge::RouteSampleNavigationShortcut;
using specforge::SampleNavigationShortcut;

class ScopedImGuiContext {
public:
    ScopedImGuiContext()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        unsigned char* font_pixels = nullptr;
        int font_width = 0;
        int font_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
        Require(font_pixels != nullptr && font_width > 0 && font_height > 0, "ImGui font atlas should build");
    }

    ~ScopedImGuiContext()
    {
        ImGui::DestroyContext();
    }
};

struct NavigationFrameObservation {
    bool previous_focused = false;
    bool next_focused = false;
    SampleNavigationShortcut shortcut = SampleNavigationShortcut::None;
};

NavigationFrameObservation RenderNavigationFrame(bool request_initial_focus, bool focus_next = false)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    ImGui::NewFrame();

    if (request_initial_focus) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::Begin("Navigation shortcut integration");
    if (request_initial_focus && !focus_next) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::Button("Previous");
    NavigationFrameObservation observation;
    observation.previous_focused = ImGui::IsItemFocused();
    ImGui::SameLine();
    if (request_initial_focus && focus_next) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::Button("Next");
    observation.next_focused = ImGui::IsItemFocused();
    observation.shortcut = RouteSampleNavigationShortcut(
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows),
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows));
    ImGui::End();
    ImGui::EndFrame();
    return observation;
}

struct CrossWindowFrameObservation {
    bool first_filter_action_focused = false;
    bool second_filter_action_focused = false;
    bool plot_focused = false;
    bool plot_hovered = false;
    SampleNavigationShortcut shortcut = SampleNavigationShortcut::None;
};

CrossWindowFrameObservation RenderPlotHoverWithFiltersFocusFrame(bool request_initial_focus)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(260.0f, 180.0f), ImGuiCond_Always);
    if (request_initial_focus) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::Begin("Filters shortcut integration");
    if (request_initial_focus) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::Button("First filter action");
    CrossWindowFrameObservation observation;
    observation.first_filter_action_focused = ImGui::IsItemFocused();
    ImGui::SameLine();
    ImGui::Button("Second filter action");
    observation.second_filter_action_focused = ImGui::IsItemFocused();
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(360.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(360.0f, 260.0f), ImGuiCond_Always);
    ImGui::Begin("Plot shortcut integration", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted("Plot");
    observation.plot_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    observation.plot_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    observation.shortcut = RouteSampleNavigationShortcut(
        observation.plot_focused,
        observation.plot_hovered);
    ImGui::End();

    ImGui::EndFrame();
    return observation;
}

struct PlotHoverFrameObservation {
    bool plot_focused = false;
    bool plot_hovered = false;
    SampleNavigationShortcut shortcut = SampleNavigationShortcut::None;
};

PlotHoverFrameObservation RenderPlotHoverFrame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    ImGui::NewFrame();

    ImGui::SetWindowFocus(nullptr);
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(360.0f, 260.0f), ImGuiCond_Always);
    ImGui::Begin("Unfocused Plot shortcut integration", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted("Plot");
    PlotHoverFrameObservation observation;
    observation.plot_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    observation.plot_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    observation.shortcut = RouteSampleNavigationShortcut(
        observation.plot_focused,
        observation.plot_hovered);
    ImGui::End();

    ImGui::EndFrame();
    return observation;
}

struct TextInputFrameObservation {
    bool input_active = false;
    SampleNavigationShortcut shortcut = SampleNavigationShortcut::None;
};

TextInputFrameObservation RenderTextInputFrame(bool request_initial_focus, char* text, std::size_t text_size)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    ImGui::NewFrame();

    if (request_initial_focus) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::Begin("Navigation text-input integration");
    if (request_initial_focus) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("Sample name", text, text_size);
    TextInputFrameObservation observation;
    observation.input_active = ImGui::IsItemActive();
    observation.shortcut = RouteSampleNavigationShortcut(
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows),
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows));
    ImGui::End();

    ImGui::EndFrame();
    return observation;
}

struct PopupFrameObservation {
    bool popup_open = false;
    SampleNavigationShortcut shortcut = SampleNavigationShortcut::None;
};

PopupFrameObservation RenderPopupFrame(bool open_popup)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    ImGui::NewFrame();

    if (open_popup) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::Begin("Navigation popup integration");
    if (open_popup) {
        ImGui::OpenPopup("Sample action");
    }
    PopupFrameObservation observation;
    if (ImGui::BeginPopup("Sample action")) {
        observation.popup_open = true;
        ImGui::Button("Popup action");
        ImGui::EndPopup();
    }
    observation.shortcut = RouteSampleNavigationShortcut(true, true);
    ImGui::End();

    ImGui::EndFrame();
    return observation;
}

void TestTextInputRetainsArrowKeyOwnership()
{
    ScopedImGuiContext context;
    char text[32] = "sample";
    (void)RenderTextInputFrame(true, text, sizeof(text));
    const TextInputFrameObservation initial = RenderTextInputFrame(false, text, sizeof(text));
    Require(initial.input_active, "the real InputText should own keyboard input");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    const TextInputFrameObservation pressed = RenderTextInputFrame(false, text, sizeof(text));
    Require(
        pressed.shortcut == SampleNavigationShortcut::None,
        "a real InputText should suppress sample navigation");
    Require(pressed.input_active, "InputText should remain active after the arrow key");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    (void)RenderTextInputFrame(false, text, sizeof(text));
}

void TestOpenPopupRetainsArrowKeyOwnership()
{
    ScopedImGuiContext context;
    const PopupFrameObservation opened = RenderPopupFrame(true);
    Require(opened.popup_open, "the test popup should open");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    const PopupFrameObservation pressed = RenderPopupFrame(false);
    Require(
        pressed.shortcut == SampleNavigationShortcut::None,
        "an actual open popup should suppress sample navigation");
    Require(pressed.popup_open, "the popup should remain open after the arrow key");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    (void)RenderPopupFrame(false);
}

void TestModifiedArrowDoesNotNavigateSamples()
{
    ScopedImGuiContext context;
    (void)RenderNavigationFrame(true);
    (void)RenderNavigationFrame(false);

    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    const NavigationFrameObservation pressed = RenderNavigationFrame(false);
    Require(
        pressed.shortcut == SampleNavigationShortcut::None,
        "a modified arrow key should not request sample navigation");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftCtrl, false);
    (void)RenderNavigationFrame(false);
}

void TestRightArrowSwitchesSampleWithoutMovingControlFocus()
{
    ScopedImGuiContext context;
    (void)RenderNavigationFrame(true);
    const NavigationFrameObservation initial = RenderNavigationFrame(false);
    Require(initial.previous_focused, "test should begin with the Previous button focused");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    const NavigationFrameObservation pressed = RenderNavigationFrame(false);
    Require(pressed.shortcut == SampleNavigationShortcut::Next, "right arrow should request the next sample");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    const NavigationFrameObservation settled = RenderNavigationFrame(false);
    Require(settled.previous_focused, "sample navigation should retain the Previous button focus");
    Require(!settled.next_focused, "sample navigation should not move focus to the Next button");
}

void TestLeftArrowSwitchesSampleWithoutMovingControlFocus()
{
    ScopedImGuiContext context;
    (void)RenderNavigationFrame(true, true);
    const NavigationFrameObservation initial = RenderNavigationFrame(false);
    Require(initial.next_focused, "test should begin with the Next button focused");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftArrow, true);
    const NavigationFrameObservation pressed = RenderNavigationFrame(false);
    Require(pressed.shortcut == SampleNavigationShortcut::Previous, "left arrow should request the previous sample");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftArrow, false);
    const NavigationFrameObservation settled = RenderNavigationFrame(false);
    Require(settled.next_focused, "sample navigation should retain the Next button focus");
    Require(!settled.previous_focused, "sample navigation should not move focus to the Previous button");
}

void TestTabStillMovesControlFocus()
{
    ScopedImGuiContext context;
    (void)RenderNavigationFrame(true);
    const NavigationFrameObservation initial = RenderNavigationFrame(false);
    Require(initial.previous_focused, "test should begin with the Previous button focused");

    ImGui::SetNavCursorVisible(true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab, true);
    (void)RenderNavigationFrame(false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab, false);
    (void)RenderNavigationFrame(false);
    const NavigationFrameObservation settled = RenderNavigationFrame(false);
    Require(settled.next_focused, "Tab should continue to move focus to the Next button");
}

void TestPlotHoverYieldsToAnotherPanelsKeyboardFocus()
{
    ScopedImGuiContext context;
    ImGui::GetIO().AddMousePosEvent(500.0f, 100.0f);
    (void)RenderPlotHoverWithFiltersFocusFrame(true);
    const CrossWindowFrameObservation initial = RenderPlotHoverWithFiltersFocusFrame(false);
    Require(initial.first_filter_action_focused, "Filters should begin with its first action focused");
    Require(initial.plot_hovered, "the test pointer should hover Plot");
    Require(!initial.plot_focused, "Plot should not own keyboard focus");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    const CrossWindowFrameObservation pressed = RenderPlotHoverWithFiltersFocusFrame(false);
    Require(
        pressed.shortcut == SampleNavigationShortcut::None,
        "Plot hover should not override another panel's keyboard focus");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    const CrossWindowFrameObservation settled = RenderPlotHoverWithFiltersFocusFrame(false);
    Require(
        settled.second_filter_action_focused,
        "the focused panel should retain ordinary arrow-key focus navigation");
}

void TestPlotHoverNavigatesWhenNoPanelHasKeyboardFocus()
{
    ScopedImGuiContext context;
    ImGui::GetIO().AddMousePosEvent(100.0f, 100.0f);
    (void)RenderPlotHoverFrame();
    const PlotHoverFrameObservation initial = RenderPlotHoverFrame();
    Require(initial.plot_hovered, "the test pointer should hover Plot");
    Require(!initial.plot_focused, "the hover-only Plot should not own keyboard focus");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true);
    const PlotHoverFrameObservation pressed = RenderPlotHoverFrame();
    Require(
        pressed.shortcut == SampleNavigationShortcut::Next,
        "Plot hover should navigate when no panel owns keyboard focus");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    (void)RenderPlotHoverFrame();
}

}  // namespace

int main()
{
    TestTextInputRetainsArrowKeyOwnership();
    TestOpenPopupRetainsArrowKeyOwnership();
    TestModifiedArrowDoesNotNavigateSamples();
    TestRightArrowSwitchesSampleWithoutMovingControlFocus();
    TestLeftArrowSwitchesSampleWithoutMovingControlFocus();
    TestTabStillMovesControlFocus();
    TestPlotHoverYieldsToAnotherPanelsKeyboardFocus();
    TestPlotHoverNavigatesWhenNoPanelHasKeyboardFocus();
    return 0;
}
