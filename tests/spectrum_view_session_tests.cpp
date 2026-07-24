#include "ui/spectrum_view_session.h"

#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_activation_transaction.h"
#include "ui/source_collection_load_queue_internal.h"

#include <imgui.h>
#include <implot.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void Require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void RequireNear(double actual, double expected, const std::string& message)
{
    Require(
        std::abs(actual - expected) < 1.0e-9,
        message + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

specforge::SpectrumSnapshotHandle MakeSnapshot(
    std::vector<double> x_values,
    specforge::SpectrumValueVector y_values)
{
    auto snapshot = std::make_shared<specforge::SpectrumSnapshot>();
    snapshot->current_spectrum.name = "test spectrum";
    snapshot->current_spectrum.x_values =
        std::make_shared<const std::vector<double>>(std::move(x_values));
    snapshot->current_spectrum.y_values = std::move(y_values);
    snapshot->current_spectrum.point_count = snapshot->current_spectrum.x_values->size();
    snapshot->axis.x_label = "wavelength";
    snapshot->axis.y_label = "flux";
    snapshot->capabilities.can_plot_current_spectrum = true;
    return snapshot;
}

specforge::SpectrumSnapshotHandle MakeSnapshot(
    std::vector<double> x_values,
    std::vector<double> y_values)
{
    return MakeSnapshot(
        std::move(x_values),
        std::make_shared<const std::vector<double>>(
            std::move(y_values)));
}

std::filesystem::path UniqueTempPath()
{
    static std::atomic_uint64_t next_id = 1;
    return std::filesystem::temp_directory_path() /
        ("specforge_activation_presentation_binding_" +
         std::to_string(next_id.fetch_add(1)) +
         ".csv");
}

specforge::SpectrumSnapshotHandle MakeActivationSnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    const specforge::SpectrumSnapshotHandle source =
        MakeSnapshot(
            {1.0, 2.0, 3.0},
            {3.0, 2.0, 1.0});
    auto snapshot =
        std::make_shared<specforge::SpectrumSnapshot>(
            *source);
    snapshot->source.id = "binding-fixture";
    snapshot->source.display_name = "binding-fixture";
    snapshot->source.path = path;
    snapshot->collection.spectrum_count = 1;
    snapshot->collection.current_index =
        spectrum_index;
    snapshot->capabilities.can_switch_spectrum = false;
    return snapshot;
}

class ScopedPlotUi {
public:
    ScopedPlotUi()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();
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

    ~ScopedPlotUi()
    {
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
    }

    ScopedPlotUi(const ScopedPlotUi&) = delete;
    ScopedPlotUi& operator=(const ScopedPlotUi&) = delete;

    specforge::SpectrumViewRenderFeedback RenderFrame(
        specforge::SpectrumViewSession& session,
        const specforge::SpectrumSnapshotHandle& snapshot,
        ImVec2 mouse_position = ImVec2(400.0f, 300.0f),
        bool left_button_down = false)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        io.AddMousePosEvent(mouse_position.x, mouse_position.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, left_button_down);
        ImGui::NewFrame();

        constexpr ImGuiWindowFlags kWindowFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(800.0f, 600.0f), ImGuiCond_Always);
        Require(
            ImGui::Begin("Spectrum view session test", nullptr, kWindowFlags),
            "test plot window should be visible");
        const specforge::SpectrumViewRenderFeedback feedback = session.Render(snapshot);
        ImGui::End();
        ImGui::EndFrame();
        return feedback;
    }
};

void ConfigureGaussianSmoothing(specforge::SpectrumViewSession& session)
{
    session.Submit(specforge::SpectrumViewSessionCommand::SetShowPoints(true));
    session.Submit(specforge::SpectrumViewSessionCommand::SetShowSmoothed(true));
    session.Submit(specforge::SpectrumViewSessionCommand::SetShowRawWhenSmoothed(false));
    session.Submit(
        specforge::SpectrumViewSessionCommand::SetSmoothingMethod(
            specforge::SpectrumSmoothingMethod::Gaussian));
    session.Submit(specforge::SpectrumViewSessionCommand::SetGaussianSigma(3.25));
}

void TestSnapshotResetPreservesControlsAndFitsNewData()
{
    ScopedPlotUi ui;
    specforge::SpectrumViewSession session;
    ConfigureGaussianSmoothing(session);

    const specforge::SpectrumViewRenderFeedback first =
        ui.RenderFrame(session, MakeSnapshot({1.0, 2.0, 3.0}, {2.0, 4.0, 3.0}));
    Require(
        first.plot_submitted && first.fit_applied && first.visible_limits,
        "first render should submit and fit the spectrum");
    std::vector<specforge::SpectrumValueVector> retained =
        session.RetainHeavySnapshotResources();
    Require(
        retained.size() == 2,
        "smoothed render should expose both heavy cache resources for retention");

    session.Submit(specforge::SpectrumViewSessionCommand::ResetForSnapshotChange());

    const specforge::SpectrumViewSessionView view = session.View();
    Require(view.show_points, "snapshot reset should preserve show-points state");
    Require(view.show_smoothed, "snapshot reset should preserve smoothing visibility");
    Require(!view.show_raw_when_smoothed, "snapshot reset should preserve raw-overlay visibility");
    Require(
        view.smoothing.method == specforge::SpectrumSmoothingMethod::Gaussian,
        "snapshot reset should preserve smoothing method");
    RequireNear(view.smoothing.gaussian_sigma, 3.25, "snapshot reset should preserve gaussian sigma");
    Require(!session.PlotPanActive(), "snapshot reset should clear plot interaction feedback");
    Require(
        session.RetainHeavySnapshotResources().empty(),
        "snapshot reset should release current smoothing caches");
    Require(retained[0] && retained[1], "retained handles should keep retired resources alive");

    const specforge::SpectrumViewRenderFeedback second =
        ui.RenderFrame(session, MakeSnapshot({100.0, 200.0, 300.0}, {5.0, 6.0, 7.0}));
    Require(
        second.plot_submitted && second.fit_applied && second.visible_limits,
        "the first render after a snapshot reset should fit new data");
    Require(
        second.visible_limits->x_min > first.visible_limits->x_max,
        "snapshot reset should discard old plot limits");
}

void TestFitAndStoredLimitReuseAreObservable()
{
    ScopedPlotUi ui;
    specforge::SpectrumViewSession session;
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot({4100.0, 4500.0, 4900.0}, {-0.25, 1.75, 0.5});

    session.Submit(specforge::SpectrumViewSessionCommand::SyncPlotLimitsOnNextRender());
    const specforge::SpectrumViewRenderFeedback first = ui.RenderFrame(session, snapshot);
    Require(
        first.fit_applied && !first.stored_limits_reused && first.visible_limits,
        "limit sync before the first render should remain a no-op");

    session.Submit(specforge::SpectrumViewSessionCommand::SyncPlotLimitsOnNextRender());
    const specforge::SpectrumViewRenderFeedback reused = ui.RenderFrame(session, snapshot);
    Require(
        reused.stored_limits_reused && !reused.fit_applied && reused.visible_limits,
        "mode-switch sync should reuse stored limits");
    RequireNear(
        reused.visible_limits->x_min,
        first.visible_limits->x_min,
        "stored limit sync should preserve x min");
    RequireNear(
        reused.visible_limits->x_max,
        first.visible_limits->x_max,
        "stored limit sync should preserve x max");
    RequireNear(
        reused.visible_limits->y_min,
        first.visible_limits->y_min,
        "stored limit sync should preserve y min");
    RequireNear(
        reused.visible_limits->y_max,
        first.visible_limits->y_max,
        "stored limit sync should preserve y max");

    session.Submit(specforge::SpectrumViewSessionCommand::RequestFitView());
    const specforge::SpectrumViewRenderFeedback fitted = ui.RenderFrame(session, snapshot);
    Require(
        fitted.fit_applied && !fitted.stored_limits_reused,
        "fit command should supersede stored-limit reuse");
}

void TestSmoothingCommandsOwnCacheInvalidation()
{
    ScopedPlotUi ui;
    specforge::SpectrumViewSession session;
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot({1.0, 2.0, 3.0, 4.0, 5.0}, {2.0, 4.0, 3.0, 5.0, 1.0});
    session.Submit(specforge::SpectrumViewSessionCommand::SetShowSmoothed(true));
    session.Submit(
        specforge::SpectrumViewSessionCommand::SetSmoothingMethod(
            specforge::SpectrumSmoothingMethod::Median));
    session.Submit(specforge::SpectrumViewSessionCommand::SetMedianKernelSize(4));

    Require(
        session.View().smoothing.median_kernel_size == 5,
        "median kernel command should normalize to an odd size");
    Require(
        session.EffectiveMedianKernelSize(4) == 3,
        "effective median kernel should clamp to data length");
    Require(ui.RenderFrame(session, snapshot).plot_submitted, "smoothed spectrum should render");
    Require(
        session.RetainHeavySnapshotResources().size() == 2,
        "render should populate smoothing resources");

    session.Submit(specforge::SpectrumViewSessionCommand::SetMedianKernelSize(9));
    Require(
        session.RetainHeavySnapshotResources().empty(),
        "changing the median kernel should clear smoothing resources");
    Require(
        session.View().smoothing.median_kernel_size == 9,
        "normalized median kernel should be observable");
    Require(ui.RenderFrame(session, snapshot).plot_submitted, "smoothing should rebuild");
    Require(
        session.RetainHeavySnapshotResources().size() == 2,
        "render should rebuild smoothing resources");

    session.Submit(specforge::SpectrumViewSessionCommand::ResetSmoothing());
    const specforge::SpectrumViewSessionView reset = session.View();
    Require(
        !reset.show_smoothed && reset.show_raw_when_smoothed &&
            reset.smoothing.method == specforge::SpectrumSmoothingMethod::None,
        "reset smoothing should restore display defaults");
    Require(
        session.RetainHeavySnapshotResources().empty(),
        "reset smoothing should clear smoothing resources");
}

void TestRenderFeedbackTracksPanLifecycle()
{
    ScopedPlotUi ui;
    specforge::SpectrumViewSession session;
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot({1.0, 2.0, 3.0}, {2.0, 4.0, 3.0});

    Require(!ui.RenderFrame(session, snapshot).pan_active, "plot pan should be inactive initially");
    Require(
        !ui.RenderFrame(session, snapshot, ImVec2(400.0f, 300.0f), true).pan_active,
        "mouse press without movement should not start a pan");
    const specforge::SpectrumViewRenderFeedback dragging =
        ui.RenderFrame(session, snapshot, ImVec2(430.0f, 300.0f), true);
    Require(
        dragging.pan_active && session.PlotPanActive(),
        "render feedback should expose an active plot pan");

    const specforge::SpectrumViewRenderFeedback released =
        ui.RenderFrame(session, snapshot, ImVec2(430.0f, 300.0f), false);
    Require(
        !released.pan_active && !session.PlotPanActive(),
        "mouse release should clear plot pan feedback");
}

void TestActivationPresentationBindingResetsAndRetiresHeavyViewResources()
{
    const std::filesystem::path path =
        UniqueTempPath();
    {
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::trunc);
        Require(
            stream.good(),
            "activation binding fixture should be created");
        stream << "fixture";
    }

    ScopedPlotUi ui;
    specforge::SourceCollectionSession source_session(
        {},
        {},
        {},
        {});
    specforge::SpectrumViewSession presentation;
    ConfigureGaussianSmoothing(presentation);

    auto destroyed_promise =
        std::make_shared<
            std::promise<std::thread::id>>();
    std::future<std::thread::id> destroyed =
        destroyed_promise->get_future();
    specforge::SpectrumValueVector old_y_values(
        new const std::vector<double>{
            2.0,
            4.0,
            3.0},
        [destroyed_promise](
            const std::vector<double>* values) {
            delete values;
            destroyed_promise->set_value(
                std::this_thread::get_id());
        });
    specforge::SpectrumSnapshotHandle old_snapshot =
        MakeSnapshot(
            {1.0, 2.0, 3.0},
            old_y_values);
    Require(
        ui.RenderFrame(
              presentation,
              old_snapshot)
            .plot_submitted,
        "old snapshot should populate the presentation cache");
    Require(
        presentation.
                RetainHeavySnapshotResources()
            .size() == 2,
        "binding regression requires both heavy smoothing resources");
    old_snapshot.reset();
    old_y_values.reset();

    specforge::SourceCollectionPreparationAdapters
        dependencies;
    dependencies.snapshot_loader =
        [](const std::filesystem::path& source,
           std::size_t index,
           const auto&) {
            return MakeActivationSnapshot(
                source,
                index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::
                SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    specforge::SourceCollectionActivationTransaction
        activation(
            source_session,
            specforge::
                MakeSourceCollectionLoadQueueForTesting(
                    std::move(dependencies)));
    specforge::
        BindSourceCollectionActivationPresentationLifecycle(
            activation,
            presentation);

    const std::thread::id caller_thread =
        std::this_thread::get_id();
    (void)activation.OpenSource(path, 0);
    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    bool activated = false;
    while (std::chrono::steady_clock::now() <
           deadline) {
        (void)activation.Drain(false);
        const specforge::SpectrumSnapshotHandle current =
            source_session.CurrentSampleSnapshot();
        activated =
            current && current->source.path == path &&
            !activation.status().loading;
        if (activated) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const bool retired =
        destroyed.wait_for(2s) ==
        std::future_status::ready;
    const std::thread::id retirement_thread =
        retired ? destroyed.get() : std::thread::id{};

    std::filesystem::remove(path);
    Require(
        activated,
        "prepared source activation should commit through the bound transaction");
    Require(
        presentation.
            RetainHeavySnapshotResources()
            .empty(),
        "snapshot activation should reset the bound presentation view");
    Require(
        retired,
        "old presentation resources should reach the background reclaimer");
    Require(
        retirement_thread != caller_thread,
        "old presentation resources must not be destroyed on the activation caller");
}

}  // namespace

int main()
{
    TestSnapshotResetPreservesControlsAndFitsNewData();
    TestFitAndStoredLimitReuseAreObservable();
    TestSmoothingCommandsOwnCacheInvalidation();
    TestRenderFeedbackTracksPanLifecycle();
    TestActivationPresentationBindingResetsAndRetiresHeavyViewResources();
    return 0;
}
