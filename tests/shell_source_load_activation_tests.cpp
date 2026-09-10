#include "legacy_annotation_fixture_io.h"
#include "legacy_labeling_test_support.h"
#include "ui/shell_ui.h"

#include "app/runtime_paths.h"
#include "domain/sample_annotation_io.h"
#include "domain/sample_labeling.h"
#include "domain/sample_labeling_source_compatibility.h"
#include "domain/source_path_identity.h"
#include "ui/sample_labeling_controller.h"
#include "ui/sample_workflow_preparation.h"
#include "ui/source_collection_load_queue_internal.h"
#include "ui/source_collection_session_state_cache_io.h"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace specforge {

struct ShellUiTestAccess {
    static std::unique_ptr<ShellUi> Create(
        SourceCollectionSession session,
        SourceCollectionLoadQueue source_load_queue)
    {
        return std::unique_ptr<ShellUi>(
            new ShellUi(std::move(session), std::move(source_load_queue)));
    }

    static SourceCollectionSession& Session(ShellUi& shell)
    {
        return shell.session_;
    }

    static LocalUserStateHealthView PersistenceHealth(ShellUi& shell)
    {
        return shell.PersistenceHealth();
    }

    static CatalogUserStateResult SubmitSpectralLines(
        ShellUi& shell,
        CatalogUserStateIntent intent)
    {
        return shell.spectral_lines_panel_.Submit(std::move(intent));
    }

    static SourceCollectionSessionResult Submit(
        ShellUi& shell,
        SourceCollectionSessionIntent intent)
    {
        return shell.SubmitSessionCommand(std::move(intent));
    }

    static SourceCollectionSessionResult SubmitThroughPanel(
        ShellUi& shell,
        SourceCollectionSessionIntent intent)
    {
        PanelSessionInteraction::Update update =
            shell.panel_session_interaction_.Submit(
                std::move(intent));
        return std::move(update.result);
    }

    static SourceCollectionSessionAction TakePanelAction(ShellUi& shell)
    {
        return shell.panel_session_interaction_.TakeAction();
    }

    static void SetLabelEditingState(ShellUi& shell)
    {
        shell.sample_workflow_panel_ui_.editing_label_code_ = 8;
        shell.sample_workflow_panel_ui_.label_shortcut_capture_active_ = true;
    }

    [[nodiscard]] static bool HasLabelEditingState(const ShellUi& shell)
    {
        return shell.sample_workflow_panel_ui_.editing_label_code_.has_value() &&
               shell.sample_workflow_panel_ui_.label_shortcut_capture_active_;
    }

    static SourceCollectionSessionResult SubmitNavigation(
        ShellUi& shell,
        SourceCollectionSessionIntent intent,
        NavigationLatencyInputKind kind,
        std::optional<NavigationLatencyTimePoint> input_at = std::nullopt)
    {
        return shell.SubmitSessionCommand(
            std::move(intent),
            SourceCollectionActivationTransaction::
                NavigationIntent{kind, input_at});
    }

    static ApplicationSettingsResult
    ApplySettingsUiIntent(
        ShellUi& shell,
        ApplicationSettingsIntent intent)
    {
        return shell.ApplyApplicationSettingsIntent(
            std::move(intent),
            {});
    }

    static void RenderMainMenuBar(
        ShellUi& shell,
        const ShellStatus& status,
        const SourceCollectionPathPicker& choose_source_file)
    {
        shell.RenderMainMenuBar(
            status,
            choose_source_file);
    }

    static void Drain(
        ShellUi& shell,
        bool allow_snapshot_prefetch = true)
    {
        shell.DrainSourceLoads(allow_snapshot_prefetch);
    }

    static void BeginDeferredRestore(ShellUi& shell)
    {
        shell.BeginDeferredSourceRestore();
    }

    static void SeedStartupSpectrumViewState(
        ShellUi& shell,
        SpectrumViewStateCache state)
    {
        shell.startup_spectrum_view_state_ =
            std::move(state);
        shell.startup_spectrum_view_mutation_revision_ =
            shell.spectrum_view_session_.
                ViewportMutationRevision();
    }

    static void RequestSpectrumViewportFit(
        ShellUi& shell)
    {
        shell.spectrum_view_session_.Submit(
            SpectrumViewSessionCommand::RequestFitView());
    }

    static SpectrumViewSessionView SpectrumView(
        const ShellUi& shell)
    {
        return shell.spectrum_view_session_.View();
    }

    static void SetSpectrumSeriesColor(
        ShellUi& shell,
        SpectrumPlotSeries series,
        PlotSeriesColor color)
    {
        shell.spectrum_view_session_.Submit(
            SpectrumViewSessionCommand::
                SetPlotSeriesColor(
                    series,
                    std::move(color)));
    }

    static void MarkSpectrumViewStateDirtyAt(
        ShellUi& shell,
        LocalUserStateSaveScheduler::TimePoint now)
    {
        shell.spectrum_view_state_persistence_.
            MarkDirtyAt(now);
    }

    static std::optional<PlotViewLimits>
    LockedViewportLimits(const ShellUi& shell)
    {
        return shell.spectrum_view_session_.
            LockedViewportLimits();
    }

    static void ConfigureSpectrumViewPersistence(
        ShellUi& shell,
        std::filesystem::path path)
    {
        shell.spectrum_view_state_path_ =
            std::move(path);
        shell.persist_local_state_ = true;
        shell.local_state_flush_result_.reset();
    }

    static bool RestoreLockedViewport(
        ShellUi& shell,
        const PlotViewLimits& limits)
    {
        return shell.spectrum_view_session_.
            RestoreLockedViewport(limits);
    }

    static void SyncNavigationInputs(ShellUi& shell)
    {
        shell.HandleSessionAction(
            SourceCollectionSessionAction{
                .navigation_inputs_changed = true});
    }

    static std::optional<std::uint64_t>
    SynchronizedNavigationTopologyRevision(
        const ShellUi& shell)
    {
        return shell.source_collection_panel_ui_.
            synchronized_navigation_topology_revision_;
    }

    static void SeedAnnotationDiagnosticDismissal(
        ShellUi& shell,
        std::string source_identity)
    {
        shell.source_collection_panel_ui_.
            annotation_diagnostic_source_identity_ =
                std::move(source_identity);
        shell.source_collection_panel_ui_.
            dismissed_annotation_diagnostic_keys_.insert(
                "dismissed-diagnostic");
    }

    [[nodiscard]] static std::size_t
    AnnotationDiagnosticDismissalCount(
        const ShellUi& shell)
    {
        return shell.source_collection_panel_ui_.
            dismissed_annotation_diagnostic_keys_.size();
    }

    [[nodiscard]] static std::string_view
    AnnotationDiagnosticSourceIdentity(
        const ShellUi& shell)
    {
        return shell.source_collection_panel_ui_.
            annotation_diagnostic_source_identity_;
    }

    static void CaptureLabelingOperationResult(
        ShellUi& shell,
        const SourceCollectionSessionResult& result)
    {
        shell.sample_workflow_panel_ui_.
            CaptureLabelingOperationResult(
                result,
                UiLanguage::English);
    }

    [[nodiscard]] static std::string_view
    LabelingOperationMessage(const ShellUi& shell)
    {
        return shell.sample_workflow_panel_ui_.
            labeling_operation_message_;
    }

    static void HandleSessionAction(
        ShellUi& shell,
        const SourceCollectionSessionAction& action)
    {
        shell.HandleSessionAction(action);
    }

    static std::size_t PendingLoadCount(const ShellUi& shell)
    {
        return shell.source_activation_.
            PendingLoadCount();
    }

    static std::size_t PendingLoadCount(
        const SourceCollectionActivationTransaction&
            activation)
    {
        return activation.PendingLoadCount();
    }

    static void RegisterCompletionReadyCallback(
        SourceCollectionActivationTransaction& activation,
        SourceCollectionLoadQueue::
            CompletionReadyCallback callback)
    {
        activation.RegisterCompletionReadyCallback(
            std::move(callback));
    }

    static bool PrefetchActive(const ShellUi& shell)
    {
        return shell.source_activation_.PrefetchActive();
    }

    static std::vector<NavigationPrefetchReport>
    TakePrefetchReports(ShellUi& shell)
    {
        return shell.source_activation_.
            TakeNavigationPrefetchReports();
    }

    static std::string_view LoadError(const ShellUi& shell)
    {
        return shell.source_activation_.ErrorMessage();
    }

    static void EnableNavigationTracing(ShellUi& shell, std::uint64_t frame_index)
    {
        shell.source_activation_.BeginFrame(
            true,
            frame_index,
            nullptr);
    }

    static std::vector<NavigationLatencyReport> CompleteFramePresentation(
        ShellUi& shell,
        std::uint64_t frame_index,
        unsigned int viewport_id = 7)
    {
        SubmitSpectrumDraw(shell, frame_index, shell.session_.CurrentSampleSnapshot());
        const NavigationLatencyPresentation presentation{
            viewport_id,
            NavigationLatencyTrace::Now()};
        return shell.source_activation_.
            CompleteNavigationFramePresentations(
                frame_index,
                std::span(&presentation, 1));
    }

    static void SubmitSpectrumDraw(
        ShellUi& shell,
        std::uint64_t frame_index,
        SpectrumSnapshotHandle snapshot,
        unsigned int viewport_id = 7)
    {
        shell.RecordSpectrumDrawSubmission(
            frame_index,
            viewport_id,
            std::move(snapshot));
    }

    static void SubmitPanelVisibilityDraw(
        ShellUi& shell,
        std::uint64_t frame_index,
        unsigned int viewport_id,
        PanelVisibilityState visibility)
    {
        shell.RecordPanelVisibilityDrawSubmission(
            frame_index,
            viewport_id,
            visibility);
    }

    static void SubmitPanelDraw(
        ShellUi& shell,
        ApplicationPanel panel,
        unsigned int viewport_id)
    {
        shell.RecordPanelDrawSubmission(
            panel,
            viewport_id);
    }

    static std::vector<NavigationLatencyReport> CompleteFramePresentationWithoutSpectrumDraw(
        ShellUi& shell,
        std::uint64_t frame_index,
        unsigned int viewport_id = 7)
    {
        const NavigationLatencyPresentation presentation{
            viewport_id,
            NavigationLatencyTrace::Now()};
        return shell.source_activation_.
            CompleteNavigationFramePresentations(
                frame_index,
                std::span(&presentation, 1));
    }

    static std::vector<SourceLoadLatencyReport>
    CompleteSourceLoadFramePresentation(
        ShellUi& shell,
        std::uint64_t frame_index,
        unsigned int viewport_id = 7)
    {
        SubmitSpectrumDraw(
            shell,
            frame_index,
            shell.session_.CurrentSampleSnapshot(),
            viewport_id);
        const NavigationLatencyPresentation presentation{
            viewport_id,
            NavigationLatencyTrace::Now()};
        return shell.source_activation_.
            CompleteSourceLoadFramePresentations(
                frame_index,
                std::span(&presentation, 1));
    }

    static std::vector<SourceLoadLatencyReport>
    CompleteSourceLoadFramePresentationWithoutSpectrumDraw(
        ShellUi& shell,
        std::uint64_t frame_index,
        std::span<const NavigationLatencyPresentation>
            presentations = {})
    {
        return shell.source_activation_.
            CompleteSourceLoadFramePresentations(
            frame_index,
            presentations);
    }

    static void RecordNavigationKeyInput(
        ShellUi& shell,
        NavigationLatencyInputKind kind,
        NavigationLatencyTimePoint at)
    {
        shell.RecordNavigationKeyInput(kind, at);
    }

    static std::optional<NavigationLatencyTimePoint> TakeNavigationKeyInput(
        ShellUi& shell,
        NavigationLatencyInputKind kind)
    {
        return shell.TakeNavigationKeyInput(kind);
    }

};

}  // namespace specforge

namespace {

using namespace std::chrono_literals;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
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
        io.Fonts->GetTexDataAsRGBA32(
            &font_pixels,
            &font_width,
            &font_height);
        Require(
            font_pixels != nullptr &&
                font_width > 0 &&
                font_height > 0,
            "shell menu test font atlas should build");
    }

    ~ScopedImGuiContext()
    {
        ImGui::DestroyContext();
    }

    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;
};

constexpr const char* kFileMenuTestHost =
    "File menu test host###SpecForgeFileMenuTestHost";

void RenderShellFileMenuFrame(
    specforge::ShellUi& shell,
    const specforge::SourceCollectionPathPicker& choose_source_file)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2(900.0f, 700.0f);
    ImGui::NewFrame();
    constexpr ImGuiWindowFlags kHostFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_MenuBar;
    ImGui::SetNextWindowPos(
        ImVec2(20.0f, 20.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(700.0f, 500.0f),
        ImGuiCond_Always);
    ImGui::Begin(kFileMenuTestHost, nullptr, kHostFlags);
    specforge::ShellUiTestAccess::RenderMainMenuBar(
        shell,
        specforge::ShellStatus{},
        choose_source_file);
    ImGui::End();
    ImGui::EndFrame();
}

template <typename Future, typename CancellationCheck>
void WaitForRelease(
    const Future& release,
    const CancellationCheck& canceled,
    std::string_view timeout_message)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (release.wait_for(2ms) != std::future_status::ready) {
        if (canceled()) {
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(std::string(timeout_message));
        }
    }
}

std::filesystem::path UniqueTempPath(std::string_view suffix)
{
    static const auto run_id = std::chrono::high_resolution_clock::now()
        .time_since_epoch().count();
    static std::atomic_uint64_t next_id = 1;
    return std::filesystem::temp_directory_path() /
           ("specforge_shell_activation_" + std::to_string(run_id) + "_" +
            std::to_string(next_id.fetch_add(1)) + std::string(suffix));
}

void WriteFixture(const std::filesystem::path& path)
{
    std::ofstream stream(
        path,
        std::ios::binary | std::ios::trunc);
    Require(
        stream.good(),
        "shell activation fixture should be writable");
    stream << "fixture";
}

specforge::SpectrumSnapshotHandle MakeSnapshot(
    const std::filesystem::path& path,
    std::size_t spectrum_index)
{
    auto snapshot = std::make_shared<specforge::SpectrumSnapshot>();
    snapshot->source.id = "fixture";
    snapshot->source.display_name = "fixture";
    snapshot->source.path = path;
    snapshot->collection.spectrum_count = 3;
    snapshot->collection.current_index = spectrum_index;
    snapshot->collection.can_move_previous = spectrum_index > 0;
    snapshot->collection.can_move_next = spectrum_index + 1 < 3;
    snapshot->capabilities.can_plot_current_spectrum = true;
    snapshot->capabilities.can_switch_spectrum = true;
    return snapshot;
}

specforge::SpectrumSnapshotHandle MakeSnapshotWithDestructionProbe(
    const std::filesystem::path& path,
    std::size_t spectrum_index,
    const std::shared_ptr<std::promise<std::thread::id>>& destroyed)
{
    const specforge::SpectrumSnapshotHandle source = MakeSnapshot(path, spectrum_index);
    auto* snapshot = new specforge::SpectrumSnapshot(*source);
    return specforge::SpectrumSnapshotHandle(
        snapshot,
        [destroyed](const specforge::SpectrumSnapshot* value) {
            delete value;
            destroyed->set_value(std::this_thread::get_id());
        });
}

bool SaveAnnotationFixture(
    const std::filesystem::path& path,
    std::vector<int> values,
    std::string* error)
{
    specforge::SampleLabelingTask task = specforge::CreateSampleLabelingTask(
        "shell-drain-annotation",
        "Shell drain annotation",
        values.size());
    task.values.Complete() = std::move(values);
    return specforge::test_support::LegacyFixtureIo{}.SaveLabelArray(path, task, error);
}

specforge::SourceCollectionSession MakePreparedDeferredSession(
    const std::filesystem::path& path,
    std::optional<std::string> context_fingerprint_override = std::nullopt)
{
    specforge::SourceCollectionSession session({}, {}, {}, {});
    const specforge::SpectrumSnapshotHandle snapshot = MakeSnapshot(path, 0);
    specforge::SourceCollectionContext context;
    context.identity = specforge::BuildSourceCollectionIdentity(
        *snapshot,
        specforge::CaptureSourceCollectionSingleFileState(path));
    if (context_fingerprint_override) {
        context.identity.context_fingerprint = std::move(*context_fingerprint_override);
    }
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    specforge::PreparedSampleWorkflowState workflow =
        specforge::PrepareSampleWorkflowState(*snapshot, context, 0, {{}, {}});
    Require(
        session.OpenPreparedSource(
                   path,
                   0,
                   snapshot,
                   std::move(context),
                   std::move(workflow))
            .loaded,
        "Shell drain fixture should commit its initial source");
    return session;
}

void OpenPreparedFixtureSource(
    specforge::SourceCollectionSession& session,
    const std::filesystem::path& path,
    std::size_t spectrum_index = 0,
    std::optional<std::filesystem::path> annotation_path = std::nullopt,
    specforge::SampleWorkflowPreparationPaths preparation_paths = {})
{
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(path, spectrum_index);
    specforge::SourceCollectionContext context;
    context.identity = specforge::BuildSourceCollectionIdentity(
        *snapshot,
        specforge::CaptureSourceCollectionSingleFileState(path));
    context.manifest.sample_names = {"alpha", "beta", "gamma"};
    if (annotation_path) {
        std::string annotation_error;
        std::optional<specforge::SampleAnnotationResult> annotation =
            specforge::test_support::LegacyFixtureIo{}.Load(
                *annotation_path,
                context.identity.spectrum_count,
                &annotation_error);
        Require(
            annotation.has_value(),
            annotation_error.empty()
                ? "fixture annotation should load"
                : annotation_error);
        context.manifest.annotations.push_back(std::move(*annotation));
    }
    specforge::PreparedSampleWorkflowState workflow =
        specforge::PrepareSampleWorkflowState(
            *snapshot,
            context,
            spectrum_index,
            preparation_paths);
    Require(
        session.OpenPreparedSource(
                   path,
                   spectrum_index,
                   snapshot,
                   std::move(context),
                   std::move(workflow))
            .loaded,
        "fixture source should enter the session");
}

struct SourceSessionCachePaths {
    std::filesystem::path source_session;
    std::filesystem::path navigation;
    std::filesystem::path labeling;
    std::filesystem::path workflow;
};

specforge::SourceCollectionSession MakeCachedSession(
    const SourceSessionCachePaths& cache_paths)
{
    return specforge::SourceCollectionSession(
        cache_paths.source_session,
        cache_paths.navigation,
        cache_paths.labeling,
        cache_paths.workflow);
}

specforge::SourceCollectionPreparationAdapters MakeFixtureLoadDependencies(
    const SourceSessionCachePaths& cache_paths)
{
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const std::filesystem::path& source,
           std::size_t index,
           const auto&) {
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&, const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.file_context_builder =
        [](const specforge::SpectrumSnapshot& snapshot,
           const specforge::SourceCollectionSingleFileState& file_state,
           const specforge::SourceCollectionCancellationCheckpoint& checkpoint) {
            checkpoint();
            specforge::SourceCollectionContext context;
            context.identity =
                specforge::BuildSourceCollectionIdentity(snapshot, file_state);
            context.manifest.sample_names = {"alpha", "beta", "gamma"};
            return context;
        };
    dependencies.workflow_cache_paths = {
        cache_paths.labeling,
        cache_paths.workflow,
        cache_paths.navigation,
    };
    return dependencies;
}

std::unique_ptr<specforge::ShellUi> MakeDeferredShell(
    const SourceSessionCachePaths& cache_paths,
    specforge::SourceCollectionPreparationAdapters dependencies)
{
    using Access = specforge::ShellUiTestAccess;
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakeCachedSession(cache_paths),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    Access::BeginDeferredRestore(*shell);
    return shell;
}

bool DrainAllSourceLoads(specforge::ShellUi& shell)
{
    using Access = specforge::ShellUiTestAccess;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        Access::Drain(shell);
        if (Access::PendingLoadCount(shell) == 0) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

bool CurrentSourceMatches(
    specforge::SourceCollectionSession& session,
    const std::filesystem::path& expected_path)
{
    const specforge::SourceCollectionSessionView view = session.View();
    if (!view.current_source_index ||
        *view.current_source_index >= view.sources.size()) {
        return false;
    }
    const specforge::SpectrumSnapshotHandle snapshot =
        session.CurrentSampleSnapshot();
    const std::string expected_key =
        specforge::SourcePathIdentityKey(expected_path);
    return snapshot &&
        specforge::SourcePathIdentityKey(
            view.sources[*view.current_source_index].path) == expected_key &&
        specforge::SourcePathIdentityKey(snapshot->source.path) == expected_key;
}

bool HasHealthMessage(
    const specforge::LocalUserStateHealthView& health,
    specforge::LocalUserStateArea area,
    specforge::LocalUserStateHealthMessageKind kind)
{
    for (const specforge::LocalUserStateHealthMessage& message :
         health.messages) {
        if (message.area == area && message.kind == kind) {
            return true;
        }
    }
    return false;
}

void TestDeferredRestoreReusesOnlyMatchingLockedViewport()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path source_path =
        UniqueTempPath("_viewport_restore.csv");
    const SourceSessionCachePaths cache_paths{
        UniqueTempPath("_viewport_source_session.json"),
        UniqueTempPath("_viewport_navigation.json"),
        UniqueTempPath("_viewport_labeling.json"),
        UniqueTempPath("_viewport_workflow.json"),
    };
    {
        std::ofstream stream(
            source_path,
            std::ios::binary | std::ios::trunc);
        Require(
            stream.good(),
            "viewport restore source fixture should be created");
        stream << "fixture";
    }

    std::string source_collection_identity;
    {
        specforge::SourceCollectionSession saved =
            MakeCachedSession(cache_paths);
        const specforge::SpectrumSnapshotHandle snapshot =
            MakeSnapshot(source_path, 0);
        const specforge::SourceCollectionSingleFileState
            file_state =
                specforge::CaptureSourceCollectionSingleFileState(
                    source_path);
        specforge::SourceCollectionContext context;
        context.identity =
            specforge::BuildSourceCollectionIdentity(
                *snapshot,
                file_state);
        context.manifest.sample_names = {
            "alpha", "beta", "gamma"};
        source_collection_identity = context.identity.id;
        specforge::PreparedSampleWorkflowState workflow =
            specforge::PrepareSampleWorkflowState(
                *snapshot,
                context,
                0,
                {{}, {}});
        Require(
            saved.OpenPreparedSource(
                     source_path,
                     0,
                     snapshot,
                     specforge::PreparedSourceCollectionPlan{
                         std::move(context),
                         std::move(workflow)},
                     {},
                     specforge::SourceCollectionContextReuseProof{
                         .identity =
                             specforge::BuildSourceCollectionIdentity(
                                 *snapshot,
                                 file_state),
                         .dependency_state = file_state,
                     })
                .loaded,
            "viewport restore source should seed the saved session");
        Require(
            saved.FlushStateCaches(),
            "viewport restore source session should be saved");
    }

    const specforge::PlotViewLimits expected{
        .x_min = 4100.25,
        .x_max = 4900.75,
        .y_min = -0.5,
        .y_max = 2.25,
    };
    std::unique_ptr<specforge::ShellUi> matching =
        Access::Create(
            MakeCachedSession(cache_paths),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                MakeFixtureLoadDependencies(cache_paths)));
    Access::SeedStartupSpectrumViewState(
        *matching,
        specforge::SpectrumViewStateCache{
            .locked = true,
            .source_collection_identity =
                source_collection_identity,
            .limits = expected,
        });
    Access::BeginDeferredRestore(*matching);
    Require(
        DrainAllSourceLoads(*matching),
        "matching viewport restore should finish its deferred source load");
    const std::optional<specforge::PlotViewLimits>
        matching_limits =
            Access::LockedViewportLimits(*matching);
    Require(
        Access::SpectrumView(*matching).viewport_range_mode ==
                specforge::SpectrumViewportRangeMode::Locked &&
            matching_limits &&
            matching_limits->x_min == expected.x_min &&
            matching_limits->x_max == expected.x_max &&
            matching_limits->y_min == expected.y_min &&
            matching_limits->y_max == expected.y_max,
        "a deferred restore of the same collection identity should reuse the locked viewport");
    matching.reset();

    std::unique_ptr<specforge::ShellUi> mutated =
        Access::Create(
            MakeCachedSession(cache_paths),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                MakeFixtureLoadDependencies(cache_paths)));
    Access::SeedStartupSpectrumViewState(
        *mutated,
        specforge::SpectrumViewStateCache{
            .locked = true,
            .source_collection_identity =
                source_collection_identity,
            .limits = expected,
        });
    Access::BeginDeferredRestore(*mutated);
    Access::RequestSpectrumViewportFit(*mutated);
    Require(
        DrainAllSourceLoads(*mutated),
        "mutated viewport restore should finish its deferred source load");
    Require(
        Access::SpectrumView(*mutated).viewport_range_mode ==
                specforge::SpectrumViewportRangeMode::Automatic &&
            !Access::LockedViewportLimits(*mutated),
        "a deferred viewport restore must not overwrite a newer user viewport mutation");
    mutated.reset();

    std::unique_ptr<specforge::ShellUi> mismatched =
        Access::Create(
            MakeCachedSession(cache_paths),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                MakeFixtureLoadDependencies(cache_paths)));
    Access::SeedStartupSpectrumViewState(
        *mismatched,
        specforge::SpectrumViewStateCache{
            .locked = true,
            .source_collection_identity =
                "different-collection",
            .limits = expected,
        });
    Access::BeginDeferredRestore(*mismatched);
    Require(
        DrainAllSourceLoads(*mismatched),
        "mismatched viewport restore should finish its deferred source load");
    Require(
        Access::SpectrumView(*mismatched).viewport_range_mode ==
                specforge::SpectrumViewportRangeMode::Automatic &&
            !Access::LockedViewportLimits(*mismatched),
        "a different collection identity must not reuse the saved viewport");
    mismatched.reset();

    std::error_code cleanup_error;
    std::filesystem::remove(source_path, cleanup_error);
    std::filesystem::remove(cache_paths.source_session, cleanup_error);
    std::filesystem::remove(cache_paths.navigation, cleanup_error);
    std::filesystem::remove(cache_paths.labeling, cleanup_error);
    std::filesystem::remove(cache_paths.workflow, cleanup_error);
}

void TestShellShutdownFlushPersistsLockedViewport()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path source_path =
        UniqueTempPath("_viewport_flush.csv");
    const std::filesystem::path state_path =
        UniqueTempPath("_viewport_state.json");
    {
        std::ofstream stream(
            source_path,
            std::ios::binary | std::ios::trunc);
        Require(
            stream.good(),
            "viewport flush source fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionSession session(
        {}, {}, {}, {});
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(source_path, 0);
    const specforge::SourceCollectionSingleFileState file_state =
        specforge::CaptureSourceCollectionSingleFileState(
            source_path);
    specforge::SourceCollectionContext context;
    context.identity =
        specforge::BuildSourceCollectionIdentity(
            *snapshot,
            file_state);
    context.manifest.sample_names = {
        "alpha", "beta", "gamma"};
    const std::string source_collection_identity =
        context.identity.id;
    specforge::PreparedSampleWorkflowState workflow =
        specforge::PrepareSampleWorkflowState(
            *snapshot,
            context,
            0,
            {{}, {}});
    Require(
        session.OpenPreparedSource(
                   source_path,
                   0,
                   snapshot,
                   specforge::PreparedSourceCollectionPlan{
                       std::move(context),
                       std::move(workflow)},
                   {},
                   specforge::SourceCollectionContextReuseProof{
                       .identity =
                           specforge::BuildSourceCollectionIdentity(
                               *snapshot,
                               file_state),
                       .dependency_state = file_state,
                   })
            .loaded,
        "viewport flush fixture should activate its source");

    const specforge::PlotViewLimits expected{
        .x_min = 100.125,
        .x_max = 200.875,
        .y_min = -3.5,
        .y_max = 8.25,
    };
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            std::move(session),
            specforge::MakeSourceCollectionLoadQueueForTesting());
    Access::ConfigureSpectrumViewPersistence(
        *shell,
        state_path);
    Require(
        Access::RestoreLockedViewport(*shell, expected),
        "viewport flush fixture should lock valid limits");
    const specforge::PlotSeriesColor custom_raw_color =
        specforge::PlotSeriesColor::ExplicitColor({
            .red = 0.12f,
            .green = 0.34f,
            .blue = 0.56f,
            .alpha = 0.78f,
        });
    Access::SetSpectrumSeriesColor(
        *shell,
        specforge::SpectrumPlotSeries::RawSpectrum,
        custom_raw_color);
    const auto color_changed_at =
        specforge::LocalUserStateSaveScheduler::
            Clock::now();
    Access::MarkSpectrumViewStateDirtyAt(
        *shell,
        color_changed_at);
    shell->RunMaintenance(
        color_changed_at + 250ms);
    const specforge::SpectrumViewStateCacheLoadResult
        maintained =
            specforge::LoadSpectrumViewStateCache(
                state_path);
    Require(
        maintained.warning.empty() &&
            maintained.state.plot_colors.raw_spectrum ==
                custom_raw_color,
        "curve color edits should debounce into the global spectrum view cache before shutdown");
    const specforge::ShellLocalStateFlushResult flushed =
        shell->FlushLocalState();
    const specforge::SpectrumViewStateCacheLoadResult loaded =
        specforge::LoadSpectrumViewStateCache(state_path);
    Require(
        flushed.spectrum_view_saved &&
            loaded.warning.empty() && loaded.state.locked &&
            loaded.state.source_collection_identity ==
                source_collection_identity &&
            loaded.state.limits.x_min == expected.x_min &&
            loaded.state.limits.x_max == expected.x_max &&
            loaded.state.limits.y_min == expected.y_min &&
            loaded.state.limits.y_max == expected.y_max &&
            loaded.state.plot_colors.raw_spectrum ==
                custom_raw_color,
        "shutdown flush should persist the locked viewport and global curve colors through the same view-state owner");
    shell.reset();

    std::unique_ptr<specforge::ShellUi> unlocked =
        Access::Create(
            specforge::SourceCollectionSession(
                {}, {}, {}, {}),
            specforge::MakeSourceCollectionLoadQueueForTesting());
    Access::ConfigureSpectrumViewPersistence(
        *unlocked,
        state_path);
    Require(
        unlocked->FlushLocalState().spectrum_view_saved &&
            !specforge::LoadSpectrumViewStateCache(
                 state_path)
                 .state.locked,
        "an unlocked shutdown should clear an older persisted lock");
    unlocked.reset();

    std::error_code cleanup_error;
    std::filesystem::remove(source_path, cleanup_error);
    std::filesystem::remove(state_path, cleanup_error);
}

void TestAutomationGotoAndTargetedLabelNavigationRespectActiveSequence()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path source_path =
        UniqueTempPath("_automation_sequence.npy");
    const std::optional<std::filesystem::path>
        annotation_path =
            specforge::
                SourceCollectionCompanionAnnotationPath(
                    source_path);
    Require(
        annotation_path.has_value(),
        "automation sequence fixture should expose a companion annotation path");
    {
        std::ofstream stream(
            source_path,
            std::ios::binary | std::ios::trunc);
        stream << "fixture";
    }
    std::string annotation_error;
    Require(
        SaveAnnotationFixture(
            *annotation_path,
            {1, 0, 1},
            &annotation_error),
        annotation_error.empty()
            ? "automation sequence annotation should save"
            : annotation_error);

    const auto make_dependencies = []() {
        specforge::
            SourceCollectionPreparationAdapters
                dependencies;
        dependencies.snapshot_loader =
            [](const std::filesystem::path& source,
               std::size_t index,
               const auto&) {
                return MakeSnapshot(source, index);
            };
        dependencies.workflow_cache_loader =
            [](const auto&,
               const std::function<void()>& checkpoint) {
                checkpoint();
                return specforge::
                    SampleWorkflowPreparationCacheBundle{};
            };
        dependencies.workflow_cache_paths = {{}, {}};
        return dependencies;
    };

    specforge::SourceCollectionSession filtered =
        MakePreparedDeferredSession(source_path);
    std::optional<specforge::SampleAnnotationResult>
        annotation =
            specforge::test_support::LegacyFixtureIo{}.
                Load(
                    *annotation_path,
                    3,
                    &annotation_error);
    Require(
        annotation.has_value(),
        "automation sequence annotation should load");
    const std::string filter_source_id =
        specforge::BuildAnnotationFilterSourceId(
            *annotation);
    Require(
        filtered.Submit(
                    specforge::
                        SourceCollectionSessionIntent::
                            EditSourceCollection(
                                specforge::
                                    SourceCollectionIntent::
                                        AddReadOnlyAnnotationResult(
                                            *annotation_path)))
            .loaded,
        "automation sequence fixture should attach its filter annotation");
    (void)filtered.Submit(
        specforge::SourceCollectionSessionIntent::
            ApplySampleFiltering(
                specforge::SampleFilteringIntent::
                    AddSource(filter_source_id)));
    (void)filtered.Submit(
        specforge::SourceCollectionSessionIntent::
            ApplySampleFiltering(
                specforge::SampleFilteringIntent::
                    SetFilterValueSelected(
                        filter_source_id,
                        "1",
                        true)));
    std::unique_ptr<specforge::ShellUi> filtered_shell =
        Access::Create(
            std::move(filtered),
            specforge::
                MakeSourceCollectionLoadQueueForTesting(
                    make_dependencies()));
    const auto included_index =
        filtered_shell->GotoSpectrumForAutomation(
            2,
            std::nullopt);
    const auto included_name =
        filtered_shell->GotoSpectrumForAutomation(
            std::nullopt,
            "gamma");
    const auto excluded_index =
        filtered_shell->GotoSpectrumForAutomation(
            1,
            std::nullopt);
    const auto excluded_name =
        filtered_shell->GotoSpectrumForAutomation(
            std::nullopt,
            "beta");

    specforge::SourceCollectionSession sorted =
        MakePreparedDeferredSession(source_path);
    (void)sorted.Submit(
        specforge::SourceCollectionSessionIntent::
            ApplySampleSorting(
                specforge::SampleSortingIntent::
                    SetSortSource("sample-name")));
    std::unique_ptr<specforge::ShellUi> sorted_shell =
        Access::Create(
            std::move(sorted),
            specforge::
                MakeSourceCollectionLoadQueueForTesting(
                    make_dependencies()));
    const auto sorted_index =
        sorted_shell->GotoSpectrumForAutomation(
            2,
            std::nullopt);
    const auto sorted_name =
        sorted_shell->GotoSpectrumForAutomation(
            std::nullopt,
            "gamma");

    filtered_shell.reset();
    sorted_shell.reset();
    std::filesystem::remove(source_path);
    std::filesystem::remove(*annotation_path);
    Require(
        included_index.error ==
                specforge::
                    ShellAutomationNavigationError::
                        None &&
            included_index.target.index == 2 &&
            included_name.error ==
                specforge::
                    ShellAutomationNavigationError::
                        None &&
            included_name.target.index == 2,
        "source-row index/name targets retained by a filter should remain valid for goto and targeted-label navigation");
    Require(
        excluded_index.error ==
                specforge::
                    ShellAutomationNavigationError::
                        FilteredOut &&
            excluded_name.error ==
                specforge::
                    ShellAutomationNavigationError::
                        FilteredOut,
        "source-row index/name targets excluded by the active sequence should remain deterministically filtered out");
    Require(
        sorted_index.error ==
                specforge::
                    ShellAutomationNavigationError::
                        None &&
            sorted_index.target.index == 2 &&
            sorted_name.error ==
                specforge::
                    ShellAutomationNavigationError::
                        None &&
            sorted_name.target.index == 2,
        "source-row index/name targets should remain valid under non-source-order sorting");
}

void TestExplicitOpenTracesAcceptedPathThroughFirstPresent()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_source_load_present.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "source load trace fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionSession session({}, {}, {}, {});
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const std::filesystem::path& source, std::size_t index, const auto&) {
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&, const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        std::move(session),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    constexpr std::uint64_t presentation_frame = 300;
    Access::EnableNavigationTracing(*shell, presentation_frame);

    shell->OpenSource(path);
    const auto activation_deadline = std::chrono::steady_clock::now() + 2s;
    bool activated = false;
    while (std::chrono::steady_clock::now() < activation_deadline) {
        Access::Drain(*shell);
        const specforge::SpectrumSnapshotHandle snapshot =
            Access::Session(*shell).CurrentSampleSnapshot();
        activated = snapshot && snapshot->source.path == path &&
                    Access::PendingLoadCount(*shell) == 0;
        if (activated) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const bool waits_for_present = activated;
    Access::SubmitSpectrumDraw(
        *shell,
        presentation_frame,
        Access::Session(*shell).CurrentSampleSnapshot(),
        7);
    const specforge::NavigationLatencyPresentation wrong_viewport_presentation{
        8,
        specforge::NavigationLatencyTrace::Now()};
    const std::vector<specforge::SourceLoadLatencyReport>
        wrong_viewport_reports =
            Access::
                CompleteSourceLoadFramePresentationWithoutSpectrumDraw(
                    *shell,
                    presentation_frame,
                    std::span(
                        &wrong_viewport_presentation,
                        1));
    const bool waits_for_matching_viewport =
        wrong_viewport_reports.empty();
    const std::vector<specforge::SourceLoadLatencyReport> reports =
        Access::CompleteSourceLoadFramePresentation(
            *shell,
            presentation_frame + 1,
            7);

    shell->OpenSource(path);
    const auto replacement_deadline = std::chrono::steady_clock::now() + 2s;
    bool replacement_activated = false;
    while (std::chrono::steady_clock::now() < replacement_deadline) {
        Access::Drain(*shell);
        replacement_activated =
            Access::PendingLoadCount(*shell) == 0;
        if (replacement_activated) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    Access::SubmitSpectrumDraw(
        *shell,
        presentation_frame + 2,
        MakeSnapshot(path, 0),
        7);
    const specforge::NavigationLatencyPresentation replacement_presentation{
        7,
        specforge::NavigationLatencyTrace::Now()};
    const std::vector<specforge::SourceLoadLatencyReport>
        wrong_snapshot_reports =
            Access::
                CompleteSourceLoadFramePresentationWithoutSpectrumDraw(
                    *shell,
                    presentation_frame + 2,
                    std::span(
                        &replacement_presentation,
                        1));
    shell.reset();
    std::filesystem::remove(path);

    Require(activated, "the explicit source should activate through the async queue");
    Require(
        waits_for_present,
        "a loaded source trace should remain pending until its snapshot is presented");
    Require(
        waits_for_matching_viewport,
        "a successful Present from another viewport must not complete the source load trace");
    Require(
        reports.size() == 1 &&
            reports.front().outcome ==
                specforge::SourceLoadLatencyOutcome::Presented &&
            reports.front().attempts.size() == 1 &&
            reports.front().presentation_viewport_id == 7,
        "the exact loaded snapshot Present should complete one source load trace");
    Require(
        reports.front().accepted_ns <=
                reports.front().attempts.front().load_enqueued_ns &&
            reports.front().attempts.front().completion_drained_ns <=
                reports.front().snapshot_activated_ns &&
            reports.front().snapshot_activated_ns <=
                reports.front().ui_updated_ns &&
            reports.front().ui_updated_ns <= reports.front().first_present_ns,
        "the source load report should retain a monotonic accepted-to-Present chain");
    Require(
        replacement_activated && wrong_snapshot_reports.size() == 1 &&
            wrong_snapshot_reports.front().outcome ==
                specforge::SourceLoadLatencyOutcome::Superseded &&
            wrong_snapshot_reports.front().first_present_ns == 0,
        "drawing a different snapshot must supersede rather than present the source load trace");
}

void TestFailedExplicitOpenProducesTerminalSourceLoadReport()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_source_load_failure.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "failed source load trace fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionSession session({}, {}, {}, {});
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const std::filesystem::path&, std::size_t, const auto&)
            -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("synthetic decode failure");
        };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        std::move(session),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    constexpr std::uint64_t frame_index = 301;
    Access::EnableNavigationTracing(*shell, frame_index);

    shell->OpenSource(path);
    const auto failure_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < failure_deadline &&
           Access::PendingLoadCount(*shell) != 0) {
        Access::Drain(*shell);
        std::this_thread::sleep_for(2ms);
    }
    Access::Drain(*shell);
    const std::vector<specforge::SourceLoadLatencyReport> reports =
        Access::
            CompleteSourceLoadFramePresentationWithoutSpectrumDraw(
                *shell,
                frame_index);
    const bool load_error_visible = !Access::LoadError(*shell).empty();
    shell.reset();
    std::filesystem::remove(path);

    Require(
        reports.size() == 1 &&
            reports.front().outcome ==
                specforge::SourceLoadLatencyOutcome::Failed &&
            reports.front().attempts.size() == 1 &&
            reports.front().first_present_ns == 0,
        "a failed explicit open should emit one terminal report without a Present");
    Require(
        reports.front().attempts.front().completion_drained_ns > 0 &&
            load_error_visible,
        "the failed report should include completion drain and preserve the UI error");
}

void TestRealDrainCommitsOnlyTheLatestRapidNavigation()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_real_drain.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "real drain fixture should be created");
        stream << "fixture";
    }

    std::promise<void> row_one_entered_promise;
    std::shared_future<void> row_one_entered = row_one_entered_promise.get_future().share();
    std::promise<void> row_two_entered_promise;
    std::shared_future<void> row_two_entered = row_two_entered_promise.get_future().share();
    std::promise<void> release_decoders_promise;
    std::shared_future<void> release_decoders = release_decoders_promise.get_future().share();
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&row_one_entered_promise,
         &row_two_entered_promise,
         release_decoders](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            if (index == 1) {
                row_one_entered_promise.set_value();
                WaitForRelease(
                    release_decoders,
                    canceled,
                    "timed out waiting to release navigation row one");
            } else if (index == 2) {
                row_two_entered_promise.set_value();
                WaitForRelease(
                    release_decoders,
                    canceled,
                    "timed out waiting to release navigation row two");
            }
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    constexpr std::uint64_t presentation_frame = 77;
    Access::EnableNavigationTracing(*shell, presentation_frame);

    const specforge::SourceCollectionSessionResult first = Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())),
        specforge::NavigationLatencyInputKind::UiNext);
    const bool first_queued = first.follow_up_spectrum_index == 1;
    const bool row_one_started = row_one_entered.wait_for(2s) == std::future_status::ready;
    const specforge::SourceCollectionSessionResult second = Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())),
        specforge::NavigationLatencyInputKind::UiNext);
    const bool second_queued = second.follow_up_spectrum_index == 2;
    const bool row_two_started = row_two_entered.wait_for(2s) == std::future_status::ready;
    release_decoders_promise.set_value();

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool latest_committed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        Access::Drain(*shell);
        const specforge::SpectrumSnapshotHandle snapshot =
            Access::Session(*shell).CurrentSampleSnapshot();
        latest_committed = snapshot && snapshot->collection.current_index == 2 &&
            Access::PendingLoadCount(*shell) == 0;
        if (latest_committed) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const bool no_load_error = Access::LoadError(*shell).empty();
    std::vector<specforge::NavigationLatencyReport> navigation_reports =
        Access::CompleteFramePresentation(*shell, presentation_frame, 99);
    const bool detached_mismatch_retained_presented_trace =
        std::none_of(
            navigation_reports.begin(),
            navigation_reports.end(),
            [](const auto& report) {
                return report.outcome ==
                    specforge::NavigationLatencyOutcome::
                        Presented;
            });
    std::vector<specforge::NavigationLatencyReport> presented_reports =
        Access::CompleteFramePresentation(*shell, presentation_frame, 7);
    navigation_reports.insert(
        navigation_reports.end(),
        presented_reports.begin(),
        presented_reports.end());
    const auto presented_report = std::find_if(
        navigation_reports.begin(),
        navigation_reports.end(),
        [](const specforge::NavigationLatencyReport& report) {
            return report.outcome == specforge::NavigationLatencyOutcome::Presented;
        });
    const auto superseded_report = std::find_if(
        navigation_reports.begin(),
        navigation_reports.end(),
        [](const specforge::NavigationLatencyReport& report) {
            return report.outcome == specforge::NavigationLatencyOutcome::Superseded;
        });
    const bool terminal_outcomes_present =
        presented_report != navigation_reports.end() &&
        superseded_report != navigation_reports.end();
    const bool trace_indices_correlated = terminal_outcomes_present &&
        presented_report->from_index == 1 && presented_report->target_index == 2;
    const bool trace_viewport_correlated = terminal_outcomes_present &&
        presented_report->presentation_viewport_id == 7;
    const bool trace_attempt_correlated = terminal_outcomes_present &&
        presented_report->attempts.size() == 1 &&
        presented_report->attempts[0].source_task_id != 0 &&
        presented_report->attempts[0].worker_started_ns > 0 &&
        presented_report->attempts[0].snapshot_load_finished_ns >=
            presented_report->attempts[0].snapshot_load_started_ns &&
        presented_report->attempts[0].completion_drained_ns >=
            presented_report->attempts[0].completion_published_ns;
    const bool trace_activation_correlated = trace_attempt_correlated &&
        presented_report->snapshot_activated_ns >=
            presented_report->attempts[0].completion_drained_ns &&
        presented_report->first_present_ns >= presented_report->snapshot_activated_ns;
    const auto target_resolution_sum = [](const specforge::NavigationLatencyReport& report) {
        const specforge::NavigationTargetResolutionReport& resolution =
            report.target_resolution;
        return resolution.effective_index_ns +
            resolution.pending_activation_supersede_ns +
            resolution.base_sequence_ns +
            resolution.target_lookup_ns +
            resolution.target_sequence_ns +
            resolution.navigation_state_result_ns;
    };
    const bool target_resolution_correlated = terminal_outcomes_present &&
        superseded_report->target_resolution.row_count == 3 &&
        !superseded_report->target_resolution.pending_present &&
        presented_report->target_resolution.row_count == 3 &&
        presented_report->target_resolution.pending_present &&
        presented_report->target_resolution.sequence_cache_hit &&
        presented_report->target_resolution.sequence_build_count == 0 &&
        target_resolution_sum(*presented_report) ==
            presented_report->target_resolved_ns -
                presented_report->requested_ns;
    shell.reset();
    std::filesystem::remove(path);

    Require(first_queued && row_one_started, "row 1 should enter the real Shell worker chain");
    Require(second_queued && row_two_started, "row 2 should supersede row 1 while both use real workers");
    Require(
        latest_committed,
        "the production drain should admit and atomically publish only the latest row 2 completion");
    Require(no_load_error, "canceling the obsolete row 1 completion must not publish a load error");
    Require(
        detached_mismatch_retained_presented_trace,
        "a Present from a viewport that does not host Spectrum must not complete the trace");
    Require(terminal_outcomes_present, "rapid navigation should report presented and superseded traces");
    Require(trace_indices_correlated, "rapid navigation should retain logical from/target indices");
    Require(trace_viewport_correlated, "the presented trace should name the Spectrum viewport");
    Require(trace_attempt_correlated, "the presented trace should retain its complete worker attempt");
    Require(trace_activation_correlated, "activation and Present timestamps should follow worker drain");
    Require(
        target_resolution_correlated,
        "rapid navigation should retain an exact target-resolution breakdown and pending state");
}

void TestGenericRowLocationDoesNotStartPreviousNextTrace()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_locate_row_trace.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "row-location trace fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = [](const std::filesystem::path& source, std::size_t index, const auto&) {
        return MakeSnapshot(source, index);
    };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    Access::EnableNavigationTracing(*shell, 90);

    const specforge::SourceCollectionSessionResult result = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::LocateRow(2))));
    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline &&
           Access::PendingLoadCount(*shell) != 0) {
        Access::Drain(*shell);
        std::this_thread::sleep_for(2ms);
    }
    const std::vector<specforge::NavigationLatencyReport>
        reports = Access::CompleteFramePresentation(
            *shell,
            90,
            7);
    shell.reset();
    std::filesystem::remove(path);

    Require(result.follow_up_spectrum_index == 2, "row location should still queue its real source load");
    Require(
        reports.empty(),
        "LocateRow must not be classified as previous/next navigation latency");
}

void TestAcceptedNavigationUsesLatestMatchingRawKeyInput()
{
    using Access = specforge::ShellUiTestAccess;
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        specforge::SourceCollectionSession({}, {}, {}, {}),
        specforge::MakeSourceCollectionLoadQueueForTesting());
    const auto stale = specforge::NavigationLatencyTimePoint(std::chrono::milliseconds(10));
    const auto accepted = specforge::NavigationLatencyTimePoint(std::chrono::milliseconds(20));
    Access::RecordNavigationKeyInput(
        *shell,
        specforge::NavigationLatencyInputKind::KeyboardNext,
        stale);
    Access::RecordNavigationKeyInput(
        *shell,
        specforge::NavigationLatencyInputKind::KeyboardNext,
        accepted);

    const std::optional<specforge::NavigationLatencyTimePoint> correlated =
        Access::TakeNavigationKeyInput(
            *shell,
            specforge::NavigationLatencyInputKind::KeyboardNext);
    Require(correlated == accepted, "an accepted shortcut must correlate with the latest raw key edge");
}

void TestWorkflowAutoAdvanceStartsExplicitTrace()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_auto_advance_trace.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "auto-advance trace fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = [](const std::filesystem::path& source, std::size_t index, const auto&) {
        return MakeSnapshot(source, index);
    };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    Access::EnableNavigationTracing(*shell, 91);

    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::StartOrResumeTemporaryLabelingTask()));
    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::UpsertActiveLabel(
                specforge::SampleLabelDefinition{7, "accepted", 'a'})));
    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::SetActiveLabelingAutoAdvance(true)));

    const auto drain_and_present =
        [&shell](std::size_t target_index, std::uint64_t presentation_frame) {
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (std::chrono::steady_clock::now() < deadline) {
                Access::Drain(*shell);
                const specforge::SpectrumSnapshotHandle snapshot =
                    Access::Session(*shell).CurrentSampleSnapshot();
                if (snapshot && snapshot->collection.current_index == target_index &&
                    Access::PendingLoadCount(*shell) == 0) {
                    break;
                }
                std::this_thread::sleep_for(2ms);
            }
            return Access::CompleteFramePresentation(*shell, presentation_frame, 7);
        };
    const auto has_complete_target_resolution =
        [](const specforge::NavigationLatencyReport& report) {
            const specforge::NavigationTargetResolutionReport& resolution =
                report.target_resolution;
            const std::int64_t stage_sum =
                resolution.effective_index_ns +
                resolution.pending_activation_supersede_ns +
                resolution.base_sequence_ns +
                resolution.target_lookup_ns +
                resolution.target_sequence_ns +
                resolution.navigation_state_result_ns;
            return resolution.row_count >= 1 &&
                !resolution.pending_present &&
                resolution.sequence_cache_hit &&
                resolution.sequence_build_count == 0 &&
                resolution.effective_index_ns >= 0 &&
                resolution.pending_activation_supersede_ns >= 0 &&
                resolution.base_sequence_ns >= 0 &&
                resolution.target_lookup_ns >= 0 &&
                resolution.target_sequence_ns >= 0 &&
                resolution.navigation_state_result_ns >= 0 &&
                stage_sum ==
                    report.target_resolved_ns - report.requested_ns;
        };

    const specforge::SourceCollectionSessionResult assign_result = Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(7)),
        specforge::NavigationLatencyInputKind::AutoAdvance);
    const std::vector<specforge::NavigationLatencyReport> assign_reports =
        drain_and_present(1, 91);

    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::SetActiveLabelingAutoAdvance(false)));
    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::AssignActiveLabelToCurrentSample(7)));
    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::SetActiveLabelingAutoAdvance(true)));
    Access::EnableNavigationTracing(*shell, 92);
    const specforge::SourceCollectionSessionResult clear_result = Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::ClearActiveLabelForCurrentSample()),
        specforge::NavigationLatencyInputKind::AutoAdvance);
    const std::vector<specforge::NavigationLatencyReport> clear_reports =
        drain_and_present(2, 92);

    const bool assign_report_complete =
        assign_reports.size() == 1 &&
        assign_reports.front().outcome == specforge::NavigationLatencyOutcome::Presented &&
        assign_reports.front().input_kind == specforge::NavigationLatencyInputKind::AutoAdvance &&
        assign_reports.front().from_index == 0 &&
        assign_reports.front().target_index == 1 &&
        has_complete_target_resolution(assign_reports.front());
    const bool clear_report_complete =
        clear_reports.size() == 1 &&
        clear_reports.front().outcome == specforge::NavigationLatencyOutcome::Presented &&
        clear_reports.front().input_kind == specforge::NavigationLatencyInputKind::AutoAdvance &&
        clear_reports.front().from_index == 1 &&
        clear_reports.front().target_index == 2 &&
        has_complete_target_resolution(clear_reports.front());
    shell.reset();
    std::filesystem::remove(path);

    Require(assign_result.follow_up_spectrum_index == 1, "label assignment should request auto-advance");
    Require(clear_result.follow_up_spectrum_index == 2, "label clearing should request auto-advance");
    Require(
        assign_report_complete,
        "label assignment auto-advance should emit an analyzer-compatible target-resolution report");
    Require(
        clear_report_complete,
        "label clearing auto-advance should emit an analyzer-compatible target-resolution report");
}

void TestWarmUiAndKeyboardNavigationReuseSequenceStateAtFixedIndices()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_warm_sequence_reuse.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "warm sequence reuse fixture should be created");
        stream << "fixture";
    }

    std::atomic_int decoder_calls = 0;
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = [&decoder_calls](
                                       const std::filesystem::path& source,
                                       std::size_t index,
                                       const auto&) {
        ++decoder_calls;
        std::this_thread::sleep_for(12ms);
        return MakeSnapshot(source, index);
    };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    Access::EnableNavigationTracing(*shell, 200);

    struct ProjectionTimings {
        std::vector<std::int64_t> base_ns;
        std::vector<std::int64_t> target_ns;
    };
    std::array<ProjectionTimings, 4> timings;
    std::vector<std::int64_t> cold_decode_ns;
    std::vector<std::int64_t> hit_decode_ns;
    std::uint64_t presentation_frame = 200;
    const auto navigate_and_present =
        [&shell,
         &presentation_frame,
         &cold_decode_ns,
         &hit_decode_ns](
            specforge::NavigationLatencyInputKind input_kind,
            const specforge::SampleNavigationRequest& request,
            std::size_t from_index,
            std::size_t target_index,
            bool expected_cache_hit,
            ProjectionTimings& projection_timings) {
            const specforge::SpectrumSnapshotHandle before =
                Access::Session(*shell).CurrentSampleSnapshot();
            Require(
                before && before->collection.current_index == from_index,
                "warm navigation repetition should start from its fixed source index");
            Access::EnableNavigationTracing(*shell, presentation_frame);
            const bool keyboard_input =
                input_kind == specforge::NavigationLatencyInputKind::KeyboardPrevious ||
                input_kind == specforge::NavigationLatencyInputKind::KeyboardNext;
            const specforge::SourceCollectionSessionResult result = Access::SubmitNavigation(
                *shell,
                specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
                    specforge::SampleNavigationIntent::Move(request)),
                input_kind,
                keyboard_input
                    ? std::optional<specforge::NavigationLatencyTimePoint>{
                          specforge::NavigationLatencyTrace::Now()}
                    : std::nullopt);
            Require(
                result.follow_up_spectrum_index == target_index,
                "warm navigation repetition should enqueue the expected target");

            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (std::chrono::steady_clock::now() < deadline) {
                Access::Drain(*shell, false);
                const specforge::SpectrumSnapshotHandle snapshot =
                    Access::Session(*shell).CurrentSampleSnapshot();
                if (snapshot && snapshot->collection.current_index == target_index &&
                    Access::PendingLoadCount(*shell) == 0) {
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }

            const std::vector<specforge::NavigationLatencyReport> reports =
                Access::CompleteFramePresentation(*shell, presentation_frame++, 7);
            if (reports.size() != 1) {
                const specforge::SpectrumSnapshotHandle after =
                    Access::Session(*shell).CurrentSampleSnapshot();
                throw std::runtime_error(
                    "each warm navigation should complete one trace; frame=" +
                    std::to_string(presentation_frame - 1) +
                    "; reports=" + std::to_string(reports.size()) +
                    "; current=" +
                    (after ? std::to_string(after->collection.current_index) : "none") +
                    "; pending_loads=" + std::to_string(Access::PendingLoadCount(*shell)) +
                    "; load_error=" + std::string(Access::LoadError(*shell)));
            }
            const specforge::NavigationLatencyReport& report = reports.front();
            const specforge::NavigationTargetResolutionReport& resolution =
                report.target_resolution;
            Require(
                report.outcome == specforge::NavigationLatencyOutcome::Presented &&
                    report.input_kind == input_kind &&
                    report.from_index == from_index &&
                    report.target_index == target_index &&
                    report.cache_hit == expected_cache_hit,
                "warm navigation should preserve input kind, indices, presented outcome, and actual decode reuse");
            Require(
                !resolution.pending_present &&
                    resolution.sequence_cache_hit &&
                    resolution.sequence_build_count == 0,
                "fixed-index warm navigation should reuse sequence state without rebuilding");
            const specforge::SourceCollectionNavigationView navigation =
                Access::Session(*shell).View().navigation;
            Require(
                !navigation.exact_sample_name_match &&
                    navigation.sample_name_matches.empty(),
                "passive UI and keyboard navigation sync must not submit the current sample name as a search query");
            Require(
                resolution.base_sequence_ns >= 0 &&
                    resolution.target_sequence_ns >= 0 &&
                    report.attempts.size() == 1,
                "warm navigation should retain valid projection timings and one load attempt");
            const std::int64_t decode_ns =
                report.attempts[0].snapshot_load_finished_ns -
                report.attempts[0].snapshot_load_started_ns;
            (expected_cache_hit ? hit_decode_ns : cold_decode_ns)
                .push_back(decode_ns);
            projection_timings.base_ns.push_back(resolution.base_sequence_ns);
            projection_timings.target_ns.push_back(resolution.target_sequence_ns);
        };

    for (std::size_t repetition = 0; repetition < 100; ++repetition) {
        navigate_and_present(
            specforge::NavigationLatencyInputKind::UiNext,
            specforge::SampleNavigationRequest::Next(),
            0,
            1,
            repetition > 0,
            timings[0]);
        navigate_and_present(
            specforge::NavigationLatencyInputKind::UiPrevious,
            specforge::SampleNavigationRequest::Previous(),
            1,
            0,
            repetition > 0,
            timings[1]);
    }
    for (std::size_t repetition = 0; repetition < 100; ++repetition) {
        navigate_and_present(
            specforge::NavigationLatencyInputKind::KeyboardNext,
            specforge::SampleNavigationRequest::Next(),
            0,
            1,
            true,
            timings[2]);
        navigate_and_present(
            specforge::NavigationLatencyInputKind::KeyboardPrevious,
            specforge::SampleNavigationRequest::Previous(),
            1,
            0,
            true,
            timings[3]);
    }

    const auto percentile = [](std::vector<std::int64_t> values, std::size_t numerator) {
        std::sort(values.begin(), values.end());
        const std::size_t rank =
            std::max<std::size_t>(1, (values.size() * numerator + 99) / 100);
        return values[rank - 1];
    };
    constexpr std::array<std::string_view, 4> kTimingLabels = {
        "ui_next",
        "ui_previous",
        "keyboard_next",
        "keyboard_previous",
    };
    for (std::size_t index = 0; index < timings.size(); ++index) {
        Require(
            timings[index].base_ns.size() == 100 &&
                timings[index].target_ns.size() == 100,
            "each fixed-index input/direction group should contain 100 warm samples");
        std::printf(
            "%.*s count=100 base_ns_p50=%lld base_ns_p95=%lld "
            "target_ns_p50=%lld target_ns_p95=%lld\n",
            static_cast<int>(kTimingLabels[index].size()),
            kTimingLabels[index].data(),
            static_cast<long long>(percentile(timings[index].base_ns, 50)),
            static_cast<long long>(percentile(timings[index].base_ns, 95)),
            static_cast<long long>(percentile(timings[index].target_ns, 50)),
            static_cast<long long>(percentile(timings[index].target_ns, 95)));
    }
    if (decoder_calls.load() != 2) {
        throw std::runtime_error(
            "400 fixed-index Previous/Next visits should decode only the two initial cold rows; decoder_calls=" +
            std::to_string(decoder_calls.load()));
    }
    Require(
        cold_decode_ns.size() == 2 &&
            hit_decode_ns.size() == 398,
        "cache-hit timing groups should match the two cold and 398 resident visits");
    const std::int64_t cold_decode_min =
        *std::min_element(
            cold_decode_ns.begin(),
            cold_decode_ns.end());
    const std::int64_t hit_decode_p95 =
        percentile(hit_decode_ns, 95);
    Require(
        cold_decode_min >= 8'000'000 &&
            hit_decode_p95 * 2 < cold_decode_min,
        "resident hit decode_ms p95 should remain near zero relative to the deliberately slow decoder");

    shell.reset();
    std::filesystem::remove(path);
}

void TestNewActivationSupersedesAnUnpresentedOlderTrace()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_activation_present_owner.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "activation ownership fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = [](const std::filesystem::path& source, std::size_t index, const auto&) {
        return MakeSnapshot(source, index);
    };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    constexpr std::uint64_t presentation_frame = 101;
    Access::EnableNavigationTracing(*shell, presentation_frame);

    const auto submit_next = [&shell]() {
        return Access::SubmitNavigation(
            *shell,
            specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
                specforge::SampleNavigationIntent::Move(
                    specforge::SampleNavigationRequest::Next())),
            specforge::NavigationLatencyInputKind::UiNext);
    };
    const auto drain_to_index = [&shell](std::size_t target_index) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            Access::Drain(*shell);
            const specforge::SpectrumSnapshotHandle snapshot =
                Access::Session(*shell).CurrentSampleSnapshot();
            if (snapshot && snapshot->collection.current_index == target_index &&
                Access::PendingLoadCount(*shell) == 0) {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return false;
    };

    const bool first_requested = submit_next().follow_up_spectrum_index == 1;
    const bool first_activated = drain_to_index(1);
    const bool first_still_waiting_for_present =
        Access::CompleteFramePresentationWithoutSpectrumDraw(
            *shell,
            presentation_frame,
            7)
            .empty();
    const bool second_requested = submit_next().follow_up_spectrum_index == 2;
    const bool second_activated = drain_to_index(2);
    const std::vector<specforge::NavigationLatencyReport> reports =
        Access::CompleteFramePresentation(*shell, presentation_frame, 7);
    const std::size_t presented_count = static_cast<std::size_t>(std::count_if(
        reports.begin(),
        reports.end(),
        [](const specforge::NavigationLatencyReport& report) {
            return report.outcome == specforge::NavigationLatencyOutcome::Presented;
        }));
    const std::size_t superseded_count = static_cast<std::size_t>(std::count_if(
        reports.begin(),
        reports.end(),
        [](const specforge::NavigationLatencyReport& report) {
            return report.outcome == specforge::NavigationLatencyOutcome::Superseded;
        }));
    shell.reset();
    std::filesystem::remove(path);

    Require(first_requested && first_activated, "the first navigation should activate row 1");
    Require(first_still_waiting_for_present, "the first trace should wait after an unsuccessful Present");
    Require(second_requested && second_activated, "the second navigation should activate row 2");
    Require(
        presented_count == 1 && superseded_count == 1,
        "row 2 Present must present only row 2 and supersede the unpresented row 1 trace");
}

void TestSameFrameSourceSwitchSupersedesActivatedNavigation()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path_a = UniqueTempPath("_present_target_a.csv");
    const std::filesystem::path path_b = UniqueTempPath("_present_target_b.csv");
    {
        std::ofstream stream_a(path_a, std::ios::binary | std::ios::trunc);
        std::ofstream stream_b(path_b, std::ios::binary | std::ios::trunc);
        Require(stream_a.good() && stream_b.good(), "source-switch fixtures should be created");
        stream_a << "fixture-a";
        stream_b << "fixture-b";
    }

    specforge::SourceCollectionSession session = MakePreparedDeferredSession(path_a);
    const specforge::SpectrumSnapshotHandle snapshot_b = MakeSnapshot(path_b, 0);
    specforge::SourceCollectionContext context_b;
    context_b.identity = {"present-target-b", "b", "b-source", "b-context", 3};
    context_b.manifest.sample_names = {"one", "two", "three"};
    specforge::PreparedSampleWorkflowState workflow_b =
        specforge::PrepareSampleWorkflowState(*snapshot_b, context_b, 0, {{}, {}});
    Require(
        session.OpenPreparedSource(
                   path_b,
                   0,
                   snapshot_b,
                   std::move(context_b),
                   std::move(workflow_b))
            .loaded,
        "source B should be cached before the navigation fixture starts");
    (void)session.Submit(specforge::SourceCollectionSessionIntent::EditSourceCollection(
        specforge::SourceCollectionIntent::SwitchActive(0)));
    Require(
        session.CurrentSampleSnapshot() &&
            session.CurrentSampleSnapshot()->source.path == path_a,
        "source A should be active before navigation");

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = [](const std::filesystem::path& source, std::size_t index, const auto&) {
        return MakeSnapshot(source, index);
    };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        std::move(session),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    constexpr std::uint64_t presentation_frame = 202;
    Access::EnableNavigationTracing(*shell, presentation_frame);

    const specforge::SourceCollectionSessionResult navigation = Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())),
        specforge::NavigationLatencyInputKind::UiNext);
    const auto activation_deadline = std::chrono::steady_clock::now() + 2s;
    bool target_activated = false;
    while (std::chrono::steady_clock::now() < activation_deadline) {
        Access::Drain(*shell);
        const specforge::SpectrumSnapshotHandle snapshot = Access::Session(*shell).CurrentSampleSnapshot();
        target_activated = snapshot && snapshot->source.path == path_a &&
            snapshot->collection.current_index == 1 && Access::PendingLoadCount(*shell) == 0;
        if (target_activated) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const specforge::SourceCollectionSessionResult switched = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::EditSourceCollection(
            specforge::SourceCollectionIntent::SwitchActive(1)));
    const specforge::SpectrumSnapshotHandle active_snapshot = Access::Session(*shell).CurrentSampleSnapshot();
    Access::SubmitSpectrumDraw(*shell, presentation_frame, active_snapshot, 7);
    const std::vector<specforge::NavigationLatencyReport> reports =
        Access::CompleteFramePresentationWithoutSpectrumDraw(*shell, presentation_frame, 7);
    const std::size_t presented_count = static_cast<std::size_t>(std::count_if(
        reports.begin(),
        reports.end(),
        [](const specforge::NavigationLatencyReport& report) {
            return report.outcome == specforge::NavigationLatencyOutcome::Presented;
        }));
    const std::size_t superseded_count = static_cast<std::size_t>(std::count_if(
        reports.begin(),
        reports.end(),
        [](const specforge::NavigationLatencyReport& report) {
            return report.outcome == specforge::NavigationLatencyOutcome::Superseded;
        }));
    shell.reset();
    std::filesystem::remove(path_a);
    std::filesystem::remove(path_b);

    Require(navigation.follow_up_spectrum_index == 1 && target_activated, "source A row 1 should activate");
    Require(
        switched.action.snapshot_changed && active_snapshot == snapshot_b,
        "the cached source B snapshot should synchronously take over in the same frame");
    Require(
        presented_count == 0 && superseded_count == 1,
        "a Present that draws source B must supersede, not present, source A's navigation trace");
}

void TestPresentationWithoutSpectrumDrawDoesNotCompleteNavigation()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_hidden_spectrum.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "hidden Spectrum fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader = [](const std::filesystem::path& source, std::size_t index, const auto&) {
        return MakeSnapshot(source, index);
    };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    constexpr std::uint64_t presentation_frame = 203;
    Access::EnableNavigationTracing(*shell, presentation_frame);

    const specforge::SourceCollectionSessionResult navigation = Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())),
        specforge::NavigationLatencyInputKind::UiNext);
    const auto activation_deadline = std::chrono::steady_clock::now() + 2s;
    bool target_activated = false;
    while (std::chrono::steady_clock::now() < activation_deadline) {
        Access::Drain(*shell);
        const specforge::SpectrumSnapshotHandle snapshot = Access::Session(*shell).CurrentSampleSnapshot();
        target_activated = snapshot && snapshot->collection.current_index == 1 &&
            Access::PendingLoadCount(*shell) == 0;
        if (target_activated) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const std::vector<specforge::NavigationLatencyReport> reports =
        Access::CompleteFramePresentationWithoutSpectrumDraw(*shell, presentation_frame, 7);
    const bool trace_retained = reports.empty();
    Access::SubmitSpectrumDraw(
        *shell,
        presentation_frame + 1,
        Access::Session(*shell).CurrentSampleSnapshot(),
        7);
    const std::vector<specforge::NavigationLatencyReport> visible_reports =
        Access::CompleteFramePresentationWithoutSpectrumDraw(
            *shell,
            presentation_frame + 1,
            7);
    const bool presented_after_visible_draw =
        visible_reports.size() == 1 &&
        visible_reports.front().outcome == specforge::NavigationLatencyOutcome::Presented;
    shell.reset();
    std::filesystem::remove(path);

    Require(navigation.follow_up_spectrum_index == 1 && target_activated, "the hidden Spectrum target should activate");
    Require(
        reports.empty() && trace_retained,
        "a successful viewport Present without a submitted Spectrum draw must not complete navigation");
    Require(
        presented_after_visible_draw,
        "the retained navigation should complete after its exact snapshot is visibly submitted later");
}

void TestPublishedStaleCompletionIsRejectedWithoutMutatingNewNavigation()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_published_stale_completion.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "published stale completion fixture should be created");
        stream << "fixture";
    }

    auto stale_snapshot_destroyed_promise =
        std::make_shared<std::promise<std::thread::id>>();
    std::future<std::thread::id> stale_snapshot_destroyed =
        stale_snapshot_destroyed_promise->get_future();
    std::promise<void> row_two_entered_promise;
    std::shared_future<void> row_two_entered = row_two_entered_promise.get_future().share();
    std::promise<void> release_row_two_promise;
    std::shared_future<void> release_row_two = release_row_two_promise.get_future().share();

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [stale_snapshot_destroyed_promise,
         &row_two_entered_promise,
         release_row_two](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            if (index == 1) {
                return MakeSnapshotWithDestructionProbe(
                    source,
                    index,
                    stale_snapshot_destroyed_promise);
            }
            if (index == 2) {
                row_two_entered_promise.set_value();
                WaitForRelease(
                    release_row_two,
                    canceled,
                    "timed out waiting to release stale row two");
            }
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));
    const specforge::SpectrumSnapshotHandle initial_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    std::promise<void> completion_ready_promise;
    std::shared_future<void> completion_ready =
        completion_ready_promise.get_future().share();
    std::atomic_bool completion_ready_signaled = false;
    shell->RegisterSourceLoadCompletionReadyCallback(
        [&completion_ready_promise,
         &completion_ready_signaled]() {
            if (!completion_ready_signaled.exchange(true)) {
                completion_ready_promise.set_value();
            }
        });

    const specforge::SourceCollectionSessionResult first = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool stale_completion_published =
        completion_ready.wait_for(2s) ==
        std::future_status::ready;

    const specforge::SourceCollectionSessionResult second = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool row_two_started =
        row_two_entered.wait_for(2s) == std::future_status::ready;
    const std::thread::id drain_thread = std::this_thread::get_id();
    Access::Drain(*shell);

    const bool stale_snapshot_retired =
        stale_snapshot_destroyed.wait_for(2s) == std::future_status::ready;
    const std::thread::id retirement_thread = stale_snapshot_retired
        ? stale_snapshot_destroyed.get()
        : std::thread::id{};
    const specforge::SpectrumSnapshotHandle snapshot_after_stale_drain =
        Access::Session(*shell).CurrentSampleSnapshot();
    const bool no_load_error = Access::LoadError(*shell).empty();
    const bool new_ticket_retained = Access::PendingLoadCount(*shell) == 1;
    const bool new_session_pending_retained =
        Access::Session(*shell).CancelPendingSampleNavigation(path, 2);

    release_row_two_promise.set_value();
    shell.reset();
    std::filesystem::remove(path);

    Require(first.follow_up_spectrum_index == 1, "row 1 should create the old Shell ticket");
    Require(
        stale_completion_published,
        "row 1 completion should be published before supersession");
    Require(
        second.follow_up_spectrum_index == 2 && row_two_started,
        "new navigation should invalidate row 1 and start the row 2 worker");
    Require(
        snapshot_after_stale_drain == initial_snapshot &&
            snapshot_after_stale_drain->collection.current_index == 0,
        "the already-published stale completion must not replace the committed snapshot");
    Require(
        new_ticket_retained && new_session_pending_retained,
        "rejecting the stale completion must retain both the new Shell ticket and session pending row");
    Require(no_load_error, "rejecting a stale successful completion must not publish an error");
    Require(
        stale_snapshot_retired && retirement_thread != drain_thread,
        "the rejected stale prepared snapshot should be reclaimed off the drain thread");
}

void TestRealDrainPreservesWorkflowChangesMadeWhileFullPlanWaits()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_stale_plan_drain.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "stale plan drain fixture should be created");
        stream << "fixture";
    }

    std::promise<void> decoder_entered_promise;
    std::shared_future<void> decoder_entered = decoder_entered_promise.get_future().share();
    std::promise<void> release_decoder_promise;
    std::shared_future<void> release_decoder = release_decoder_promise.get_future().share();
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&decoder_entered_promise,
         release_decoder](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            decoder_entered_promise.set_value();
            WaitForRelease(
                release_decoder,
                canceled,
                "timed out waiting to release the stale-plan decoder");
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path, "older-context"),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));

    const specforge::SourceCollectionSessionResult navigation = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool navigation_queued = navigation.follow_up_spectrum_index == 1;
    const bool decoder_started = decoder_entered.wait_for(2s) == std::future_status::ready;
    const specforge::SourceCollectionSessionResult labeling_started = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::StartOrResumeTemporaryLabelingTask()));
    const specforge::SourceCollectionSessionResult label_added = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::ChangeActiveSampleWorkflow(
            specforge::ActiveSampleWorkflowIntent::UpsertActiveLabel(
                specforge::SampleLabelDefinition{7, "live", 'l'})));
    release_decoder_promise.set_value();

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool completion_drained = false;
    while (std::chrono::steady_clock::now() < deadline) {
        Access::Drain(*shell);
        const specforge::SpectrumSnapshotHandle snapshot =
            Access::Session(*shell).CurrentSampleSnapshot();
        completion_drained = snapshot && snapshot->collection.current_index == 1 &&
            Access::PendingLoadCount(*shell) == 0;
        if (completion_drained) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const specforge::SourceCollectionSessionView view = Access::Session(*shell).View();
    const bool live_workflow_preserved =
        view.labeling.has_active_task &&
        specforge::ContainsSampleLabelCode(view.labeling.label_set, 7);
    const bool no_load_error = Access::LoadError(*shell).empty();
    shell.reset();
    std::filesystem::remove(path);

    Require(navigation_queued && decoder_started, "the full-plan worker should wait on row 1");
    Require(
        labeling_started.action.workflow_changed && label_added.changed,
        "the UI should commit newer workflow intent while the full plan waits");
    Require(completion_drained, "production DrainSourceLoads should commit the full plan snapshot");
    Require(
        live_workflow_preserved,
        "the drained older full plan must not replace workflow intent committed while it waited");
    Require(no_load_error, "live workflow reconciliation should complete without a load error");
}

void TestRealDrainRequeuesReconciledTargetAndRetiresIntermediateSnapshotOffThread()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_reentrant_drain.npy");
    const std::optional<std::filesystem::path> annotation_path =
        specforge::SourceCollectionCompanionAnnotationPath(path);
    Require(annotation_path.has_value(), "NPY drain fixture should expose its companion path");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "reentrant drain source fixture should be created");
        stream << "fixture";
    }
    std::string annotation_error;
    Require(
        SaveAnnotationFixture(*annotation_path, {1, 1, 0}, &annotation_error),
        annotation_error.empty() ? "initial drain annotation should save" : annotation_error);

    specforge::SourceCollectionSession session = MakePreparedDeferredSession(path);
    std::optional<specforge::SampleAnnotationResult> initial_annotation =
        specforge::test_support::LegacyFixtureIo{}.Load(
            *annotation_path,
            3,
            &annotation_error);
    Require(initial_annotation.has_value(), "initial drain annotation should load");
    const std::string filter_source_id =
        specforge::BuildAnnotationFilterSourceId(*initial_annotation);
    Require(
        session.Submit(specforge::SourceCollectionSessionIntent::EditSourceCollection(
                           specforge::SourceCollectionIntent::AddReadOnlyAnnotationResult(
                               *annotation_path)))
            .loaded,
        "reentrant drain fixture should attach its annotation");
    (void)session.Submit(specforge::SourceCollectionSessionIntent::ApplySampleFiltering(
        specforge::SampleFilteringIntent::AddSource(filter_source_id)));
    (void)session.Submit(specforge::SourceCollectionSessionIntent::ApplySampleFiltering(
        specforge::SampleFilteringIntent::SetFilterValueSelected(
            filter_source_id,
            "1",
            true)));

    std::promise<void> first_decode_entered_promise;
    std::shared_future<void> first_decode_entered =
        first_decode_entered_promise.get_future().share();
    std::promise<void> release_first_decode_promise;
    std::shared_future<void> release_first_decode =
        release_first_decode_promise.get_future().share();
    std::promise<void> row_two_entered_promise;
    std::shared_future<void> row_two_entered = row_two_entered_promise.get_future().share();
    std::promise<void> release_row_two_promise;
    std::shared_future<void> release_row_two = release_row_two_promise.get_future().share();
    auto intermediate_destroyed_promise =
        std::make_shared<std::promise<std::thread::id>>();
    std::future<std::thread::id> intermediate_destroyed =
        intermediate_destroyed_promise->get_future();
    std::atomic_bool initial_open_failed = false;
    std::atomic_int row_one_decode_count = 0;
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&first_decode_entered_promise,
         release_first_decode,
         &row_two_entered_promise,
         release_row_two,
         intermediate_destroyed_promise,
         &initial_open_failed,
         &row_one_decode_count](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            if (index == 0 &&
                !initial_open_failed.exchange(true)) {
                throw std::runtime_error(
                    "previous source load failure");
            }
            if (index == 1) {
                if (row_one_decode_count.fetch_add(1) == 0) {
                    first_decode_entered_promise.set_value();
                    WaitForRelease(
                        release_first_decode,
                        canceled,
                        "timed out waiting to release the first retry decoder");
                    return MakeSnapshot(source, index);
                }
                return MakeSnapshotWithDestructionProbe(
                    source,
                    index,
                    intermediate_destroyed_promise);
            }
            if (index == 2) {
                row_two_entered_promise.set_value();
                WaitForRelease(
                    release_row_two,
                    canceled,
                    "timed out waiting to release retry row two");
            }
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        std::move(session),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));

    shell->OpenSource(path);
    const auto initial_failure_deadline =
        std::chrono::steady_clock::now() + 2s;
    bool initial_failure_visible = false;
    while (std::chrono::steady_clock::now() <
           initial_failure_deadline) {
        Access::Drain(*shell);
        initial_failure_visible =
            Access::LoadError(*shell).find(
                "previous source load failure") !=
                std::string_view::npos &&
            Access::PendingLoadCount(*shell) == 0;
        if (initial_failure_visible) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }

    const specforge::SourceCollectionSessionResult navigation = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool navigation_queued = navigation.follow_up_spectrum_index == 1;
    const bool first_decode_started =
        first_decode_entered.wait_for(2s) == std::future_status::ready;
    annotation_error.clear();
    const bool annotation_changed =
        SaveAnnotationFixture(*annotation_path, {0, 0, 1}, &annotation_error);
    release_first_decode_promise.set_value();

    const auto requeue_deadline = std::chrono::steady_clock::now() + 2s;
    bool row_two_requeued = false;
    while (std::chrono::steady_clock::now() < requeue_deadline) {
        Access::Drain(*shell);
        row_two_requeued =
            row_two_entered.wait_for(0ms) == std::future_status::ready;
        if (row_two_requeued) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const specforge::SpectrumSnapshotHandle snapshot_while_requeued =
        Access::Session(*shell).CurrentSampleSnapshot();
    const specforge::SourceCollectionSessionView view_while_requeued =
        Access::Session(*shell).View();
    const bool failure_retained_while_requeued =
        Access::LoadError(*shell).find(
            "previous source load failure") !=
        std::string_view::npos;
    const std::thread::id caller_thread = std::this_thread::get_id();
    const bool intermediate_retired =
        intermediate_destroyed.wait_for(2s) == std::future_status::ready;
    const std::thread::id retirement_thread = intermediate_retired
        ? intermediate_destroyed.get()
        : std::thread::id{};
    release_row_two_promise.set_value();

    const auto commit_deadline = std::chrono::steady_clock::now() + 2s;
    bool final_row_committed = false;
    while (std::chrono::steady_clock::now() < commit_deadline) {
        Access::Drain(*shell);
        const specforge::SpectrumSnapshotHandle snapshot =
            Access::Session(*shell).CurrentSampleSnapshot();
        final_row_committed = snapshot && snapshot->collection.current_index == 2 &&
            Access::PendingLoadCount(*shell) == 0;
        if (final_row_committed) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const specforge::SourceCollectionSessionView final_view = Access::Session(*shell).View();
    const bool no_load_error = Access::LoadError(*shell).empty();
    shell.reset();
    std::filesystem::remove(*annotation_path);
    std::filesystem::remove(path);

    Require(
        initial_failure_visible,
        "the retry fixture should retain its previous source failure");
    Require(navigation_queued && first_decode_started, "row 1 should enter the real drain worker");
    Require(annotation_changed, "the annotation context should change while row 1 is decoded");
    Require(row_two_requeued, "draining row 1 should enqueue the reconciled row 2 follow-up");
    Require(
        failure_retained_while_requeued,
        "an intermediate loaded result must not clear the source failure before its follow-up succeeds");
    Require(
        snapshot_while_requeued && snapshot_while_requeued->collection.current_index == 0,
        "the reentrant row 2 load must keep the complete row 0 snapshot visible");
    Require(
        view_while_requeued.filter.evaluation.included_count == 2 &&
            view_while_requeued.navigation.sequence_count == 2 &&
            view_while_requeued.navigation.current_sample_in_filter,
        "the real drain must keep the old context projections visible while row 2 loads");
    Require(
        intermediate_retired && retirement_thread != caller_thread,
        "the rejected row 1 prepared snapshot should retire off the drain caller thread");
    Require(final_row_committed, "the reentrant row 2 completion should commit through the real drain");
    Require(
        final_view.filter.evaluation.included_count == 1 &&
            final_view.navigation.sequence_count == 1 &&
            final_view.navigation.current_index == 2,
        "the final drain should publish the changed context and row 2 snapshot together");
    Require(no_load_error, "reentrant reconciliation should not publish a load error");
}

void TestDeferredRestoreCompletionPreservesUnrelatedNavigationTicket()
{
    const std::filesystem::path path = UniqueTempPath("_late.csv");
    const std::filesystem::path other_path = UniqueTempPath("_other.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "late completion fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionSession session =
        MakePreparedDeferredSession(path);
    const specforge::SpectrumSnapshotHandle
        initial_snapshot = session.CurrentSampleSnapshot();

    const specforge::SpectrumSnapshotHandle other_snapshot = MakeSnapshot(other_path, 0);
    specforge::SourceCollectionContext other_context;
    other_context.identity = {"shell-other", "other", "other-source", "other-context", 3};
    other_context.manifest.sample_names = {"one", "two", "three"};
    specforge::PreparedSampleWorkflowState other_workflow =
        specforge::PrepareSampleWorkflowState(
            *other_snapshot,
            other_context,
            0,
            {{}, {}});
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const std::filesystem::path& source,
           std::size_t index,
           const auto&) {
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::
                SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    specforge::SourceCollectionActivationTransaction activation(
        session,
        specforge::MakeSourceCollectionLoadQueueForTesting(
            std::move(dependencies)));
    std::promise<void> completion_ready_promise;
    std::shared_future<void> completion_ready =
        completion_ready_promise.get_future().share();
    specforge::ShellUiTestAccess::
        RegisterCompletionReadyCallback(
            activation,
            [&completion_ready_promise]() {
                completion_ready_promise.set_value();
            });

    const specforge::SourceCollectionSessionResult
        navigation = activation.Submit(
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    Require(
        navigation.follow_up_spectrum_index == 1,
        "source A should issue the async row 1 follow-up used by the Shell ticket");

    const bool completion_published =
        completion_ready.wait_for(2s) ==
        std::future_status::ready;
    const specforge::SourceCollectionSessionResult
        switched_to_b = session.OpenPreparedSource(
            other_path,
            0,
            other_snapshot,
            std::move(other_context),
            std::move(other_workflow));
    const bool session_navigation_preserved =
        switched_to_b.loaded &&
        !switched_to_b.canceled_source_follow_up_path;
    const bool lifecycle_ticket_preserved =
        specforge::ShellUiTestAccess::
            PendingLoadCount(activation) == 1;

    const specforge::SourceCollectionSessionResult
        switched_back_to_a = activation.Submit(
            specforge::SourceCollectionSessionIntent::
                EditSourceCollection(
                    specforge::SourceCollectionIntent::
                        SwitchActive(0)));
    const bool pending_intent_restored =
        switched_back_to_a.follow_up_spectrum_index == 1 &&
        session.CurrentSampleSnapshot() ==
            initial_snapshot;
    const bool existing_ticket_reused =
        specforge::ShellUiTestAccess::
            PendingLoadCount(activation) == 1;
    bool row_one_committed = false;
    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)activation.Drain(false);
        const auto snapshot =
            session.CurrentSampleSnapshot();
        row_one_committed =
            snapshot &&
            snapshot->collection.current_index == 1 &&
            !activation.status().loading;
        if (row_one_committed) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    std::filesystem::remove(path);

    Require(
        session_navigation_preserved,
        "source B's deferred completion must not cancel source A's navigation intent");
    Require(
        completion_published &&
            lifecycle_ticket_preserved,
        "source B's completion must retain source A's lifecycle ticket");
    Require(
        pending_intent_restored,
        "switching back to A should expose its still-pending row 1 intent");
    Require(
        existing_ticket_reused,
        "restoring A should reuse its existing row 1 Shell ticket instead of restarting the worker");
    Require(
        row_one_committed,
        "source A should finish the user-requested row 1 navigation");
}

void TestDeferredRestoreFollowUpFailureClearsPendingAndAllowsRetry()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path = UniqueTempPath("_failed_restore.csv");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "deferred restore failure fixture should be created");
        stream << "fixture";
    }

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const std::filesystem::path&,
           std::size_t,
           const auto&) -> specforge::SpectrumSnapshotHandle {
            throw std::runtime_error("expected deferred restore follow-up failure");
        };
    dependencies.workflow_cache_loader = [](const auto&, const std::function<void()>& checkpoint) {
        checkpoint();
        return specforge::SampleWorkflowPreparationCacheBundle{};
    };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(std::move(dependencies)));

    const specforge::SourceCollectionSessionResult navigation = Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool deferred_follow_up_created = navigation.follow_up_spectrum_index == 1;

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool failure_drained = false;
    while (std::chrono::steady_clock::now() < deadline) {
        Access::Drain(*shell);
        failure_drained = !Access::LoadError(*shell).empty() &&
            Access::PendingLoadCount(*shell) == 0;
        if (failure_drained) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const specforge::SpectrumSnapshotHandle retained_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    const specforge::SourceCollectionSessionResult retry = Access::Session(*shell).Submit(
        specforge::SourceCollectionSessionIntent::UpdateSampleNavigation(
            specforge::SampleNavigationIntent::Move(
                specforge::SampleNavigationRequest::Next())));
    const bool retry_created = retry.follow_up_spectrum_index == 1;
    const bool retry_canceled =
        Access::Session(*shell).CancelPendingSampleNavigation(path, 1);
    shell.reset();
    std::filesystem::remove(path);

    Require(
        deferred_follow_up_created,
        "restore navigation should produce a deferred row 1 follow-up");
    Require(
        failure_drained,
        "production DrainSourceLoads should consume the failed DeferredRestore ticket");
    Require(
        retained_snapshot && retained_snapshot->collection.current_index == 0,
        "a failed restore follow-up should retain the complete committed row 0 snapshot");
    Require(
        retry_created && retry_canceled,
        "retrying the failed row must create a new worker follow-up instead of being deduplicated");
}

void TestDeferredRestorePreservesSavedActiveSourceAfterLaterCompletion()
{
    using Access = specforge::ShellUiTestAccess;
    const std::array<std::filesystem::path, 3> source_paths{
        UniqueTempPath("_restore_a.csv"),
        UniqueTempPath("_restore_b.csv"),
        UniqueTempPath("_restore_c.csv"),
    };
    const SourceSessionCachePaths cache_paths{
        UniqueTempPath("_source_session.json"),
        UniqueTempPath("_navigation.json"),
        UniqueTempPath("_labeling.json"),
        UniqueTempPath("_workflow.json"),
    };
    const std::filesystem::path explicit_source_path =
        UniqueTempPath("_restore_explicit.csv");
    std::filesystem::remove(cache_paths.source_session);
    std::filesystem::remove(cache_paths.navigation);
    std::filesystem::remove(cache_paths.labeling);
    std::filesystem::remove(cache_paths.workflow);
    for (const std::filesystem::path& path : source_paths) {
        std::filesystem::remove(path);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(stream.good(), "deferred restore source fixture should be created");
        stream << "fixture";
    }
    {
        std::ofstream stream(
            explicit_source_path,
            std::ios::binary | std::ios::trunc);
        Require(
            stream.good(),
            "explicit source fixture should be created");
        stream << "fixture";
    }

    {
        specforge::SourceCollectionSession saved_session =
            MakeCachedSession(cache_paths);
        for (const std::filesystem::path& path : source_paths) {
            OpenPreparedFixtureSource(saved_session, path);
        }
        (void)saved_session.Submit(
            specforge::SourceCollectionSessionIntent::EditSourceCollection(
                specforge::SourceCollectionIntent::SwitchActive(1)));
        const specforge::SourceCollectionSessionView saved_view =
            saved_session.View();
        Require(
            saved_view.current_source_index &&
                *saved_view.current_source_index == 1 &&
                specforge::SourcePathIdentityKey(
                    saved_view.sources[*saved_view.current_source_index].path) ==
                    specforge::SourcePathIdentityKey(source_paths[1]),
            "fixture should activate source B before saving");
        Require(
            saved_session.FlushStateCaches(),
            "source-session fixture should flush successfully");
    }

    const specforge::SourceCollectionSessionStateCache saved_cache =
        specforge::LoadSourceCollectionSessionStateCache(
            cache_paths.source_session)
            .cache;
    Require(
        saved_cache.sources.size() == 3 &&
            saved_cache.active_source_index &&
            *saved_cache.active_source_index == 1 &&
            specforge::SourcePathIdentityKey(
                saved_cache.sources[*saved_cache.active_source_index].path) ==
                specforge::SourcePathIdentityKey(source_paths[1]),
        "save phase should persist source B as the active source");

    std::promise<void> release_a_promise;
    std::shared_future<void> release_a =
        release_a_promise.get_future().share();
    std::promise<void> release_c_promise;
    std::shared_future<void> release_c =
        release_c_promise.get_future().share();
    std::array<std::promise<void>, 3> decoder_entered_promises;
    std::array<std::shared_future<void>, 3> decoder_entered{
        decoder_entered_promises[0].get_future().share(),
        decoder_entered_promises[1].get_future().share(),
        decoder_entered_promises[2].get_future().share(),
    };
    std::array<std::atomic_bool, 3> decoder_entered_once{};
    std::promise<void> source_b_decoded_promise;
    std::shared_future<void> source_b_decoded =
        source_b_decoded_promise.get_future().share();
    std::atomic_bool source_b_decoded_once = false;

    specforge::SourceCollectionPreparationAdapters dependencies =
        MakeFixtureLoadDependencies(cache_paths);
    dependencies.snapshot_loader =
        [&](const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            const auto match = std::find_if(
                source_paths.begin(),
                source_paths.end(),
                [&source](const std::filesystem::path& candidate) {
                    return specforge::SourcePathIdentityKey(candidate) ==
                           specforge::SourcePathIdentityKey(source);
                });
            Require(
                match != source_paths.end(),
                "deferred restore should only load isolated fixture sources");
            const std::size_t source_index =
                static_cast<std::size_t>(
                    std::distance(source_paths.begin(), match));
            if (!decoder_entered_once[source_index].exchange(
                    true,
                    std::memory_order_relaxed)) {
                decoder_entered_promises[source_index].set_value();
            }
            if (source_index == 0) {
                WaitForRelease(
                    release_a,
                    canceled,
                    "source A decoder should be released after source B");
            } else if (source_index == 2) {
                WaitForRelease(
                    release_c,
                    canceled,
                    "source C decoder should be released last");
            }
            specforge::SpectrumSnapshotHandle snapshot =
                MakeSnapshot(source, index);
            if (source_index == 1 &&
                !source_b_decoded_once.exchange(
                    true,
                    std::memory_order_relaxed)) {
                source_b_decoded_promise.set_value();
            }
            return snapshot;
        };
    std::unique_ptr<specforge::ShellUi> shell =
        MakeDeferredShell(cache_paths, std::move(dependencies));

    const bool source_b_finished_first =
        decoder_entered[0].wait_for(2s) == std::future_status::ready &&
        decoder_entered[2].wait_for(2s) == std::future_status::ready &&
        source_b_decoded.wait_for(2s) == std::future_status::ready;
    if (!source_b_finished_first) {
        release_a_promise.set_value();
        release_c_promise.set_value();
    }
    Require(
        source_b_finished_first,
        "source B should decode while source A and C remain blocked");
    const specforge::ShellWindowTitleView
        active_restore_loading = shell->WindowTitleView();
    Require(
        active_restore_loading.loading_source_path != nullptr &&
            specforge::SourcePathIdentityKey(
                *active_restore_loading.loading_source_path) ==
                specforge::SourcePathIdentityKey(
                    source_paths[1]),
        "deferred restore should project only its saved active source while multiple sources are loading");

    release_a_promise.set_value();
    const auto source_b_activation_deadline =
        std::chrono::steady_clock::now() + 2s;
    bool source_b_activated = false;
    while (std::chrono::steady_clock::now() <
           source_b_activation_deadline) {
        Access::Drain(*shell);
        const specforge::SourceCollectionSessionView view =
            Access::Session(*shell).View();
        source_b_activated =
            view.sources.size() == 2 &&
            view.current_source_index &&
            specforge::SourcePathIdentityKey(
                view.sources[*view.current_source_index].path) ==
                specforge::SourcePathIdentityKey(source_paths[1]);
        if (source_b_activated) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    release_c_promise.set_value();
    Require(
        source_b_activated,
        "source B should be active before the later source C completion");
    const specforge::ShellWindowTitleView
        active_source_with_inactive_restore_pending =
            shell->WindowTitleView();
    Require(
        active_source_with_inactive_restore_pending
                .loading_source_path == nullptr &&
            active_source_with_inactive_restore_pending
                .source_path != nullptr &&
            specforge::SourcePathIdentityKey(
                *active_source_with_inactive_restore_pending
                     .source_path) ==
                specforge::SourcePathIdentityKey(
                    source_paths[1]),
        "an inactive deferred source should not replace the restored active source projection");

    const auto restore_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < restore_deadline) {
        Access::Drain(*shell);
        if (Access::PendingLoadCount(*shell) == 0) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    const specforge::SourceCollectionSessionView restored_view =
        Access::Session(*shell).View();
    Require(
        restored_view.sources.size() == 3 &&
            CurrentSourceMatches(
                Access::Session(*shell),
                source_paths[1]),
        "deferred restore should reactivate saved source B after source C completes");

    Require(
        Access::Session(*shell).FlushStateCaches(),
        "restored source-session cache should flush successfully");
    const specforge::SourceCollectionSessionStateCache flushed_cache =
        specforge::LoadSourceCollectionSessionStateCache(
            cache_paths.source_session)
            .cache;
    Require(
        flushed_cache.active_source_index &&
            *flushed_cache.active_source_index < flushed_cache.sources.size() &&
            specforge::SourcePathIdentityKey(
                flushed_cache.sources[*flushed_cache.active_source_index].path) ==
                specforge::SourcePathIdentityKey(source_paths[1]),
        "flush after deferred restore should retain source B as active");

    shell.reset();
    shell = MakeDeferredShell(
        cache_paths,
        MakeFixtureLoadDependencies(cache_paths));
    Require(
        DrainAllSourceLoads(*shell),
        "second deferred restore should finish");
    const specforge::SourceCollectionSessionView second_restored_view =
        Access::Session(*shell).View();
    Require(
        second_restored_view.sources.size() == 3 &&
            CurrentSourceMatches(
                Access::Session(*shell),
                source_paths[1]),
        "a second deferred Shell should restore source B from the flushed cache");
    shell.reset();

    shell = MakeDeferredShell(
        cache_paths,
        MakeFixtureLoadDependencies(cache_paths));
    shell->OpenSource(explicit_source_path);
    Require(
        DrainAllSourceLoads(*shell),
        "deferred restore with an explicit open should finish");
    Require(
        CurrentSourceMatches(
            Access::Session(*shell),
            explicit_source_path),
        "an explicit open during restore should override the saved source B intent");
    shell.reset();

    for (const std::filesystem::path& path : source_paths) {
        std::filesystem::remove(path);
    }
    std::filesystem::remove(explicit_source_path);
    std::filesystem::remove(cache_paths.source_session);
    std::filesystem::remove(cache_paths.navigation);
    std::filesystem::remove(cache_paths.labeling);
    std::filesystem::remove(cache_paths.workflow);
}

void TestIdlePrefetchIsConsumedBySecondForwardNavigation()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path =
        UniqueTempPath("_prefetch_consumed.csv");
    {
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::trunc);
        stream << "fixture";
    }
    std::array<std::atomic_int, 3> decoder_calls{};
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&decoder_calls](
            const std::filesystem::path& source,
            std::size_t index,
            const auto&) {
            ++decoder_calls.at(index);
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::
                SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            MakePreparedDeferredSession(path),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                std::move(dependencies)));
    Access::EnableNavigationTracing(*shell, 300);

    const specforge::SourceCollectionSessionResult first =
        Access::SubmitNavigation(
            *shell,
            specforge::SourceCollectionSessionIntent::
                UpdateSampleNavigation(
                    specforge::SampleNavigationIntent::Move(
                        specforge::
                            SampleNavigationRequest::Next())),
            specforge::NavigationLatencyInputKind::UiNext);
    Require(
        first.follow_up_spectrum_index == 1,
        "first next should queue raw row 1");
    const auto first_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           first_deadline) {
        Access::Drain(*shell);
        const auto snapshot =
            Access::Session(*shell)
                .CurrentSampleSnapshot();
        if (snapshot &&
            snapshot->collection.current_index == 1) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    const auto first_reports =
        Access::CompleteFramePresentation(
            *shell,
            300);
    Require(
        first_reports.size() == 1 &&
            first_reports.front().cache_kind ==
                specforge::
                    NavigationSnapshotCacheKind::None,
        "first next should remain a foreground decode");

    std::vector<specforge::NavigationPrefetchReport>
        prefetch_reports;
    const auto prefetch_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           prefetch_deadline) {
        Access::Drain(*shell);
        auto reports =
            Access::TakePrefetchReports(*shell);
        prefetch_reports.insert(
            prefetch_reports.end(),
            reports.begin(),
            reports.end());
        if (std::any_of(
                prefetch_reports.begin(),
                prefetch_reports.end(),
                [](const auto& report) {
                    return report.outcome ==
                        specforge::
                            NavigationPrefetchOutcome::
                                Completed;
                })) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    Access::EnableNavigationTracing(*shell, 301);
    const specforge::SourceCollectionSessionResult second =
        Access::SubmitNavigation(
            *shell,
            specforge::SourceCollectionSessionIntent::
                UpdateSampleNavigation(
                    specforge::SampleNavigationIntent::Move(
                        specforge::
                            SampleNavigationRequest::Next())),
            specforge::NavigationLatencyInputKind::UiNext);
    Require(
        second.follow_up_spectrum_index == 2,
        "second next should queue raw row 2");
    const auto second_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           second_deadline) {
        Access::Drain(*shell);
        const auto snapshot =
            Access::Session(*shell)
                .CurrentSampleSnapshot();
        if (snapshot &&
            snapshot->collection.current_index == 2 &&
            Access::PendingLoadCount(*shell) == 0) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    const auto second_reports =
        Access::CompleteFramePresentation(
            *shell,
            301);
    auto consumed_reports =
        Access::TakePrefetchReports(*shell);
    prefetch_reports.insert(
        prefetch_reports.end(),
        consumed_reports.begin(),
        consumed_reports.end());

    Require(
        second_reports.size() == 1 &&
            second_reports.front().cache_hit &&
            second_reports.front().cache_kind ==
                specforge::
                    NavigationSnapshotCacheKind::
                        Prefetch,
        "second same-direction navigation should identify a prefetch cache hit");
    Require(
        second_reports.front().attempts.size() == 1 &&
            second_reports.front().attempts[0]
                    .snapshot_load_finished_ns -
                second_reports.front().attempts[0]
                    .snapshot_load_started_ns <
                5'000'000,
        "prefetch consumption should keep demand-side decode near zero");
    Require(
        decoder_calls[1].load() == 1 &&
            decoder_calls[2].load() == 1,
        "demand must consume row 2 without invoking its decoder again");
    Require(
        std::any_of(
            prefetch_reports.begin(),
            prefetch_reports.end(),
            [](const auto& report) {
                return report.outcome ==
                    specforge::
                        NavigationPrefetchOutcome::Completed;
            }) &&
            std::any_of(
                prefetch_reports.begin(),
                prefetch_reports.end(),
                [](const auto& report) {
                    return report.outcome ==
                        specforge::
                            NavigationPrefetchOutcome::Consumed;
                }),
        "prefetch observability should record completed and consumed");
    shell.reset();
    std::filesystem::remove(path);
}

void TestPublishedPrefetchBecomesStaleAfterQueryInput()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path =
        UniqueTempPath("_prefetch_stale.csv");
    {
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::trunc);
        stream << "fixture";
    }
    std::array<std::atomic_int, 3> decoder_calls{};
    std::promise<void> prefetch_entered_promise;
    std::shared_future<void> prefetch_entered =
        prefetch_entered_promise.get_future().share();
    std::promise<void> release_prefetch_promise;
    std::shared_future<void> release_prefetch =
        release_prefetch_promise.get_future().share();
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&decoder_calls,
         &prefetch_entered_promise,
         release_prefetch](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            ++decoder_calls.at(index);
            if (index == 2) {
                prefetch_entered_promise.set_value();
                WaitForRelease(
                    release_prefetch,
                    canceled,
                    "timed out waiting to release published prefetch");
            }
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::
                SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            MakePreparedDeferredSession(path),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                std::move(dependencies)));

    (void)Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::Move(
                    specforge::
                        SampleNavigationRequest::Next())),
        specforge::NavigationLatencyInputKind::UiNext);
    const auto activation_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           activation_deadline) {
        Access::Drain(*shell);
        if (Access::PrefetchActive(*shell)) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    Require(
        prefetch_entered.wait_for(2s) ==
            std::future_status::ready,
        "prefetch worker should start before publication");
    std::promise<void> prefetch_ready_promise;
    std::shared_future<void> prefetch_ready =
        prefetch_ready_promise.get_future().share();
    std::atomic_bool prefetch_ready_signaled = false;
    shell->RegisterSourceLoadCompletionReadyCallback(
        [&prefetch_ready_promise,
         &prefetch_ready_signaled]() {
            if (!prefetch_ready_signaled.exchange(true)) {
                prefetch_ready_promise.set_value();
            }
        });
    release_prefetch_promise.set_value();
    Require(
        prefetch_ready.wait_for(2s) ==
            std::future_status::ready,
        "prefetch fixture should hold one already-published completion");

    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::
                    SetSampleNameQuery("gamma")));
    Access::Drain(*shell);
    const auto reports =
        Access::TakePrefetchReports(*shell);

    Require(
        std::any_of(
            reports.begin(),
            reports.end(),
            [](const auto& report) {
                return report.outcome ==
                    specforge::
                        NavigationPrefetchOutcome::Stale;
            }),
        "query input should invalidate an already-published old prefetch");
    Require(
        Access::Session(*shell)
                .CurrentSampleSnapshot()
                ->collection.current_index == 1,
        "stale prefetch must not activate or move the committed index");
    shell.reset();
    std::filesystem::remove(path);
}

void TestCanceledPrefetchReportsOnlyAfterWorkerExit()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path =
        UniqueTempPath("_prefetch_canceled.csv");
    {
        std::ofstream stream(
            path,
            std::ios::binary | std::ios::trunc);
        stream << "fixture";
    }
    std::promise<void> prefetch_entered_promise;
    std::shared_future<void> prefetch_entered =
        prefetch_entered_promise.get_future().share();
    std::promise<void> cancellation_observed_promise;
    std::shared_future<void> cancellation_observed =
        cancellation_observed_promise.get_future().share();
    std::promise<void> release_canceled_worker_promise;
    std::shared_future<void> release_canceled_worker =
        release_canceled_worker_promise.get_future().share();
    std::atomic_bool entered_once = false;
    std::atomic_int64_t decoder_returned_ns = 0;
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&](const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            if (index == 2) {
                if (!entered_once.exchange(
                        true,
                        std::memory_order_relaxed)) {
                    prefetch_entered_promise.set_value();
                }
                while (!canceled()) {
                    std::this_thread::sleep_for(1ms);
                }
                cancellation_observed_promise.set_value();
                release_canceled_worker.wait();
                decoder_returned_ns.store(
                    std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                        specforge::
                            NavigationLatencyTrace::Now()
                                .time_since_epoch())
                        .count(),
                    std::memory_order_relaxed);
            }
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::
                SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            MakePreparedDeferredSession(path),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                std::move(dependencies)));

    (void)Access::SubmitNavigation(
        *shell,
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::Move(
                    specforge::
                        SampleNavigationRequest::Next())),
        specforge::NavigationLatencyInputKind::UiNext);
    const auto activation_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           activation_deadline) {
        Access::Drain(*shell);
        if (prefetch_entered.wait_for(0ms) ==
            std::future_status::ready) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::
            UpdateSampleNavigation(
                specforge::SampleNavigationIntent::
                    SetSampleNameQuery("gamma")));
    const bool cancel_seen =
        cancellation_observed.wait_for(2s) ==
        std::future_status::ready;
    std::vector<specforge::NavigationPrefetchReport>
        reports = Access::TakePrefetchReports(*shell);
    const bool canceled_reported_before_exit =
        std::any_of(
            reports.begin(),
            reports.end(),
            [](const auto& report) {
                return report.outcome ==
                    specforge::
                        NavigationPrefetchOutcome::
                            Canceled;
            });

    release_canceled_worker_promise.set_value();
    const auto terminal_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           terminal_deadline) {
        Access::Drain(*shell);
        auto drained =
            Access::TakePrefetchReports(*shell);
        reports.insert(
            reports.end(),
            drained.begin(),
            drained.end());
        if (std::any_of(
                reports.begin(),
                reports.end(),
                [](const auto& report) {
                    return report.outcome ==
                        specforge::
                            NavigationPrefetchOutcome::
                                Canceled;
                })) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    const auto canceled_report = std::find_if(
        reports.begin(),
        reports.end(),
        [](const auto& report) {
            return report.outcome ==
                specforge::
                    NavigationPrefetchOutcome::Canceled;
        });
    const bool terminal_after_decoder =
        canceled_report != reports.end() &&
        std::chrono::duration_cast<
            std::chrono::nanoseconds>(
            canceled_report->terminal_at
                .time_since_epoch())
                .count() >=
            decoder_returned_ns.load(
                std::memory_order_relaxed);
    const bool cancellation_timestamps_ordered =
        canceled_report != reports.end() &&
        canceled_report->scheduled_at <=
            canceled_report->cancel_requested_at &&
        canceled_report->cancel_requested_at <=
            canceled_report->terminal_at;

    Require(
        cancel_seen,
        "the blocked prefetch worker should observe cancellation");
    Require(
        !canceled_reported_before_exit,
        "canceled must not be recorded at request time while the worker is still unwinding");
    Require(
        terminal_after_decoder &&
            cancellation_timestamps_ordered,
        "canceled should preserve schedule, request, and worker-terminal order");
    shell.reset();
    std::filesystem::remove(path);
}

void TestAutomationSourceObservationHidesActivationToken()
{
    using Access = specforge::ShellUiTestAccess;
    using State = specforge::ShellAutomationSourceOutcome::State;
    const auto path = UniqueTempPath("_automation_source_observation.csv");
    WriteFixture(path);
    auto shell = Access::Create(
        MakePreparedDeferredSession(path),
        specforge::MakeSourceCollectionLoadQueueForTesting(
            MakeFixtureLoadDependencies({{}, {}, {}, {}})));
    auto observe = shell->BeginSourceOpenForAutomation(path);
    Require(DrainAllSourceLoads(*shell), "source observation fixture should finish loading");
    Require(observe().state == State::Pending,
        "source observation must remain pending until successful presentation");
    Access::SubmitSpectrumDraw(*shell, 400,
        Access::Session(*shell).CurrentSampleSnapshot(), 17);
    shell->PresentFrame(400, {});
    Require(observe().state == State::Pending,
        "failed Present must not complete the opaque source operation");
    const specforge::NavigationLatencyPresentation wrong{
        18, specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(400, std::span(&wrong, 1));
    Require(observe().state == State::Pending,
        "another viewport cannot complete the opaque source operation");
    const specforge::NavigationLatencyPresentation presented{
        17, specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(400, std::span(&presented, 1));
    const auto outcome = observe();
    Require(outcome.state == State::Succeeded && outcome.source_path == path &&
        outcome.source_id == shell->AutomationView().source_id &&
        outcome.spectrum_count == 3 && outcome.spectrum_index == 0,
        "source observation should expose only the completed source and sample facts");
    observe = {};
    shell.reset();
    std::filesystem::remove(path);
}

void TestAutomationPresentedViewAdvancesOnlyAfterSuccessfulPresent()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path path =
        UniqueTempPath(
            "_automation_presented_view.csv");
    {
        std::ofstream stream(
            path,
            std::ios::binary |
                std::ios::trunc);
        Require(
            stream.good(),
            "automation presented-view fixture should be created");
        stream << "fixture";
    }

    auto dependencies =
        MakeFixtureLoadDependencies(
            {{}, {}, {}, {}});
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            MakePreparedDeferredSession(path),
            specforge::
                MakeSourceCollectionLoadQueueForTesting(
                    std::move(dependencies)));
    (void)Access::Submit(
        *shell,
        specforge::
            SourceCollectionSessionIntent::
                ChangeActiveSampleWorkflow(
                    specforge::
                        ActiveSampleWorkflowIntent::
                            StartOrResumeTemporaryLabelingTask()));
    (void)Access::Submit(
        *shell,
        specforge::
            SourceCollectionSessionIntent::
                ChangeActiveSampleWorkflow(
                    specforge::
                        ActiveSampleWorkflowIntent::
                            UpsertActiveLabel(
                                specforge::
                                    SampleLabelDefinition{
                                        7,
                                        "presented",
                                        'p'})));
    (void)Access::Submit(
        *shell,
        specforge::
            SourceCollectionSessionIntent::
                ChangeActiveSampleWorkflow(
                    specforge::
                        ActiveSampleWorkflowIntent::
                            AssignActiveLabelToCurrentSample(
                                7)));

    Access::EnableNavigationTracing(
        *shell,
        401);
    Access::SubmitSpectrumDraw(
        *shell,
        401,
        Access::Session(*shell).
            CurrentSampleSnapshot(),
        17);
    const specforge::NavigationLatencyPresentation
        first_presentation{
            17,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        401,
        std::span(&first_presentation, 1));
    const specforge::ShellAutomationView
        first_presented =
            shell->PresentedAutomationView();

    const specforge::ShellAutomationNavigationResult
        navigation =
            shell->GotoSpectrumForAutomation(
                1,
                std::nullopt);
    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() <
           deadline) {
        Access::Drain(*shell);
        const auto snapshot =
            Access::Session(*shell).
                CurrentSampleSnapshot();
        if (snapshot &&
            snapshot->collection.current_index ==
                1 &&
            Access::PendingLoadCount(*shell) == 0) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    const specforge::ShellAutomationView live_after_drain =
        shell->AutomationView();
    const specforge::ShellAutomationView
        presented_before_retry =
            shell->PresentedAutomationView();

    Access::EnableNavigationTracing(
        *shell,
        402);
    Access::SubmitSpectrumDraw(
        *shell,
        402,
        Access::Session(*shell).
            CurrentSampleSnapshot(),
        17);
    shell->PresentFrame(402, {});
    const specforge::ShellAutomationView
        presented_after_retry =
            shell->PresentedAutomationView();
    const specforge::NavigationLatencyPresentation
        wrong_viewport{
            18,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        402,
        std::span(&wrong_viewport, 1));
    const specforge::ShellAutomationView
        presented_after_wrong_viewport =
            shell->PresentedAutomationView();
    const specforge::NavigationLatencyPresentation
        latest_presentation{
            17,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        402,
        std::span(&latest_presentation, 1));
    const specforge::ShellAutomationView
        latest_presented =
            shell->PresentedAutomationView();

    shell.reset();
    std::filesystem::remove(path);
    Require(
        navigation.error ==
                specforge::
                    ShellAutomationNavigationError::
                        None &&
            first_presented.spectrum.present &&
            first_presented.spectrum.index == 0 &&
            first_presented.labeling
                    .has_active_task &&
            first_presented.labeling
                    .current_spectrum_code ==
                7,
        "the first successful Present should establish source, row, task, and label projection");
    Require(
        live_after_drain.spectrum.present &&
            live_after_drain.spectrum.index == 1 &&
            live_after_drain.labeling
                    .current_spectrum_code !=
                7 &&
            presented_before_retry.spectrum.index ==
                0,
        "maintenance may advance the live Session without changing the presented automation projection");
    Require(
        presented_after_retry.spectrum.index ==
                0 &&
            presented_after_retry.labeling
                    .current_spectrum_code ==
                7 &&
            presented_after_wrong_viewport
                    .spectrum.index ==
                0,
        "PresentRetry and a successful Present from the wrong viewport must retain the prior projection");
    Require(
        latest_presented.source_id ==
                live_after_drain.source_id &&
            latest_presented.spectrum.present &&
            latest_presented.spectrum.index == 1 &&
            latest_presented.labeling
                    .has_active_task &&
            latest_presented.labeling
                    .current_spectrum_code ==
                live_after_drain.labeling
                    .current_spectrum_code,
        "only the exact successful Present should publish the new source, spectrum, task, and code projection");
}

void TestShellFlushResultNamesEveryFailedOwner()
{
    specforge::ShellLocalStateFlushResult result;
    Require(
        result.all_saved() &&
            result.FailureMessage().empty(),
        "a complete shell flush should not produce a warning");

    result.application_settings.language_saved = false;
    result.application_settings.appearance_saved = false;
    result.application_settings.panel_visibility_saved = false;
    result.source_collection.navigation_saved = false;
    result.source_collection.workflow_saved = false;
    result.spectrum_view_saved = false;
    result.spectral_lines_saved = false;
    const std::string message = result.FailureMessage();
    Require(
        !result.all_saved(),
        "any failed owner should make the shell flush incomplete");
    Require(
        message.find("Language") !=
                std::string::npos &&
            message.find("Appearance") !=
                std::string::npos &&
            message.find("Panel visibility") !=
                std::string::npos &&
            message.find("Sample navigation") !=
                std::string::npos &&
            message.find("Sample workflow") !=
                std::string::npos &&
            message.find("Spectrum view") !=
                std::string::npos &&
            message.find("Spectral-line state") !=
                std::string::npos,
        "the shutdown warning should name every failed owner");
    Require(
        message.find("UI scale") ==
                std::string::npos &&
            message.find("Input") ==
                std::string::npos &&
            message.find("Profile output directory") ==
                std::string::npos &&
            message.find("Source session") ==
                std::string::npos &&
            message.find("Sample labeling") ==
                std::string::npos,
        "the shutdown warning should omit successful owners");

    const std::string chinese_message =
        result.FailureMessage(
            specforge::UiLanguage::SimplifiedChinese);
    Require(
        chinese_message.find("语言") !=
                std::string::npos &&
            chinese_message.find("面板可见性") !=
                std::string::npos &&
            chinese_message.find("样本导航") !=
                std::string::npos &&
            chinese_message.find("样本工作流") !=
                std::string::npos &&
            chinese_message.find("谱线状态") !=
                std::string::npos,
        "the Chinese shutdown warning should name every failed owner");
    Require(
        chinese_message.find("界面缩放") ==
                std::string::npos &&
            chinese_message.find("输入") ==
                std::string::npos &&
            chinese_message.find("配置文件输出目录") ==
                std::string::npos &&
            chinese_message.find("源会话") ==
                std::string::npos &&
            chinese_message.find("样本标注") ==
                std::string::npos,
        "the Chinese shutdown warning should omit successful owners");
}

void TestExternalStartupPreservesPreferredMemberForFitsAndCsvAndOtherOriginsStayDirect()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path folder =
        UniqueTempPath("_external_fits_folder");
    std::error_code cleanup_error;
    std::filesystem::remove_all(folder, cleanup_error);
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "first.csv");
    const std::filesystem::path preferred =
        folder / "selected.fits";
    WriteFixture(preferred);
    WriteFixture(folder / "zzz.csv");

    std::vector<std::size_t> folder_decode_indices;
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const auto& source, std::size_t index, const auto&) {
            return MakeSnapshot(source, index);
        };
    dependencies.folder_snapshot_loader =
        [&folder_decode_indices](
            const auto& source,
            std::size_t index,
            const auto& listing,
            const auto&) {
            (void)listing;
            folder_decode_indices.push_back(index);
            return MakeSnapshot(
                source,
                index);
        };
    dependencies.file_context_builder =
        [](const auto& snapshot,
           const auto& state,
           const auto& checkpoint) {
            checkpoint();
            specforge::SourceCollectionContext context;
            context.identity =
                specforge::BuildSourceCollectionIdentity(
                    snapshot,
                    state);
            context.manifest.sample_names = {"file"};
            return context;
        };
    dependencies.folder_context_builder =
        [](const auto& snapshot,
           const auto& listing,
           const auto& checkpoint) {
            checkpoint();
            return specforge::BuildFolderSourceCollectionContext(
                snapshot,
                listing);
        };
    dependencies.workflow_cache_loader =
        [](const auto&, const auto& checkpoint) {
            checkpoint();
            return specforge::SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};

    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            specforge::SourceCollectionSession({}, {}, {}, {}),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                std::move(dependencies)));

    shell->OpenSource(folder);
    Require(
        DrainAllSourceLoads(*shell),
        "an existing folder source should finish loading before the external open");
    const specforge::SpectrumSnapshotHandle existing_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    Require(
        existing_snapshot &&
            existing_snapshot->source.path == folder &&
            existing_snapshot->collection.current_index == 0,
        "the external-open regression should start from an existing folder source at its first member");
    folder_decode_indices.clear();

    Require(
        Access::ApplySettingsUiIntent(
            *shell,
            specforge::ApplicationSettingsIntent::
                SetOpenExternalSourceAsFolder(true))
            .applied(),
        "external FITS folder setting should apply in the startup shell");
    constexpr std::uint64_t external_presentation_frame = 500;
    Access::EnableNavigationTracing(
        *shell,
        external_presentation_frame);
    shell->OpenExternalSource(preferred);
    Require(
        DrainAllSourceLoads(*shell),
        "external FITS startup source should finish loading");
    const specforge::SpectrumSnapshotHandle external_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    Require(
        external_snapshot &&
            external_snapshot->source.path == folder,
        "external FITS startup should keep the containing folder as the source");
    Require(
        folder_decode_indices.size() == 1 &&
            folder_decode_indices.front() == 1,
        "external FITS preparation should decode the requested non-first folder member");
    Require(
        external_snapshot->collection.current_index == 1,
        "external FITS startup should present the requested non-first folder member");
    const std::vector<specforge::SourceLoadLatencyReport>
        external_reports =
            Access::CompleteSourceLoadFramePresentation(
                *shell,
                external_presentation_frame + 1);
    Require(
        external_reports.size() == 1 &&
            external_reports.front().target_index == 1 &&
            external_reports.front().attempts.size() == 1 &&
            external_reports.front().attempts.front().target_index == 1,
        "external FITS source-load tracing should report the resolved preferred member index");
    Require(
        external_reports.front().attempts.front().preparation_rounds.size() == 1 &&
            !external_reports.front().attempts.front().preparation_rounds.front().context_reused,
        "preferred FITS preparation should report that its rebuilt context was not reused");

    const std::filesystem::path csv_folder =
        UniqueTempPath("_external_csv_folder");
    std::filesystem::remove_all(csv_folder);
    std::filesystem::create_directory(csv_folder);
    WriteFixture(csv_folder / "first.csv");
    const std::filesystem::path csv_preferred =
        csv_folder / "selected.CSV";
    WriteFixture(csv_preferred);
    WriteFixture(csv_folder / "zzz.fits");

    shell->OpenExternalSource(folder / "missing.fits");
    Require(
        DrainAllSourceLoads(*shell),
        "missing external FITS startup should report after background resolution");
    const std::string missing_error(Access::LoadError(*shell));
    Require(
        missing_error.find("does not exist") != std::string::npos &&
            missing_error.find("missing.fits") != std::string::npos,
        "missing external FITS startup target should retain a clear diagnostic");

    Require(
        Access::ApplySettingsUiIntent(
            *shell,
            specforge::ApplicationSettingsIntent::
                SetOpenExternalSourceAsFolder(false))
            .applied(),
        "external FITS folder setting should be disableable");
    shell->OpenExternalSource(preferred);
    Require(
        DrainAllSourceLoads(*shell),
        "disabled external FITS startup should finish loading");
    const specforge::SpectrumSnapshotHandle disabled_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    Require(
        disabled_snapshot &&
            disabled_snapshot->source.path == preferred &&
            disabled_snapshot->collection.current_index == 0,
        "disabled external FITS startup should retain single-file semantics");

    Require(
        Access::ApplySettingsUiIntent(
            *shell,
            specforge::ApplicationSettingsIntent::
                SetOpenExternalSourceAsFolder(true))
            .applied(),
        "external FITS folder setting should be re-enabled for origin checks");
    shell->OpenSource(preferred);
    Require(
        DrainAllSourceLoads(*shell),
        "in-app FITS open should finish loading");
    const specforge::SpectrumSnapshotHandle in_app_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    Require(
        in_app_snapshot &&
            in_app_snapshot->source.path == preferred &&
            in_app_snapshot->collection.current_index == 0,
        "in-app FITS open should remain a single-file source");

    (void)shell->OpenSourceForAutomation(preferred);
    Require(
        DrainAllSourceLoads(*shell),
        "automation FITS open should finish loading");
    const specforge::SpectrumSnapshotHandle automation_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    Require(
        automation_snapshot &&
            automation_snapshot->source.path == preferred &&
            automation_snapshot->collection.current_index == 0,
        "automation FITS open should remain a single-file source");

    folder_decode_indices.clear();
    shell->OpenExternalSource(csv_preferred);
    Require(
        DrainAllSourceLoads(*shell),
        "external CSV startup source should finish loading");
    const specforge::SpectrumSnapshotHandle external_csv_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    Require(
        external_csv_snapshot &&
            external_csv_snapshot->source.path == csv_folder &&
            folder_decode_indices.size() == 1 &&
            folder_decode_indices.front() == 1 &&
            external_csv_snapshot->collection.current_index == 1,
        "external CSV startup should retain the requested non-first folder member");

    shell->OpenExternalSource(csv_folder / "missing.CSV");
    Require(
        DrainAllSourceLoads(*shell),
        "missing external CSV startup should report after background resolution");
    const std::string missing_csv_error(Access::LoadError(*shell));
    Require(
        missing_csv_error.find("does not exist") != std::string::npos &&
            missing_csv_error.find("missing.CSV") != std::string::npos,
        "missing external CSV startup target should retain a clear diagnostic");

    Require(
        Access::ApplySettingsUiIntent(
            *shell,
            specforge::ApplicationSettingsIntent::
                SetOpenExternalSourceAsFolder(false))
            .applied(),
        "external CSV folder setting should be disableable");
    shell->OpenExternalSource(csv_preferred);
    Require(
        DrainAllSourceLoads(*shell),
        "disabled external CSV startup should finish loading");
    const specforge::SpectrumSnapshotHandle disabled_csv_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    Require(
        disabled_csv_snapshot &&
            disabled_csv_snapshot->source.path == csv_preferred &&
            disabled_csv_snapshot->collection.current_index == 0,
        "disabled external CSV startup should retain single-file semantics");

    Require(
        Access::ApplySettingsUiIntent(
            *shell,
            specforge::ApplicationSettingsIntent::
                SetOpenExternalSourceAsFolder(true))
            .applied(),
        "external CSV folder setting should be re-enabled for origin checks");

    folder_decode_indices.clear();
    bool file_picker_called = false;
    {
        ScopedImGuiContext menu_context;
        const specforge::SourceCollectionPathPicker choose_source_file =
            [&]() -> std::optional<std::filesystem::path> {
                file_picker_called = true;
                return csv_preferred;
            };
        RenderShellFileMenuFrame(*shell, choose_source_file);
        ImGuiWindow* host_window = ImGui::FindWindowByName(
            kFileMenuTestHost);
        Require(
            host_window != nullptr,
            "File menu test should render its host window");
        bool file_menu_hovered = false;
        ImVec2 file_menu_click_pos;
        for (float y = 20.0f;
             y <= 70.0f && !file_menu_hovered;
             y += 2.0f) {
            for (float x = 20.0f;
                 x <= 220.0f && !file_menu_hovered;
                 x += 4.0f) {
                ImGui::GetIO().AddMousePosEvent(x, y);
                RenderShellFileMenuFrame(
                    *shell,
                    choose_source_file);
                file_menu_hovered =
                    GImGui->HoveredWindow == host_window &&
                    GImGui->HoveredId != 0;
                if (file_menu_hovered) {
                    file_menu_click_pos = ImVec2(x, y);
                }
            }
        }
        Require(
            file_menu_hovered,
            "File menu test should hover the File menu item");

        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            true);
        ImGui::GetIO().AddMousePosEvent(
            file_menu_click_pos.x,
            file_menu_click_pos.y);
        RenderShellFileMenuFrame(*shell, choose_source_file);
        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            false);
        RenderShellFileMenuFrame(*shell, choose_source_file);

        Require(
            !GImGui->OpenPopupStack.empty(),
            "File menu test should open the File menu popup");
        ImGuiWindow* file_popup =
            GImGui->OpenPopupStack.back().Window;
        Require(
            file_popup != nullptr,
            "File menu test should expose its popup window");
        bool open_file_hovered = false;
        ImVec2 open_file_click_pos;
        for (float y = file_popup->Pos.y;
             y <= file_popup->Pos.y + 42.0f && !open_file_hovered;
             y += 2.0f) {
            for (float x = file_popup->Pos.x;
                 x <= file_popup->Pos.x + file_popup->Size.x &&
                     !open_file_hovered;
                 x += 4.0f) {
                ImGui::GetIO().AddMousePosEvent(x, y);
                RenderShellFileMenuFrame(
                    *shell,
                    choose_source_file);
                open_file_hovered =
                    GImGui->HoveredWindow == file_popup &&
                    GImGui->HoveredId != 0;
                if (open_file_hovered) {
                    open_file_click_pos = ImVec2(x, y);
                }
            }
        }
        Require(
            open_file_hovered,
            "File menu test should hover the Open File menu item");

        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            true);
        ImGui::GetIO().AddMousePosEvent(
            open_file_click_pos.x,
            open_file_click_pos.y);
        RenderShellFileMenuFrame(*shell, choose_source_file);
        ImGui::GetIO().AddMouseButtonEvent(
            ImGuiMouseButton_Left,
            false);
        RenderShellFileMenuFrame(*shell, choose_source_file);
    }
    Require(
        file_picker_called,
        "File > Open File should invoke the injected file picker");
    Require(
        DrainAllSourceLoads(*shell),
        "File > Open File CSV should finish loading");
    const specforge::SpectrumSnapshotHandle file_menu_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    Require(
        file_menu_snapshot &&
            file_menu_snapshot->source.path == csv_preferred &&
            file_menu_snapshot->collection.current_index == 0 &&
            folder_decode_indices.empty(),
        "File > Open File CSV should use an in-app single-file source despite the external folder preference");

    shell->OpenSource(csv_preferred);
    Require(
        DrainAllSourceLoads(*shell),
        "in-app CSV open should finish loading");
    const specforge::SpectrumSnapshotHandle in_app_csv_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    Require(
        in_app_csv_snapshot &&
            in_app_csv_snapshot->source.path == csv_preferred &&
            in_app_csv_snapshot->collection.current_index == 0,
        "in-app CSV open should remain a single-file source");

    (void)shell->OpenSourceForAutomation(csv_preferred);
    Require(
        DrainAllSourceLoads(*shell),
        "automation CSV open should finish loading");
    const specforge::SpectrumSnapshotHandle automation_csv_snapshot =
        Access::Session(*shell).CurrentSampleSnapshot();
    Require(
        automation_csv_snapshot &&
            automation_csv_snapshot->source.path == csv_preferred &&
            automation_csv_snapshot->collection.current_index == 0,
        "automation CSV open should remain a single-file source");

    shell.reset();
    std::filesystem::remove_all(folder);
    std::filesystem::remove_all(csv_folder);
}

void TestExternalStartupPreferredMemberDoesNotYieldFilteredFallback()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path folder =
        UniqueTempPath("_external_fits_filtered_folder");
    const std::filesystem::path state_root =
        UniqueTempPath("_external_fits_filtered_state");
    std::error_code cleanup_error;
    std::filesystem::remove_all(folder, cleanup_error);
    std::filesystem::remove_all(state_root, cleanup_error);
    std::filesystem::create_directory(folder);
    std::filesystem::create_directory(state_root);
    WriteFixture(folder / "first.csv");
    const std::filesystem::path preferred =
        folder / "selected.fits";
    WriteFixture(preferred);
    WriteFixture(folder / "zzz.csv");

    const std::filesystem::path annotation_path =
        state_root / "external-filter_y.npy";
    std::string annotation_error;
    Require(
        SaveAnnotationFixture(
            annotation_path,
            {1, 0, 1},
            &annotation_error),
        annotation_error.empty()
            ? "filtered external FITS annotation should save"
            : annotation_error);
    const std::optional<specforge::SampleAnnotationResult>
        annotation =
            specforge::test_support::LegacyFixtureIo{}.Load(
                annotation_path,
                3,
                &annotation_error);
    Require(
        annotation.has_value(),
        "filtered external FITS annotation should load");
    const std::string filter_source_id =
        specforge::BuildAnnotationFilterSourceId(*annotation);
    const std::filesystem::path workflow_cache_path =
        state_root / "workflow.json";

    std::vector<std::size_t> folder_decode_indices;
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const auto& source, std::size_t index, const auto&) {
            return MakeSnapshot(source, index);
        };
    dependencies.folder_snapshot_loader =
        [&folder_decode_indices](
            const auto& source,
            std::size_t index,
            const auto& listing,
            const auto&) {
            (void)listing;
            folder_decode_indices.push_back(index);
            return MakeSnapshot(source, index);
        };
    dependencies.file_context_builder =
        [](const auto& snapshot,
           const auto& state,
           const auto& checkpoint) {
            checkpoint();
            specforge::SourceCollectionContext context;
            context.identity =
                specforge::BuildSourceCollectionIdentity(
                    snapshot,
                    state);
            context.manifest.sample_names = {
                "first",
                "selected",
                "zzz"};
            return context;
        };
    dependencies.folder_context_builder =
        [](const auto& snapshot,
           const auto& listing,
           const auto& checkpoint) {
            checkpoint();
            return specforge::BuildFolderSourceCollectionContext(
                snapshot,
                listing);
        };
    dependencies.workflow_cache_loader =
        [&workflow_cache_path, &state_root](
            const auto&,
            const auto& checkpoint) {
            return specforge::LoadSampleWorkflowPreparationCacheBundle(
                specforge::SampleWorkflowPreparationPaths{
                    .labeling_state_cache_path =
                        state_root / "labeling.json",
                    .workflow_state_cache_path =
                        workflow_cache_path,
                    .navigation_state_cache_path =
                        state_root / "navigation.json"},
                checkpoint);
        };
    dependencies.workflow_cache_paths = {
        .labeling_state_cache_path =
            state_root / "labeling.json",
        .workflow_state_cache_path = workflow_cache_path,
        .navigation_state_cache_path =
            state_root / "navigation.json"};

    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            specforge::SourceCollectionSession(
                state_root / "source-session.json",
                state_root / "navigation.json",
                state_root / "labeling.json",
                workflow_cache_path),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                std::move(dependencies)));

    shell->OpenSource(folder);
    Require(
        DrainAllSourceLoads(*shell),
        "filtered external FITS fixture should open its folder source");
    specforge::SourceCollectionSession& session =
        Access::Session(*shell);
    Require(
        session.Submit(
                   specforge::SourceCollectionSessionIntent::
                       EditSourceCollection(
                           specforge::SourceCollectionIntent::
                               AddReadOnlyAnnotationResult(
                                   annotation_path)))
            .loaded,
        "filtered external FITS fixture should attach its annotation");
    (void)session.Submit(
        specforge::SourceCollectionSessionIntent::
            ApplySampleFiltering(
                specforge::SampleFilteringIntent::
                    AddSource(filter_source_id)));
    (void)session.Submit(
        specforge::SourceCollectionSessionIntent::
            ApplySampleFiltering(
                specforge::SampleFilteringIntent::
                    SetFilterValueSelected(
                        filter_source_id,
                        "1",
                        true)));
    const specforge::SourceCollectionSessionView filtered_view =
        session.View();
    Require(
        filtered_view.filter.evaluation.active &&
            filtered_view.filter.evaluation.included_count == 2 &&
            filtered_view.navigation.sequence_count == 2 &&
            filtered_view.navigation.current_index == 0,
        "filtered external FITS fixture should exclude the preferred member");
    Require(
        session.FlushStateCaches(),
        "filtered external FITS fixture should persist its workflow state");

    folder_decode_indices.clear();
    Require(
        Access::ApplySettingsUiIntent(
                *shell,
                specforge::ApplicationSettingsIntent::
                    SetOpenExternalSourceAsFolder(true))
            .applied(),
        "filtered external FITS folder setting should apply");
    shell->OpenExternalSource(preferred);
    Require(
        DrainAllSourceLoads(*shell),
        "filtered external FITS startup should settle after rejection");
    const std::string load_error(Access::LoadError(*shell));
    const specforge::SpectrumSnapshotHandle snapshot =
        session.CurrentSampleSnapshot();
    Require(
        load_error.find("excluded by the active sample filter") !=
            std::string::npos,
        "a preferred member excluded by filtering should fail closed with a diagnostic");
    Require(
        folder_decode_indices.size() == 1 &&
            folder_decode_indices.front() == 1 &&
            snapshot &&
            snapshot->source.path == folder &&
            snapshot->collection.current_index == 0,
        "filter rejection must not silently activate the filtered first member");

    shell.reset();
    std::filesystem::remove_all(folder);
    std::filesystem::remove_all(state_root);
}

void TestExternalStartupPreferredMemberCannotBeOverriddenByLiveSampleFilter()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path folder =
        UniqueTempPath("_external_fits_live_filter_folder");
    const std::filesystem::path state_root =
        UniqueTempPath("_external_fits_live_filter_state");
    std::error_code cleanup_error;
    std::filesystem::remove_all(folder, cleanup_error);
    std::filesystem::remove_all(state_root, cleanup_error);
    std::filesystem::create_directory(folder);
    std::filesystem::create_directory(state_root);
    WriteFixture(folder / "first.csv");
    const std::filesystem::path preferred =
        folder / "selected.fits";
    WriteFixture(preferred);
    WriteFixture(folder / "zzz.csv");

    const std::filesystem::path annotation_path =
        state_root / "external-live-filter_y.npy";
    std::string annotation_error;
    Require(
        SaveAnnotationFixture(
            annotation_path,
            {1, 0, 1},
            &annotation_error),
        annotation_error.empty()
            ? "live-filter external FITS annotation should save"
            : annotation_error);
    const std::optional<specforge::SampleAnnotationResult>
        annotation =
            specforge::test_support::LegacyFixtureIo{}.Load(
                annotation_path,
                3,
                &annotation_error);
    Require(
        annotation.has_value(),
        "live-filter external FITS annotation should load");
    const std::string filter_source_id =
        specforge::BuildAnnotationFilterSourceId(*annotation);

    std::promise<void> external_decode_entered_promise;
    std::shared_future<void> external_decode_entered =
        external_decode_entered_promise.get_future().share();
    std::promise<void> release_external_decode_promise;
    std::shared_future<void> release_external_decode =
        release_external_decode_promise.get_future().share();
    std::atomic_size_t folder_loader_calls = 0;
    std::vector<std::size_t> folder_decode_indices;
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const auto& source, std::size_t index, const auto&) {
            return MakeSnapshot(source, index);
        };
    dependencies.folder_snapshot_loader =
        [&external_decode_entered_promise,
         &release_external_decode,
         &folder_loader_calls,
         &folder_decode_indices](
            const auto& source,
            std::size_t index,
            const auto& listing,
            const auto& canceled) {
            (void)listing;
            const bool is_external_decode =
                folder_loader_calls.fetch_add(
                    1,
                    std::memory_order_relaxed) == 1;
            if (is_external_decode) {
                external_decode_entered_promise.set_value();
                while (release_external_decode.wait_for(2ms) !=
                       std::future_status::ready) {
                    if (canceled()) {
                        return MakeSnapshot(source, index);
                    }
                }
            }
            folder_decode_indices.push_back(index);
            return MakeSnapshot(source, index);
        };
    dependencies.file_context_builder =
        [](const auto& snapshot,
           const auto& state,
           const auto& checkpoint) {
            checkpoint();
            specforge::SourceCollectionContext context;
            context.identity =
                specforge::BuildSourceCollectionIdentity(
                    snapshot,
                    state);
            context.manifest.sample_names = {
                "first",
                "selected",
                "zzz"};
            return context;
        };
    dependencies.folder_context_builder =
        [](const auto& snapshot,
           const auto& listing,
           const auto& checkpoint) {
            checkpoint();
            return specforge::BuildFolderSourceCollectionContext(
                snapshot,
                listing);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const auto& checkpoint) {
            checkpoint();
            return specforge::SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};

    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            specforge::SourceCollectionSession(
                {},
                {},
                {},
                {}),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                std::move(dependencies)));
    shell->OpenSource(folder);
    Require(
        DrainAllSourceLoads(*shell),
        "live-filter external FITS fixture should open its folder source");
    specforge::SourceCollectionSession& session =
        Access::Session(*shell);
    Require(
        session.Submit(
                   specforge::SourceCollectionSessionIntent::
                       EditSourceCollection(
                           specforge::SourceCollectionIntent::
                               AddReadOnlyAnnotationResult(
                                   annotation_path)))
            .loaded,
        "live-filter external FITS fixture should attach its annotation");
    folder_decode_indices.clear();
    Require(
        Access::ApplySettingsUiIntent(
                *shell,
                specforge::ApplicationSettingsIntent::
                    SetOpenExternalSourceAsFolder(true))
            .applied(),
        "live-filter external FITS folder setting should apply");

    shell->OpenExternalSource(preferred);
    Require(
        external_decode_entered.wait_for(2s) ==
            std::future_status::ready,
        "external FITS worker should reach its folder decoder before the live filter update");
    (void)session.Submit(
        specforge::SourceCollectionSessionIntent::
            ApplySampleFiltering(
                specforge::SampleFilteringIntent::
                    AddSource(filter_source_id)));
    (void)session.Submit(
        specforge::SourceCollectionSessionIntent::
            ApplySampleFiltering(
                specforge::SampleFilteringIntent::
                    SetFilterValueSelected(
                        filter_source_id,
                        "1",
                        true)));
    Require(
        session.View().filter.evaluation.active &&
            session.View().filter.evaluation.included_count == 2 &&
            session.View().navigation.current_index == 0,
        "live sample filter should exclude the preferred member while its worker is running");
    release_external_decode_promise.set_value();
    Require(
        DrainAllSourceLoads(*shell),
        "live-filter external FITS startup should settle after the worker update");

    const std::string load_error(Access::LoadError(*shell));
    const specforge::SpectrumSnapshotHandle snapshot =
        session.CurrentSampleSnapshot();
    Require(
        load_error.find("excluded by the active sample filter") !=
            std::string::npos,
        "a live sample filter excluding the preferred member should fail closed with a diagnostic");
    Require(
        folder_decode_indices.size() == 1 &&
            folder_decode_indices.front() == 1 &&
            snapshot &&
            snapshot->source.path == folder &&
            snapshot->collection.current_index == 0,
        "live sample filtering must not queue the first visible member over the preferred request");

    shell.reset();
    std::filesystem::remove_all(folder);
    std::filesystem::remove_all(state_root);
}

void TestSourceOpenResolutionRunsOnWorkerAndCancels()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path folder =
        UniqueTempPath("_source_open_worker_folder");
    std::error_code cleanup_error;
    std::filesystem::remove_all(folder, cleanup_error);
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "first.csv");
    const std::filesystem::path preferred =
        folder / "selected.fits";
    WriteFixture(preferred);

    std::promise<void> probe_entered_promise;
    std::shared_future<void> probe_entered =
        probe_entered_promise.get_future().share();
    std::promise<void> probe_canceled_promise;
    std::shared_future<void> probe_canceled =
        probe_canceled_promise.get_future().share();
    std::promise<void> release_probe_promise;
    std::shared_future<void> release_probe =
        release_probe_promise.get_future().share();
    std::promise<void> first_open_returned_promise;
    std::shared_future<void> first_open_returned =
        first_open_returned_promise.get_future().share();
    std::atomic_bool probe_started = false;
    std::atomic_bool probe_cancel_was_observed = false;
    std::atomic_bool release_signaled = false;
    const auto signal_release = [&]() {
        if (!release_signaled.exchange(
                true,
                std::memory_order_relaxed)) {
            release_probe_promise.set_value();
        }
    };

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const auto& source, std::size_t index, const auto&) {
            return MakeSnapshot(source, index);
        };
    dependencies.folder_snapshot_loader =
        [](const auto& source,
           std::size_t index,
           const auto& listing,
           const auto&) {
            (void)listing;
            return MakeSnapshot(source, index);
        };
    dependencies.file_context_builder =
        [](const auto& snapshot,
           const auto& state,
           const auto& checkpoint) {
            checkpoint();
            specforge::SourceCollectionContext context;
            context.identity =
                specforge::BuildSourceCollectionIdentity(
                    snapshot,
                    state);
            context.manifest.sample_names = {
                "first",
                "selected"};
            return context;
        };
    dependencies.folder_context_builder =
        [](const auto& snapshot,
           const auto& listing,
           const auto& checkpoint) {
            checkpoint();
            return specforge::BuildFolderSourceCollectionContext(
                snapshot,
                listing);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const auto& checkpoint) {
            checkpoint();
            return specforge::SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};
    dependencies.source_open_probe =
        [&probe_entered_promise,
         &probe_canceled_promise,
         &release_probe,
         &probe_started,
         &probe_cancel_was_observed](
            const specforge::SourceOpenRequest& request,
            const auto& checkpoint) {
            if (!probe_started.exchange(
                    true,
                    std::memory_order_relaxed)) {
                probe_entered_promise.set_value();
            }
            try {
                while (release_probe.wait_for(2ms) !=
                       std::future_status::ready) {
                    checkpoint();
                }
            } catch (const specforge::SourceCollectionPreparationCanceled&) {
                if (!probe_cancel_was_observed.exchange(
                        true,
                        std::memory_order_relaxed)) {
                    probe_canceled_promise.set_value();
                }
                throw;
            }
            return specforge::ProbeSourceOpenRequest(
                request,
                checkpoint);
        };

    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            specforge::SourceCollectionSession({}, {}, {}, {}),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                std::move(dependencies)));
    Require(
        Access::ApplySettingsUiIntent(
                *shell,
                specforge::ApplicationSettingsIntent::
                    SetOpenExternalSourceAsFolder(true))
            .applied(),
        "worker resolver test should enable external FITS folder opening");

    std::thread first_open_thread([&]() {
        shell->OpenExternalSource(preferred);
        first_open_returned_promise.set_value();
    });
    const bool probe_was_entered =
        probe_entered.wait_for(2s) ==
        std::future_status::ready;
    const bool open_was_returned =
        first_open_returned.wait_for(2s) ==
        std::future_status::ready;
    if (!open_was_returned) {
        signal_release();
    }
    first_open_thread.join();
    Require(
        probe_was_entered,
        "source-open filesystem probe should run on the load worker");
    Require(
        open_was_returned,
        "external source-open request should return while the worker probe is blocked");

    shell->OpenExternalSource(preferred);
    Require(
        probe_canceled.wait_for(2s) ==
            std::future_status::ready,
        "replacing an external source-open should cancel its worker probe");
    signal_release();
    Require(
        DrainAllSourceLoads(*shell),
        "the replacement source-open should settle after probe cancellation");
    Require(
        Access::LoadError(*shell).empty(),
        "a canceled resolver probe must not publish a load error for the replacement request");

    shell.reset();
    std::filesystem::remove_all(folder);
}

void TestExternalStartupPreservesDeferredRestoreAnnotationContext()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path folder =
        UniqueTempPath("_external_fits_deferred_context_folder");
    const std::filesystem::path state_root =
        UniqueTempPath("_external_fits_deferred_context_state");
    std::error_code cleanup_error;
    std::filesystem::remove_all(folder, cleanup_error);
    std::filesystem::remove_all(state_root, cleanup_error);
    std::filesystem::create_directory(folder);
    std::filesystem::create_directory(state_root);
    WriteFixture(folder / "first.csv");
    const std::filesystem::path preferred =
        folder / "selected.fits";
    WriteFixture(preferred);
    WriteFixture(folder / "zzz.csv");

    const std::filesystem::path annotation_path =
        state_root / "deferred-filter_y.npy";
    std::string annotation_error;
    Require(
        SaveAnnotationFixture(
            annotation_path,
            {1, 0, 1},
            &annotation_error),
        annotation_error.empty()
            ? "deferred external FITS annotation should save"
            : annotation_error);
    const std::optional<specforge::SampleAnnotationResult>
        annotation =
            specforge::test_support::LegacyFixtureIo{}.Load(
                annotation_path,
                3,
                &annotation_error);
    Require(
        annotation.has_value(),
        "deferred external FITS annotation should load");
    const std::string filter_source_id =
        specforge::BuildAnnotationFilterSourceId(*annotation);
    const SourceSessionCachePaths cache_paths{
        state_root / "source-session.json",
        state_root / "navigation.json",
        state_root / "labeling.json",
        state_root / "workflow.json"};

    const auto configure_common_dependencies =
        [&cache_paths](
            specforge::SourceCollectionPreparationAdapters& dependencies) {
        dependencies.snapshot_loader =
            [](const auto& source, std::size_t index, const auto&) {
                return MakeSnapshot(source, index);
            };
        dependencies.file_context_builder =
            [](const auto& snapshot,
               const auto& state,
               const auto& checkpoint) {
                checkpoint();
                specforge::SourceCollectionContext context;
                context.identity =
                    specforge::BuildSourceCollectionIdentity(
                        snapshot,
                        state);
                context.manifest.sample_names = {
                    "first",
                    "selected",
                    "zzz"};
                return context;
            };
        dependencies.folder_context_builder =
            [](const auto& snapshot,
               const auto& listing,
               const auto& checkpoint) {
                checkpoint();
                return specforge::BuildFolderSourceCollectionContext(
                    snapshot,
                    listing);
            };
        dependencies.workflow_cache_loader =
            [&cache_paths](
                const auto&,
                const auto& checkpoint) {
                return specforge::LoadSampleWorkflowPreparationCacheBundle(
                    specforge::SampleWorkflowPreparationPaths{
                        .labeling_state_cache_path =
                            cache_paths.labeling,
                        .workflow_state_cache_path =
                            cache_paths.workflow,
                        .navigation_state_cache_path =
                            cache_paths.navigation},
                    checkpoint);
            };
        dependencies.workflow_cache_paths = {
            .labeling_state_cache_path =
                cache_paths.labeling,
            .workflow_state_cache_path =
                cache_paths.workflow,
            .navigation_state_cache_path =
                cache_paths.navigation};
    };

    // Persist the same folder annotation and Sample Filter context that a
    // previous application run would leave for deferred startup restore.
    {
        specforge::SourceCollectionPreparationAdapters dependencies;
        configure_common_dependencies(dependencies);
        dependencies.folder_snapshot_loader =
            [](const auto& source,
               std::size_t index,
               const auto& listing,
               const auto&) {
                (void)listing;
                return MakeSnapshot(
                    source,
                    index);
            };
        std::unique_ptr<specforge::ShellUi> seed_shell =
            Access::Create(
                MakeCachedSession(cache_paths),
                specforge::MakeSourceCollectionLoadQueueForTesting(
                    std::move(dependencies)));
        seed_shell->OpenSource(folder);
        Require(
            DrainAllSourceLoads(*seed_shell),
            "deferred external FITS seed folder should open");
        specforge::SourceCollectionSession& seed_session =
            Access::Session(*seed_shell);
        Require(
            seed_session.Submit(
                       specforge::SourceCollectionSessionIntent::
                           EditSourceCollection(
                               specforge::SourceCollectionIntent::
                                   AddReadOnlyAnnotationResult(
                                       annotation_path)))
                .loaded,
            "deferred external FITS seed annotation should attach");
        (void)seed_session.Submit(
            specforge::SourceCollectionSessionIntent::
                ApplySampleFiltering(
                    specforge::SampleFilteringIntent::
                        AddSource(filter_source_id)));
        (void)seed_session.Submit(
            specforge::SourceCollectionSessionIntent::
                ApplySampleFiltering(
                    specforge::SampleFilteringIntent::
                        SetFilterValueSelected(
                            filter_source_id,
                            "1",
                            true)));
        const specforge::SourceCollectionSessionView seed_view =
            seed_session.View();
        Require(
            seed_view.filter.evaluation.active &&
                seed_view.filter.evaluation.included_count == 2,
            "deferred external FITS seed should persist an active Sample Filter");
        Require(
            seed_session.FlushStateCaches(),
            "deferred external FITS seed caches should flush");
        seed_shell.reset();
    }

    std::promise<void> restore_decode_entered_promise;
    std::shared_future<void> restore_decode_entered =
        restore_decode_entered_promise.get_future().share();
    std::promise<void> restore_canceled_promise;
    std::shared_future<void> restore_canceled =
        restore_canceled_promise.get_future().share();
    std::promise<void> release_restore_promise;
    std::shared_future<void> release_restore =
        release_restore_promise.get_future().share();
    std::promise<void> external_completion_ready_promise;
    std::shared_future<void> external_completion_ready =
        external_completion_ready_promise.get_future().share();
    std::atomic_bool restore_decode_started = false;
    std::atomic_bool restore_cancel_was_observed = false;
    std::atomic_bool external_completion_was_signaled = false;
    std::vector<std::size_t> folder_decode_indices;
    specforge::SourceCollectionPreparationAdapters dependencies;
    configure_common_dependencies(dependencies);
    dependencies.folder_snapshot_loader =
        [&restore_decode_entered_promise,
         &restore_canceled_promise,
         &release_restore,
         &restore_decode_started,
         &restore_cancel_was_observed,
         &folder_decode_indices](
            const auto& source,
            std::size_t index,
            const auto& listing,
            const auto& canceled) {
            (void)listing;
            const bool is_deferred_restore =
                !restore_decode_started.exchange(
                    true,
                    std::memory_order_relaxed);
            if (is_deferred_restore) {
                restore_decode_entered_promise.set_value();
                for (;;) {
                    if (canceled()) {
                        if (!restore_cancel_was_observed.exchange(
                                true,
                                std::memory_order_relaxed)) {
                            restore_canceled_promise.set_value();
                        }
                        return MakeSnapshot(
                            source,
                            index);
                    }
                    if (release_restore.wait_for(2ms) ==
                        std::future_status::ready) {
                        break;
                    }
                }
            }
            folder_decode_indices.push_back(index);
            return MakeSnapshot(
                source,
                index);
        };

    std::unique_ptr<specforge::ShellUi> shell =
        MakeDeferredShell(cache_paths, std::move(dependencies));
    Require(
        restore_decode_entered.wait_for(2s) ==
            std::future_status::ready,
        "deferred startup restore should enter its folder decoder before external open");
    Require(
        Access::PendingLoadCount(*shell) == 1,
        "deferred startup restore should own the initial folder ticket");
    Require(
        Access::ApplySettingsUiIntent(
                *shell,
                specforge::ApplicationSettingsIntent::
                    SetOpenExternalSourceAsFolder(true))
            .applied(),
        "deferred external FITS folder setting should apply");
    shell->RegisterSourceLoadCompletionReadyCallback(
        [&external_completion_ready_promise,
         &external_completion_was_signaled]() {
            if (!external_completion_was_signaled.exchange(
                    true,
                    std::memory_order_relaxed)) {
                external_completion_ready_promise.set_value();
            }
        });

    shell->OpenExternalSource(preferred);
    Require(
        restore_canceled.wait_for(2s) ==
            std::future_status::ready,
        "external open should cancel the replaced deferred restore worker");
    Require(
        external_completion_ready.wait_for(2s) ==
            std::future_status::ready,
        "deferred external FITS completion should publish after replacing restore");
    Access::Drain(*shell);
    release_restore_promise.set_value();

    specforge::SourceCollectionSession& session =
        Access::Session(*shell);
    const std::string load_error(Access::LoadError(*shell));
    const bool unresolved_source_retained =
        session.HasUnresolvedSourceIntent(folder);
    Require(
        load_error.find("excluded by the active sample filter") !=
            std::string::npos,
        "external startup should inherit deferred restore annotation/filter context and fail closed");
    Require(
        folder_decode_indices.size() == 1 &&
            folder_decode_indices.front() == 1,
        "deferred external FITS should decode only its preferred member");
    Require(
        unresolved_source_retained,
        "a failed replacement must retain the unresolved deferred source intent");
    Require(
        session.FlushStateCaches(),
        "deferred external FITS failure should preserve source session state");
    const specforge::SourceCollectionSessionStateCache persisted =
        specforge::LoadSourceCollectionSessionStateCache(
            cache_paths.source_session)
            .cache;
    const auto persisted_source = std::find_if(
        persisted.sources.begin(),
        persisted.sources.end(),
        [&folder](const auto& source) {
            return specforge::SourcePathIdentityKey(source.path) ==
                specforge::SourcePathIdentityKey(folder);
        });
    Require(
        persisted_source != persisted.sources.end() &&
            persisted_source->annotation_paths.size() == 1 &&
            specforge::SourcePathIdentityKey(
                persisted_source->annotation_paths.front()) ==
                specforge::SourcePathIdentityKey(annotation_path),
        "deferred restore annotation path should remain persisted after replacement failure");

    shell->UnregisterSourceLoadCompletionReadyCallback();
    shell.reset();
    std::filesystem::remove_all(folder);
    std::filesystem::remove_all(state_root);
}

void TestSupersededExternalPreferredTraceUsesResolvedMemberIndex()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path folder =
        UniqueTempPath("_external_fits_superseded_folder");
    std::error_code cleanup_error;
    std::filesystem::remove_all(folder, cleanup_error);
    std::filesystem::create_directory(folder);
    WriteFixture(folder / "first.csv");
    const std::filesystem::path preferred =
        folder / "selected.fits";
    WriteFixture(preferred);
    WriteFixture(folder / "zzz.csv");

    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [](const auto& source, std::size_t index, const auto&) {
            return MakeSnapshot(source, index);
        };
    dependencies.folder_snapshot_loader =
        [](const auto& source,
           std::size_t index,
           const auto& listing,
           const auto&) {
            (void)listing;
            return MakeSnapshot(source, index);
        };
    dependencies.folder_context_builder =
        [](const auto& snapshot,
           const auto& listing,
           const auto& checkpoint) {
            checkpoint();
            return specforge::BuildFolderSourceCollectionContext(
                snapshot,
                listing);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const auto& checkpoint) {
            checkpoint();
            return specforge::SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = {{}, {}};

    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            specforge::SourceCollectionSession({}, {}, {}, {}),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                std::move(dependencies)));
    Require(
        Access::ApplySettingsUiIntent(
                *shell,
                specforge::ApplicationSettingsIntent::
                    SetOpenExternalSourceAsFolder(true))
            .applied(),
        "superseded external FITS folder setting should apply");

    constexpr std::uint64_t presentation_frame = 700;
    Access::EnableNavigationTracing(*shell, presentation_frame);
    std::promise<void> completion_ready_promise;
    std::shared_future<void> completion_ready =
        completion_ready_promise.get_future().share();
    std::atomic_bool completion_ready_signaled = false;
    shell->RegisterSourceLoadCompletionReadyCallback(
        [&completion_ready_promise,
         &completion_ready_signaled]() {
            if (!completion_ready_signaled.exchange(true)) {
                completion_ready_promise.set_value();
            }
        });

    shell->OpenExternalSource(preferred);
    Require(
        completion_ready.wait_for(2s) ==
            std::future_status::ready,
        "the preferred external FITS completion should publish before supersession");
    shell->OpenExternalSource(preferred);
    // The first completion is now stale by admission, so drain it through the
    // activation transaction before collecting its superseded trace.
    Access::Drain(*shell);

    const std::vector<specforge::SourceLoadLatencyReport> reports =
        Access::CompleteSourceLoadFramePresentationWithoutSpectrumDraw(
            *shell,
            presentation_frame + 1);
    const auto superseded = std::find_if(
        reports.begin(),
        reports.end(),
        [](const specforge::SourceLoadLatencyReport& report) {
            return report.outcome ==
                specforge::SourceLoadLatencyOutcome::Superseded;
        });
    const bool resolved_trace =
        superseded != reports.end() &&
        superseded->target_index == 1 &&
        superseded->attempts.size() == 1 &&
        superseded->attempts.front().target_index == 1;

    shell.reset();
    std::filesystem::remove_all(folder);

    Require(
        resolved_trace,
        "a superseded external FITS trace should retain the resolved preferred member index");
}

void TestRealShellFlushAndHealthKeepIndependentSettingsOwners()
{
    using namespace std::chrono_literals;

    const std::filesystem::path root =
        UniqueTempPath("-settings-persistence");
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);

    const std::filesystem::path language_path =
        root / "ui-language.json";
    const std::filesystem::path ui_scale_path =
        root / "ui-scale.json";
    const std::filesystem::path panel_path =
        root / "panel-visibility.json";
    std::filesystem::create_directory(language_path);
    std::filesystem::create_directory(ui_scale_path);
    std::filesystem::create_directory(panel_path);

    specforge::RuntimePathInputs inputs;
    inputs.executable_path = specforge::CurrentExecutablePath();
    inputs.local_user_state_root_override = root;
    const specforge::SpecForgeStartup startup =
        specforge::PrepareSpecForgeStartup(std::move(inputs));
    const std::filesystem::path spectral_path =
        startup.runtime_paths().spectral_line_user_state_path;
    std::filesystem::create_directory(spectral_path);

    {
        specforge::ShellUi shell(startup);
        Require(
            shell.SetUiLanguageForAutomation(
                     specforge::UiLanguage::SimplifiedChinese)
                    .outcome ==
                specforge::ApplicationSettingsOutcome::PersistenceFailed,
            "real ShellUi language owner should report a blocked save");
        Require(
            shell.SetUiScaleForAutomation(125).outcome ==
                specforge::ApplicationSettingsOutcome::PersistenceFailed,
            "real ShellUi UI scale owner should report a blocked save");
        Require(
            shell.SetPanelVisibilityForAutomation(
                       specforge::ApplicationPanel::Annotations,
                       false)
                .applied(),
            "real ShellUi panel owner should retain its live mutation while blocked");
        const specforge::CatalogUserStateResult spectral_result =
            specforge::ShellUiTestAccess::SubmitSpectralLines(
                shell,
                specforge::CatalogUserStateIntent::CreateUserGroupingView());
        Require(
            spectral_result.status ==
                    specforge::CatalogUserStateResultStatus::Applied &&
                spectral_result.persistent_state_changed,
            "real spectral-line owner should create a dirty persistent state");

        const specforge::LocalUserStateHealthView warned =
            specforge::ShellUiTestAccess::PersistenceHealth(shell);
        Require(
            HasHealthMessage(
                warned,
                specforge::LocalUserStateArea::Language,
                specforge::LocalUserStateHealthMessageKind::LoadWarning) &&
                HasHealthMessage(
                    warned,
                    specforge::LocalUserStateArea::UiScale,
                    specforge::LocalUserStateHealthMessageKind::LoadWarning) &&
                HasHealthMessage(
                    warned,
                    specforge::LocalUserStateArea::PanelVisibility,
                    specforge::LocalUserStateHealthMessageKind::LoadWarning) &&
                HasHealthMessage(
                    warned,
                    specforge::LocalUserStateArea::SpectralLines,
                    specforge::LocalUserStateHealthMessageKind::LoadWarning),
            "real ShellUi health should retain each settings owner's load warning");

        const specforge::ShellLocalStateFlushResult failed =
            shell.FlushLocalState();
        Require(
            failed.application_settings.language_saved &&
                failed.application_settings.ui_scale_saved &&
                failed.application_settings.input_saved &&
                failed.application_settings.profile_output_directory_saved &&
                !failed.application_settings.panel_visibility_saved &&
                failed.source_collection.all_saved() &&
                !failed.spectral_lines_saved &&
                !failed.all_saved(),
            "real ShellUi shutdown flush should retain independent settings failures");
        const std::string shutdown_message = failed.FailureMessage();
        Require(
            shutdown_message.find("Language") == std::string::npos &&
                shutdown_message.find("UI scale") == std::string::npos &&
                shutdown_message.find("Panel visibility") !=
                    std::string::npos &&
                shutdown_message.find("Input") == std::string::npos &&
                shutdown_message.find("Profile output directory") ==
                    std::string::npos &&
                shutdown_message.find("Source session") ==
                    std::string::npos &&
                shutdown_message.find("Spectral-line state") !=
                    std::string::npos,
            "real shutdown aggregation should retain a failed top-level spectral owner");

        const specforge::LocalUserStateHealthView retrying =
            specforge::ShellUiTestAccess::PersistenceHealth(shell);
        Require(
            retrying.kind == specforge::LocalUserStateHealthKind::Retrying &&
                HasHealthMessage(
                    retrying,
                    specforge::LocalUserStateArea::Language,
                    specforge::LocalUserStateHealthMessageKind::SaveWarning) &&
                HasHealthMessage(
                    retrying,
                    specforge::LocalUserStateArea::UiScale,
                    specforge::LocalUserStateHealthMessageKind::SaveWarning) &&
                !HasHealthMessage(
                    retrying,
                    specforge::LocalUserStateArea::Language,
                    specforge::LocalUserStateHealthMessageKind::SaveRetrying) &&
                !HasHealthMessage(
                    retrying,
                    specforge::LocalUserStateArea::UiScale,
                    specforge::LocalUserStateHealthMessageKind::SaveRetrying) &&
                HasHealthMessage(
                    retrying,
                    specforge::LocalUserStateArea::PanelVisibility,
                    specforge::LocalUserStateHealthMessageKind::SaveRetrying) &&
                HasHealthMessage(
                    retrying,
                    specforge::LocalUserStateArea::SpectralLines,
                    specforge::LocalUserStateHealthMessageKind::SaveRetrying),
            "real ShellUi health should retain retrying messages per failed owner");

        std::filesystem::remove_all(language_path);
        std::filesystem::remove_all(ui_scale_path);
        std::filesystem::remove_all(panel_path);
        std::filesystem::remove_all(spectral_path);
        const auto retry_deadline = shell.NextMaintenanceDeadline();
        Require(
            retry_deadline.has_value(),
            "real ShellUi should expose a settings retry deadline after shutdown failure");
        shell.RunMaintenance(*retry_deadline + 10s);

        const std::optional<specforge::UiLanguage> applied_language =
            shell.TakeAppliedUiLanguage();
        const std::optional<int> applied_ui_scale =
            shell.TakeAppliedUiScalePercentage();
        Require(
            !applied_language && !applied_ui_scale &&
                shell.ui_language() == specforge::UiLanguage::English &&
                shell.ui_scale_percentage() ==
                    specforge::kDefaultUiScalePercentage,
            "crossing the retry deadline must not publish a terminally failed automation setting");
        Require(
            !shell.TakeAppliedUiLanguage().has_value() &&
                !shell.TakeAppliedUiScalePercentage().has_value(),
            "a terminally failed automation setting must not publish delayed duplicate updates");

        shell.RunMaintenance(
            specforge::LocalUserStateSaveScheduler::Clock::now() + 10s);
        Require(
            !shell.TakeAppliedUiLanguage().has_value() &&
                !shell.TakeAppliedUiScalePercentage().has_value(),
            "later maintenance should not publish a terminally failed automation setting");
        Require(
            std::filesystem::is_regular_file(spectral_path),
            "the real spectral owner should write after settings failures are recovered");

        const specforge::LocalUserStateHealthView recovered =
            specforge::ShellUiTestAccess::PersistenceHealth(shell);
        Require(
                HasHealthMessage(
                    recovered,
                    specforge::LocalUserStateArea::Language,
                    specforge::LocalUserStateHealthMessageKind::SaveWarning) &&
                HasHealthMessage(
                    recovered,
                    specforge::LocalUserStateArea::UiScale,
                    specforge::LocalUserStateHealthMessageKind::SaveWarning) &&
                !HasHealthMessage(
                    recovered,
                    specforge::LocalUserStateArea::Language,
                    specforge::LocalUserStateHealthMessageKind::Recovered) &&
                !HasHealthMessage(
                    recovered,
                    specforge::LocalUserStateArea::UiScale,
                    specforge::LocalUserStateHealthMessageKind::Recovered) &&
                HasHealthMessage(
                    recovered,
                    specforge::LocalUserStateArea::PanelVisibility,
                    specforge::LocalUserStateHealthMessageKind::Recovered) &&
                HasHealthMessage(
                    recovered,
                    specforge::LocalUserStateArea::SpectralLines,
                    specforge::LocalUserStateHealthMessageKind::Recovered),
            "real ShellUi health should retain terminal setting warnings and cache-owner recovery");

        Require(
            shell.SetUiLanguageForAutomation(
                       specforge::UiLanguage::SimplifiedChinese)
                    .applied() &&
                shell.TakeAppliedUiLanguage() ==
                    specforge::UiLanguage::SimplifiedChinese,
            "a new explicit automation language request should retry after repair");
        Require(
            shell.SetUiScaleForAutomation(125).applied() &&
                shell.TakeAppliedUiScalePercentage() == 125,
            "a new explicit automation UI-scale request should retry after repair");
    }

    {
        specforge::ShellUi reloaded(startup);
        Require(
            reloaded.ui_language() ==
                    specforge::UiLanguage::SimplifiedChinese &&
                reloaded.ui_scale_percentage() == 125 &&
                !reloaded.PanelVisibilityForAutomation().annotations,
            "a real ShellUi restart should reload all recovered settings owners");
        Require(
            reloaded.FlushLocalState().all_saved(),
            "a real ShellUi restart should have a clean independent shutdown flush");
    }

    std::filesystem::remove_all(root, cleanup_error);
}

void TestAutomationSettingsUseApplicationSettingsOwner()
{
    using Access = specforge::ShellUiTestAccess;
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            specforge::SourceCollectionSession(
                std::filesystem::path{},
                std::filesystem::path{},
                std::filesystem::path{},
                std::filesystem::path{}),
            specforge::
                MakeSourceCollectionLoadQueueForTesting());

    const specforge::ApplicationSettingsResult
        language_result =
            shell->SetUiLanguageForAutomation(
                specforge::UiLanguage::
                    SimplifiedChinese);
    Require(
        language_result.outcome ==
                specforge::
                    ApplicationSettingsOutcome::
                        Applied &&
            shell->ui_language() ==
                specforge::UiLanguage::
                    SimplifiedChinese &&
            shell->TakeAppliedUiLanguage() ==
                specforge::UiLanguage::
                    SimplifiedChinese &&
            !shell->TakeAppliedUiLanguage(),
        "automation language changes should use the production owner and emit the normal one-shot UI notification");

    const specforge::ApplicationSettingsResult
        rejected_language =
            shell->SetUiLanguageForAutomation(
                specforge::UiLanguage::Count);
    Require(
        rejected_language.outcome ==
                specforge::
                    ApplicationSettingsOutcome::
                        Rejected &&
            shell->ui_language() ==
                specforge::UiLanguage::
                    SimplifiedChinese &&
            !shell->TakeAppliedUiLanguage(),
        "automation should retain the previous language when production validation rejects a value");

    const specforge::ApplicationSettingsResult
        rejected_scale =
            shell->SetUiScaleForAutomation(151);
    Require(
        rejected_scale.outcome ==
                specforge::
                    ApplicationSettingsOutcome::
                        Rejected &&
            shell->ui_scale_percentage() ==
                specforge::
                    kDefaultUiScalePercentage &&
            !shell->TakeAppliedUiScalePercentage(),
        "automation should not partially apply an out-of-range UI scale");

    const specforge::ApplicationSettingsResult
        scale_result =
            shell->SetUiScaleForAutomation(125);
    Require(
        scale_result.outcome ==
                specforge::
                    ApplicationSettingsOutcome::
                        Applied &&
            shell->ui_scale_percentage() == 125 &&
            shell->TakeAppliedUiScalePercentage() == 125 &&
            !shell->TakeAppliedUiScalePercentage(),
        "automation UI scale changes should emit the same one-shot application notification as the settings panel");

    const specforge::ApplicationSettingsResult
        settings_ui_scale_result =
            Access::ApplySettingsUiIntent(
                *shell,
                specforge::ApplicationSettingsIntent::
                    SetUiScale(130));
    const specforge::ApplicationSettingsResult
        same_value_automation_result =
            shell->SetUiScaleForAutomation(130);
    Require(
        settings_ui_scale_result.outcome ==
                specforge::
                    ApplicationSettingsOutcome::
                        Applied &&
            same_value_automation_result.outcome ==
                specforge::
                    ApplicationSettingsOutcome::
                        Unchanged &&
            shell->ui_scale_percentage() == 130 &&
            shell->TakeAppliedUiScalePercentage() == 130 &&
            !shell->TakeAppliedUiScalePercentage(),
        "a same-value automation write must preserve the production UI scale notification that was already pending from the Settings UI");

    const specforge::ThemeSelection explicit_light =
        specforge::ThemeSelection::Explicit(
            specforge::BuiltInLightThemeId());
    const specforge::ApplicationSettingsResult
        theme_result = Access::ApplySettingsUiIntent(
            *shell,
            specforge::ApplicationSettingsIntent::
                SetThemeSelection(explicit_light));
    Require(
        theme_result.outcome ==
                specforge::ApplicationSettingsOutcome::Applied &&
            shell->theme_selection() == explicit_light &&
            shell->TakeAppliedThemeSelection() ==
                explicit_light &&
            !shell->TakeAppliedThemeSelection(),
        "a Settings appearance change should publish one runtime theme notification through the application settings owner");

    const specforge::ApplicationSettingsResult panel_result =
        shell->SetPanelVisibilityForAutomation(
            specforge::ApplicationPanel::Navigation,
            false);
    const specforge::ApplicationSettingsResult
        same_panel_result =
            shell->SetPanelVisibilityForAutomation(
                specforge::ApplicationPanel::Navigation,
                false);
    const specforge::PanelVisibilityState panel_visibility =
        shell->PanelVisibilityForAutomation();
    Require(
        panel_result.outcome ==
                specforge::ApplicationSettingsOutcome::Applied &&
            same_panel_result.outcome ==
                specforge::ApplicationSettingsOutcome::Unchanged &&
            !panel_visibility.navigation &&
            panel_visibility.files &&
            panel_visibility.annotations,
        "automation panel writes should use the production owner and preserve unrelated panel visibility");
}

void TestAutomationPanelProjectionRequiresExactNormalShellPresent()
{
    using Access = specforge::ShellUiTestAccess;
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            specforge::SourceCollectionSession(
                std::filesystem::path{},
                std::filesystem::path{},
                std::filesystem::path{},
                std::filesystem::path{}),
            specforge::
                MakeSourceCollectionLoadQueueForTesting());

    specforge::PanelVisibilityState first;
    first.files = false;
    first.navigation = false;
    first.annotations = false;
    first.labeling = false;
    first.filters = false;
    first.sorting = false;
    first.smoothing = false;
    first.information = false;
    first.spectral_lines = false;
    Access::SubmitPanelVisibilityDraw(
        *shell,
        501,
        17,
        first);
    const specforge::NavigationLatencyPresentation
        first_presentation{
            17,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        501,
        std::span(&first_presentation, 1));
    const auto first_presented =
        shell->PresentedPanelVisibilityForAutomation();

    specforge::PanelVisibilityState second = first;
    second.files = true;
    Access::SubmitPanelVisibilityDraw(
        *shell,
        502,
        17,
        second);
    Access::SubmitPanelDraw(
        *shell,
        specforge::ApplicationPanel::Files,
        18);
    const std::array<
        specforge::ShellAutomationViewportPresentationState,
        2>
        detached_active{{
            {.viewport_id = 17, .renderable = true},
            {.viewport_id = 18, .renderable = true},
        }};
    const specforge::NavigationLatencyPresentation
        main_only_presentation{
            17,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        502,
        std::span(&main_only_presentation, 1),
        detached_active);
    const auto after_main_only_present =
        shell->PresentedPanelVisibilityForAutomation();
    const specforge::NavigationLatencyPresentation
        wrong_viewport{
            19,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        502,
        std::span(&wrong_viewport, 1),
        detached_active);
    const auto after_wrong_viewport =
        shell->PresentedPanelVisibilityForAutomation();
    const specforge::NavigationLatencyPresentation
        target_presentation{
            18,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        502,
        std::span(&target_presentation, 1),
        detached_active);
    const auto second_presented =
        shell->PresentedPanelVisibilityForAutomation();

    specforge::PanelVisibilityState third = first;
    Access::SubmitPanelVisibilityDraw(
        *shell,
        503,
        17,
        third);
    shell->PresentFrame(
        503,
        std::span(&main_only_presentation, 1),
        detached_active);
    const auto after_hide_main_only =
        shell->PresentedPanelVisibilityForAutomation();
    shell->PresentFrame(
        503,
        std::span(&target_presentation, 1),
        detached_active);
    const auto hidden_after_target_present =
        shell->PresentedPanelVisibilityForAutomation();

    specforge::PanelVisibilityState fourth = first;
    fourth.files = true;
    Access::SubmitPanelVisibilityDraw(
        *shell,
        504,
        17,
        fourth);
    Access::SubmitPanelDraw(
        *shell,
        specforge::ApplicationPanel::Files,
        20);
    const std::array<
        specforge::ShellAutomationViewportPresentationState,
        2>
        second_detached_active{{
            {.viewport_id = 17, .renderable = true},
            {.viewport_id = 20, .renderable = true},
        }};
    const specforge::NavigationLatencyPresentation
        second_target_presentation{
            20,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        504,
        std::span(&second_target_presentation, 1),
        second_detached_active);
    Access::SubmitPanelVisibilityDraw(
        *shell,
        505,
        17,
        first);
    const std::array<
        specforge::ShellAutomationViewportPresentationState,
        1>
        main_only_active{{
            {.viewport_id = 17, .renderable = true},
        }};
    shell->PresentFrame(
        505,
        {},
        main_only_active);
    const auto hidden_after_target_teardown =
        shell->PresentedPanelVisibilityForAutomation();

    specforge::PanelVisibilityState shared_visible = first;
    shared_visible.files = true;
    shared_visible.navigation = true;
    Access::SubmitPanelVisibilityDraw(
        *shell,
        506,
        17,
        shared_visible);
    Access::SubmitPanelDraw(
        *shell,
        specforge::ApplicationPanel::Files,
        30);
    Access::SubmitPanelDraw(
        *shell,
        specforge::ApplicationPanel::Navigation,
        30);
    const std::array<
        specforge::ShellAutomationViewportPresentationState,
        2>
        shared_active{{
            {.viewport_id = 17, .renderable = true},
            {.viewport_id = 30, .renderable = true},
        }};
    const specforge::NavigationLatencyPresentation
        shared_target_presentation{
            30,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        506,
        std::span(&shared_target_presentation, 1),
        shared_active);

    Access::SubmitPanelVisibilityDraw(
        *shell,
        507,
        17,
        shared_visible);
    Access::SubmitPanelDraw(
        *shell,
        specforge::ApplicationPanel::Files,
        30);
    Access::SubmitPanelDraw(
        *shell,
        specforge::ApplicationPanel::Navigation,
        30);
    const std::array<
        specforge::ShellAutomationViewportPresentationState,
        2>
        shared_minimized{{
            {.viewport_id = 17, .renderable = true},
            {.viewport_id = 30, .renderable = false},
        }};
    shell->PresentFrame(
        507,
        std::span(&main_only_presentation, 1),
        shared_minimized);
    const auto shown_while_target_minimized =
        shell->PresentedPanelVisibilityForAutomation();
    const auto shown_minimized_status =
        shell->PanelPresentationStatusForAutomation();

    specforge::PanelVisibilityState shared_hidden =
        shared_visible;
    shared_hidden.files = false;
    Access::SubmitPanelVisibilityDraw(
        *shell,
        508,
        17,
        shared_hidden);
    Access::SubmitPanelDraw(
        *shell,
        specforge::ApplicationPanel::Navigation,
        30);
    shell->PresentFrame(
        508,
        std::span(&main_only_presentation, 1),
        shared_minimized);
    const auto hidden_while_shared_target_minimized =
        shell->PresentedPanelVisibilityForAutomation();
    const auto hidden_minimized_status =
        shell->PanelPresentationStatusForAutomation();
    shell->PresentFrame(
        508,
        std::span(&shared_target_presentation, 1),
        shared_active);
    const auto hidden_after_shared_target_present =
        shell->PresentedPanelVisibilityForAutomation();

    shell->EnterImmersivePlotMode();
    const specforge::NavigationLatencyPresentation
        immersive_presentation{
            17,
            specforge::NavigationLatencyTrace::Now()};
    shell->PresentFrame(
        509,
        std::span(&immersive_presentation, 1));
    const auto after_immersive_present =
        shell->PresentedPanelVisibilityForAutomation();

    Require(
        first_presented.FrameIndex(
                specforge::ApplicationPanel::Files) ==
                501 &&
            first_presented.visibility == first,
        "the exact successful main-viewport Present should publish the normal Shell panel snapshot");
    Require(
        after_main_only_present.FrameIndex(
                specforge::ApplicationPanel::Files) ==
                501 &&
            !after_main_only_present.visibility.files &&
            after_wrong_viewport ==
                after_main_only_present,
        "a main-only or unrelated Present must not publish a panel submitted to a detached viewport");
    Require(
        second_presented.FrameIndex(
                specforge::ApplicationPanel::Files) ==
                502 &&
            second_presented.visibility == second,
        "the target detached viewport Present should publish the shown panel state");
    Require(
        after_hide_main_only.FrameIndex(
                specforge::ApplicationPanel::Files) ==
                502 &&
            after_hide_main_only.visibility.files &&
            hidden_after_target_present.FrameIndex(
                    specforge::ApplicationPanel::Files) ==
                503 &&
            !hidden_after_target_present.visibility.files,
        "hiding a detached panel should wait until its prior viewport presents a frame without the panel");
    Require(
        hidden_after_target_teardown.FrameIndex(
                specforge::ApplicationPanel::Files) ==
                505 &&
            !hidden_after_target_teardown.visibility.files,
        "tearing down a detached panel viewport should also qualify the hidden state without an impossible Present");
    Require(
        shown_while_target_minimized.FrameIndex(
                specforge::ApplicationPanel::Files) ==
                506 &&
            shown_minimized_status.BlockedAfter(
                specforge::ApplicationPanel::Files,
                506),
        "a minimized detached target must block a same-value shown-panel presentation instead of remaining silently pending");
    Require(
        hidden_while_shared_target_minimized.FrameIndex(
                specforge::ApplicationPanel::Files) ==
                506 &&
            hidden_while_shared_target_minimized.visibility.files &&
            hidden_minimized_status.BlockedAfter(
                specforge::ApplicationPanel::Files,
                507) &&
            hidden_after_shared_target_present.FrameIndex(
                    specforge::ApplicationPanel::Files) ==
                508 &&
            !hidden_after_shared_target_present.visibility.files &&
            hidden_after_shared_target_present.visibility.navigation,
        "hiding from a shared minimized viewport must block until that viewport can present without the target panel");
    Require(
        after_immersive_present ==
            hidden_after_shared_target_present,
        "an immersive frame without a normal Shell panel draw must not publish panel visibility");
}

void TestMaintenanceResynchronizesRetainedNavigationTopology()
{
    using Access = specforge::ShellUiTestAccess;
    std::string task_id;
    const std::filesystem::path source_path =
        UniqueTempPath("_maintenance_navigation_source.npy");
    const std::filesystem::path navigation_cache =
        UniqueTempPath("_maintenance_navigation.json");
    const std::filesystem::path labeling_cache =
        UniqueTempPath("_maintenance_labeling.json");
    const std::filesystem::path workflow_cache =
        UniqueTempPath("_maintenance_workflow.json");
    const std::filesystem::path output_path =
        UniqueTempPath("_maintenance_labels.asdf");
    {
        std::ofstream stream(
            source_path,
            std::ios::binary | std::ios::trunc);
        Require(
            stream.good(),
            "maintenance topology fixture should create its source");
        stream << "fixture";
    }
    const specforge::SpectrumSnapshotHandle snapshot =
        MakeSnapshot(source_path, 1);
    specforge::SourceCollectionContext context;
    context.identity =
        specforge::BuildSourceCollectionIdentity(
            *snapshot,
            specforge::CaptureSourceCollectionSingleFileState(
                source_path));
    context.manifest.sample_names = {
        "alpha",
        "beta",
        "gamma"};
    const specforge::SampleWorkflowPreparationPaths paths{
        labeling_cache,
        workflow_cache,
        navigation_cache};

    {
        specforge::SampleLabelingController creator(labeling_cache);
        creator.ActivateSource(context.identity,
            specforge::BuildSampleLabelingCanonicalSourceDescriptor(*snapshot, context));
        Require(creator.CreateTask("Maintenance task").accepted &&
            creator.UpsertActiveLabel({2, "selected", 's'}).accepted &&
            creator.AssignLabel(1, 2).write.accepted &&
            creator.AssignLabel(2, 2).write.accepted &&
            creator.SaveActiveTemporaryTaskToOutput(output_path).output_saved,
            "maintenance topology fixture should publish a canonical task");
        task_id = creator.View().active_task->task_id;
        Require(creator.DeactivateActiveTask().state_saved,
            "maintenance topology fixture should release its owner");
    }

    {
        specforge::SourceCollectionSession seed(
            {},
            navigation_cache,
            labeling_cache,
            workflow_cache);
        specforge::PreparedSampleWorkflowState prepared =
            specforge::PrepareSampleWorkflowState(
                *snapshot,
                context,
                1,
                paths);
        Require(
            seed.OpenPreparedSource(
                    source_path,
                    1,
                    snapshot,
                    context,
                    std::move(prepared))
                .loaded,
            "maintenance topology fixture should open its seed source");
        const std::string filter_source_id =
            "labeling:" + task_id;
        (void)seed.Submit(
            specforge::SourceCollectionSessionIntent::
                ApplySampleFiltering(
                    specforge::SampleFilteringIntent::
                        AddSource(filter_source_id)));
        (void)seed.Submit(
            specforge::SourceCollectionSessionIntent::
                ApplySampleFiltering(
                    specforge::SampleFilteringIntent::
                        SetFilterValueSelected(
                            filter_source_id,
                            "2",
                            true)));
        Require(
            seed.FlushStateCaches(),
            "maintenance topology fixture should persist seed workflow state");
    }

    specforge::SampleLabelingStateCacheLoadResult pending =
        specforge::LoadSampleLabelingStateCache(
            labeling_cache);
    auto pending_source = pending.cache.sources.find(
        context.identity.id);
    Require(
        pending_source != pending.cache.sources.end() &&
            pending_source->second.tasks.size() == 1,
        "maintenance topology fixture should load its task");
    pending_source->second.active_task_id.reset();
    pending_source->second.tasks[0]
        .persistence.pending_sample_indices.insert(0);
    pending_source->second.tasks[0].values.SetPendingValue(0, -1);
    pending_source->second.tasks[0].persistence.save_state.kind =
        specforge::SampleLabelSaveStateKind::Pending;
    pending_source->second.tasks[0].persistence.save_state.pending_count = 1;
    Require(
        specforge::SaveSampleLabelingStateCache(
            labeling_cache,
            pending.cache),
        "maintenance topology fixture should persist a pending retry");

    specforge::SourceCollectionSession session(
        {},
        navigation_cache,
        labeling_cache,
        workflow_cache);
    specforge::PreparedSampleWorkflowState prepared =
        specforge::PrepareSampleWorkflowState(
            *snapshot,
            context,
            1,
            paths);
    Require(
        session.OpenPreparedSource(
                   source_path,
                   1,
                   snapshot,
                   context,
                   std::move(prepared))
            .loaded,
        "maintenance topology fixture should open its stale source");
    Require(
        session.View().navigation.sequence_count == 2 &&
            session.View().navigation.current_sequence_position == 0,
        "maintenance topology fixture should preheat the current row and its pending successor");
    std::promise<void> row_two_entered_promise;
    std::shared_future<void> row_two_entered =
        row_two_entered_promise.get_future().share();
    std::promise<void> release_row_two_promise;
    std::shared_future<void> release_row_two =
        release_row_two_promise.get_future().share();
    specforge::SourceCollectionPreparationAdapters dependencies;
    dependencies.snapshot_loader =
        [&row_two_entered_promise, release_row_two](
            const std::filesystem::path& source,
            std::size_t index,
            const auto& canceled) {
            if (index == 2) {
                row_two_entered_promise.set_value();
                WaitForRelease(
                    release_row_two,
                    canceled,
                    "timed out waiting to cancel the stale maintenance follow-up");
            }
            return MakeSnapshot(source, index);
        };
    dependencies.workflow_cache_loader =
        [](const auto&,
           const std::function<void()>& checkpoint) {
            checkpoint();
            return specforge::
                SampleWorkflowPreparationCacheBundle{};
        };
    dependencies.workflow_cache_paths = paths;
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            std::move(session),
            specforge::MakeSourceCollectionLoadQueueForTesting(
                std::move(dependencies)));
    Access::SyncNavigationInputs(*shell);
    const std::optional<std::uint64_t> old_revision =
        Access::SynchronizedNavigationTopologyRevision(
            *shell);
    const specforge::SourceCollectionSessionResult pending_navigation =
        Access::Submit(
            *shell,
            specforge::SourceCollectionSessionIntent::
                UpdateSampleNavigation(
                    specforge::SampleNavigationIntent::Move(
                        specforge::SampleNavigationRequest::Next())));
    Require(
        pending_navigation.follow_up_spectrum_index == 2 &&
            row_two_entered.wait_for(2s) ==
                std::future_status::ready,
        "maintenance topology fixture should start the stale row-two follow-up");

    specforge::SampleLabelingController editor(
        labeling_cache);
    editor.ActivateSource(context.identity,
        specforge::BuildSampleLabelingCanonicalSourceDescriptor(*snapshot, context));
    Require(
        editor.ActivateTask(
                  task_id)
            .accepted &&
            editor.ClearLabel(2).operation.output_saved &&
            editor.DeactivateActiveTask().state_saved,
        "maintenance topology editor should publish the latest task and release it");

    const auto deadline =
        Access::Session(*shell).NextMaintenanceDeadline();
    Require(
        deadline.has_value(),
        "pending output should schedule Shell maintenance");
    shell->RunMaintenance(*deadline);
    const specforge::SourceCollectionSessionView latest =
        Access::Session(*shell).View();
    const std::optional<std::uint64_t> synchronized_revision =
        Access::SynchronizedNavigationTopologyRevision(
            *shell);
    Require(
        Access::PendingLoadCount(*shell) == 0 &&
            latest.navigation.sequence_count == 1 &&
            latest.navigation.current_index == 1 &&
            latest.navigation.current_sequence_position == 0,
        "maintenance topology change should cancel the invalid row-two follow-up and retain the visible row");
    Require(
        old_revision &&
            synchronized_revision &&
            *synchronized_revision ==
                latest.navigation.sequence_topology_revision &&
            *synchronized_revision != *old_revision,
        "Shell maintenance must route navigation topology actions through retained-input synchronization");
    release_row_two_promise.set_value();
}

void TestHiddenAnnotationsPanelClearsDismissalAcrossSourceRoundTrip()
{
    using Access = specforge::ShellUiTestAccess;
    const std::filesystem::path source_a =
        UniqueTempPath("_annotation_dismissal_a.npy");
    const std::filesystem::path source_b =
        UniqueTempPath("_annotation_dismissal_b.npy");
    WriteFixture(source_a);
    WriteFixture(source_b);

    specforge::SourceCollectionSession session({}, {}, {}, {});
    OpenPreparedFixtureSource(session, source_a);
    OpenPreparedFixtureSource(session, source_b);
    (void)session.Submit(
        specforge::SourceCollectionSessionIntent::
            EditSourceCollection(
                specforge::SourceCollectionIntent::
                    SwitchActive(0)));
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            std::move(session),
            specforge::
                MakeSourceCollectionLoadQueueForTesting());
    Require(
        shell->SetPanelVisibilityForAutomation(
                 specforge::ApplicationPanel::Annotations,
                 false)
            .applied(),
        "annotation dismissal fixture should hide the Annotations panel");

    const std::string source_a_identity =
        Access::Session(*shell).View().labeling.source_identity;
    Require(
        !source_a_identity.empty(),
        "annotation dismissal fixture should expose source A identity");
    Access::SeedAnnotationDiagnosticDismissal(
        *shell,
        source_a_identity);

    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::
            EditSourceCollection(
                specforge::SourceCollectionIntent::
                    SwitchActive(1)));
    (void)Access::Submit(
        *shell,
        specforge::SourceCollectionSessionIntent::
            EditSourceCollection(
                specforge::SourceCollectionIntent::
                    SwitchActive(0)));

    Require(
        !shell->PanelVisibilityForAutomation().annotations &&
            Access::AnnotationDiagnosticSourceIdentity(
                *shell) == source_a_identity &&
            Access::AnnotationDiagnosticDismissalCount(
                *shell) == 0,
        "a hidden Annotations panel must observe the intermediate source and clear source A dismissal state across A-to-B-to-A");

    shell.reset();
    std::filesystem::remove(source_a);
    std::filesystem::remove(source_b);
}

void TestShellWorkflowResetPreservesSameFrameLabelingIssue()
{
    using Access = specforge::ShellUiTestAccess;
    std::unique_ptr<specforge::ShellUi> shell =
        Access::Create(
            specforge::SourceCollectionSession(
                {},
                {},
                {},
                {}),
            specforge::
                MakeSourceCollectionLoadQueueForTesting());
    specforge::SourceCollectionSessionResult rejected;
    rejected.labeling_issue =
        specforge::SampleLabelingOperationResult::Issue::
            EditTargetChanged;
    rejected.action.workflow_changed = true;
    Access::CaptureLabelingOperationResult(
        *shell,
        rejected);
    const std::string expected{
        specforge::UiText(
            specforge::UiLanguage::English,
            specforge::UiTextId::
                LabelingEditTargetChanged)};
    Require(
        Access::LabelingOperationMessage(*shell) ==
            expected,
        "Shell notice fixture should capture the structured labeling issue before workflow reset");

    Access::HandleSessionAction(
        *shell,
        rejected.action);
    Require(
        Access::LabelingOperationMessage(*shell) ==
            expected,
        "same-frame Shell workflow reset must not erase the labeling issue captured by the submitting panel");
}

void TestShellRecoveryProjectionDoesNotResetUnrelatedEditingState()
{
    const std::filesystem::path source_path =
        UniqueTempPath("_shell_recovery_projection.npy");
    const std::filesystem::path legacy_annotation_path =
        UniqueTempPath("_shell_recovery_projection_legacy_labels.npy");
    const std::filesystem::path formal_output_path =
        UniqueTempPath("_shell_recovery_projection_labels.asdf");
    const SourceSessionCachePaths cache_paths{
        .source_session = UniqueTempPath("_shell_recovery_projection_sources.json"),
        .navigation = UniqueTempPath("_shell_recovery_projection_navigation.json"),
        .labeling = UniqueTempPath("_shell_recovery_projection_labeling.json"),
        .workflow = UniqueTempPath("_shell_recovery_projection_workflow.json")};
    std::error_code cleanup_error;
    for (const std::filesystem::path& path : {
             source_path,
             legacy_annotation_path,
             specforge::test_support::LegacyFixtureIo::
                 MetadataPathForResult(legacy_annotation_path),
             formal_output_path,
             cache_paths.source_session,
             cache_paths.navigation,
             cache_paths.labeling,
             cache_paths.workflow,
             std::filesystem::path(cache_paths.labeling.string() + ".locks")}) {
        std::filesystem::remove_all(path, cleanup_error);
    }
    WriteFixture(source_path);
    specforge::SampleLabelingTask annotation_seed =
        specforge::CreateSampleLabelingTask(
            "legacy-formal-task",
            "Legacy formal task",
            3);
    annotation_seed.label_set.labels.push_back(
        specforge::SampleLabelDefinition{
            8,
            "Review",
            'r'});
    const specforge::test_support::SampleLabelResultWriteOutcome annotation_write =
        specforge::test_support::LegacyFixtureIo{}.SaveLabelResult(
            legacy_annotation_path,
            annotation_seed);
    Require(
        annotation_write.array_saved && annotation_write.metadata_saved,
        "Shell recovery fixture should seed a formal annotation file");

    const auto change_workflow = [](
                                      specforge::ActiveSampleWorkflowIntent intent) {
        return specforge::SourceCollectionSessionIntent::
            ChangeActiveSampleWorkflow(std::move(intent));
    };
    std::string source_identity;
    std::string draft_task_id;
    {
        specforge::SourceCollectionSession editor =
            MakeCachedSession(cache_paths);
        OpenPreparedFixtureSource(
            editor,
            source_path,
            0,
            legacy_annotation_path,
            specforge::SampleWorkflowPreparationPaths{
                .labeling_state_cache_path = cache_paths.labeling,
                .workflow_state_cache_path = cache_paths.workflow,
                .navigation_state_cache_path = cache_paths.navigation});
        Require(
            editor.FlushStateCaches(),
            "Shell recovery fixture should seed its persistent cache before task output save");
        (void)editor.Submit(
            change_workflow(
                specforge::ActiveSampleWorkflowIntent::
                    StartOrResumeTemporaryLabelingTask()));
        Require(
            editor.View().labeling.has_active_task &&
                editor.View().labeling.active_task_is_temporary,
            "Shell recovery fixture should create its formal precursor draft");
        source_identity = editor.View().labeling.source_identity;
        (void)editor.Submit(
            change_workflow(
                specforge::ActiveSampleWorkflowIntent::
                    SetActiveLabelingOutputPath(
                        formal_output_path)));
        Require(
            !editor.View().labeling.active_task_is_temporary,
            "Shell recovery fixture should formalize the precursor task");
        Require(
            editor.Submit(
                       change_workflow(
                           specforge::ActiveSampleWorkflowIntent::
                               UpsertActiveLabel(
                                   specforge::SampleLabelDefinition{
                                       8,
                                       "Review",
                                       'r'})))
                .changed,
            "Shell recovery fixture should seed a formal label");
        Require(
            editor.Submit(
                       change_workflow(
                           specforge::ActiveSampleWorkflowIntent::
                               AssignActiveLabelToCurrentSample(8)))
                .label_write
                .has_value(),
            "Shell recovery fixture should seed formal undo history");
        (void)editor.Submit(
            change_workflow(
                specforge::ActiveSampleWorkflowIntent::
                    StartOrResumeTemporaryLabelingTask()));
        Require(
            editor.View().labeling.active_task_is_temporary,
            "Shell recovery fixture should create a paused-draft companion");
        draft_task_id = editor.View().labeling.task_id;
        (void)editor.Submit(
            change_workflow(
                specforge::ActiveSampleWorkflowIntent::
                    ActivateLabelingTaskFromAnnotation(
                        formal_output_path)));
        Require(
            !editor.View().labeling.active_task_is_temporary &&
                editor.View().labeling.task_id != draft_task_id,
            "Shell recovery fixture should restore the formal task while retaining the draft");
        Require(
            editor.FlushStateCaches(),
            "Shell recovery fixture should persist its formal task and paused draft");
    }

    std::unique_ptr<specforge::ShellUi> shell;
    {
        specforge::SourceCollectionSession session =
            MakeCachedSession(cache_paths);
        OpenPreparedFixtureSource(
            session,
            source_path,
            0,
            legacy_annotation_path,
            specforge::SampleWorkflowPreparationPaths{
                .labeling_state_cache_path = cache_paths.labeling,
                .workflow_state_cache_path = cache_paths.workflow,
                .navigation_state_cache_path = cache_paths.navigation});
        shell = specforge::ShellUiTestAccess::Create(
            std::move(session),
            specforge::MakeSourceCollectionLoadQueueForTesting());
    }
    const auto& shell_initial_view =
        specforge::ShellUiTestAccess::Session(*shell).View();
    Require(
        shell_initial_view.labeling.has_active_task &&
            !shell_initial_view.labeling.active_task_is_temporary,
        "Shell recovery fixture should restore the formal active task");

    (void)specforge::ShellUiTestAccess::Submit(
        *shell,
        change_workflow(
            specforge::ActiveSampleWorkflowIntent::
                ClearActiveLabelForCurrentSample()));
    const specforge::SourceCollectionSessionResult shell_assignment =
        specforge::ShellUiTestAccess::Submit(
            *shell,
            change_workflow(
                specforge::ActiveSampleWorkflowIntent::
                    AssignActiveLabelToCurrentSample(8)));
    Require(
        shell_assignment.label_write &&
            shell_assignment.label_write->write.changed &&
            specforge::ShellUiTestAccess::Session(*shell).View().labeling.current_code == 8,
        "Shell recovery fixture should seed undo history on the restored formal task");

    const std::string active_task_id_before_delete =
        specforge::ShellUiTestAccess::Session(*shell).View().labeling.task_id;
    specforge::ShellUiTestAccess::SetLabelEditingState(*shell);
    const specforge::SourceCollectionSessionResult deleted =
        specforge::ShellUiTestAccess::SubmitThroughPanel(
            *shell,
            change_workflow(
                specforge::ActiveSampleWorkflowIntent::
                    DeleteTemporaryLabelingTask(
                        source_identity,
                        draft_task_id)));
    const specforge::SourceCollectionSessionAction delete_action =
        specforge::ShellUiTestAccess::TakePanelAction(*shell);
    specforge::ShellUiTestAccess::HandleSessionAction(
        *shell,
        delete_action);
    const std::string active_task_id_after_delete =
        specforge::ShellUiTestAccess::Session(*shell).View().labeling.task_id;
    Require(
        deleted.changed &&
            deleted.view_invalidated &&
            !delete_action.workflow_changed &&
            specforge::ShellUiTestAccess::HasLabelEditingState(*shell) &&
            specforge::ShellUiTestAccess::Session(*shell).View().labeling.has_active_task &&
            !specforge::ShellUiTestAccess::Session(*shell).View().labeling.active_task_is_temporary &&
            active_task_id_after_delete == active_task_id_before_delete &&
            specforge::ShellUiTestAccess::Session(*shell).View().labeling.current_code == 8 &&
            specforge::ShellUiTestAccess::Session(*shell).View().labeling.recovery_drafts.empty(),
        "Shell delete of an unrelated draft should refresh only recovery projection and preserve editing state");

    const specforge::SourceCollectionSessionResult undone =
        specforge::ShellUiTestAccess::Submit(
            *shell,
            change_workflow(
                specforge::ActiveSampleWorkflowIntent::
                    UndoLastLabelWrite()));
    Require(
        undone.label_write &&
            undone.label_write->write.changed &&
            specforge::ShellUiTestAccess::Session(*shell).View().labeling.current_code ==
                specforge::kUnlabeledSampleLabelCode,
        "Shell delete of an unrelated draft should preserve formal undo history");

    const specforge::SourceCollectionSessionResult replacement_started =
        specforge::ShellUiTestAccess::Submit(
            *shell,
            change_workflow(
                specforge::ActiveSampleWorkflowIntent::
                    StartOrResumeTemporaryLabelingTask()));
    const auto& replacement_view =
        specforge::ShellUiTestAccess::Session(*shell).View().labeling;
    Require(
        replacement_started.view_invalidated &&
            replacement_view.has_active_task &&
            replacement_view.active_task_is_temporary &&
            replacement_view.task_id != active_task_id_before_delete,
        "Shell recovery fixture should recreate a replacement draft");
    const std::string replacement_task_id =
        specforge::ShellUiTestAccess::Session(*shell).View().labeling.task_id;
    const specforge::SourceCollectionSessionResult paused_replacement =
        specforge::ShellUiTestAccess::Submit(
            *shell,
            change_workflow(
                specforge::ActiveSampleWorkflowIntent::
                    ActivateLabelingTaskFromAnnotation(
                        formal_output_path)));
    Require(
        paused_replacement.view_invalidated &&
            specforge::ShellUiTestAccess::Session(*shell).View().labeling.has_temporary_task &&
            !specforge::ShellUiTestAccess::Session(*shell).View().labeling.active_task_is_temporary &&
            specforge::ShellUiTestAccess::Session(*shell).FlushStateCaches(),
        "Shell recovery fixture should pause the replacement draft beside the formal task");

    specforge::SampleLabelingController holder(cache_paths.labeling);
    holder.ActivateSource(source_identity, 3);
    Require(
        holder.ActivateTask(replacement_task_id).accepted,
        "Shell recovery fixture should hold the replacement draft lease externally");

    specforge::ShellUiTestAccess::SetLabelEditingState(*shell);
    const specforge::SourceCollectionSessionResult rejected_delete =
        specforge::ShellUiTestAccess::SubmitThroughPanel(
            *shell,
            change_workflow(
                specforge::ActiveSampleWorkflowIntent::
                    DeleteTemporaryLabelingTask(
                        source_identity,
                        replacement_task_id)));
    const specforge::SourceCollectionSessionAction rejected_delete_action =
        specforge::ShellUiTestAccess::TakePanelAction(*shell);
    specforge::ShellUiTestAccess::HandleSessionAction(
        *shell,
        rejected_delete_action);
    Require(
        rejected_delete.labeling_issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditLeaseUnavailable &&
            !rejected_delete_action.workflow_changed &&
            specforge::ShellUiTestAccess::HasLabelEditingState(*shell),
        "Shell should preserve editing state when an unrelated draft delete is lease-rejected");

    Require(
        holder.DeleteActiveTask().accepted,
        "Shell recovery fixture should delete the externally held replacement draft");
    specforge::ShellUiTestAccess::SetLabelEditingState(*shell);
    const specforge::SourceCollectionSessionResult reconciled_recovery =
        specforge::ShellUiTestAccess::SubmitThroughPanel(
            *shell,
            change_workflow(
                specforge::ActiveSampleWorkflowIntent::
                    RecoverTemporaryLabelingTask(
                        source_identity,
                        replacement_task_id)));
    const specforge::SourceCollectionSessionAction recovery_action =
        specforge::ShellUiTestAccess::TakePanelAction(*shell);
    specforge::ShellUiTestAccess::HandleSessionAction(
        *shell,
        recovery_action);
    Require(
        reconciled_recovery.labeling_issue ==
                specforge::SampleLabelingOperationResult::Issue::
                    EditTargetChanged &&
            reconciled_recovery.view_invalidated &&
            !recovery_action.workflow_changed &&
            specforge::ShellUiTestAccess::HasLabelEditingState(*shell) &&
            specforge::ShellUiTestAccess::Session(*shell).View().labeling.recovery_drafts.empty(),
        "Shell recovery projection convergence should not reset an unchanged formal workflow");

    shell.reset();
    for (const std::filesystem::path& path : {
             source_path,
             legacy_annotation_path,
             specforge::test_support::LegacyFixtureIo::
                 MetadataPathForResult(legacy_annotation_path),
             formal_output_path,
             cache_paths.source_session,
             cache_paths.navigation,
             cache_paths.labeling,
             cache_paths.workflow,
             std::filesystem::path(cache_paths.labeling.string() + ".locks")}) {
        std::filesystem::remove_all(path, cleanup_error);
    }
}

}  // namespace

#define RUN_SHELL_TEST(test) do { \
    std::fprintf(stderr, "Running %s\n", #test); \
    std::fflush(stderr); \
    test(); \
} while (false)

int main()
{
    try {
        RUN_SHELL_TEST(TestAutomationGotoAndTargetedLabelNavigationRespectActiveSequence);
        RUN_SHELL_TEST(TestExplicitOpenTracesAcceptedPathThroughFirstPresent);
        RUN_SHELL_TEST(TestSupersededExternalPreferredTraceUsesResolvedMemberIndex);
        RUN_SHELL_TEST(TestExternalStartupPreferredMemberDoesNotYieldFilteredFallback);
        RUN_SHELL_TEST(TestExternalStartupPreferredMemberCannotBeOverriddenByLiveSampleFilter);
        RUN_SHELL_TEST(TestSourceOpenResolutionRunsOnWorkerAndCancels);
        RUN_SHELL_TEST(TestExternalStartupPreservesDeferredRestoreAnnotationContext);
        RUN_SHELL_TEST(TestExternalStartupPreservesPreferredMemberForFitsAndCsvAndOtherOriginsStayDirect);
        RUN_SHELL_TEST(TestFailedExplicitOpenProducesTerminalSourceLoadReport);
        RUN_SHELL_TEST(TestRealDrainCommitsOnlyTheLatestRapidNavigation);
        RUN_SHELL_TEST(TestAcceptedNavigationUsesLatestMatchingRawKeyInput);
        RUN_SHELL_TEST(TestGenericRowLocationDoesNotStartPreviousNextTrace);
        RUN_SHELL_TEST(TestWorkflowAutoAdvanceStartsExplicitTrace);
        RUN_SHELL_TEST(TestWarmUiAndKeyboardNavigationReuseSequenceStateAtFixedIndices);
        RUN_SHELL_TEST(TestNewActivationSupersedesAnUnpresentedOlderTrace);
        RUN_SHELL_TEST(TestPresentationWithoutSpectrumDrawDoesNotCompleteNavigation);
        RUN_SHELL_TEST(TestAutomationSourceObservationHidesActivationToken);
        RUN_SHELL_TEST(TestAutomationPresentedViewAdvancesOnlyAfterSuccessfulPresent);
        RUN_SHELL_TEST(TestSameFrameSourceSwitchSupersedesActivatedNavigation);
        RUN_SHELL_TEST(TestPublishedStaleCompletionIsRejectedWithoutMutatingNewNavigation);
        RUN_SHELL_TEST(TestRealDrainPreservesWorkflowChangesMadeWhileFullPlanWaits);
        RUN_SHELL_TEST(TestRealDrainRequeuesReconciledTargetAndRetiresIntermediateSnapshotOffThread);
        RUN_SHELL_TEST(TestShellShutdownFlushPersistsLockedViewport);
        RUN_SHELL_TEST(TestDeferredRestoreReusesOnlyMatchingLockedViewport);
        RUN_SHELL_TEST(TestDeferredRestoreCompletionPreservesUnrelatedNavigationTicket);
        RUN_SHELL_TEST(TestDeferredRestoreFollowUpFailureClearsPendingAndAllowsRetry);
        RUN_SHELL_TEST(TestDeferredRestorePreservesSavedActiveSourceAfterLaterCompletion);
        RUN_SHELL_TEST(TestIdlePrefetchIsConsumedBySecondForwardNavigation);
        RUN_SHELL_TEST(TestPublishedPrefetchBecomesStaleAfterQueryInput);
        RUN_SHELL_TEST(TestCanceledPrefetchReportsOnlyAfterWorkerExit);
        RUN_SHELL_TEST(TestAutomationSettingsUseApplicationSettingsOwner);
        RUN_SHELL_TEST(TestAutomationPanelProjectionRequiresExactNormalShellPresent);
        RUN_SHELL_TEST(TestMaintenanceResynchronizesRetainedNavigationTopology);
        RUN_SHELL_TEST(TestHiddenAnnotationsPanelClearsDismissalAcrossSourceRoundTrip);
        RUN_SHELL_TEST(TestShellWorkflowResetPreservesSameFrameLabelingIssue);
        RUN_SHELL_TEST(TestShellRecoveryProjectionDoesNotResetUnrelatedEditingState);
        RUN_SHELL_TEST(TestShellFlushResultNamesEveryFailedOwner);
        RUN_SHELL_TEST(TestRealShellFlushAndHealthKeepIndependentSettingsOwners);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
