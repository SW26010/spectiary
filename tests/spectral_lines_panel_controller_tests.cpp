
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
using Intent = SpectralLineStateIntent;
using Status = SpectralLineStateResultStatus;
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void Applied(const SpectralLineStateResult& result) { Require(result.status == Status::Applied, result.message); }
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
const SpectralLineGroupingView& View(const SpectralLinePanelView& state, const std::string& id)
{
    for (const auto& view : state.grouping_views) if (view.id == id) return view;
    throw std::runtime_error("missing view: " + id);
}
std::string Active(const SpectralLinePanelView& state)
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
void TestUserLineLists(const std::filesystem::path& root)
{
    const auto managed = root / "managed";
    const auto outside = root / "documents";
    std::filesystem::create_directories(managed);
    std::filesystem::create_directories(outside);
    RuntimePaths paths;
    paths.application_data_root = managed;
    paths.package_root = managed;
    paths.storage_profile = StorageProfile::Portable;
    const auto state_path = managed / "state" / "built-in.json";
    SpectralLinesPanelController controller(Base(), state_path);
    auto snapshot = std::make_shared<SpectrumSnapshot>();
    snapshot->capabilities.can_show_spectral_lines = true;
    snapshot->capabilities.requires_rest_frame_warning = true;
    const auto builtin_view = CreateView(controller);
    Applied(controller.Submit(Intent::SetMarkerVisibility("a", false)));
    Applied(controller.Submit(Intent::SetGroupingViewSearch("builtin search")));
    Applied(controller.Submit(Intent::SetMarkerLabelsVisible(false)));
    Require(controller.OpenUserLineList(outside / "missing", paths).status == Status::Rejected,
            "failed first open preserves built-in owner");
    Require(!controller.View().user_owned && Active(controller.View()) == builtin_view &&
            controller.PlotView(snapshot).visible_markers.size() == 2, "built-in model/session preserved");

    auto user = Base(); user.id = "user-list"; user.name = "Same name";
    user.markers.front().coordinate = 1000;
    user.grouping_views.front().id = "__catalog_grouping_view__";
    user.grouping_views.front().name = "My authored name";
    user.color_schemes = {{"red", "Red", {{"a", "#FF0000FF"}}}, {"green", "Green", {{"a", "#00FF00FF"}}}};
    const auto file = outside / L"用户谱线.data";
    std::string error;
    const auto save = [&](const std::filesystem::path& path, const SpectralLineList& list) {
        Require(SaveSpectralLineListToPathAtomic(path, list, error), error);
    };
    save(file, user);
    const auto original_bytes = Bytes(file);
    Applied(controller.OpenUserLineList(file, paths));
    auto view = controller.View();
    Require(view.user_owned && !controller.CanCustomize() && view.line_list_id == user.id &&
            view.grouping_view_search.empty() && view.marker_labels_visible, "new owner starts independent session");
    Require(view.grouping_views.front().generated_name == GeneratedNameMetadata{} &&
            view.grouping_views.front().name == "My authored name", "user names never receive built-in localization");
    Require(controller.PlotView(snapshot).visible_markers.size() == 3 &&
            controller.PlotView(snapshot).visible_markers.front().marker->coordinate == 1000,
            "sole active user markers reach rest-warning capable plot");
    Require(controller.Flush(), "pending built-in save works while user owner is active");
    const auto durable_builtin = Bytes(state_path);
    const auto slot = controller.PlotView(snapshot).visible_markers.front().automatic_color_slot;
    Applied(controller.Submit(Intent::SelectColorScheme("green")));
    Require(controller.PlotView(snapshot).visible_markers.front().color == DecodeLineListColor("#00FF00FF"),
            "canonical color selection is session-only");
    Require(controller.Submit(Intent::SetMarkerColor("a", PlotSeriesColor::Auto())).status == Status::Rejected &&
            controller.Submit(Intent::CreateUserGroupingView()).status == Status::Rejected &&
            controller.Submit(Intent::DuplicateGroupingView("__catalog_grouping_view__")).status == Status::Rejected &&
            controller.Submit(Intent::AddUserGroup("__catalog_grouping_view__")).status == Status::Rejected &&
            controller.Submit(Intent::DeleteUserGroupingView("__catalog_grouping_view__")).status == Status::Rejected,
            "all canonical editing entry points reject user owner");
    const auto hidden = controller.Submit(Intent::SetMarkerVisibility("a", false));
    Applied(hidden); Require(!hidden.persistent_state_changed, "user visibility never schedules canonical/state mirror save");
    Applied(controller.Submit(Intent::SelectGroupingView("base-two")));
    Applied(controller.Submit(Intent::SetGroupExpanded("base-two", "base-g2", true)));
    Applied(controller.Submit(Intent::SetGroupingViewSearch("C")));
    Applied(controller.Submit(Intent::SetMarkerLabelsVisible(false)));
    const auto generation = controller.View().generation;
    const auto unchanged = [&] {
        const auto current = controller.View();
        Require(current.generation == generation && current.line_list_id == user.id &&
                current.grouping_view_search == "C" && !current.marker_labels_visible &&
                Active(current) == "base-two" && current.active_color_scheme_id == "green" &&
                controller.PlotView(snapshot).visible_markers.size() == 2,
                "failed candidate leaves generation and all interaction state untouched");
    };
    for (const auto* role : {"config", "state", "logs", "unsaved", "STATE"}) {
        const auto reserved = managed / role / "list.json";
        save(reserved, user);
        Require(controller.OpenUserLineList(reserved, paths).status == Status::Rejected, "reserved namespace rejected");
        unchanged();
    }
    for (const auto& bytes : {std::string{}, std::string("{"), std::string("{}"),
            std::string("{\"format\":\"other\",\"schema_version\":1}"),
            std::string("{\"format\":\"spectiary.spectral_line_list\",\"schema_version\":99}")}) {
        const auto bad = outside / "bad.json";
        { std::ofstream stream(bad, std::ios::binary); stream << bytes; }
        Require(controller.OpenUserLineList(bad, paths).status == Status::Rejected, "invalid candidate rejected"); unchanged();
    }
    Require(controller.OpenUserLineList(outside / "absent", paths).status == Status::Rejected, "missing rejected"); unchanged();
    const HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(locked != INVALID_HANDLE_VALUE, "lock fixture");
    const auto blocked = controller.OpenUserLineList(file, paths);
    CloseHandle(locked);
    Require(blocked.status == Status::Rejected, "inaccessible file rejected"); unchanged();
    Require(controller.Flush() && Bytes(state_path) == durable_builtin && Bytes(file) == original_bytes,
            "user interactions do not persist canonical contents or touch built-in state");

    Applied(controller.SelectBuiltInLineList());
    Require(controller.View().grouping_view_search == "builtin search" && !controller.View().marker_labels_visible &&
            Active(controller.View()) == builtin_view, "built-in interaction restored");
    Applied(controller.SelectOpenedUserLineList());
    Require(Active(controller.View()) == "base-two" && View(controller.View(), "base-two").groups.front().expanded,
            "switch restores user identity-scoped session");
    const auto moved = managed / "legal-at-root.anything";
    std::filesystem::rename(file, moved);
    Require(controller.PlotView(snapshot).visible_markers.size() == 2, "external move does not invalidate generation");
    Require(controller.OpenUserLineList(file, paths).status == Status::Rejected, "stale explicit locator does not rebind");
    Applied(controller.OpenUserLineList(moved, paths));
    Require(controller.View().grouping_view_search == "C" && controller.View().active_color_scheme_id == "green" &&
            controller.PlotView(snapshot).visible_markers.size() == 2, "explicit moved-path reopen resumes stable identity");
    Applied(controller.Submit(Intent::SetMarkerVisibility("a", true)));
    Require(controller.PlotView(snapshot).visible_markers.front().automatic_color_slot == slot, "stable colors after switches and move");
    auto other = user; other.id = "other-identity";
    save(moved, other);
    Applied(controller.OpenUserLineList(moved, paths));
    Require(controller.View().line_list_id == other.id && controller.View().grouping_view_search.empty() &&
            controller.View().marker_labels_visible && controller.PlotView(snapshot).visible_markers.size() == 3,
            "explicit replacement at same path/name adopts new identity without old session");
    const auto child = managed / "my-documents" / "list.json";
    save(child, user); Applied(controller.OpenUserLineList(child, paths));
    Require(controller.View().grouping_view_search == "C", "non-reserved child legal and identity resumes");
    // Even a user-authored document declaring the built-in ID cannot acquire built-in ownership.
    other.id = Base().id; other.grouping_views.clear();
    save(moved, other); Applied(controller.OpenUserLineList(moved, paths));
    Require(!controller.CanCustomize() && controller.View().grouping_views.front().groups.back().marker_references.size() == 3,
            "ungrouped user list inspectable; built-in identity cannot grant editing");
    Applied(controller.Submit(Intent::SetGroupMarkerVisibility("", "__unassigned__", false)));
    Require(controller.PlotView(snapshot).visible_markers.empty(), "ungrouped visibility is session-only");
    for (const auto unit : {line_list::Unit::Angstrom, line_list::Unit::Nanometer, line_list::Unit::Micrometer}) {
        for (const auto medium : {line_list::Medium::Vacuum, line_list::Medium::Air}) {
            auto coordinates = user; coordinates.id = "coordinates"; coordinates.coordinate.unit = unit; coordinates.coordinate.medium = medium;
            save(moved, coordinates); Applied(controller.OpenUserLineList(moved, paths));
            const bool compatible = unit == line_list::Unit::Angstrom && medium == line_list::Medium::Vacuum;
            Require(controller.View().plot_compatible == compatible && controller.View().line_list_marker_count == 3 &&
                    controller.PlotView(snapshot).visible_markers.size() == (compatible ? 3U : 0U), "incompatible coordinates inspectable but never plotted raw");
        }
    }
    save(moved, user); Applied(controller.OpenUserLineList(moved, paths));
    snapshot->capabilities.can_show_spectral_lines = false;
    Require(controller.PlotView(snapshot).visible_markers.empty() && controller.PlotView({}).visible_markers.empty(), "snapshot capability remains authoritative");
    auto empty = user; empty.markers.clear(); empty.grouping_views.clear(); empty.color_schemes.clear();
    save(moved, empty); Applied(controller.OpenUserLineList(moved, paths));
    Require(controller.View().user_owned && controller.View().line_list_marker_count == 0, "valid zero-marker file activates");
    std::filesystem::remove(moved);
    Require(controller.View().user_owned, "external deletion leaves adopted in-memory generation intact");
    Applied(controller.SelectBuiltInLineList());
    Require(controller.CanCustomize(), "built-in capabilities restored");
    Applied(controller.Submit(Intent::AddUserGroup(builtin_view)));
    Require(controller.Flush(), "built-in editing still persists after user switching");
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
    Applied(controller.Submit(Intent::RenameUserGroup(user, first, "Unassigned", SpectralLineRenameEditState::Edited)));
    Applied(controller.Submit(Intent::SetGroupExpanded(user, first, true)));
    state = controller.View();
    Require(View(state, user).groups[0].id == second && View(state, user).groups[1].name == "Unassigned" &&
            !View(state, user).groups[1].is_unassigned, "authored Unassigned has ordinary semantics");
    Applied(controller.Submit(Intent::DuplicateGroupingView(user)));
    const auto copy = Active(controller.View());
    Require(copy != user, "duplicate has fresh identity");
    state = controller.View();
    Require(View(state, copy).groups[0].id != second, "duplicate group identities fresh");
    Applied(controller.Submit(Intent::RenameUserGroupingView(copy, "Grouping 1", SpectralLineRenameEditState::Edited)));
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
    Applied(left.Submit(Intent::RenameUserGroupingView(a, "Pending", SpectralLineRenameEditState::Edited)));
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
        TestUserLineLists(directory.path() / "user-files");
        TestOperations(directory.path() / "operations.json"); TestPersistence(directory.path()); TestProcesses(directory.path() / "processes");
        std::cout << "Spectral-line controller tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
