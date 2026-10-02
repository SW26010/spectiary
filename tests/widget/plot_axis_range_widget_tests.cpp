#include "imgui_widget_harness.h"

#include <implot.h>
#include <implot_internal.h>

#include <iostream>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void TestRangeInput(ImAxis selected_axis, const char* field, const char* invalid, ImGuiKey submit)
{
    ImPlotContext* context = ImPlot::CreateContext();
    {
        ImPlotAxis* axis = nullptr;
        std::string menu_text;
        spectiary::test::WidgetHarness ui{[&] {
            ImGui::Begin("Plot");
            if (ImPlot::BeginPlot("Spectrum", ImVec2(400, 250))) {
                ImPlot::SetupAxesLimits(10, 20, 10, 20, ImGuiCond_Once);
                ImPlot::SetupFinish();
                axis = &ImPlot::GetCurrentPlot()->Axes[selected_axis];
                ImPlot::EndPlot();
            }
            ImGui::End();
            ImGui::SetNextWindowPos(ImVec2(450, 0), ImGuiCond_Always);
            ImGui::Begin("Axis menu");
            // Exercise the dependency's actual menu widgets and setters.
            ImGui::LogToBuffer();
            ImPlot::ShowAxisContextMenu(*axis, nullptr, false);
            menu_text = GImGui->LogBuffer.c_str();
            ImGui::LogFinish();
            ImGui::PopItemWidth();
            ImGui::End();
        }};
        ui.Frames(3);
        const auto edit = [&](const char* text) {
            ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
            ui.Click(field);
            ui.Key(ImGuiKey_A);
            ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
            ui.Frames();
            ui.Text(text);
        };
        edit(invalid);
        Require(axis->Range.Min == 10 && axis->Range.Max == 20,
            "Non-finite input must preserve both endpoints while editing");
        Require(menu_text.find("previous value kept") != std::string::npos,
            "Rejected input must display an explanation");
        ui.Key(submit);
        Require(axis->Range.Min == 10 && axis->Range.Max == 20,
            "Non-finite input must preserve both endpoints after submission");
        Require(menu_text.find("previous value kept") != std::string::npos,
            "Rejection explanation must survive submission");
        edit("not-a-number");
        Require(menu_text.find("previous value kept") != std::string::npos,
            "An unparseable edit must not clear the explanation");
        edit(field[1] == 'i' ? "10" : "20");
        Require(axis->Range.Min == 10 && axis->Range.Max == 20,
            "Re-entering the original value must preserve both endpoints");
        Require(menu_text.find("previous value kept") == std::string::npos,
            "A valid text edit equal to the original value must clear the explanation");
        ui.Key(submit);
        Require(menu_text.find("previous value kept") == std::string::npos,
            "Submitting the original value must not restore the explanation");
        edit(field[1] == 'i' ? "1.25e1" : "1.75e1");
        ui.Key(ImGuiKey_Enter);
        Require(field[1] == 'i'
                ? axis->Range.Min == 12.5 && axis->Range.Max == 20
                : axis->Range.Min == 10 && axis->Range.Max == 17.5,
            "A valid scientific-notation edit must still update only its endpoint");
        Require(menu_text.find("previous value kept") == std::string::npos,
            "A successful edit must clear the rejection explanation");
    }
    ImPlot::DestroyContext(context);
}
} // namespace

int main()
{
    try {
        for (ImAxis axis : {ImAxis_X1, ImAxis_Y1}) {
            for (const char* field : {"Min", "Max"}) {
                for (ImGuiKey submit : {ImGuiKey_Enter, ImGuiKey_Tab}) {
                    TestRangeInput(axis, field, "NaN", submit);
                    TestRangeInput(axis, field, "nan", submit);
                    TestRangeInput(axis, field, field[1] == 'i' ? "-inf" : "inf", submit);
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
