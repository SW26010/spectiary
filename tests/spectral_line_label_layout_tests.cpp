#include "plot/spectral_line_label_layout.h"

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

void TestSeparatedLabelsShareTheBottomLane()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"a", 20.0f, 16.0f},
        {"b", 70.0f, 20.0f},
    };
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    Require(layout.size() == 2, "every label should receive a placement");
    Require(layout[0].lane == 0 && layout[1].lane == 0, "separated labels should use the lowest lane");
}

void TestOverlappingLabelsMoveUpAndReuseAvailableLanes()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
        {"c", 55.0f, 20.0f},
    };
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    Require(layout[0].lane == 0, "the first label should use the bottom lane");
    Require(layout[1].lane == 1, "an overlapping label should move up one lane");
    Require(layout[2].lane == 0, "a later label should reuse the bottom lane when it fits");
}

void TestEdgeClampingStillParticipatesInCollisionDetection()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"left-a", 0.0f, 30.0f},
        {"left-b", 5.0f, 20.0f},
        {"right", 100.0f, 30.0f},
    };
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 4.0f);

    Require(layout[0].left == 0.0f, "a left-edge label should remain inside the plot");
    Require(layout[1].left == 0.0f, "a second left-edge label should also be clamped");
    Require(layout[0].lane != layout[1].lane, "clamped overlapping labels should use different lanes");
    Require(layout[2].left == 70.0f, "a right-edge label should remain inside the plot");
}

void TestOnlyViewportAnchorsAreEligibleForLabelLayout()
{
    Require(
        specforge::IsSpectralLineLabelAnchorInViewport(50.0, 0.0, 100.0),
        "an anchor inside the viewport should be eligible");
    Require(
        specforge::IsSpectralLineLabelAnchorInViewport(0.0, 0.0, 100.0) &&
            specforge::IsSpectralLineLabelAnchorInViewport(100.0, 0.0, 100.0),
        "anchors on viewport edges should remain eligible");
    Require(
        !specforge::IsSpectralLineLabelAnchorInViewport(-0.01, 0.0, 100.0) &&
            !specforge::IsSpectralLineLabelAnchorInViewport(100.01, 0.0, 100.0),
        "an anchor outside the viewport must not enter label layout");
}

void TestOverwideLabelsUseTheClippedPlotWidthForCollision()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"wide-a", 50.0f, 200.0f},
        {"wide-b", 50.0f, 160.0f},
    };
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 4.0f);

    Require(
        layout[0].left == 0.0f && layout[1].left == 0.0f,
        "overwide text should use the full clipped plot width");
    Require(
        layout[0].lane != layout[1].lane,
        "overwide labels should collide across the full plot width");
}

void TestInputOrderDoesNotChangeLeftToRightLayout()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"right", 25.0f, 20.0f},
        {"left", 20.0f, 20.0f},
    };
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    Require(layout[1].lane == 0, "the leftmost label should be laid out first");
    Require(layout[0].lane == 1, "placements should map back to the original input order");
}

void TestEarlierVisibleLabelGetsComfortableLaneWhenLeftLabelPansIntoView()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    Require(
        specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f)[0].lane == 0,
        "the first visible label should start in the lowest lane");

    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[1].lane == 0, "the earlier visible label should get the most comfortable lane");
    Require(layout[0].lane == 1, "the label entering later should move to another lane");
}

void TestEarlierVisibleLabelGetsComfortableLaneWhenRightLabelPansIntoView()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {{"a", 20.0f, 20.0f}};
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0, "the earlier visible left label should get the most comfortable lane");
    Require(layout[1].lane == 1, "the later visible right label should move to another lane");
}

void TestRemainingLabelReturnsToItsMostComfortableLane()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    workspace.inputs = {{"b", 25.0f, 20.0f}};
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0,
        "a label should return to the most comfortable lane when the conflict is no longer visible");
}

void TestReturningLabelDoesNotDisplaceContinuouslyVisibleLabel()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    workspace.inputs = {{"b", 25.0f, 20.0f}};
    Require(
        specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f)[0].lane == 0,
        "the continuously visible label should move into the comfortable lane");

    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[1].lane == 0,
        "the continuously visible label should keep priority over a returning label");
    Require(layout[0].lane == 1, "the returning label should avoid the continuously visible label");
}

void TestEmptyFrameEndsEveryVisibilityTenure()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    workspace.inputs.clear();
    Require(
        specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f).empty(),
        "an empty frame should not produce placements");

    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0, "after an empty frame, visible labels should start a new spatial order");
    Require(layout[1].lane == 1, "old appearance priority must not survive an empty frame");
}

void TestSmallPanContextNoisePreservesContinuousVisibilityPriority()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    specforge::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 100.0, 100.0f, 16.0f});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0005, 104.0f, 16.0f});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 104.0f, 6.0f);
    Require(layout[1].lane == 0, "minor pan context noise should not discard continuous visibility");
    Require(layout[0].lane == 1, "a newly visible label should still avoid the continuous label");
}

void TestActivePanDefersComfortableLaneCompaction()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0 && layout[1].lane == 1, "the initial collision should be staggered");

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, true});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 1, "an active pan should defer compaction after a neighbor leaves");

    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1,
        "panning back during the gesture should restore the prior staggered lanes");

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0, "after pan release, the remaining label should compact");
}

void TestPanReleasePreservesPriorityWhenLabelsReturnDuringTheGesture()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0 && layout[1].lane == 1, "the initial collision should be staggered");

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, true});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1,
        "returning during the gesture should restore the pre-gesture lanes");

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1,
        "release should atomically preserve the priority of labels visible before and after the gesture");
}

void TestLabelFirstSeenDuringPanCommitsAfterPreGestureLabels()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, true});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[1].lane == 0 && layout[0].lane == 1, "a pan entrant should avoid a pre-gesture label");

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[1].lane == 0 && layout[0].lane == 1,
        "release should commit a pan entrant after surviving pre-gesture labels");
}

void TestReleaseFrameEntrantFollowsGestureEntrants()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {{"b", 22.0f, 20.0f}};
    auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0, "the pre-gesture label should start in lane zero");

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, true});
    workspace.inputs = {
        {"b", 22.0f, 20.0f},
        {"c", 24.0f, 20.0f},
    };
    layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1,
        "a gesture entrant should follow the pre-gesture label");

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 22.0f, 20.0f},
        {"c", 24.0f, 20.0f},
    };
    layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[1].lane == 0, "the pre-gesture label should retain first priority");
    Require(layout[2].lane == 1, "the gesture entrant should retain second priority");
    Require(layout[0].lane == 2, "a release-frame entrant should receive later priority");
}

void TestPanTransactionPreservesThreeLabelPriority()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 22.0f, 20.0f},
        {"c", 24.0f, 20.0f},
    };
    auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1 && layout[2].lane == 2,
        "the initial three-label priority should follow spatial order");

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, true});
    workspace.inputs = {{"c", 24.0f, 20.0f}};
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 22.0f, 20.0f},
        {"c", 24.0f, 20.0f},
    };
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
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

    specforge::SpectralLineLabelLayoutWorkspace workspace;
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
            specforge::LayoutSpectralLineLabels(workspace, 0.0f, 240.0f, horizontal_gap);
        Require(layout.size() == expected_lanes.size(), "all dense labels should be visible");
        for (std::size_t index = 0; index < layout.size(); ++index) {
            Require(layout[index].lane == expected_lanes[index], message);
        }
    };
    const auto inspect_active_layout = [&](std::span<const specforge::SpectralLineLabelLayoutResult> layout) {
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

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 740.0, 240.0f, 16.0f, false, false});
    set_visible_inputs(0.0f);
    require_expected_lanes("the initial dense layout should use its comfortable lanes");

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 740.0, 240.0f, 16.0f, false, true});
    for (int step = 0; step <= 120; ++step) {
        set_visible_inputs(-1.5f * static_cast<float>(step));
        inspect_active_layout(
            specforge::LayoutSpectralLineLabels(workspace, 0.0f, 240.0f, horizontal_gap));
    }
    for (int step = 120; step >= 0; --step) {
        set_visible_inputs(-1.5f * static_cast<float>(step));
        inspect_active_layout(
            specforge::LayoutSpectralLineLabels(workspace, 0.0f, 240.0f, horizontal_gap));
    }
    Require(
        observed_temporary_fallback,
        "the regression path should exercise a necessary temporary collision fallback");
    set_visible_inputs(0.0f);
    require_expected_lanes(
        "returning to the gesture-start viewport should restore its lanes before release");

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 740.0, 240.0f, 16.0f, false, false});
    require_expected_lanes("release should preserve the restored dense layout");
}

void TestPanStartHoldsLanesFromTheLastPreGestureFrame()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, false});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 22.0f, 20.0f},
        {"c", 24.0f, 20.0f},
    };
    auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 0 && layout[1].lane == 1 && layout[2].lane == 2,
        "the initial three-way collision should occupy three lanes");

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, false, true});
    workspace.inputs = {{"c", 24.0f, 20.0f}};
    layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 2, "the label still visible on the first pan frame should keep lane two");

    workspace.inputs = {
        {"b", 22.0f, 20.0f},
        {"c", 24.0f, 20.0f},
    };
    layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(
        layout[0].lane == 1 && layout[1].lane == 2,
        "a label that left on the first pan frame should restore its pre-gesture lane");
}

void TestZoomStartsANewSpatiallyOrderedEpoch()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    specforge::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 100.0, 100.0f, 16.0f});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    specforge::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 100.0, 100.0f, 16.0f});
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    specforge::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 80.0, 100.0f, 16.0f});
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0, "after zoom, the leftmost visible label should start the new epoch");
    Require(layout[1].lane == 1, "after zoom, collisions should be rebuilt in spatial order");
}

void TestMaterialWidthChangeUsesTheEpochBaseline()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    specforge::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 100.0, 100.0f, 16.0f});
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);

    specforge::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 100.0, 104.0f, 16.0f});
    auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 104.0f, 6.0f);
    Require(layout[1].lane == 0, "a small width change should preserve temporal priority");

    specforge::UpdateSpectralLineLabelLayoutContext(workspace, {"catalog", 100.0, 109.0f, 16.0f});
    layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 109.0f, 6.0f);
    Require(layout[0].lane == 0, "a cumulative material width change should start a new epoch");
    Require(layout[1].lane == 1, "the new width epoch should restore spatial ordering");
}

void SeedRightLabelAppearancePriority(
    specforge::SpectralLineLabelLayoutWorkspace& workspace,
    const specforge::SpectralLineLabelLayoutContext& context)
{
    specforge::UpdateSpectralLineLabelLayoutContext(workspace, context);
    workspace.inputs = {{"b", 25.0f, 20.0f}};
    (void)specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    workspace.inputs = {
        {"a", 20.0f, 20.0f},
        {"b", 25.0f, 20.0f},
    };
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[1].lane == 0, "the seeded right label should initially retain priority");
}

void RequireNewEpochRestoresSpatialOrder(
    specforge::SpectralLineLabelLayoutWorkspace& workspace,
    std::string_view reason)
{
    const auto layout = specforge::LayoutSpectralLineLabels(workspace, 0.0f, 100.0f, 6.0f);
    Require(layout[0].lane == 0, reason);
    Require(layout[1].lane == 1, "a new epoch should place the right collision second");
}

void TestCatalogScopeChangeStartsANewSpatiallyOrderedEpoch()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    SeedRightLabelAppearancePriority(workspace, {"catalog-a", 100.0, 100.0f, 16.0f});

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog-b", 100.0, 100.0f, 16.0f});
    RequireNewEpochRestoresSpatialOrder(workspace, "a catalog change should reset temporal priority");
}

void TestFontChangeStartsANewSpatiallyOrderedEpoch()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    SeedRightLabelAppearancePriority(workspace, {"catalog", 100.0, 100.0f, 16.0f});

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 20.0f});
    RequireNewEpochRestoresSpatialOrder(workspace, "a font or DPI change should reset temporal priority");
}

void TestForceFitStartsANewSpatiallyOrderedEpoch()
{
    specforge::SpectralLineLabelLayoutWorkspace workspace;
    SeedRightLabelAppearancePriority(workspace, {"catalog", 100.0, 100.0f, 16.0f});

    specforge::UpdateSpectralLineLabelLayoutContext(
        workspace,
        {"catalog", 100.0, 100.0f, 16.0f, true});
    RequireNewEpochRestoresSpatialOrder(workspace, "fit view should reset temporal priority");
}

void TestVerticalPlacementsStayInTheirPlotHalves()
{
    const specforge::SpectralLineVerticalLayoutContext context{
        .plot_top = 0.0f,
        .plot_bottom = 100.0f,
        .plot_mid_y = 50.0f,
        .top_inset = 0.0f,
        .bottom_inset = 10.0f,
        .lane_height = 15.0f,
    };

    const auto top_lane_zero = specforge::PlaceSpectralLineNameLabel(context, 10.0f, 0);
    const auto top_lane_two = specforge::PlaceSpectralLineNameLabel(context, 10.0f, 2);
    const auto top_lane_three = specforge::PlaceSpectralLineNameLabel(context, 10.0f, 3);
    Require(top_lane_zero.y == 0.0f, "top lane zero should be closest to the plot top");
    Require(top_lane_two.y == 30.0f, "top lanes should grow downward");
    Require(top_lane_two.visible, "a top label fully above the midpoint should remain visible");
    Require(!top_lane_three.visible, "a top label crossing the midpoint should be hidden");

    const auto bottom_lane_zero = specforge::PlaceSpectralLineWavelengthLabel(context, 10.0f, 0);
    const auto bottom_lane_two = specforge::PlaceSpectralLineWavelengthLabel(context, 10.0f, 2);
    const auto bottom_lane_three = specforge::PlaceSpectralLineWavelengthLabel(context, 10.0f, 3);
    Require(bottom_lane_zero.y == 80.0f, "bottom lane zero should be closest to the x axis");
    Require(bottom_lane_two.y == 50.0f, "bottom lanes should grow upward");
    Require(bottom_lane_two.visible, "a bottom label touching the midpoint should remain visible");
    Require(!bottom_lane_three.visible, "a bottom label crossing the midpoint should be hidden");
}

void TestLabelMetricsScaleWithFontSize()
{
    const auto small = specforge::MakeSpectralLineLabelMetrics(10.0f, 12.0f);
    const auto large = specforge::MakeSpectralLineLabelMetrics(20.0f, 24.0f);
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
