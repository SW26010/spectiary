
#include "ui/spectral_lines_panel_controller.h"
#include "overlays/built_in_spectral_line_state_io.h"
#include "platform/exclusive_file_lease.h"
#include "helpers/temporary_directory.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace spectiary;
using Intent = CatalogUserStateIntent;
using Status = CatalogUserStateResultStatus;
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void Applied(const CatalogUserStateResult& result) { Require(result.status == Status::Applied, result.message); }
SpectralLineList Base()
{
    SpectralLineList list;
    list.id = "public-spectral-lines.v1"; list.name = "Public spectral lines";
    list.markers = {{"a", "Hα", line_list::MarkerKind::Line, 6564.608, {}, {}, {}},
                    {"b", "C₂", line_list::MarkerKind::Band, {}, 5100, 5200, "band note"},
                    {"c", "C I", line_list::MarkerKind::Line, 6000, {}, {}, {}}};
    list.grouping_views = {{"base-one", "Base one", {{"base-g1", "Hydrogen", {"a"}}}},
                           {"base-two", "Base two", {{"base-g2", "Carbon", {"c", "b"}}}}};
    return list;
}
const SpectralLineGroupingView& View(const CatalogUserStateView& state, const std::string& id)
{
    for (const auto& view : state.grouping_views) if (view.id == id) return view;
    throw std::runtime_error("missing view: " + id);
}
std::string Active(const CatalogUserStateView& state)
{
    for (const auto& view : state.grouping_views) if (view.active) return view.id;
    return {};
}
std::string CreateView(SpectralLinesPanelController& controller)
{
    Applied(controller.Submit(Intent::CreateUserGroupingView()));
    return Active(controller.View());
}
std::string AddGroup(SpectralLinesPanelController& controller, const std::string& view)
{
    Applied(controller.Submit(Intent::AddUserGroup(view)));
    auto state = controller.View();
    const auto& groups = View(state, view).groups;
    return groups.at(groups.size() - 2).id;
}
std::string Bytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void TestOperations(const std::filesystem::path& path)
{
    SpectralLinesPanelController controller(Base(), path);
    auto state = controller.View();
    Require(state.grouping_views.size() == 2 && state.user_grouping_view_count == 0, "multiple base views exposed");
    for (const auto& view : state.grouping_views) {
        Require(!view.editable, "base ownership is explicit for every view");
        Require(controller.Submit(Intent::DeleteUserGroupingView(view.id)).status == Status::Rejected, "base delete rejected");
        Require(controller.Submit(Intent::AddUserGroup(view.id)).status == Status::Rejected, "base add rejected");
    }
    Require(controller.Submit(Intent::SetMarkerVisibility("missing", false)).status == Status::Rejected, "foreign marker rejected");
    Require(controller.Submit(Intent::SelectGroupingView("missing")).status == Status::Rejected, "foreign view rejected");
    auto snapshot = std::make_shared<SpectrumSnapshot>(); snapshot->capabilities.can_show_spectral_lines = true;
    Require(controller.PlotView(snapshot).visible_markers.size() == 3, "default marker visibility remains visible");
    Applied(controller.Submit(Intent::SetMarkerVisibility("a", false)));
    Require(controller.PlotView(snapshot).visible_markers.size() == 2, "visibility controls plot independently of grouping");
    const auto user = CreateView(controller);
    auto first = AddGroup(controller, user), second = AddGroup(controller, user);
    Applied(controller.Submit(Intent::MoveMarkerReference(user, "a", "__unassigned__", first)));
    Applied(controller.Submit(Intent::CopyMarkerReference(user, "a", first, second)));
    state = controller.View();
    Require(View(state, user).groups[0].marker_references[0].shared, "shared references detected");
    Applied(controller.Submit(Intent::MoveMarkerReference(user, "b", "__unassigned__", first)));
    Require(controller.Submit(Intent::MoveMarkerReference(user, "a", first, first)).status == Status::NoChange, "same-source move no-op");
    Applied(controller.Submit(Intent::SetGroupingViewSearch("Hα")));
    Require(controller.Submit(Intent::SetGroupMarkerVisibility(user, first, true)).status == Status::Rejected, "search cannot bulk toggle");
    Applied(controller.Submit(Intent::CopyMarkerReference(user, "b", first, second)));
    Applied(controller.Submit(Intent::SetGroupingViewSearch("")));
    Applied(controller.Submit(Intent::SetGroupMarkerVisibility(user, first, true)));
    Require(controller.PlotView(snapshot).visible_markers.size() == 3, "group visibility changes shared marker state");
    Applied(controller.Submit(Intent::ReorderUserGroupBefore(user, second, first)));
    Applied(controller.Submit(Intent::RenameUserGroup(user, first, "Unassigned", CatalogUserRenameEditState::Edited)));
    Applied(controller.Submit(Intent::SetGroupExpanded(user, first, true)));
    state = controller.View();
    Require(View(state, user).groups[0].id == second && View(state, user).groups[1].name == "Unassigned" &&
            !View(state, user).groups[1].is_unassigned, "authored Unassigned has ordinary semantics");
    Applied(controller.Submit(Intent::DuplicateGroupingView(user)));
    const auto copy = Active(controller.View());
    Require(copy != user, "duplicate has fresh identity");
    state = controller.View();
    Require(View(state, copy).groups[0].id != second, "duplicate group identities fresh");
    Applied(controller.Submit(Intent::RenameUserGroupingView(copy, "Grouping 1", CatalogUserRenameEditState::Edited)));
    Require(View(controller.View(), copy).generated_name == GeneratedNameMetadata{}, "explicit rename clears generated provenance");
    Applied(controller.Submit(Intent::DeleteUserGroupingView(copy)));
    Applied(controller.Submit(Intent::SelectGroupingView(user)));
    Applied(controller.Submit(Intent::RemoveMarkerReference(user, "a", first)));
    Require(View(controller.View(), user).groups[0].marker_references[1].marker_id == "a", "removing one shared reference preserves others");
    Applied(controller.Submit(Intent::SetMarkerColor("a", PlotSeriesColor::ExplicitColor({1, 0, 0, 1}))));
    Applied(controller.Submit(Intent::SetMarkerColor("b", PlotSeriesColor::ExplicitColor({0, 1, 0, 1}))));
    const auto slot = controller.PlotView(snapshot).visible_markers[0].automatic_color_slot;
    Applied(controller.Submit(Intent::SelectGroupingView("base-two")));
    Require(controller.PlotView(snapshot).visible_markers[0].automatic_color_slot == slot, "palette slots remain stable across view selection");
    Require(controller.Flush(), "controller saves state");
    auto loaded = LoadBuiltInSpectralLineState(path, Base());
    Require(loaded.error.empty() && loaded.state.overlay.grouping_views.size() == 1, loaded.error);
    Require(loaded.state.overlay.grouping_views[0].groups.size() == 2, "Unassigned projection not persisted");
    Require(loaded.state.overlay.grouping_views[0].groups[0].marker_ids == std::vector<std::string>{"a", "b"}, "stored order survives display and save");
    Require(View(controller.View(), user).groups[0].marker_references[0].marker_id == "b",
            "UI may project wavelength order independently of the preserved marker_ids array");
    Require(Bytes(path).find("is_unassigned") == std::string::npos && Bytes(path).find("marker_references") == std::string::npos, "new writer emits only canonical subrecords");
    SpectralLinesPanelController reopened(Base(), path);
    Require(Active(reopened.View()) == "base-two", "selection restored independently");
    Require(reopened.PlotView(snapshot).visible_markers[0].color == PlotSeriesColor::ExplicitColor({1, 0, 0, 1}), "explicit color persists");
    Applied(reopened.Submit(Intent::SetMarkerColor("a", PlotSeriesColor::Auto())));
    Require(reopened.Flush(), "Auto reset saves");
    loaded = LoadBuiltInSpectralLineState(path, Base());
    Require(loaded.state.overlay.color_schemes && !loaded.state.overlay.color_schemes->front().colors.contains("a") &&
            loaded.state.overlay.color_schemes->front().colors.contains("b"), "Auto reset removes only selected marker mapping");
}
void TestPersistence(const std::filesystem::path& root)
{
    const auto path = root / "persistence.json";
    SpectralLinesPanelController stale(Base(), path), peer(Base(), path);
    const auto a = CreateView(stale), b = CreateView(peer);
    Require(peer.Flush() && stale.Flush(), "stale independent views reconcile");
    auto loaded = LoadBuiltInSpectralLineState(path, Base());
    Require(loaded.state.overlay.grouping_views.size() == 2, "both additions preserved");
    SpectralLinesPanelController left(Base(), path), right(Base(), path);
    Applied(left.Submit(Intent::RenameUserGroupingView(a, "Pending", CatalogUserRenameEditState::Edited)));
    Applied(right.Submit(Intent::DeleteUserGroupingView(a)));
    Require(right.Flush() && !left.Flush(), "deleted edited view is controlled conflict");
    Require(View(left.View(), a).name == "Pending", "conflict retains in-memory edit");
    Require(LoadBuiltInSpectralLineState(path, Base()).state.overlay.grouping_views.size() == 1, "conflict does not recreate entity");

    const auto corrupt = root / "corrupt.json";
    { std::ofstream(corrupt) << "{broken"; }
    {
        SpectralLinesPanelController warning(Base(), corrupt);
        Require(warning.View().grouping_views.size() == 2 && warning.View().persistence.load_issue != SpectralLineCacheLoadIssueKind::None,
                "invalid overlay shows base with diagnostic");
        Applied(warning.Submit(Intent::SetMarkerVisibility("a", false)));
        Require(!warning.Flush() && Bytes(corrupt) == "{broken", "rejected startup state cannot be overwritten");
        std::string error; Require(SaveBuiltInSpectralLineState(corrupt, Base(), {}, error), error);
        Require(warning.Flush(), "explicit external repair permits pending write recovery");
        Require(warning.View().persistence.recovered, "recovery is presented");
    }
    const auto corrupt_latest = root / "latest.json";
    {
        SpectralLinesPanelController controller(Base(), corrupt_latest);
        Applied(controller.Submit(Intent::SetMarkerVisibility("b", false)));
        { std::ofstream(corrupt_latest) << "{\"x\":1,\"x\":2}"; }
        const auto before = Bytes(corrupt_latest);
        Require(!controller.Flush() && Bytes(corrupt_latest) == before, "latest duplicate keys fail closed");
    }
    const auto shutdown = root / "shutdown.json";
    { SpectralLinesPanelController controller(Base(), shutdown); Applied(controller.Submit(Intent::SetMarkerVisibility("c", false))); }
    Require(!LoadBuiltInSpectralLineState(shutdown, Base()).state.session.marker_visibility.at("c"), "destructor flushes pending state");
    const auto lock_path = root / "leased.json";
    auto lease_file = lock_path; lease_file += ".commit.lock";
    auto lease = TryAcquireExclusiveFileLease(lease_file);
    Require(lease.status == ExclusiveFileLeaseAcquireStatus::Acquired, "test acquires commit lease");
    SpectralLinesPanelController locked(Base(), lock_path);
    Applied(locked.Submit(Intent::SetMarkerVisibility("a", false)));
    Require(!locked.Flush() && !std::filesystem::exists(lock_path), "busy lease cannot publish");
    lease.lease = ExclusiveFileLease{};
    Require(locked.Flush(), "released lease allows retry");
    (void)b;
}
void WaitFile(const std::filesystem::path& path)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!std::filesystem::exists(path)) {
        Require(std::chrono::steady_clock::now() < deadline, "child barrier timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
int Child(const std::filesystem::path& root, const std::string& id)
{
    SpectralLinesPanelController controller(Base(), root / "shared.json");
    Applied(controller.Submit(Intent::SetMarkerColor(id, PlotSeriesColor::ExplicitColor(id == "a" ? RgbaColor{1,0,0,1} : RgbaColor{0,1,0,1}))));
    { std::ofstream(root / (id + ".ready")) << "ready"; }
    WaitFile(root / "go");
    Require(controller.Flush(), "child concurrent save failed");
    return 0;
}
void TestProcesses(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);
    wchar_t executable[32768]{};
    Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "test executable path");
    struct Process {
        PROCESS_INFORMATION info{};
        ~Process() { if (info.hProcess) { if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(info.hProcess, 10); CloseHandle(info.hProcess); } if (info.hThread) CloseHandle(info.hThread); }
    } a, b;
    const auto start = [&](Process& process, const wchar_t* id) {
        auto command = L"\"" + std::wstring(executable) + L"\" --child \"" + root.wstring() + L"\" " + id;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        Require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process.info) != FALSE, "start child process");
    };
    start(a, L"a"); start(b, L"b");
    WaitFile(root / "a.ready"); WaitFile(root / "b.ready");
    { std::ofstream(root / "go") << "go"; }
    for (auto* process : {&a, &b}) {
        Require(WaitForSingleObject(process->info.hProcess, 15000) == WAIT_OBJECT_0, "child completes bounded save");
        DWORD code = 1; GetExitCodeProcess(process->info.hProcess, &code); Require(code == 0, "child succeeds");
    }
    const auto state = LoadBuiltInSpectralLineState(root / "shared.json", Base());
    Require(state.error.empty() && state.state.overlay.color_schemes && state.state.overlay.color_schemes->front().colors.size() == 2,
            "OS commit lease preserves concurrent first overrides across processes");
}
}
int wmain(int argc, wchar_t* argv[])
{
    try {
        if (argc == 4 && std::wstring_view(argv[1]) == L"--child") return Child(argv[2], std::wstring_view(argv[3]) == L"a" ? "a" : "b");
        spectiary::test_support::TemporaryDirectory directory;
        TestOperations(directory.path() / "operations.json"); TestPersistence(directory.path()); TestProcesses(directory.path() / "processes");
        std::cout << "Spectral-line controller tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
