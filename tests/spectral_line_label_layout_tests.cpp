#include "plot/spectral_line_label_layout.h"
#include "plot/spectrum_plot_renderer.h"

#include <implot.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void TestBandModeUsesFullProjectedWidthAndTextMetrics()
{
    using spectiary::UseSpectralBandEndpointLabels;
    Require(!UseSpectralBandEndpointLabels(0, 33.9, 20, 40, 4),
        "a compressed band must use its midpoint");
    Require(UseSpectralBandEndpointLabels(0, 34, 20, 40, 4),
        "the exact measured threshold must use endpoints");
    Require(UseSpectralBandEndpointLabels(-30, 4, 20, 40, 4),
        "translation and clipping must not change the mode");
    Require(UseSpectralBandEndpointLabels(34, 0, 20, 40, 4),
        "reversed screen coordinates must use absolute width");
    Require(!UseSpectralBandEndpointLabels(0, 34, 40, 80, 8),
        "larger font metrics must raise the threshold");
}

void TestRenderedBandLabelsFollowViewportAndScale()
{
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(900, 700);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    auto snapshot = std::make_shared<spectiary::SpectrumSnapshot>();
    snapshot->capabilities.can_plot_current_spectrum = true;
    snapshot->capabilities.can_show_spectral_lines = true;
    snapshot->current_spectrum.x_values =
        std::make_shared<const std::vector<double>>(std::initializer_list<double>{0, 10000});
    snapshot->current_spectrum.y_values =
        std::make_shared<const std::vector<double>>(std::initializer_list<double>{0, 1});
    snapshot->current_spectrum.point_count = 2;
    spectiary::line_list::Marker band;
    band.id = "opaque:s/end";
    band.name = "Band";
    band.kind = spectiary::line_list::MarkerKind::Band;
    band.start = 100;
    band.end = 200;
    const ImVec4 color(0.73f, 0.19f, 0.41f, 1.0f);
    spectiary::SpectralLinePlotMarker marker{
        &band, spectiary::PlotSeriesColor::ExplicitColor({color.x, color.y, color.z, color.w})};
    spectiary::SpectrumPlotOverlays overlays{&marker, 1, true, "band-test"};
    spectiary::SpectrumPlotState state;
    state.fit_next_frame = false;
    state.show_raw_curve = false;
    struct FrameGeometry {
        std::array<int, 2> vertices{};
        std::vector<ImVec2> bottom_vertices;
    };
    const auto frame = [&](double min, double max, bool labels, bool set_limits = true) {
        if (set_limits) {
            state.has_last_limits = true;
            state.sync_last_limits_next_frame = true;
            state.last_x_min = min;
            state.last_x_max = max;
            state.last_y_min = 0;
            state.last_y_max = 1;
        }
        overlays.show_spectral_line_labels = labels;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(900, 700));
        ImGui::Begin("Band rendering", nullptr, ImGuiWindowFlags_NoDecoration);
        const auto result = spectiary::RenderSpectrumPlot(
            snapshot, state, spectiary::UiLanguage::English, {}, {}, overlays, {}, nullptr);
        Require(result.plot_submitted, "the real plot must be submitted");
        FrameGeometry geometry;
        // Compare labels on/off using a unique explicit color. Band geometry is
        // identical in both frames; each default-font glyph adds one quad.
        const auto* draw = ImGui::GetWindowDrawList();
        const ImU32 packed_color = ImGui::ColorConvertFloat4ToU32(color);
        for (const auto& vertex : draw->VtxBuffer) {
            if (vertex.col == packed_color) {
                ++geometry.vertices[vertex.pos.y < 350 ? 0 : 1];
                if (vertex.pos.y >= 350) {
                    geometry.bottom_vertices.push_back(vertex.pos);
                }
            }
        }
        ImGui::End();
        ImGui::Render();
        return geometry;
    };
    const auto check = [&](double min, double max, int names, int wavelengths,
                           bool check_endpoint_lanes = false) {
        (void)frame(min, max, false);
        const auto without = frame(min, max, false);
        const auto with = frame(min, max, true);
        Require(with.vertices[0] - without.vertices[0] == names * 4,
            "band name must retain its midpoint visibility");
        Require(with.vertices[1] - without.vertices[1] == wavelengths * 4,
            "drawn wavelength glyphs must match eligible physical anchors");
        if (check_endpoint_lanes) {
            // Band geometry precedes text in the draw list. Verify that prefix
            // before inspecting the six seven-glyph endpoint labels in this fixture.
            Require(std::equal(
                without.bottom_vertices.begin(), without.bottom_vertices.end(),
                with.bottom_vertices.begin(), [](ImVec2 left, ImVec2 right) {
                    return left.x == right.x && left.y == right.y;
                }), "labels must not change the underlying band geometry");
            constexpr std::size_t vertices_per_label = 7 * 4;
            const auto text_begin = without.bottom_vertices.size();
            Require(with.bottom_vertices.size() - text_begin == 6 * vertices_per_label,
                "dense endpoint fixture must draw six complete labels");
            std::array<std::vector<ImVec2>, 2> endpoint_y_ranges;
            for (std::size_t begin = text_begin; begin < with.bottom_vertices.size();
                 begin += vertices_per_label) {
                ImVec2 minimum = with.bottom_vertices[begin];
                ImVec2 maximum = minimum;
                for (std::size_t offset = 1; offset < vertices_per_label; ++offset) {
                    const auto point = with.bottom_vertices[begin + offset];
                    minimum.x = std::min(minimum.x, point.x);
                    minimum.y = std::min(minimum.y, point.y);
                    maximum.x = std::max(maximum.x, point.x);
                    maximum.y = std::max(maximum.y, point.y);
                }
                // In the 50..250 viewport, start/end labels lie on opposite
                // sides of the window center. They may share lanes across sides.
                endpoint_y_ranges[(minimum.x + maximum.x) * 0.5f < 450 ? 0 : 1]
                    .push_back(ImVec2(minimum.y, maximum.y));
            }
            for (auto& ranges : endpoint_y_ranges) {
                Require(ranges.size() == 3, "each endpoint must have three labels");
                std::sort(ranges.begin(), ranges.end(), [](ImVec2 left, ImVec2 right) {
                    return left.x < right.x;
                });
                for (std::size_t index = 1; index < ranges.size(); ++index) {
                    Require(ranges[index - 1].y < ranges[index].x,
                        "labels at the same endpoint must have disjoint vertical bounds");
                }
            }
        }
    };
    check(0, 10000, 4, 7); // Compressed: 150.000.
    check(50, 250, 4, 14); // Expanded: 100.000 and 200.000.
    check(95, 105, 0, 7);  // Start visible, midpoint outside.
    check(195, 205, 0, 7); // End visible, midpoint outside.
    check(120, 130, 0, 0); // Both endpoints and midpoint outside.
    check(140, 160, 4, 0); // Name visible, both endpoints outside.
    check(50, 250, 4, 14); // Returning endpoints after empty frames.
    check(0, 10000, 4, 7); // Zoom back to midpoint mode.
    // Exercise actual ImPlot wheel/drag input through ImGui, retaining the
    // production pan transaction and zoom epoch across consecutive frames.
    (void)frame(50, 250, true);
    io.AddMousePosEvent(450, 350);
    (void)frame(0, 0, true, false);
    const double span_before_zoom = state.last_x_max - state.last_x_min;
    io.AddMouseWheelEvent(0, 1);
    (void)frame(0, 0, true, false);
    Require(state.last_x_max - state.last_x_min < span_before_zoom,
        "real wheel input must zoom the production plot");
    const double min_before_pan = state.last_x_min;
    const double span_before_pan = state.last_x_max - state.last_x_min;
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    (void)frame(0, 0, true, false);
    io.AddMousePosEvent(490, 350);
    (void)frame(0, 0, true, false);
    Require(state.pan_drag_active && state.last_x_min != min_before_pan,
        "real drag input must enter the pan transaction and translate the plot");
    Require(std::abs((state.last_x_max - state.last_x_min) - span_before_pan) < 1e-8,
        "dragging must retain the x scale");
    io.AddMousePosEvent(450, 350);
    (void)frame(0, 0, true, false);
    Require(std::abs(state.last_x_min - min_before_pan) < 1e-8,
        "returning a drag must restore the original viewport");
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    (void)frame(0, 0, true, false);
    Require(!state.pan_drag_active, "release must commit the pan transaction");

    std::array<spectiary::line_list::Marker, 3> dense_bands{band, band, band};
    std::array<spectiary::SpectralLinePlotMarker, 3> dense_markers{marker, marker, marker};
    for (std::size_t index = 0; index < dense_bands.size(); ++index) {
        dense_bands[index].id = std::string(index + 1, 's') + ":/end";
        dense_markers[index].marker = &dense_bands[index];
    }
    overlays.spectral_lines = dense_markers.data();
    overlays.spectral_line_count = dense_markers.size();
    check(50, 250, 12, 42, true); // Coincident labels must occupy distinct lanes.
    check(120, 130, 0, 0);
    check(0, 10000, 12, 21);
    check(50, 250, 12, 42, true); // Mode changes must not leave stale placements.
    overlays.spectral_lines = &marker;
    overlays.spectral_line_count = 1;
    band.kind = spectiary::line_list::MarkerKind::Line;
    band.coordinate = 150;
    check(50, 250, 4, 7); // Ordinary line presentation stays unchanged.
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}

void TestSeparatedLabelsShareTheBottomLane()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"a", 20.0f, 16.0f},
        {"b", 70.0f, 20.0f},
    };
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    Require(layout.size() == 2, "every label should receive a placement");
    Require(layout[0].lane == 0 && layout[1].lane == 0, "separated labels should use the lowest lane");
}

void TestOverlappingLabelsMoveUpAndReuseAvailableLanes()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
        {"c", 55.0f, 20.0f},
    };
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    Require(layout[0].lane == 0, "the first label should use the bottom lane");
    Require(layout[1].lane == 1, "an overlapping label should move up one lane");
    Require(layout[2].lane == 0, "a later label should reuse the bottom lane when it fits");
}

void TestEdgeClampingStillParticipatesInCollisionDetection()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"left-a", 0.0f, 30.0f},
        {"left-b", 5.0f, 20.0f},
        {"right", 100.0f, 30.0f},
    };
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 4.0f);

    Require(layout[0].left == 0.0f, "a left-edge label should remain inside the plot");
    Require(layout[1].left == 0.0f, "a second left-edge label should also be clamped");
    Require(layout[0].lane != layout[1].lane, "clamped overlapping labels should use different lanes");
    Require(layout[2].left == 70.0f, "a right-edge label should remain inside the plot");
}

void TestOnlyViewportAnchorsAreEligibleForLabelLayout()
{
    Require(
        spectiary::IsSpectralLineLabelAnchorInViewport(50.0, 0.0, 100.0),
        "an anchor inside the viewport should be eligible");
    Require(
        spectiary::IsSpectralLineLabelAnchorInViewport(0.0, 0.0, 100.0) &&
            spectiary::IsSpectralLineLabelAnchorInViewport(100.0, 0.0, 100.0),
        "anchors on viewport edges should remain eligible");
    Require(
        !spectiary::IsSpectralLineLabelAnchorInViewport(-0.01, 0.0, 100.0) &&
            !spectiary::IsSpectralLineLabelAnchorInViewport(100.01, 0.0, 100.0),
        "an anchor outside the viewport must not enter label layout");
}

void TestOverwideLabelsUseTheClippedPlotWidthForCollision()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"wide-a", 50.0f, 200.0f},
        {"wide-b", 50.0f, 160.0f},
    };
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 4.0f);

    Require(
        layout[0].left == 0.0f && layout[1].left == 0.0f,
        "overwide text should use the full clipped plot width");
    Require(
        layout[0].lane != layout[1].lane,
        "overwide labels should collide across the full plot width");
}

void TestInputOrderDoesNotChangeLeftToRightLayout()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"right", 25.0f, 20.0f},
        {"left", 20.0f, 20.0f},
    };
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    Require(layout[1].lane == 0, "the leftmost label should be laid out first");
    Require(layout[0].lane == 1, "placements should map back to the original input order");
}

void TestEarlierVisibleLabelGetsComfortableLaneWhenLeftLabelPansIntoView()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    Require(
        spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f)[0].lane == 0,
        "the first visible label should start in the lowest lane");

    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[1].lane == 0, "the earlier visible label should get the most comfortable lane");
    Require(layout[0].lane == 1, "the label entering later should move to another lane");
}

void TestEarlierVisibleLabelGetsComfortableLaneWhenRightLabelPansIntoView()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {{"a", 20.0f, 20.0f}};
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0, "the earlier visible left label should get the most comfortable lane");
    Require(layout[1].lane == 1, "the later visible right label should move to another lane");
}

void TestRemainingLabelReturnsToItsMostComfortableLane()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    workspace.inputs = {{"b", 25.0f, 20.0f}};
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0,
        "a label should return to the most comfortable lane when the conflict is no longer visible");
}

void TestReturningLabelDoesNotDisplaceContinuouslyVisibleLabel()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    workspace.inputs = {{"b", 25.0f, 20.0f}};
    Require(
        spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f)[0].lane == 0,
        "the continuously visible label should move into the comfortable lane");

    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[1].lane == 0,
        "the continuously visible label should keep priority over a returning label");
    Require(layout[0].lane == 1, "the returning label should avoid the continuously visible label");
}

void TestEmptyFrameEndsEveryVisibilityTenure()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    workspace.inputs.clear();
    Require(
        spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f).empty(),
        "an empty frame should not produce placements");

    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0, "after an empty frame, visible labels should start a new spatial order");
    Require(layout[1].lane == 1, "old appearance priority must not survive an empty frame");
}

void TestSmallPanContextNoisePreservesContinuousVisibilityPriority()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    spectiary::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 100.0, 100.0f, 16.0f});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0005, 104.0f, 16.0f});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 104.0f, 6.0f);
    Require(layout[1].lane == 0, "minor pan context noise should not discard continuous visibility");
    Require(layout[0].lane == 1, "a newly visible label should still avoid the continuous label");
}

void TestActivePanDefersComfortableLaneCompaction()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0 && layout[1].lane == 1, "the initial collision should be staggered");

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, true});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 1, "an active pan should defer compaction after a neighbor leaves");

    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1,
        "panning back during the gesture should restore the prior staggered lanes");

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0, "after pan release, the remaining label should compact");
}

void TestPanReleasePreservesPriorityWhenLabelsReturnDuringTheGesture()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0 && layout[1].lane == 1, "the initial collision should be staggered");

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, true});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1,
        "returning during the gesture should restore the pre-gesture lanes");

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1,
        "release should atomically preserve the priority of labels visible before and after the gesture");
}

void TestLabelFirstSeenDuringPanCommitsAfterPreGestureLabels()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, true});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[1].lane == 0 && layout[0].lane == 1, "a pan entrant should avoid a pre-gesture label");

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[1].lane == 0 && layout[0].lane == 1,
        "release should commit a pan entrant after surviving pre-gesture labels");
}

void TestReleaseFrameEntrantFollowsGestureEntrants()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {{"b", 22.0f, 20.0f}};
    auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0, "the pre-gesture label should start in lane zero");

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, true});
    workspace.inputs = {
        {"b", 22.0f, 20.0f},
        {"c", 24.0f, 20.0f},
    };
    layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1,
        "a gesture entrant should follow the pre-gesture label");

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 22.0f, 20.0f},
        {"c", 24.0f, 20.0f},
    };
    layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[1].lane == 0, "the pre-gesture label should retain first priority");
    Require(layout[2].lane == 1, "the gesture entrant should retain second priority");
    Require(layout[0].lane == 2, "a release-frame entrant should receive later priority");
}

void TestPanTransactionPreservesThreeLabelPriority()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 22.0f, 20.0f},
        {"c", 24.0f, 20.0f},
    };
    auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1 && layout[2].lane == 2,
        "the initial three-label priority should follow spatial order");

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, true});
    workspace.inputs = {{"c", 24.0f, 20.0f}};
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 22.0f, 20.0f},
        {"c", 24.0f, 20.0f},
    };
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1 && layout[2].lane == 2,
        "release should preserve the relative priority of all surviving pre-gesture labels");
}

void TestDenseLabelsReturnToGestureStartLanesBeforeRelease()
{
    constexpr std::array<std::string_view, 5> ids = {
        "c13_c12_6100",
        "c13_c12_6168",
        "feature_6192",
        "c12_cn_6206",
        "c13_cn_6260",
    };
    // The five real wavelengths projected at 0.325 px/Angstrom into a 240 px viewport.
    constexpr std::array<float, 5> anchors = {94.0f, 116.1f, 123.9f, 128.45f, 146.0f};
    constexpr std::array<std::size_t, 5> expected_lanes = {0, 1, 0, 2, 1};
    constexpr float horizontal_gap = 5.6f;

    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    bool observed_temporary_fallback = false;
    const auto set_visible_inputs = [&](float offset) {
        workspace.inputs.clear();
        for (std::size_t index = 0; index < ids.size(); ++index) {
            const float anchor_x = anchors[index] + offset;
            if (anchor_x >= 0.0f && anchor_x <= 240.0f) {
                workspace.inputs.push_back({ids[index], anchor_x, 24.0f});
            }
        }
    };
    const auto require_expected_lanes = [&](std::string_view message) {
        const auto layout =
            spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 240.0f, horizontal_gap);
        Require(layout.size() == expected_lanes.size(), "all dense labels should be visible");
        for (std::size_t index = 0; index < layout.size(); ++index) {
            Require(layout[index].lane == expected_lanes[index], message);
        }
    };
    const auto inspect_active_layout = [&](std::span<const spectiary::SpectralLineLabelLayoutResult> layout) {
        for (std::size_t left = 0; left < layout.size(); ++left) {
            for (std::size_t right = left + 1; right < layout.size(); ++right) {
                if (layout[left].lane != layout[right].lane) {
                    continue;
                }
                const float left_right = layout[left].left + workspace.inputs[left].width;
                const float right_right = layout[right].left + workspace.inputs[right].width;
                const bool separated =
                    left_right + horizontal_gap <= layout[right].left ||
                    right_right + horizontal_gap <= layout[left].left;
                Require(separated, "temporary gesture lanes must still prevent label overlap");
            }

            for (std::size_t id_index = 0; id_index < ids.size(); ++id_index) {
                if (workspace.inputs[left].stable_id == ids[id_index] &&
                    layout[left].lane != expected_lanes[id_index]) {
                    observed_temporary_fallback = true;
                }
            }
        }
    };

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 740.0, 240.0f, 16.0f, false, false});
    set_visible_inputs(0.0f);
    require_expected_lanes("the initial dense layout should use its comfortable lanes");

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 740.0, 240.0f, 16.0f, false, true});
    for (int step = 0; step <= 120; ++step) {
        set_visible_inputs(-1.5f * static_cast<float>(step));
        inspect_active_layout(
            spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 240.0f, horizontal_gap));
    }
    for (int step = 120; step >= 0; --step) {
        set_visible_inputs(-1.5f * static_cast<float>(step));
        inspect_active_layout(
            spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 240.0f, horizontal_gap));
    }
    Require(
        observed_temporary_fallback,
        "the regression path should exercise a necessary temporary collision fallback");
    set_visible_inputs(0.0f);
    require_expected_lanes(
        "returning to the gesture-start viewport should restore its lanes before release");

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 740.0, 240.0f, 16.0f, false, false});
    require_expected_lanes("release should preserve the restored dense layout");
}

void TestPanStartHoldsLanesFromTheLastPreGestureFrame()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 22.0f, 20.0f},
        {"c", 24.0f, 20.0f},
    };
    auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1 && layout[2].lane == 2,
        "the initial three-way collision should occupy three lanes");

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, true});
    workspace.inputs = {{"c", 24.0f, 20.0f}};
    layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 2, "the label still visible on the first pan frame should keep lane two");

    workspace.inputs = {
        {"b", 22.0f, 20.0f},
        {"c", 24.0f, 20.0f},
    };
    layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 1 && layout[1].lane == 2,
        "a label that left on the first pan frame should restore its pre-gesture lane");
}

void TestZoomStartsANewSpatiallyOrderedEpoch()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    spectiary::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 100.0, 100.0f, 16.0f});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    spectiary::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 100.0, 100.0f, 16.0f});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    spectiary::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 80.0, 100.0f, 16.0f});
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0, "after zoom, the leftmost visible label should start the new epoch");
    Require(layout[1].lane == 1, "after zoom, collisions should be rebuilt in spatial order");
}

void TestMaterialWidthChangeUsesTheEpochBaseline()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    spectiary::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 100.0, 100.0f, 16.0f});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    spectiary::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 100.0, 104.0f, 16.0f});
    auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 104.0f, 6.0f);
    Require(layout[1].lane == 0, "a small width change should preserve temporal priority");

    spectiary::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 100.0, 109.0f, 16.0f});
    layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 109.0f, 6.0f);
    Require(layout[0].lane == 0, "a cumulative material width change should start a new epoch");
    Require(layout[1].lane == 1, "the new width epoch should restore spatial ordering");
}

void SeedRightLabelAppearancePriority(
    spectiary::SpectralLineLabelLayoutWorkspace& workspace,
    const spectiary::SpectralLineLabelLayoutContext& context)
{
    spectiary::UpdateSpectralLineLabelLayoutContext(workspace, context);
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[1].lane == 0, "the seeded right label should initially retain priority");
}

void RequireNewEpochRestoresSpatialOrder(
    spectiary::SpectralLineLabelLayoutWorkspace& workspace,
    std::string_view reason)
{
    const auto layout = spectiary::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0, reason);
    Require(layout[1].lane == 1, "a new epoch should place the right collision second");
}

void TestCatalogScopeChangeStartsANewSpatiallyOrderedEpoch()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    SeedRightLabelAppearancePriority(workspace, {"catalog-a", 100.0, 100.0f, 16.0f});

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog-b", 100.0, 100.0f, 16.0f});
    RequireNewEpochRestoresSpatialOrder(workspace, "a catalog change should reset temporal priority");
}

void TestFontChangeStartsANewSpatiallyOrderedEpoch()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    SeedRightLabelAppearancePriority(workspace, {"catalog", 100.0, 100.0f, 16.0f});

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 20.0f});
    RequireNewEpochRestoresSpatialOrder(workspace, "a font or DPI change should reset temporal priority");
}

void TestForceFitStartsANewSpatiallyOrderedEpoch()
{
    spectiary::SpectralLineLabelLayoutWorkspace workspace;
    SeedRightLabelAppearancePriority(workspace, {"catalog", 100.0, 100.0f, 16.0f});

    spectiary::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, true});
    RequireNewEpochRestoresSpatialOrder(workspace, "fit view should reset temporal priority");
}

void TestVerticalPlacementsStayInTheirPlotHalves()
{
    const spectiary::SpectralLineVerticalLayoutContext context{
        .plot_top = 0.0f,
        .plot_bottom = 100.0f,
        .plot_mid_y = 50.0f,
        .top_inset = 0.0f,
        .bottom_inset = 10.0f,
        .lane_height = 15.0f,
    };

    const auto top_lane_zero = spectiary::PlaceSpectralLineNameLabel(context, 10.0f, 0);
    const auto top_lane_two = spectiary::PlaceSpectralLineNameLabel(context, 10.0f, 2);
    const auto top_lane_three = spectiary::PlaceSpectralLineNameLabel(context, 10.0f, 3);
    Require(top_lane_zero.y == 0.0f, "top lane zero should be closest to the plot top");
    Require(top_lane_two.y == 30.0f, "top lanes should grow downward");
    Require(top_lane_two.visible, "a top label fully above the midpoint should remain visible");
    Require(!top_lane_three.visible, "a top label crossing the midpoint should be hidden");

    const auto bottom_lane_zero = spectiary::PlaceSpectralLineWavelengthLabel(context, 10.0f, 0);
    const auto bottom_lane_two = spectiary::PlaceSpectralLineWavelengthLabel(context, 10.0f, 2);
    const auto bottom_lane_three = spectiary::PlaceSpectralLineWavelengthLabel(context, 10.0f, 3);
    Require(bottom_lane_zero.y == 80.0f, "bottom lane zero should be closest to the x axis");
    Require(bottom_lane_two.y == 50.0f, "bottom lanes should grow upward");
    Require(bottom_lane_two.visible, "a bottom label touching the midpoint should remain visible");
    Require(!bottom_lane_three.visible, "a bottom label crossing the midpoint should be hidden");
}

void TestLabelMetricsScaleWithFontSize()
{
    const auto small = spectiary::MakeSpectralLineLabelMetrics(10.0f, 12.0f);
    const auto large = spectiary::MakeSpectralLineLabelMetrics(20.0f, 24.0f);
    const auto twice = [](float first, float second) {
        return std::abs(second - first * 2.0f) < 0.0001f;
    };
    Require(twice(small.horizontal_gap, large.horizontal_gap), "horizontal gap should scale with font size");
    Require(twice(small.lane_gap, large.lane_gap), "lane gap should scale with font size");
    Require(twice(small.edge_padding, large.edge_padding), "edge padding should scale with font size");
    Require(twice(small.top_legend_inset, large.top_legend_inset), "top inset should scale with text size");
}

}  // namespace

int main()
{
    TestBandModeUsesFullProjectedWidthAndTextMetrics();
    TestRenderedBandLabelsFollowViewportAndScale();
    TestSeparatedLabelsShareTheBottomLane();
    TestOverlappingLabelsMoveUpAndReuseAvailableLanes();
    TestEdgeClampingStillParticipatesInCollisionDetection();
    TestOnlyViewportAnchorsAreEligibleForLabelLayout();
    TestOverwideLabelsUseTheClippedPlotWidthForCollision();
    TestInputOrderDoesNotChangeLeftToRightLayout();
    TestEarlierVisibleLabelGetsComfortableLaneWhenLeftLabelPansIntoView();
    TestEarlierVisibleLabelGetsComfortableLaneWhenRightLabelPansIntoView();
    TestRemainingLabelReturnsToItsMostComfortableLane();
    TestReturningLabelDoesNotDisplaceContinuouslyVisibleLabel();
    TestEmptyFrameEndsEveryVisibilityTenure();
    TestSmallPanContextNoisePreservesContinuousVisibilityPriority();
    TestActivePanDefersComfortableLaneCompaction();
    TestPanReleasePreservesPriorityWhenLabelsReturnDuringTheGesture();
    TestLabelFirstSeenDuringPanCommitsAfterPreGestureLabels();
    TestReleaseFrameEntrantFollowsGestureEntrants();
    TestPanTransactionPreservesThreeLabelPriority();
    TestDenseLabelsReturnToGestureStartLanesBeforeRelease();
    TestPanStartHoldsLanesFromTheLastPreGestureFrame();
    TestZoomStartsANewSpatiallyOrderedEpoch();
    TestMaterialWidthChangeUsesTheEpochBaseline();
    TestCatalogScopeChangeStartsANewSpatiallyOrderedEpoch();
    TestFontChangeStartsANewSpatiallyOrderedEpoch();
    TestForceFitStartsANewSpatiallyOrderedEpoch();
    TestVerticalPlacementsStayInTheirPlotHalves();
    TestLabelMetricsScaleWithFontSize();
    return 0;
}
