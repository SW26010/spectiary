#include "overlays/spectral_line_user_state_cache_io.h"
#include "platform/exclusive_file_lease.h"
#include "ui/spectral_lines_panel_controller.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace {

using namespace std::chrono_literals;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void RequireApplied(
    const specforge::CatalogUserStateResult& result,
    std::string_view message);

specforge::SpectralLineMarker Line(
    std::string id,
    std::string label,
    std::string group,
    double vacuum_angstrom)
{
    specforge::SpectralLineMarker marker;
    marker.id = std::move(id);
    marker.label = std::move(label);
    marker.kind = specforge::SpectralLineMarkerKind::Line;
    marker.group = std::move(group);
    marker.vacuum_angstrom = vacuum_angstrom;
    marker.display_label = marker.label;
    marker.source_ref = "test";
    return marker;
}

specforge::SpectralLineCatalog GroupedCatalog()
{
    specforge::SpectralLineCatalog catalog;
    catalog.markers.push_back(Line("h_beta", "H beta", "Balmer", 4862.683));
    catalog.markers.push_back(Line("h_alpha", "H alpha", "Balmer", 6564.614));
    catalog.markers.push_back(Line("ca_ii_8500", "Ca II", "Ca II", 8500.360));
    return catalog;
}

specforge::SpectrumSnapshotHandle Snapshot(bool can_show_spectral_lines)
{
    auto snapshot = std::make_shared<specforge::SpectrumSnapshot>();
    snapshot->capabilities.can_show_spectral_lines = can_show_spectral_lines;
    return snapshot;
}

std::filesystem::path TestCachePath(std::string_view test_name)
{
    return std::filesystem::temp_directory_path() /
           ("specforge_catalog_user_state_session_" + std::string(test_name) + ".json");
}

void RemoveTestCache(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + ".tmp", error);
    std::filesystem::remove(
        specforge::CatalogUserStateCacheCommitLeasePath(path),
        error);
    const std::filesystem::path parent = path.parent_path();
    const std::string temporary_prefix = path.filename().string() + ".tmp.";
    for (std::filesystem::directory_iterator iterator(parent, error);
         !error && iterator != std::filesystem::directory_iterator();
         iterator.increment(error)) {
        if (iterator->path().filename().string().starts_with(temporary_prefix)) {
            std::error_code remove_error;
            std::filesystem::remove(iterator->path(), remove_error);
        }
    }
}

void TestCatalogCommitLeasePathPreservesNativePath()
{
#if defined(_WIN32)
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        L"specforge_catalog_commit_\u8def\u5f84" /
        L"state-\u6570\u636e.json";
#else
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "specforge_catalog_commit_unicode" /
        "state-data.json";
#endif

    const std::filesystem::path lease_path =
        specforge::CatalogUserStateCacheCommitLeasePath(path);
    std::filesystem::path expected_filename = path.filename().native();
#if defined(_WIN32)
    expected_filename += L".commit.lock";
#else
    expected_filename += ".commit.lock";
#endif
    Require(
        lease_path.parent_path() == path.parent_path() &&
            lease_path.filename().native() == expected_filename,
        "catalog commit lease path must append its suffix without narrowing the native path");
}

std::filesystem::path MultiProcessCachePath()
{
    return std::filesystem::temp_directory_path() /
           ("specforge_catalog_user_state_multiprocess_" +
            std::to_string(GetCurrentProcessId()) + ".json");
}

void WriteMarkerFile(
    const std::filesystem::path& path,
    std::string_view contents = "ready\n")
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Require(stream.good(), "could not create the multi-process marker file");
    stream << contents;
    Require(stream.good(), "could not write the multi-process marker file");
}

bool WaitForMarkerFile(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout)
{
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (std::filesystem::exists(path, error) && !error) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

std::wstring CurrentExecutablePath()
{
    std::vector<wchar_t> buffer(32'768);
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    Require(length != 0 && length < buffer.size(), "could not resolve the test executable path");
    return std::wstring(buffer.data(), length);
}

std::wstring QuoteWindowsArgument(std::wstring value)
{
    std::wstring result = L"\"";
    for (const wchar_t character : value) {
        if (character == L'\"') {
            result += L"\\\"";
        } else {
            result.push_back(character);
        }
    }
    result += L"\"";
    return result;
}

struct ChildProcess {
    HANDLE process = nullptr;

    ChildProcess() = default;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept
        : process(std::exchange(other.process, nullptr))
    {
    }
    ChildProcess& operator=(ChildProcess&& other) noexcept
    {
        if (this != &other) {
            if (process != nullptr) {
                (void)TerminateProcess(process, 3);
                CloseHandle(process);
            }
            process = std::exchange(other.process, nullptr);
        }
        return *this;
    }

    ~ChildProcess()
    {
        if (process == nullptr) {
            return;
        }
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process, 3);
            (void)WaitForSingleObject(process, 5'000);
        }
        CloseHandle(process);
    }
};

ChildProcess StartCatalogChild(
    std::string_view operation,
    const std::filesystem::path& cache_path,
    const std::filesystem::path& ready_path,
    const std::filesystem::path& go_path,
    const std::filesystem::path& done_path)
{
    const std::wstring command =
        QuoteWindowsArgument(CurrentExecutablePath()) +
        L" --catalog-reconcile-child " +
        QuoteWindowsArgument(std::wstring(operation.begin(), operation.end())) +
        L" " + QuoteWindowsArgument(cache_path.wstring()) +
        L" " + QuoteWindowsArgument(ready_path.wstring()) +
        L" " + QuoteWindowsArgument(go_path.wstring()) +
        L" " + QuoteWindowsArgument(done_path.wstring());
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    Require(
        CreateProcessW(
            nullptr,
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process) != FALSE,
        "could not start catalog reconciliation child process");
    CloseHandle(process.hThread);
    ChildProcess child;
    child.process = process.hProcess;
    return child;
}

DWORD WaitForCatalogChild(
    ChildProcess& child,
    std::string_view operation)
{
    const DWORD wait = WaitForSingleObject(child.process, 20'000);
    Require(
        wait == WAIT_OBJECT_0,
        std::string("catalog reconciliation child timed out: ") +
            std::string(operation));
    DWORD exit_code = 1;
    Require(
        GetExitCodeProcess(child.process, &exit_code) != FALSE,
        "could not read catalog reconciliation child exit code");
    return exit_code;
}

void SeedMultiProcessCache(const std::filesystem::path& path)
{
    RemoveTestCache(path);
    specforge::SpectralLinesPanelController seed(
        GroupedCatalog(),
        specforge::PublicSpectralLineCatalogIdentity(),
        path);
    RequireApplied(
        seed.Submit(specforge::CatalogUserStateIntent::CreateUserGroupingView()),
        "multi-process seed should create the first grouping view");
    RequireApplied(
        seed.Submit(specforge::CatalogUserStateIntent::CreateUserGroupingView()),
        "multi-process seed should create the second grouping view");
    RequireApplied(
        seed.Submit(specforge::CatalogUserStateIntent::AddUserGroup("view-2")),
        "multi-process seed should create the first ordinary group");
    RequireApplied(
        seed.Submit(specforge::CatalogUserStateIntent::AddUserGroup("view-2")),
        "multi-process seed should create the second ordinary group");
    RequireApplied(
        seed.Submit(specforge::CatalogUserStateIntent::SelectGroupingView(
            specforge::CatalogGroupingViewId())),
        "multi-process seed should make selection conflicts explicit");
    Require(seed.Flush(), "multi-process seed should flush");
}

void SeedMultiProcessCacheWithReferences(const std::filesystem::path& path)
{
    SeedMultiProcessCache(path);
    specforge::SpectralLinesPanelController seed(
        GroupedCatalog(),
        specforge::PublicSpectralLineCatalogIdentity(),
        path);
    RequireApplied(
        seed.Submit(
            specforge::CatalogUserStateIntent::MoveMarkerReference(
                "view-2",
                "h_alpha",
                specforge::UnassignedUserGroupId(),
                "group-1")),
        "reference seed should move h_alpha into group-1");
    Require(seed.Flush(), "reference seed should flush");
    const auto loaded = specforge::LoadCatalogUserStateCache(path);
    const auto& views = loaded.cache.catalogs.at("specforge.public").grouping_views;
    const auto view = std::find_if(
        views.begin(),
        views.end(),
        [](const auto& candidate) { return candidate.id == "view-2"; });
    Require(view != views.end(), "reference seed should retain view-2");
    const auto group_one = std::find_if(
        view->groups.begin(),
        view->groups.end(),
        [](const auto& group) { return group.id == "group-1"; });
    const auto group_two = std::find_if(
        view->groups.begin(),
        view->groups.end(),
        [](const auto& group) { return group.id == "group-2"; });
    const auto has_alpha = [](const auto& group) {
        return std::any_of(
            group.marker_references.begin(),
            group.marker_references.end(),
            [](const auto& reference) { return reference.marker_id == "h_alpha"; });
    };
    Require(
        group_one != view->groups.end() && group_two != view->groups.end() &&
            has_alpha(*group_one) && !has_alpha(*group_two),
        "reference seed should put h_alpha only in group-1");
}

void SeedMultiProcessCacheWithThreeGroups(const std::filesystem::path& path)
{
    SeedMultiProcessCache(path);
    specforge::SpectralLinesPanelController seed(
        GroupedCatalog(),
        specforge::PublicSpectralLineCatalogIdentity(),
        path);
    RequireApplied(
        seed.Submit(
            specforge::CatalogUserStateIntent::AddUserGroup("view-2")),
        "ordering seed should create group-3");
    Require(seed.Flush(), "ordering seed should flush");
}

void SeedMultiProcessCacheWithThreeViews(const std::filesystem::path& path)
{
    SeedMultiProcessCache(path);
    specforge::SpectralLinesPanelController seed(
        GroupedCatalog(),
        specforge::PublicSpectralLineCatalogIdentity(),
        path);
    RequireApplied(
        seed.Submit(
            specforge::CatalogUserStateIntent::CreateUserGroupingView()),
        "selection fallback seed should create view-3");
    RequireApplied(
        seed.Submit(
            specforge::CatalogUserStateIntent::SelectGroupingView(
                "view-1")),
        "selection fallback seed should make view-1 active");
    Require(seed.Flush(), "selection fallback seed should flush");
}

void RunConcurrentCatalogChildren(
    const std::filesystem::path& cache_path,
    std::string_view first_operation,
    std::string_view second_operation)
{
    const std::filesystem::path marker_root =
        cache_path.parent_path() /
        (cache_path.stem().string() + "_markers");
    std::error_code remove_error;
    std::filesystem::remove_all(marker_root, remove_error);
    std::filesystem::create_directories(marker_root);
    const std::filesystem::path go_path = marker_root / "go";
    std::vector<ChildProcess> children;
    std::vector<std::string> operations = {
        std::string(first_operation),
        std::string(second_operation),
    };
    for (std::size_t index = 0; index < operations.size(); ++index) {
        children.push_back(StartCatalogChild(
            operations[index],
            cache_path,
            marker_root / ("ready-" + std::to_string(index)),
            go_path,
            marker_root / ("done-" + std::to_string(index))));
    }
    for (std::size_t index = 0; index < operations.size(); ++index) {
        Require(
            WaitForMarkerFile(
                marker_root / ("ready-" + std::to_string(index)),
                10s),
            "catalog reconciliation child did not publish its startup snapshot");
    }
    WriteMarkerFile(go_path);
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const DWORD exit_code =
            WaitForCatalogChild(children[index], operations[index]);
        Require(
            exit_code == 0,
            std::string("catalog reconciliation child failed: ") + operations[index]);
    }
    std::filesystem::remove_all(marker_root, remove_error);
}

void RunCreateDeleteViewRace(const std::filesystem::path& cache_path)
{
    SeedMultiProcessCache(cache_path);
    const std::filesystem::path marker_root =
        cache_path.parent_path() /
        (cache_path.stem().string() + "_view_create_delete_markers");
    std::error_code remove_error;
    std::filesystem::remove_all(marker_root, remove_error);
    std::filesystem::create_directories(marker_root);

    ChildProcess stale_peer = StartCatalogChild(
        "create_view",
        cache_path,
        marker_root / "ready",
        marker_root / "go",
        marker_root / "done");
    Require(
        WaitForMarkerFile(marker_root / "ready", 10s),
        "stale view-add peer should publish its startup snapshot");

    {
        specforge::SpectralLinesPanelController owner(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            cache_path);
        RequireApplied(
            owner.Submit(
                specforge::CatalogUserStateIntent::CreateUserGroupingView()),
            "the first view-add peer should create view-3");
        Require(owner.Flush(), "the first view-add peer should commit view-3");
        RequireApplied(
            owner.Submit(
                specforge::CatalogUserStateIntent::DeleteUserGroupingView(
                    "view-3")),
            "the first view-add peer should delete its own view-3");
        Require(
            owner.Flush(),
            "the first view-add peer should commit the view-3 tombstone reservation");
    }

    WriteMarkerFile(marker_root / "go");
    Require(
        WaitForCatalogChild(stale_peer, "create_view") == 0,
        "the stale view-add peer should reconcile after view-3 was deleted");
    const auto loaded = specforge::LoadCatalogUserStateCache(cache_path);
    const auto& state = loaded.cache.catalogs.at("specforge.public");
    const auto has_view = [&](std::string_view id) {
        return std::any_of(
            state.grouping_views.begin(),
            state.grouping_views.end(),
            [&](const auto& view) { return view.id == id; });
    };
    Require(
        state.grouping_views.size() == 3 && !has_view("view-3") &&
            state.reserved_view_ids.contains("view-3") &&
            std::any_of(
                state.grouping_views.begin(),
                state.grouping_views.end(),
                [](const auto& view) {
                    return view.id != "view-1" && view.id != "view-2";
                }),
        "a stale view addition must receive a fresh identity after its requested id was created and deleted");
    std::filesystem::remove_all(marker_root, remove_error);
}

void RunCreateDeleteGroupRace(const std::filesystem::path& cache_path)
{
    SeedMultiProcessCache(cache_path);
    const std::filesystem::path marker_root =
        cache_path.parent_path() /
        (cache_path.stem().string() + "_group_create_delete_markers");
    std::error_code remove_error;
    std::filesystem::remove_all(marker_root, remove_error);
    std::filesystem::create_directories(marker_root);

    ChildProcess stale_peer = StartCatalogChild(
        "add_group_view_2",
        cache_path,
        marker_root / "ready",
        marker_root / "go",
        marker_root / "done");
    Require(
        WaitForMarkerFile(marker_root / "ready", 10s),
        "stale group-add peer should publish its startup snapshot");

    {
        specforge::SpectralLinesPanelController owner(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            cache_path);
        RequireApplied(
            owner.Submit(
                specforge::CatalogUserStateIntent::AddUserGroup(
                    "view-2")),
            "the first group-add peer should create group-3");
        Require(owner.Flush(), "the first group-add peer should commit group-3");
        RequireApplied(
            owner.Submit(
                specforge::CatalogUserStateIntent::DeleteUserGroup(
                    "view-2",
                    "group-3")),
            "the first group-add peer should delete its own group-3");
        Require(
            owner.Flush(),
            "the first group-add peer should commit the group-3 reservation");
    }

    WriteMarkerFile(marker_root / "go");
    Require(
        WaitForCatalogChild(stale_peer, "add_group_view_2") == 0,
        "the stale group-add peer should reconcile after group-3 was deleted");
    const auto loaded = specforge::LoadCatalogUserStateCache(cache_path);
    const auto& state = loaded.cache.catalogs.at("specforge.public");
    const auto view = std::find_if(
        state.grouping_views.begin(),
        state.grouping_views.end(),
        [](const auto& candidate) { return candidate.id == "view-2"; });
    Require(view != state.grouping_views.end(), "group identity race should retain view-2");
    const auto has_group = [&](std::string_view id) {
        return std::any_of(
            view->groups.begin(),
            view->groups.end(),
            [&](const auto& group) { return group.id == id; });
    };
    Require(
        view->groups.size() == 4 && !has_group("group-3") &&
            state.reserved_group_ids.contains("group-3") &&
            std::any_of(
                view->groups.begin(),
                view->groups.end(),
                [](const auto& group) {
                    return group.id != specforge::UnassignedUserGroupId() &&
                           group.id != "group-1" && group.id != "group-2";
                }),
        "a stale group addition must receive a fresh identity after its requested id was created and deleted");
    std::filesystem::remove_all(marker_root, remove_error);
}

void RunDeterministicOrderingRace(const std::filesystem::path& cache_path)
{
    SeedMultiProcessCacheWithThreeGroups(cache_path);
    const std::filesystem::path marker_root =
        cache_path.parent_path() /
        (cache_path.stem().string() + "_deterministic_order_markers");
    std::error_code remove_error;
    std::filesystem::remove_all(marker_root, remove_error);
    std::filesystem::create_directories(marker_root);

    ChildProcess stale_task = StartCatalogChild(
        "reorder_and_add_group",
        cache_path,
        marker_root / "ready",
        marker_root / "go",
        marker_root / "done");
    Require(
        WaitForMarkerFile(marker_root / "ready", 10s),
        "ordering stale task should publish its startup snapshot");

    {
        specforge::SpectralLinesPanelController durable_peer(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            cache_path);
        RequireApplied(
            durable_peer.Submit(
                specforge::CatalogUserStateIntent::AddUserGroup(
                    "view-2")),
            "ordering durable peer should add group-4");
        Require(
            durable_peer.Flush(),
            "ordering durable peer should commit before the stale task");
    }

    WriteMarkerFile(marker_root / "go");
    Require(
        WaitForCatalogChild(stale_task, "reorder_and_add_group") == 0,
        "ordering stale task should reconcile after the durable addition");
    const auto loaded = specforge::LoadCatalogUserStateCache(cache_path);
    const auto& state = loaded.cache.catalogs.at("specforge.public");
    const auto view = std::find_if(
        state.grouping_views.begin(),
        state.grouping_views.end(),
        [](const auto& candidate) { return candidate.id == "view-2"; });
    Require(view != state.grouping_views.end(), "ordering race should retain view-2");
    Require(
        view->groups.size() == 6 &&
            view->groups[0].id == "group-2" &&
            view->groups[1].id == "group-1" &&
            view->groups[2].id == "group-3" &&
            view->groups[3].id == "group-4" &&
            view->groups[4].id == "group-4-2" &&
            view->groups[5].id == specforge::UnassignedUserGroupId(),
        "multi-process ordering must preserve the task reorder and place durable addition before task addition");
    std::filesystem::remove_all(marker_root, remove_error);
}

void RunDeleteActiveViewWithStaleSelectionRace(
    const std::filesystem::path& cache_path)
{
    SeedMultiProcessCacheWithThreeViews(cache_path);
    const std::filesystem::path marker_root =
        cache_path.parent_path() /
        (cache_path.stem().string() + "_selection_fallback_markers");
    std::error_code remove_error;
    std::filesystem::remove_all(marker_root, remove_error);
    std::filesystem::create_directories(marker_root);

    ChildProcess stale_delete = StartCatalogChild(
        "delete_view_1",
        cache_path,
        marker_root / "ready",
        marker_root / "go",
        marker_root / "done");
    Require(
        WaitForMarkerFile(marker_root / "ready", 10s),
        "stale deletion task should publish its active-view snapshot");

    {
        specforge::SpectralLinesPanelController durable_peer(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            cache_path);
        RequireApplied(
            durable_peer.Submit(
                specforge::CatalogUserStateIntent::SelectGroupingView(
                    "view-3")),
            "durable peer should explicitly select surviving view-3");
        Require(
            durable_peer.Flush(),
            "durable peer should commit the surviving selection before deletion");
    }

    WriteMarkerFile(marker_root / "go");
    Require(
        WaitForCatalogChild(stale_delete, "delete_view_1") == 0,
        "stale deletion task should reconcile after the explicit selection");
    const auto loaded = specforge::LoadCatalogUserStateCache(cache_path);
    const auto& state = loaded.cache.catalogs.at("specforge.public");
    Require(
        state.active_view_id == "view-3" &&
            std::none_of(
                state.grouping_views.begin(),
                state.grouping_views.end(),
                [](const auto& view) { return view.id == "view-1"; }),
        "automatic fallback after deleting the active view must not override a durable explicit selection");
    std::filesystem::remove_all(marker_root, remove_error);
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void WriteTextFile(
    const std::filesystem::path& path,
    std::string_view contents)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    Require(stream.good(), "could not open test text file");
    stream << contents;
    Require(stream.good(), "could not write test text file");
}

std::string ReplaceFirst(
    std::string contents,
    std::string_view needle,
    std::string_view replacement)
{
    const std::size_t position = contents.find(needle);
    Require(position != std::string::npos, "test fixture replacement target is missing");
    contents.replace(position, needle.size(), replacement);
    return contents;
}

void SeedLegacyMultiCatalogCache(
    const std::filesystem::path& path,
    int schema_version)
{
    SeedMultiProcessCache(path);
    specforge::CatalogUserStateCacheLoadResult loaded =
        specforge::LoadCatalogUserStateCache(path);
    Require(
        loaded.issue_kind ==
            specforge::CatalogUserStateCacheLoadIssueKind::None,
        "legacy multi-catalog fixture should load its schema-four seed");

    specforge::CatalogUserState foreign_state =
        loaded.cache.catalogs.at("specforge.public");
    foreign_state.catalog_identity = {
        "foreign.catalog",
        "Foreign catalog"};
    loaded.cache.catalogs.emplace(
        "foreign.catalog",
        std::move(foreign_state));
    loaded.cache.catalog_panel_state.emplace(
        "foreign.catalog",
        specforge::CatalogPanelState{});

    std::string error;
    Require(
        specforge::SaveCatalogUserStateCache(
            path,
            loaded.cache,
            error),
        error.empty()
            ? "legacy multi-catalog fixture should save its schema-four seed"
            : error);
    WriteTextFile(
        path,
        ReplaceFirst(
            ReadFile(path),
            "\"schema_version\": 4",
            "\"schema_version\": " + std::to_string(schema_version)));
}

std::string RemoveSerializedObjectMemberLine(
    std::string contents,
    std::string_view member_name)
{
    const std::string needle =
        "\"" + std::string(member_name) + "\":";
    const std::size_t position = contents.find(needle);
    Require(
        position != std::string::npos,
        "serialized test object member is missing");
    const std::size_t previous_newline =
        contents.rfind('\n', position);
    const std::size_t line_start =
        previous_newline == std::string::npos
            ? 0
            : previous_newline + 1;
    const std::size_t line_end = contents.find('\n', position);
    Require(
        line_end != std::string::npos,
        "serialized test object member line is unterminated");
    contents.erase(line_start, line_end + 1 - line_start);
    return contents;
}

void RunMissingAllocatorHistoryFailsClosedRace(
    const std::filesystem::path& cache_path)
{
    SeedMultiProcessCache(cache_path);
    const std::filesystem::path marker_root =
        cache_path.parent_path() /
        (cache_path.stem().string() + "_missing_allocator_history_markers");
    std::error_code remove_error;
    std::filesystem::remove_all(marker_root, remove_error);
    std::filesystem::create_directories(marker_root);

    ChildProcess stale_peer = StartCatalogChild(
        "create_view",
        cache_path,
        marker_root / "ready",
        marker_root / "go",
        marker_root / "done");
    Require(
        WaitForMarkerFile(marker_root / "ready", 10s),
        "allocator-history stale peer should publish its startup snapshot");

    {
        specforge::SpectralLinesPanelController owner(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            cache_path);
        RequireApplied(
            owner.Submit(
                specforge::CatalogUserStateIntent::CreateUserGroupingView()),
            "allocator-history owner should create view-3");
        Require(owner.Flush(), "allocator-history owner should commit view-3");
        RequireApplied(
            owner.Submit(
                specforge::CatalogUserStateIntent::DeleteUserGroupingView(
                    "view-3")),
            "allocator-history owner should delete view-3");
        Require(
            owner.Flush(),
            "allocator-history owner should commit the view-3 reservation");
    }

    std::string missing_history = ReadFile(cache_path);
    for (const std::string_view member_name :
         {"next_view_sequence",
          "next_group_sequence",
          "reserved_view_ids",
          "reserved_group_ids"}) {
        missing_history = RemoveSerializedObjectMemberLine(
            std::move(missing_history),
            member_name);
    }
    WriteTextFile(cache_path, missing_history);
    WriteMarkerFile(marker_root / "go");
    Require(
        WaitForCatalogChild(stale_peer, "create_view") == 2,
        "a current-schema cache missing allocator history must reject stale create");
    Require(
        ReadFile(cache_path) == missing_history,
        "missing allocator history must preserve the durable file");

    std::filesystem::remove_all(marker_root, remove_error);
}

void RunExplicitSelectionRoundTripRace(
    const std::filesystem::path& cache_path)
{
    SeedMultiProcessCacheWithThreeViews(cache_path);
    const std::filesystem::path marker_root =
        cache_path.parent_path() /
        (cache_path.stem().string() + "_selection_round_trip_markers");
    std::error_code remove_error;
    std::filesystem::remove_all(marker_root, remove_error);
    std::filesystem::create_directories(marker_root);

    ChildProcess stale_task = StartCatalogChild(
        "select_view_2_then_1",
        cache_path,
        marker_root / "ready",
        marker_root / "go",
        marker_root / "done");
    Require(
        WaitForMarkerFile(marker_root / "ready", 10s),
        "selection round-trip task should publish its startup snapshot");

    {
        specforge::SpectralLinesPanelController durable_peer(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            cache_path);
        RequireApplied(
            durable_peer.Submit(
                specforge::CatalogUserStateIntent::SelectGroupingView(
                    "view-3")),
            "durable peer should explicitly select view-3");
        Require(
            durable_peer.Flush(),
            "durable peer should commit view-3 before the round-trip task");
    }

    WriteMarkerFile(marker_root / "go");
    Require(
        WaitForCatalogChild(stale_task, "select_view_2_then_1") == 0,
        "selection round-trip task should reconcile successfully");
    const auto loaded = specforge::LoadCatalogUserStateCache(cache_path);
    Require(
        loaded.cache.catalogs.at("specforge.public").active_view_id ==
            "view-1",
        "the final explicit selection back to the base value must beat a peer selection");
    std::filesystem::remove_all(marker_root, remove_error);
}

void WriteNonCanonicalCache(const std::filesystem::path& path)
{
    std::ofstream stream(path);
    stream << R"json({
  "format_kind": "specforge.catalog_user_state.cache",
  "schema_version": 2,
  "catalogs": {
    "specforge.public": {
      "active_view_id": "missing-view",
      "marker_visibility": {"": false},
      "grouping_views": [{
        "id": "view-1",
        "name": "  Review  ",
        "read_only": true,
        "groups": [{
          "id": "group-1",
          "name": "  Hydrogen  ",
          "is_unassigned": false,
          "marker_references": [
            {"catalog_identity": "specforge.public", "marker_id": "h_alpha"},
            {"catalog_identity": "specforge.public", "marker_id": "missing-marker"}
          ]
        }, {
          "id": "__unassigned__",
          "name": "Wrong",
          "is_unassigned": false,
          "marker_references": [
            {"catalog_identity": "specforge.public", "marker_id": "h_beta"}
          ]
        }]
      }]
    }
  },
  "catalog_panel_state": {
    "specforge.public": {
      "expanded_group_ids": ["missing/group", "view-1/group-1"]
    }
  }
})json";
}

void WriteMalformedLegacyCacheBody(
    const std::filesystem::path& path,
    std::string_view catalogs_json)
{
    std::ofstream stream(path, std::ios::binary);
    stream << R"json({
  "format_kind": "specforge.catalog_user_state.cache",
  "schema_version": 2,
  "catalogs": )json"
           << catalogs_json
           << R"json(,
  "catalog_panel_state": {}
})json";
}

void WriteLegacyExactShapeUserNames(
    const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    stream << R"json({
  "format_kind": "specforge.catalog_user_state.cache",
  "schema_version": 2,
  "catalogs": {
    "specforge.public": {
      "active_view_id": "view-1",
      "marker_visibility": {},
      "grouping_views": [{
        "id": "view-1",
        "name": "Grouping 1",
        "groups": [{
          "id": "group-1",
          "name": "Group 1",
          "is_unassigned": false,
          "marker_references": []
        }]
      }]
    }
  },
  "catalog_panel_state": {}
})json";
}

void WriteLegacySchemaTwoCacheWithoutUnassignedFlag(
    const std::filesystem::path& path)
{
    WriteTextFile(
        path,
        R"json({
  "format_kind": "specforge.catalog_user_state.cache",
  "schema_version": 2,
  "catalogs": {
    "specforge.public": {
      "active_view_id": "view-1",
      "marker_visibility": {},
      "grouping_views": [{
        "id": "view-1",
        "name": "Legacy view",
        "groups": [{
          "id": "group-1",
          "name": "Legacy group",
          "marker_references": []
        }]
      }]
    }
  },
  "catalog_panel_state": {}
})json");
}

const specforge::SpectralLineGroupingView* FindGroupingView(
    const specforge::CatalogUserStateView& state,
    std::string_view view_id)
{
    const auto match = std::find_if(
        state.grouping_views.begin(),
        state.grouping_views.end(),
        [view_id](const specforge::SpectralLineGroupingView& view) {
            return view.id == view_id;
        });
    return match == state.grouping_views.end() ? nullptr : &(*match);
}

const specforge::SpectralLineGroupingView* FindActiveEditableView(
    const specforge::CatalogUserStateView& state)
{
    const auto match = std::find_if(
        state.grouping_views.begin(),
        state.grouping_views.end(),
        [](const specforge::SpectralLineGroupingView& view) {
            return view.active && view.editable;
        });
    return match == state.grouping_views.end() ? nullptr : &(*match);
}

const specforge::SpectralLineGroupView* FindGroup(
    const specforge::SpectralLineGroupingView& view,
    std::string_view group_id)
{
    const auto match = std::find_if(
        view.groups.begin(),
        view.groups.end(),
        [group_id](const specforge::SpectralLineGroupView& group) {
            return group.id == group_id;
        });
    return match == view.groups.end() ? nullptr : &(*match);
}

const specforge::SpectralLineGroupView* FindUnassignedGroup(
    const specforge::SpectralLineGroupingView& view)
{
    const auto match = std::find_if(
        view.groups.begin(),
        view.groups.end(),
        [](const specforge::SpectralLineGroupView& group) {
            return group.is_unassigned;
        });
    return match == view.groups.end() ? nullptr : &(*match);
}

const specforge::SpectralLineMarkerReferenceView* FindMarker(
    const specforge::SpectralLineGroupView& group,
    std::string_view marker_id)
{
    const auto match = std::find_if(
        group.marker_references.begin(),
        group.marker_references.end(),
        [marker_id](const specforge::SpectralLineMarkerReferenceView& marker) {
            return marker.marker_id == marker_id;
        });
    return match == group.marker_references.end() ? nullptr : &(*match);
}

void RequireApplied(const specforge::CatalogUserStateResult& result, std::string_view message)
{
    Require(result.status == specforge::CatalogUserStateResultStatus::Applied, message);
    Require(result.changed, "applied result should report a change");
}

void RequireRejected(const specforge::CatalogUserStateResult& result, std::string_view message)
{
    Require(result.status == specforge::CatalogUserStateResultStatus::Rejected, message);
    Require(!result.changed, "rejected result must not report a change");
    Require(!result.message.empty(), "rejected result should explain the invalid identity or invariant");
}

void RequireNoChange(
    const specforge::CatalogUserStateResult& result,
    std::string_view message)
{
    Require(
        result.status ==
            specforge::CatalogUserStateResultStatus::NoChange,
        message);
    Require(
        !result.changed &&
            !result.persistent_state_changed,
        "no-change result must not report a persistent mutation");
}

void TestForeignIdentitiesAreRejectedWithoutPersistence()
{
    const std::filesystem::path path = TestCachePath("identity_rejection");
    RemoveTestCache(path);
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);

        RequireRejected(
            session.Submit(specforge::CatalogUserStateIntent::SelectGroupingView("missing-view")),
            "unknown active grouping view should be rejected");
        RequireRejected(
            session.Submit(specforge::CatalogUserStateIntent::AddUserGroup("missing-view")),
            "unknown grouping view mutation should be rejected");
        RequireRejected(
            session.Submit(specforge::CatalogUserStateIntent::SetMarkerVisibility("missing-marker", false)),
            "unknown catalog marker should be rejected");

        const specforge::CatalogUserStateView state = session.View();
        Require(state.user_grouping_view_count == 0, "rejected intents must leave user grouping views unchanged");
        Require(session.Flush(), "a clean session should flush successfully");
    }
    Require(!std::filesystem::exists(path), "rejected intents must not create a cache file");
    RemoveTestCache(path);
}

void TestPlotViewProjectsOnlyPlotOverlayState()
{
    const std::filesystem::path path = TestCachePath("plot_view");
    RemoveTestCache(path);
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);

        const specforge::SpectralLinePlotView no_snapshot = session.PlotView(nullptr);
        Require(no_snapshot.visible_markers.empty(), "a missing snapshot must not expose plot markers");
        Require(no_snapshot.marker_labels_visible, "plot label visibility should be available without a snapshot");
        Require(
            no_snapshot.layout_scope_id == specforge::PublicSpectralLineCatalogIdentity().id,
            "plot layout scope should use the stable catalog identity");

        const specforge::SpectralLinePlotView unsupported = session.PlotView(Snapshot(false));
        Require(unsupported.visible_markers.empty(), "an unsupported snapshot must not expose plot markers");

        specforge::SpectralLinePlotView plot_view = session.PlotView(Snapshot(true));
        Require(plot_view.visible_markers.size() == 3, "a supported snapshot should expose visible catalog markers");
        Require(
            plot_view.visible_markers[0]->id == "h_beta" &&
                plot_view.visible_markers[1]->id == "h_alpha" &&
                plot_view.visible_markers[2]->id == "ca_ii_8500",
            "plot markers should preserve catalog order");

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SetMarkerLabelsVisible(false)),
            "plot label visibility intent should be applied");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SetMarkerVisibility("h_alpha", false)),
            "plot marker visibility intent should be applied");

        plot_view = session.PlotView(Snapshot(true));
        Require(!plot_view.marker_labels_visible, "plot view should reflect current marker label visibility");
        Require(plot_view.visible_markers.size() == 2, "plot view should omit hidden markers");
        Require(
            std::none_of(
                plot_view.visible_markers.begin(),
                plot_view.visible_markers.end(),
                [](const specforge::SpectralLineMarker* marker) {
                    return marker != nullptr && marker->id == "h_alpha";
                }),
            "plot view should not expose a marker hidden through catalog user state");
    }
    RemoveTestCache(path);
}

void TestCacheLoadUsesDomainCanonicalizationAndPreservesUnresolvedMarkers()
{
    const std::filesystem::path path =
        TestCachePath("canonicalization");
    RemoveTestCache(path);
    WriteNonCanonicalCache(path);

    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        const specforge::CatalogUserStateView state = session.View();
        Require(
            state.user_grouping_view_count == 1,
            "domain canonicalization should retain the one valid user grouping view");

        const specforge::SpectralLineGroupingView* catalog_view =
            FindGroupingView(state, specforge::CatalogGroupingViewId());
        Require(
            catalog_view != nullptr && catalog_view->active &&
                catalog_view->selection_requested,
            "domain canonicalization should repair invalid active selection and request presentation");

        const specforge::SpectralLineGroupingView* user_view =
            FindGroupingView(state, "view-1");
        Require(
            user_view != nullptr && user_view->editable &&
                user_view->name == "Review",
            "domain canonicalization should own persisted view flags and names");
        const specforge::SpectralLineGroupView* group =
            FindGroup(*user_view, "group-1");
        const specforge::SpectralLineGroupView* unassigned =
            FindUnassignedGroup(*user_view);
        Require(
            group != nullptr && group->name == "Hydrogen" &&
                group->expanded,
            "domain canonicalization should trim group names and retain valid panel state");
        Require(
            unassigned != nullptr &&
                unassigned->id == specforge::UnassignedUserGroupId() &&
                unassigned->name == "Unassigned",
            "domain canonicalization should create exactly the canonical unassigned group");

        const specforge::SpectralLineMarkerReferenceView* unresolved =
            FindMarker(*group, "missing-marker");
        Require(
            unresolved != nullptr && !unresolved->resolved &&
                unresolved->label == "missing-marker" &&
                unresolved->wavelength_text.empty(),
            "unresolved persisted markers should survive the canonicalization path");
        Require(
            FindMarker(*group, "h_beta") == nullptr &&
                FindMarker(*unassigned, "h_beta") != nullptr,
            "foreign references should be removed while current catalog markers remain assigned");

        Require(
            session.Flush(),
            "canonical state repaired during cache load should persist: " +
                session.View().persistence.save_diagnostic_detail);
    }

    const specforge::CatalogUserStateCacheLoadResult persisted =
        specforge::LoadCatalogUserStateCache(path);
    const specforge::CatalogUserState& state =
        persisted.cache.catalogs.at("specforge.public");
    const specforge::CatalogPanelState& panel =
        persisted.cache.catalog_panel_state.at("specforge.public");
    Require(
        !state.marker_visibility.contains("") &&
            state.grouping_views.size() == 1 &&
            !state.grouping_views.front().read_only,
        "persisted cache should contain the domain-canonical form");
    Require(
        panel.expanded_group_ids.size() == 1 &&
            panel.expanded_group_ids.contains("view-1/group-1"),
        "domain canonicalization should discard unresolved panel expansion keys");

    RemoveTestCache(path);
}

void TestPersistentIntentUsesDomainCanonicalizationForSelection()
{
    const std::filesystem::path path =
        TestCachePath("intent_canonicalization");
    RemoveTestCache(path);

    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    CreateUserGroupingView()),
            "creating a view should apply");
        specforge::CatalogUserStateView state = session.View();
        const specforge::SpectralLineGroupingView* user_view =
            FindActiveEditableView(state);
        Require(
            user_view != nullptr,
            "created view should become active");
        const std::string user_view_id = user_view->id;
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    AcknowledgeGroupingViewSelection(user_view_id)),
            "selection acknowledgement should apply");

        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    DeleteUserGroupingView(user_view_id)),
            "deleting the active view should apply");
        state = session.View();
        const specforge::SpectralLineGroupingView* catalog_view =
            FindGroupingView(state, specforge::CatalogGroupingViewId());
        Require(
            catalog_view != nullptr && catalog_view->active &&
                catalog_view->selection_requested,
            "persistent intent should use domain canonicalization to select the fallback view");
    }

    RemoveTestCache(path);
}

void TestGeneratedNamesPreserveStoredValuesAndOrigins()
{
    const std::filesystem::path path =
        TestCachePath("generated_names");
    RemoveTestCache(path);

    std::string view_id;
    std::string group_id;
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    CreateUserGroupingView()),
            "generated-name fixture should create a user view");

        specforge::CatalogUserStateView state =
            session.View();
        const specforge::SpectralLineGroupingView* view =
            FindActiveEditableView(state);
        Require(
            view != nullptr &&
                view->name == "Grouping 1" &&
                view->generated_name.source ==
                    specforge::GeneratedNameSource::
                        DefaultGroupingView &&
                view->generated_name.ordinal == 1,
            "created grouping views should carry explicit generated-name metadata");
        view_id = view->id;

        RequireNoChange(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroupingView(
                        view_id,
                        view->name,
                        specforge::
                            CatalogUserRenameEditState::
                                Unedited)),
            "confirming an unedited grouping view name should preserve the stored value");

        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    AddUserGroup(view_id)),
            "generated-name fixture should add a user group");
        state = session.View();
        view = FindGroupingView(state, view_id);
        const specforge::SpectralLineGroupView* group =
            view == nullptr || view->groups.empty()
                ? nullptr
                : &view->groups.front();
        Require(
            group != nullptr &&
                !group->is_unassigned &&
                group->name == "Group 1" &&
                group->generated_name.source ==
                    specforge::GeneratedNameSource::
                        DefaultGroup &&
                group->generated_name.ordinal == 1,
            "created groups should carry explicit generated-name metadata");
        group_id = group->id;

        RequireNoChange(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroup(
                        view_id,
                        group_id,
                        group->name,
                        specforge::
                            CatalogUserRenameEditState::
                                Unedited)),
            "confirming an unedited group name should preserve the stored value");
        Require(
            session.Flush(),
            "generated-name metadata should persist");
    }

    {
        specforge::SpectralLinesPanelController restored(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        specforge::CatalogUserStateView state =
            restored.View();
        const specforge::SpectralLineGroupingView* view =
            FindGroupingView(state, view_id);
        const specforge::SpectralLineGroupView* group =
            view == nullptr
                ? nullptr
                : FindGroup(*view, group_id);
        Require(
            view != nullptr &&
                view->name == "Grouping 1" &&
                view->generated_name.source ==
                    specforge::GeneratedNameSource::
                        DefaultGroupingView &&
                group != nullptr &&
                group->name == "Group 1" &&
                group->generated_name.source ==
                    specforge::GeneratedNameSource::
                        DefaultGroup,
            "generated-name metadata should survive the cache round trip");

        RequireApplied(
            restored.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroupingView(
                        view_id,
                        "Grouping 7",
                        specforge::
                            CatalogUserRenameEditState::
                                Edited)),
            "same-shaped user grouping names should be accepted");
        RequireApplied(
            restored.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroup(
                        view_id,
                        group_id,
                        "Group 7",
                        specforge::
                            CatalogUserRenameEditState::
                                Edited)),
            "same-shaped user group names should be accepted");
        state = restored.View();
        view = FindGroupingView(state, view_id);
        group =
            view == nullptr
                ? nullptr
                : FindGroup(*view, group_id);
        Require(
            view != nullptr &&
                view->name == "Grouping 7" &&
                view->generated_name.source ==
                    specforge::GeneratedNameSource::None &&
                group != nullptr &&
                group->name == "Group 7" &&
                group->generated_name.source ==
                    specforge::GeneratedNameSource::None,
            "user renames should clear generated-name semantics even when their shape matches a default");

        RequireApplied(
            restored.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroupingView(
                        view_id,
                        "Draft copy",
                        specforge::
                            CatalogUserRenameEditState::
                                Edited)),
            "user names ending in copy should be accepted verbatim");
        RequireApplied(
            restored.Submit(
                specforge::CatalogUserStateIntent::
                    DuplicateGroupingView(view_id)),
            "duplicating a user-named view should apply");
        state = restored.View();
        const specforge::SpectralLineGroupingView*
            duplicated = FindActiveEditableView(state);
        Require(
            duplicated != nullptr &&
                duplicated->id != view_id &&
                duplicated->name == "Draft copy copy" &&
                duplicated->generated_name.source ==
                    specforge::GeneratedNameSource::None &&
                duplicated->generated_name.copy_count == 1 &&
                duplicated->generated_name.copy_base_name ==
                    "Draft copy",
            "generated copies should carry an explicit base name instead of inferring suffixes");
    }

    RemoveTestCache(path);
}

void TestExplicitStoredValueRenamesClearGeneratedMetadata()
{
    const std::filesystem::path path =
        TestCachePath("explicit_stored_value_rename");
    RemoveTestCache(path);

    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    CreateUserGroupingView()),
            "explicit-rename fixture should create a user view");
        specforge::CatalogUserStateView state =
            session.View();
        const specforge::SpectralLineGroupingView* view =
            FindActiveEditableView(state);
        Require(
            view != nullptr,
            "explicit-rename fixture should expose the created view");
        const std::string view_id = view->id;

        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    AddUserGroup(view_id)),
            "explicit-rename fixture should create a group");
        state = session.View();
        view = FindGroupingView(state, view_id);
        const specforge::SpectralLineGroupView* group =
            view == nullptr || view->groups.empty()
                ? nullptr
                : &view->groups.front();
        Require(
            group != nullptr,
            "explicit-rename fixture should expose the created group");
        const std::string group_id = group->id;

        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroupingView(
                        view_id,
                        "Grouping 1",
                        specforge::
                            CatalogUserRenameEditState::
                                Edited)),
            "an explicit rename to the stored grouping-view text should clear generated-name semantics");
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::
                    RenameUserGroup(
                        view_id,
                        group_id,
                        "Group 1",
                        specforge::
                            CatalogUserRenameEditState::
                                Edited)),
            "an explicit rename to the stored group text should clear generated-name semantics");

        state = session.View();
        view = FindGroupingView(state, view_id);
        group =
            view == nullptr
                ? nullptr
                : FindGroup(*view, group_id);
        Require(
            view != nullptr &&
                view->name == "Grouping 1" &&
                view->generated_name.source ==
                    specforge::GeneratedNameSource::None &&
                group != nullptr &&
                group->name == "Group 1" &&
                group->generated_name.source ==
                    specforge::GeneratedNameSource::None,
            "explicit same-text renames should preserve text while clearing both generated-name markers");
        Require(
            session.Flush(),
            "explicit same-text renames should be persisted");
    }

    {
        specforge::SpectralLinesPanelController restored(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        const specforge::CatalogUserStateView state =
            restored.View();
        const specforge::SpectralLineGroupingView* view =
            FindActiveEditableView(state);
        const specforge::SpectralLineGroupView* group =
            view == nullptr || view->groups.empty()
                ? nullptr
                : &view->groups.front();
        Require(
            view != nullptr &&
                view->generated_name.source ==
                    specforge::GeneratedNameSource::None &&
                group != nullptr &&
                group->generated_name.source ==
                    specforge::GeneratedNameSource::None,
            "cleared generated-name markers should survive the cache round trip");
    }

    RemoveTestCache(path);
}

void TestLegacyExactShapeNamesRemainUserOwnedAcrossRestart()
{
    const std::filesystem::path path =
        TestCachePath("legacy_exact_shape_user_names");
    RemoveTestCache(path);
    WriteLegacyExactShapeUserNames(path);

    const auto require_user_owned_names =
        [](const specforge::CatalogUserStateView& state,
           std::string_view context) {
            const specforge::SpectralLineGroupingView* view =
                FindGroupingView(state, "view-1");
            const specforge::SpectralLineGroupView* group =
                view == nullptr
                    ? nullptr
                    : FindGroup(*view, "group-1");
            Require(
                view != nullptr &&
                    view->name == "Grouping 1" &&
                    view->generated_name.source ==
                        specforge::GeneratedNameSource::None &&
                    group != nullptr &&
                    group->name == "Group 1" &&
                    group->generated_name.source ==
                        specforge::GeneratedNameSource::None,
                context);
        };

    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        require_user_owned_names(
            session.View(),
            "schema-2 exact-shape names must load as user-owned");
    }

    const std::string rewritten = ReadFile(path);
    Require(
        rewritten.find("\"schema_version\": 4") !=
                std::string::npos &&
            rewritten.find("\"name_source\"") ==
                std::string::npos,
        "schema rewrite must not invent generated-name provenance");

    {
        specforge::SpectralLinesPanelController restarted(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        require_user_owned_names(
            restarted.View(),
            "restart must preserve user ownership of exact-shape names");
    }

    RemoveTestCache(path);
}

void TestMalformedLegacyCacheBodyIsNotSilentlyOverwritten()
{
    const auto require_preserved =
        [](const std::filesystem::path& path) {
            const std::string original =
                ReadFile(path);
            {
                specforge::
                    SpectralLinesPanelController
                        session(
                            GroupedCatalog(),
                            specforge::
                                PublicSpectralLineCatalogIdentity(),
                            path);
                Require(
                    session.View()
                            .persistence
                            .load_issue ==
                        specforge::
                            SpectralLineCacheLoadIssueKind::
                                InvalidDocument,
                    "a malformed legacy cache body should expose an invalid-document warning");
            }
            Require(
                ReadFile(path) == original,
                "a malformed legacy cache body must remain byte-identical until an explicit user mutation");
        };

    for (const auto& [test_name, catalogs_json] :
         std::initializer_list<
             std::pair<std::string_view, std::string_view>>{
             {"null_catalogs", "null"},
             {"array_catalogs", "[]"},
             {"non_object_catalog", R"json({"specforge.public": []})json"},
             {"object_grouping_views",
              R"json({"specforge.public": {"active_view_id": "", "marker_visibility": {}, "grouping_views": {}}})json"},
             {"object_groups",
              R"json({"specforge.public": {"active_view_id": "view-1", "marker_visibility": {}, "grouping_views": [{"id": "view-1", "name": "Grouping 1", "groups": {}}]}})json"},
         }) {
        const std::filesystem::path path =
            TestCachePath(test_name);
        RemoveTestCache(path);
        WriteMalformedLegacyCacheBody(
            path,
            catalogs_json);
        require_preserved(path);
        RemoveTestCache(path);
    }

    const std::filesystem::path missing_path =
        TestCachePath("missing_catalogs");
    RemoveTestCache(missing_path);
    {
        std::ofstream stream(
            missing_path,
            std::ios::binary);
        stream << R"json({
  "format_kind": "specforge.catalog_user_state.cache",
  "schema_version": 2,
  "catalog_panel_state": {}
})json";
    }
    require_preserved(missing_path);
    RemoveTestCache(missing_path);
}

void TestModificationSelectionAndPersistenceRoundTrip()
{
    const std::filesystem::path path = TestCachePath("round_trip");
    RemoveTestCache(path);

    std::string user_view_id;
    std::string first_group_id;
    std::string second_group_id;
    std::string first_flush_contents;
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::CreateUserGroupingView()),
            "creating a user grouping view should be applied");
        specforge::CatalogUserStateView state = session.View();
        const specforge::SpectralLineGroupingView* user_view = FindActiveEditableView(state);
        Require(user_view != nullptr, "created user grouping view should become active");
        user_view_id = user_view->id;
        const specforge::SpectralLineGroupView* unassigned = FindUnassignedGroup(*user_view);
        Require(unassigned != nullptr, "created user grouping view should have an unassigned group");
        const std::string unassigned_group_id = unassigned->id;

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::AddUserGroup(user_view_id)),
            "first user group should be added");
        state = session.View();
        user_view = FindGroupingView(state, user_view_id);
        Require(user_view != nullptr, "user grouping view should still exist after adding a group");
        first_group_id = user_view->groups.front().id;

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::MoveMarkerReference(
                user_view_id,
                "h_alpha",
                unassigned_group_id,
                first_group_id)),
            "marker reference should move from unassigned into the first user group");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::CopyMarkerReference(
                user_view_id,
                "h_beta",
                unassigned_group_id,
                first_group_id)),
            "copying from unassigned should organize the marker without persisting a stale unassigned reference");
        state = session.View();
        user_view = FindGroupingView(state, user_view_id);
        unassigned = user_view == nullptr ? nullptr : FindUnassignedGroup(*user_view);
        const specforge::SpectralLineGroupView* first_group =
            user_view == nullptr ? nullptr : FindGroup(*user_view, first_group_id);
        Require(
            unassigned != nullptr && FindMarker(*unassigned, "h_beta") == nullptr,
            "a marker copied into an ordinary group must leave the derived unassigned group");
        Require(
            first_group != nullptr && FindMarker(*first_group, "h_beta") != nullptr,
            "the ordinary group should contain the marker copied from unassigned");
        RequireRejected(
            session.Submit(specforge::CatalogUserStateIntent::CopyMarkerReference(
                user_view_id,
                "h_alpha",
                unassigned_group_id,
                first_group_id)),
            "copy should reject a source group that no longer owns the marker reference");

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::AddUserGroup(user_view_id)),
            "second user group should be added");
        state = session.View();
        user_view = FindGroupingView(state, user_view_id);
        Require(user_view != nullptr && user_view->groups.size() >= 3, "two ordinary groups should be visible");
        second_group_id = user_view->groups[1].id;

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::CopyMarkerReference(
                user_view_id,
                "h_alpha",
                first_group_id,
                second_group_id)),
            "marker reference should copy between owned user groups");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::RenameUserGroupingView(
                user_view_id,
                "Balmer review",
                specforge::CatalogUserRenameEditState::Edited)),
            "user grouping view rename should be applied");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::RenameUserGroup(
                user_view_id,
                first_group_id,
                "Hydrogen",
                specforge::CatalogUserRenameEditState::Edited)),
            "user group rename should be applied");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SetGroupExpanded(
                user_view_id,
                first_group_id,
                true)),
            "expanded state should be persisted by the Module");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SetGroupMarkerVisibility(
                user_view_id,
                first_group_id,
                false)),
            "group visibility should update shared marker visibility");

        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SetGroupingViewSearch("alpha")),
            "grouping view search should update the read view");
        RequireRejected(
            session.Submit(specforge::CatalogUserStateIntent::SetGroupMarkerVisibility(
                user_view_id,
                first_group_id,
                true)),
            "bulk group visibility should be rejected while grouping view search is active");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SetGroupingViewSearch("")),
            "clearing grouping view search should be applied");

        const std::string catalog_view_id = specforge::CatalogGroupingViewId();
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SelectGroupingView(catalog_view_id)),
            "catalog grouping view selection should be applied");
        RequireApplied(
            session.Submit(specforge::CatalogUserStateIntent::SelectGroupingView(user_view_id)),
            "user grouping view should be selected before persistence");
        Require(session.Flush(), "modified catalog user state should flush successfully");
        Require(std::filesystem::exists(path), "modified catalog user state should create a cache file");
        first_flush_contents = ReadFile(path);
        Require(!first_flush_contents.empty(), "first flush should write a non-empty cache file");
    }

    {
        specforge::SpectralLinesPanelController restored(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        const specforge::CatalogUserStateView state = restored.View();
        const specforge::SpectralLineGroupingView* user_view = FindGroupingView(state, user_view_id);
        Require(user_view != nullptr, "persisted user grouping view should be restored");
        Require(user_view->active, "persisted active grouping view selection should be restored");
        Require(user_view->name == "Balmer review", "persisted grouping view name should be restored");

        const specforge::SpectralLineGroupView* first_group = FindGroup(*user_view, first_group_id);
        const specforge::SpectralLineGroupView* second_group = FindGroup(*user_view, second_group_id);
        Require(first_group != nullptr && second_group != nullptr, "persisted user groups should be restored");
        Require(first_group->name == "Hydrogen", "persisted user group name should be restored");
        Require(first_group->expanded, "persisted expanded state should be restored");

        const specforge::SpectralLineMarkerReferenceView* first_marker = FindMarker(*first_group, "h_alpha");
        const specforge::SpectralLineMarkerReferenceView* second_marker = FindMarker(*second_group, "h_alpha");
        Require(first_marker != nullptr && second_marker != nullptr, "shared marker references should be restored");
        Require(first_marker->shared && second_marker->shared, "restored marker references should remain shared");
        Require(!first_marker->visible && !second_marker->visible, "shared marker visibility should be restored");
        Require(restored.Flush(), "restored clean session should flush successfully");
        Require(
            ReadFile(path) == first_flush_contents,
            "restoring and flushing should not rewrite a non-canonical first save");
    }

    RemoveTestCache(path);
}

void TestPersistenceViewReportsLoadWarningRetryAndRecovery()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge_catalog_user_state_health";
    const std::filesystem::path blocker =
        root / "not-a-directory";
    const std::filesystem::path path =
        blocker / "catalog-user-state.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const std::filesystem::path warning_path =
        root / "corrupt-catalog-user-state.json";
    {
        std::ofstream stream(warning_path);
        stream << "{ invalid json";
    }
    {
        specforge::SpectralLinesPanelController warned(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            warning_path);
        Require(
            warned.View().persistence.load_issue ==
                    specforge::SpectralLineCacheLoadIssueKind::
                        InvalidDocument &&
                !warned.View()
                     .persistence
                     .load_diagnostic_detail.empty(),
            "spectral-line view should expose a typed cache load issue and parser detail");
    }
    specforge::SpectralLinesPanelController session(
        GroupedCatalog(),
        specforge::PublicSpectralLineCatalogIdentity(),
        path);
    RequireApplied(
        session.Submit(
            specforge::CatalogUserStateIntent::
                SetMarkerVisibility(
                    "h_alpha",
                    false)),
        "spectral-line state mutation should become dirty");
    Require(
        session.Flush(),
        "the recovery fixture should establish an initial cache");
    std::filesystem::remove_all(blocker, error);
    {
        std::ofstream stream(blocker);
        stream << "block cache directory creation";
    }
    RequireApplied(
        session.Submit(
            specforge::CatalogUserStateIntent::
                SetMarkerVisibility(
                    "h_alpha",
                    true)),
        "a second spectral-line mutation should become dirty");
    Require(
        !session.Flush(),
        "blocked spectral-line cache path should fail to flush");
    Require(
        session.View().persistence.retrying &&
            !session.View()
                 .persistence
                 .save_diagnostic_detail.empty() &&
            session.View()
                    .persistence
                    .save_diagnostic_detail.find(
                        "Could not save spectral-line grouping cache:") ==
                std::string::npos,
        "spectral-line view should expose retry state and raw diagnostics without an English application prefix");

    std::filesystem::remove(blocker);
    std::filesystem::create_directories(blocker);
    Require(
        session.Flush(),
        "spectral-line cache should retry after repairing its path");
    Require(
        session.View().persistence.recovered,
        "spectral-line view should expose successful recovery");

    RequireApplied(
        session.Submit(
            specforge::CatalogUserStateIntent::
                SetMarkerVisibility(
                    "h_alpha",
                    false)),
        "a later spectral-line mutation should apply");
    Require(
        !session.View().persistence.recovered,
        "a later spectral-line mutation should clear recovery");
    Require(
        session.Flush(),
        "the final spectral-line state should flush");

    std::filesystem::remove_all(root, error);
}

void TestStartupCanonicalizationIsNotAnExplicitReconciliationDelta()
{
    const std::filesystem::path path =
        TestCachePath("canonicalization_reconciliation_base");
    RemoveTestCache(path);
    WriteNonCanonicalCache(path);

    // This controller keeps its canonicalized startup snapshot stale while a
    // peer commits a real rename and selection change.
    specforge::SpectralLinesPanelController stale(
        GroupedCatalog(),
        specforge::PublicSpectralLineCatalogIdentity(),
        path);
    {
        specforge::SpectralLinesPanelController peer(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            peer.Submit(
                specforge::CatalogUserStateIntent::RenameUserGroupingView(
                    "view-1",
                    "Peer rename",
                    specforge::CatalogUserRenameEditState::Edited)),
            "peer should commit a real rename after stale startup canonicalization");
        RequireApplied(
            peer.Submit(
                specforge::CatalogUserStateIntent::SelectGroupingView(
                    "view-1")),
            "peer should commit a real selection after stale startup canonicalization");
        Require(peer.Flush(), "peer canonicalization and task delta should flush");
    }

    RequireApplied(
        stale.Submit(
            specforge::CatalogUserStateIntent::SetMarkerVisibility(
                "h_alpha",
                false)),
        "stale controller should accept an unrelated explicit task delta");
    Require(
        stale.Flush(),
        "stale controller should reconcile without treating startup repairs as edits");

    const specforge::CatalogUserStateCacheLoadResult loaded =
        specforge::LoadCatalogUserStateCache(path);
    const auto& state = loaded.cache.catalogs.at("specforge.public");
    const auto view = std::find_if(
        state.grouping_views.begin(),
        state.grouping_views.end(),
        [](const auto& candidate) { return candidate.id == "view-1"; });
    Require(
        view != state.grouping_views.end() &&
            view->name == "Peer rename" &&
            state.active_view_id == "view-1" &&
            !specforge::IsMarkerVisible(state, "h_alpha"),
        "startup canonicalization must not overwrite a peer rename or selection");
    RemoveTestCache(path);
}

void TestLegacySchemaFirstExplicitWriteMigratesBeforeReconciliation()
{
    const std::filesystem::path path =
        TestCachePath("legacy_schema_two_first_write");
    RemoveTestCache(path);
    for (const int schema_version : {2, 3}) {
        WriteLegacySchemaTwoCacheWithoutUnassignedFlag(path);
        if (schema_version == 3) {
            std::string schema_three = ReadFile(path);
            schema_three = ReplaceFirst(
                std::move(schema_three),
                "\"schema_version\": 2",
                "\"schema_version\": 3");
            schema_three = ReplaceFirst(
                std::move(schema_three),
                "\"name\": \"Legacy group\",\n          \"marker_references\":",
                "\"name\": \"Legacy group\",\n          \"is_unassigned\": false,\n          \"marker_references\":");
            WriteTextFile(path, schema_three);
        }

        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::SetMarkerVisibility(
                    "h_alpha",
                    false)),
            "a first user mutation on legacy state should be accepted");
        Require(
            session.Flush(),
            "a first user mutation must migrate legacy state before reconciliation");

        const auto loaded = specforge::LoadCatalogUserStateCache(path);
        const auto& state = loaded.cache.catalogs.at("specforge.public");
        const auto view = std::find_if(
            state.grouping_views.begin(),
            state.grouping_views.end(),
            [](const auto& candidate) { return candidate.id == "view-1"; });
        Require(
            view != state.grouping_views.end() &&
                std::any_of(
                    view->groups.begin(),
                    view->groups.end(),
                    [](const auto& group) {
                        return group.id == specforge::UnassignedUserGroupId() &&
                               group.is_unassigned;
                    }) &&
                !specforge::IsMarkerVisible(state, "h_alpha"),
            "legacy first-write migration must preserve canonical unassigned state and the user delta");
    }
    RemoveTestCache(path);
}

void TestGeneratedCatalogIdsRemainMonotonicAcrossDeletionAndRestart()
{
    const std::filesystem::path path =
        TestCachePath("monotonic_generated_ids");
    RemoveTestCache(path);

    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::CreateUserGroupingView()),
            "the id fixture should create view-1");
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::AddUserGroup("view-1")),
            "the id fixture should create group-1");
        Require(session.Flush(), "the initial generated ids should flush");
    }
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::DeleteUserGroup(
                    "view-1",
                    "group-1")),
            "the id fixture should delete group-1");
        Require(session.Flush(), "the deleted group should flush");
    }
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::AddUserGroup("view-1")),
            "the recreated group should be accepted");
        const specforge::CatalogUserStateView view_state = session.View();
        const specforge::SpectralLineGroupingView* view =
            FindGroupingView(view_state, "view-1");
        Require(
            view != nullptr && FindGroup(*view, "group-2") != nullptr,
            "deleting and recreating a group must not reuse group-1");
        Require(session.Flush(), "the recreated group should flush");
    }
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::DeleteUserGroupingView(
                    "view-1")),
            "the id fixture should delete view-1");
        Require(session.Flush(), "the deleted view should flush");
    }
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::CreateUserGroupingView()),
            "the recreated view should be accepted");
        Require(
            FindGroupingView(session.View(), "view-2") != nullptr,
            "deleting and recreating a view must not reuse view-1");
        Require(session.Flush(), "the recreated view should flush");
    }

    const specforge::CatalogUserStateCacheLoadResult loaded =
        specforge::LoadCatalogUserStateCache(path);
    const auto& state = loaded.cache.catalogs.at("specforge.public");
    Require(
        state.next_view_sequence >= 3 &&
            state.next_group_sequence >= 3 &&
            state.reserved_view_ids.contains("view-1") &&
            state.reserved_view_ids.contains("view-2") &&
            state.reserved_group_ids.contains("group-1") &&
            state.reserved_group_ids.contains("group-2"),
        "generated identity high-water marks and durable reservations must survive restart");
    RemoveTestCache(path);
}

int RunCatalogReconciliationChild(int argc, char* argv[])
{
    if (argc < 7) {
        std::cerr << "catalog reconciliation child received too few arguments\n";
        return 2;
    }

    const std::string operation = argv[2];
    const std::filesystem::path cache_path = argv[3];
    const std::filesystem::path ready_path = argv[4];
    const std::filesystem::path go_path = argv[5];
    const std::filesystem::path done_path = argv[6];
    try {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            cache_path);
        WriteMarkerFile(ready_path);
        if (!WaitForMarkerFile(go_path, 30s)) {
            throw std::runtime_error("catalog reconciliation child did not receive go marker");
        }

        if (operation == "set_alpha_hidden") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::SetMarkerVisibility(
                        "h_alpha",
                        false)),
                "child should hide h_alpha");
        } else if (operation == "set_alpha_visible") {
            // Make the explicit true value a real local map delta rather than
            // relying on the default-visible fallback.
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::SetMarkerVisibility(
                        "h_alpha",
                        false)),
                "child should establish the visible conflict baseline");
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::SetMarkerVisibility(
                        "h_alpha",
                        true)),
                "child should set h_alpha visible explicitly");
        } else if (operation == "set_beta_hidden") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::SetMarkerVisibility(
                        "h_beta",
                        false)),
                "child should hide h_beta");
        } else if (operation == "create_view") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::CreateUserGroupingView()),
                "child should create a grouping view");
        } else if (operation == "create_view_with_group") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::CreateUserGroupingView()),
                "child should create a grouping view before adding its group");
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::AddUserGroup(
                        "view-3")),
                "child should add a group to its new view");
        } else if (operation == "delete_view_1") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::DeleteUserGroupingView(
                        "view-1")),
                "child should delete view-1");
        } else if (operation == "rename_view_1_local" ||
                   operation == "rename_view_1_remote") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::RenameUserGroupingView(
                        "view-1",
                        operation == "rename_view_1_local"
                            ? "Local view 1"
                            : "Remote view 1",
                        specforge::CatalogUserRenameEditState::Edited)),
                "child should commit its conflicting view rename");
        } else if (operation == "add_group_view_2") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::AddUserGroup(
                        "view-2")),
                "child should add a group to view-2");
        } else if (operation == "delete_group_1_view_2") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::DeleteUserGroup(
                        "view-2",
                        "group-1")),
                "child should delete group-1 from view-2");
        } else if (operation == "copy_reference_group_1_to_group_2") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::CopyMarkerReference(
                        "view-2",
                        "h_alpha",
                        "group-1",
                        "group-2")),
                "child should copy the reference into group-2");
        } else if (operation == "remove_reference_group_1") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::RemoveMarkerReference(
                        "view-2",
                        "h_alpha",
                        "group-1")),
                "child should remove the reference from group-1");
        } else if (operation == "rename_view_2") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::RenameUserGroupingView(
                        "view-2",
                        "Remote view 2",
                        specforge::CatalogUserRenameEditState::Edited)),
                "child should rename view-2");
        } else if (operation == "reorder_groups") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::ReorderUserGroupBefore(
                        "view-2",
                        "group-2",
                        "group-1")),
                "child should reorder view-2 groups");
        } else if (operation == "reorder_groups_reverse") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::ReorderUserGroupBefore(
                        "view-2",
                        "group-3",
                        "group-1")),
                "child should reverse the view-2 group order");
        } else if (operation == "reorder_and_add_group") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::ReorderUserGroupBefore(
                        "view-2",
                        "group-2",
                        "group-1")),
                "ordering child should explicitly reorder view-2 groups");
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::AddUserGroup(
                        "view-2")),
                "ordering child should add its task group after reordering");
        } else if (operation == "select_view_1") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::SelectGroupingView(
                        "view-1")),
                "child should select view-1");
        } else if (operation == "select_view_2") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::SelectGroupingView(
                        "view-2")),
                "child should select view-2");
        } else if (operation == "select_view_2_then_1") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::SelectGroupingView(
                        "view-2")),
                "child should select view-2 before returning to view-1");
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::SelectGroupingView(
                        "view-1")),
                "child should explicitly return to view-1");
        } else if (operation == "hold_after_mutation") {
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::SetMarkerVisibility(
                        "h_alpha",
                        false)),
                "interrupted child should mutate before being stopped");
            specforge::ExclusiveFileLeaseAcquireResult held_commit_lease =
                specforge::TryAcquireExclusiveFileLease(
                    specforge::CatalogUserStateCacheCommitLeasePath(
                        cache_path));
            Require(
                held_commit_lease.status ==
                    specforge::ExclusiveFileLeaseAcquireStatus::Acquired,
                "interrupted child should hold the catalog commit lease before termination");
            WriteMarkerFile(done_path, "mutated\n");
            if (!WaitForMarkerFile(cache_path.string() + ".release", 60s)) {
                // The parent normally terminates this child. The bounded
                // deadline prevents a failed test from orphaning it.
                throw std::runtime_error("interrupted child release timed out");
            }
            return 0;
        } else if (operation == "interrupt_during_atomic_replace") {
            const std::filesystem::path release_path =
                cache_path.string() + ".replace.release";
            specforge::SetCatalogUserStateCacheBeforeReplaceHookForTests(
                [&](const std::filesystem::path& temporary_path,
                    const std::filesystem::path&) {
                    WriteMarkerFile(
                        done_path,
                        std::string("before-replace\n") +
                            temporary_path.string() + "\n");
                    if (!WaitForMarkerFile(release_path, 60s)) {
                        throw std::runtime_error(
                            "atomic replace interruption release timed out");
                    }
                });
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::SetMarkerVisibility(
                        "h_alpha",
                        false)),
                "atomic interruption child should mutate before replacement");
            Require(
                session.Flush(),
                "atomic interruption child should reach replacement checkpoint");
            return 0;
        } else if (operation == "corrupt_latest_after_startup") {
            std::ofstream stream(cache_path, std::ios::binary | std::ios::trunc);
            Require(stream.good(), "corruption child should open the latest cache");
            stream << "{ invalid latest catalog cache after startup";
            Require(stream.good(), "corruption child should write the latest cache");
            WriteMarkerFile(done_path, "corrupted\n");
            return 0;
        } else if (operation == "invalid_latest_identity_after_startup") {
            std::string contents = ReadFile(cache_path);
            const std::string needle = "\"id\": \"view-2\"";
            const std::size_t position = contents.find(needle);
            Require(
                position != std::string::npos,
                "semantic corruption child should find view-2 in the latest cache");
            contents.replace(
                position,
                needle.size(),
                "\"id\": \"view-1\"");
            std::ofstream stream(
                cache_path,
                std::ios::binary | std::ios::trunc);
            Require(
                stream.good(),
                "semantic corruption child should open the latest cache");
            stream << contents;
            Require(
                stream.good(),
                "semantic corruption child should write the latest cache");
            WriteMarkerFile(done_path, "duplicate-identity\n");
            return 0;
        } else if (operation == "duplicate_catalog_key_after_startup") {
            const std::string contents = ReadFile(cache_path);
            const std::string line_break =
                contents.find("\r\n") != std::string::npos
                    ? "\r\n"
                    : "\n";
            const std::string corrupted = ReplaceFirst(
                contents,
                line_break + "    }" + line_break +
                    "  }," + line_break +
                    "  \"catalog_panel_state\"",
                line_break + "    }," + line_break +
                    "    \"specforge.public\": {}," + line_break +
                    "  }," + line_break +
                    "  \"catalog_panel_state\"");
            WriteTextFile(cache_path, corrupted);
            WriteMarkerFile(done_path, "duplicate-catalog-key\n");
            return 0;
        } else if (operation == "duplicate_identity_member_after_startup") {
            const std::string contents = ReadFile(cache_path);
            const std::string needle = "        \"id\": \"view-2\",";
            const std::string corrupted = ReplaceFirst(
                contents,
                needle,
                needle +
                    (contents.find("\r\n") != std::string::npos
                         ? "\r\n"
                         : "\n") +
                    "        \"id\": \"view-1\",");
            WriteTextFile(cache_path, corrupted);
            WriteMarkerFile(done_path, "duplicate-identity-member\n");
            return 0;
        } else {
            throw std::runtime_error("unknown catalog reconciliation child operation");
        }

        Require(session.Flush(), "catalog reconciliation child should flush");
        WriteMarkerFile(done_path, "flushed\n");
        return 0;
    } catch (const std::exception& error) {
        std::ofstream stream(done_path, std::ios::binary | std::ios::trunc);
        stream << "error: " << error.what() << '\n';
        std::cerr << "catalog reconciliation child FAILED: " << error.what() << '\n';
        return 2;
    }
}

void TestCorruptLatestCatalogStateFailsClosed()
{
    const std::filesystem::path path =
        TestCachePath("corrupt_latest_reconciliation");
    RemoveTestCache(path);
    SeedMultiProcessCache(path);

    // Keep a valid startup snapshot in this controller, then let a separate
    // process corrupt the durable latest file after startup.
    const std::filesystem::path marker_root =
        path.parent_path() / (path.stem().string() + "_corrupt_markers");
    std::error_code marker_error;
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        std::filesystem::remove_all(marker_root, marker_error);
        std::filesystem::create_directories(marker_root);
        ChildProcess corruptor = StartCatalogChild(
            "corrupt_latest_after_startup",
            path,
            marker_root / "ready",
            marker_root / "go",
            marker_root / "done");
        Require(
            WaitForMarkerFile(marker_root / "ready", 10s),
            "corruption child should publish its startup snapshot");
        WriteMarkerFile(marker_root / "go");
        Require(
            WaitForMarkerFile(marker_root / "done", 10s),
            "corruption child should corrupt the latest cache after startup");
        Require(
            WaitForCatalogChild(corruptor, "corrupt_latest_after_startup") == 0,
            "corruption child should exit after corrupting the latest cache");
        const std::string original = ReadFile(path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::SetMarkerVisibility(
                    "h_alpha",
                    false)),
            "a mutation against a corrupt cache should still be accepted in memory");
        Require(
            !session.Flush(),
            "a corrupt latest cache must fail closed instead of being overwritten");
        Require(
            ReadFile(path) == original &&
                session.View().persistence.save_diagnostic_detail.find(
                    "latest durable catalog user-state cache is not trusted") !=
                    std::string::npos,
            "failed reconciliation should preserve the corrupt file and expose an actionable diagnostic");
    }
    std::filesystem::remove_all(marker_root, marker_error);
    RemoveTestCache(path);
}

void TestSemanticallyInvalidLatestCatalogStateFailsClosed()
{
    const std::filesystem::path path =
        TestCachePath("invalid_latest_identity_reconciliation");
    RemoveTestCache(path);
    SeedMultiProcessCache(path);

    const std::filesystem::path marker_root =
        path.parent_path() /
        (path.stem().string() + "_invalid_identity_markers");
    std::error_code marker_error;
    {
        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        std::filesystem::remove_all(marker_root, marker_error);
        std::filesystem::create_directories(marker_root);
        ChildProcess corruptor = StartCatalogChild(
            "invalid_latest_identity_after_startup",
            path,
            marker_root / "ready",
            marker_root / "go",
            marker_root / "done");
        Require(
            WaitForMarkerFile(marker_root / "ready", 10s),
            "semantic corruption child should publish its startup snapshot");
        WriteMarkerFile(marker_root / "go");
        Require(
            WaitForMarkerFile(marker_root / "done", 10s),
            "semantic corruption child should write the duplicate identity");
        Require(
            WaitForCatalogChild(
                corruptor,
                "invalid_latest_identity_after_startup") == 0,
            "semantic corruption child should exit after writing the duplicate identity");
        const std::string original = ReadFile(path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::SetMarkerVisibility(
                    "h_alpha",
                    false)),
            "a mutation against a semantically invalid cache should be accepted in memory");
        Require(
            !session.Flush(),
            "duplicate latest view identity must fail closed instead of being canonicalized");
        Require(
            ReadFile(path) == original &&
                session.View().persistence.save_diagnostic_detail.find(
                    "semantic identity") != std::string::npos,
            "semantic latest validation should preserve the file and expose its identity diagnostic");
    }
    std::filesystem::remove_all(marker_root, marker_error);
    RemoveTestCache(path);
}

void TestEmptyLatestCatalogIdentitiesFailClosed()
{
    const std::filesystem::path path =
        TestCachePath("empty_latest_catalog_identity_reconciliation");
    const std::string key = "\"specforge.public\":";
    for (const bool panel_state : {false, true}) {
        RemoveTestCache(path);
        SeedMultiProcessCache(path);
        std::string corrupted = ReadFile(path);
        if (!panel_state) {
            corrupted = ReplaceFirst(
                std::move(corrupted),
                key,
                "\"\":");
        } else {
            const std::size_t panel_position =
                corrupted.find("\"catalog_panel_state\"");
            Require(
                panel_position != std::string::npos,
                "empty panel identity fixture should find panel state");
            const std::size_t key_position =
                corrupted.find(key, panel_position);
            Require(
                key_position != std::string::npos,
                "empty panel identity fixture should find its catalog key");
            corrupted.replace(key_position, key.size(), "\"\":");
        }
        WriteTextFile(path, corrupted);

        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::SetMarkerVisibility(
                    "h_alpha",
                    false)),
            "an empty latest catalog identity should not reject the in-memory task");
        Require(
            !session.Flush(),
            "an empty catalog or panel identity must fail closed before replacement");
        Require(
            ReadFile(path) == corrupted &&
                session.View().persistence.save_diagnostic_detail.find(
                    "empty catalog identity") != std::string::npos,
            "empty catalog identities must preserve the latest file and expose a diagnostic");
    }
    RemoveTestCache(path);
}

void TestDuplicateLatestJsonKeysFailClosed()
{
    const std::filesystem::path path =
        TestCachePath("duplicate_latest_json_keys_reconciliation");
    for (const std::string_view operation : {
             "duplicate_catalog_key_after_startup",
             "duplicate_identity_member_after_startup"}) {
        RemoveTestCache(path);
        SeedMultiProcessCache(path);
        const std::filesystem::path marker_root =
            path.parent_path() /
            (path.stem().string() + "_" + std::string(operation) + "_markers");
        std::error_code marker_error;
        std::filesystem::remove_all(marker_root, marker_error);
        std::filesystem::create_directories(marker_root);

        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        ChildProcess corruptor = StartCatalogChild(
            operation,
            path,
            marker_root / "ready",
            marker_root / "go",
            marker_root / "done");
        Require(
            WaitForMarkerFile(marker_root / "ready", 10s),
            "duplicate-key child should publish its startup snapshot");
        WriteMarkerFile(marker_root / "go");
        Require(
            WaitForMarkerFile(marker_root / "done", 10s),
            "duplicate-key child should write the duplicate JSON key");
        Require(
            WaitForCatalogChild(corruptor, operation) == 0,
            "duplicate-key child should exit after writing the duplicate JSON key");
        const std::string corrupted = ReadFile(path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::SetMarkerVisibility(
                    "h_alpha",
                    false)),
            "a mutation against a duplicate-key cache should still be accepted in memory");
        Require(
            !session.Flush(),
            "duplicate JSON keys in the latest cache must fail closed before replacement");
        Require(
            ReadFile(path) == corrupted,
            "duplicate JSON keys must preserve the latest file");
        Require(
            session.View().persistence.save_diagnostic_detail.find(
                "duplicate JSON object member") != std::string::npos,
            "duplicate JSON keys must expose an actionable diagnostic: " +
                session.View().persistence.save_diagnostic_detail);

        std::filesystem::remove_all(marker_root, marker_error);
    }
    RemoveTestCache(path);
}

void TestCurrentSchemaSemanticCorruptionFailsClosedWithoutMaintenanceRewrite()
{
    const std::filesystem::path path =
        TestCachePath("current_schema_maintenance_corruption");
    for (const bool empty_view_identity : {false, true}) {
        RemoveTestCache(path);
        SeedMultiProcessCache(path);
        std::string corrupted = ReadFile(path);
        corrupted = ReplaceFirst(
            std::move(corrupted),
            "\"id\": \"view-2\"",
            empty_view_identity
                ? "\"id\": \"\""
                : "\"id\": \"view-1\"");
        WriteTextFile(path, corrupted);

        {
            specforge::SpectralLinesPanelController session(
                GroupedCatalog(),
                specforge::PublicSpectralLineCatalogIdentity(),
                path);
            Require(
                !session.Flush(),
                "maintenance flush must reject current-schema semantic corruption");
            Require(
                session.View().persistence.save_diagnostic_detail.find(
                    "semantic identity") != std::string::npos,
                "maintenance rejection must expose the semantic identity diagnostic");
        }
        Require(
            ReadFile(path) == corrupted,
            "maintenance/destructor flush must preserve a semantically corrupt schema-four cache");
    }
    RemoveTestCache(path);
}

void TestLegacyIdentityCorruptionFailsClosedBeforeMigration()
{
    const std::filesystem::path path =
        TestCachePath("legacy_identity_corruption_before_migration");
    const std::vector<std::pair<std::string_view, std::string_view>> corruptions = {
        {"\"id\": \"view-2\"", "\"id\": \"\""},
        {"\"id\": \"view-2\"", "\"id\": \"view-1\""},
        {"\"id\": \"group-2\"", "\"id\": \"\""},
        {"\"id\": \"group-2\"", "\"id\": \"group-1\""},
    };

    for (const int schema_version : {1, 2, 3}) {
        for (const auto& [needle, replacement] : corruptions) {
            RemoveTestCache(path);
            SeedMultiProcessCache(path);
            std::string corrupted = ReplaceFirst(
                ReadFile(path),
                "\"schema_version\": 4",
                "\"schema_version\": " + std::to_string(schema_version));
            corrupted = ReplaceFirst(
                std::move(corrupted),
                needle,
                replacement);
            WriteTextFile(path, corrupted);

            specforge::SpectralLinesPanelController session(
                GroupedCatalog(),
                specforge::PublicSpectralLineCatalogIdentity(),
                path);
            Require(
                session.View().persistence.load_issue ==
                    specforge::SpectralLineCacheLoadIssueKind::InvalidDocument,
                "legacy identity corruption must be reported before startup canonicalization");
            RequireApplied(
                session.Submit(
                    specforge::CatalogUserStateIntent::SetMarkerVisibility(
                        "h_alpha",
                        false)),
                "legacy identity corruption test should create a replacement attempt");
            Require(
                !session.Flush(),
                "legacy empty or duplicate view/group identities must fail before canonicalization");
            Require(
                session.View().persistence.save_diagnostic_detail.find(
                    "legacy") != std::string::npos &&
                    session.View().persistence.save_diagnostic_detail.find(
                        "identity") != std::string::npos,
                "legacy identity rejection must expose an actionable diagnostic: " +
                    session.View().persistence.save_diagnostic_detail);
            Require(
                ReadFile(path) == corrupted,
                "legacy identity corruption must preserve the original latest file");
        }
    }
    RemoveTestCache(path);
}

void TestCurrentSchemaAllocatorHistoryCorruptionFailsClosed()
{
    const std::filesystem::path path =
        TestCachePath("current_schema_allocator_history_corruption");
    const std::vector<std::pair<std::string_view, std::string_view>> corruptions = {
        {"\"next_view_sequence\": 3", "\"next_view_sequence\": 0"},
        {"\"next_view_sequence\": 3", "\"next_view_sequence\": 2"},
        {"\"reserved_view_ids\": [\"view-1\", \"view-2\"]",
         "\"reserved_view_ids\": [\"view-1\"]"},
        {"\"next_group_sequence\": 3", "\"next_group_sequence\": 2"},
        {"\"reserved_group_ids\": [\"group-1\", \"group-2\"]",
         "\"reserved_group_ids\": [\"group-1\"]"},
        {"\"reserved_view_ids\": [\"view-1\", \"view-2\"]",
         "\"reserved_view_ids\": [\"view-1\", \"view-2\", \"view-99\"]"},
        {"\"reserved_view_ids\": [\"view-1\", \"view-2\"]",
         "\"reserved_view_ids\": [\"view-1\", \"view-2\", \"view-2\"]"},
    };

    for (const auto& [needle, replacement] : corruptions) {
        RemoveTestCache(path);
        SeedMultiProcessCache(path);
        const std::string corrupted = ReplaceFirst(
            ReadFile(path),
            needle,
            replacement);
        WriteTextFile(path, corrupted);

        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::SetMarkerVisibility(
                    "h_alpha",
                    false)),
            "allocator-history corruption test should create a replacement attempt");
        Require(
            !session.Flush(),
            "inconsistent current-schema allocator history must fail closed");
        Require(
            session.View().persistence.save_diagnostic_detail.find(
                "allocator") != std::string::npos,
            "allocator-history rejection must expose an actionable diagnostic: " +
                session.View().persistence.save_diagnostic_detail);
        Require(
            ReadFile(path) == corrupted,
            "allocator-history corruption must preserve the original latest file");
    }
    RemoveTestCache(path);
}

void TestLegacyReferenceAndUnassignedCorruptionFailsClosedBeforeMigration()
{
    const std::filesystem::path path =
        TestCachePath("legacy_reference_and_unassigned_corruption");

    for (const int schema_version : {1, 2, 3}) {
        RemoveTestCache(path);
        SeedMultiProcessCache(path);
        std::string corrupted = ReplaceFirst(
            ReadFile(path),
            "\"schema_version\": 4",
            "\"schema_version\": " + std::to_string(schema_version));
        corrupted = ReplaceFirst(
            std::move(corrupted),
            "\"catalog_identity\": \"specforge.public\"",
            "\"catalog_identity\": \"foreign.catalog\"");
        WriteTextFile(path, corrupted);

        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        Require(
            session.View().persistence.load_issue ==
                specforge::SpectralLineCacheLoadIssueKind::InvalidDocument,
            "a legacy foreign marker reference must fail before canonicalization");
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::SetMarkerVisibility(
                    "h_alpha",
                    false)),
            "legacy foreign-reference corruption should create a replacement attempt");
        Require(
            !session.Flush(),
            "a legacy foreign marker reference must fail closed");
        Require(
            ReadFile(path) == corrupted &&
                session.View().persistence.save_diagnostic_detail.find(
                    "marker reference catalog identity") != std::string::npos,
            "legacy foreign marker reference must preserve bytes and expose a diagnostic");
    }

    for (const std::pair<std::string_view, std::string_view> corruption : {
             std::pair<std::string_view, std::string_view>{
                 "\"is_unassigned\": true",
                 "\"is_unassigned\": false"},
             std::pair<std::string_view, std::string_view>{
                 "\"is_unassigned\": false",
                 "\"is_unassigned\": true"},
         }) {
        RemoveTestCache(path);
        SeedMultiProcessCache(path);
        std::string corrupted = ReplaceFirst(
            ReadFile(path),
            "\"schema_version\": 4",
            "\"schema_version\": 3");
        corrupted = ReplaceFirst(
            std::move(corrupted),
            corruption.first,
            corruption.second);
        WriteTextFile(path, corrupted);

        specforge::SpectralLinesPanelController session(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        Require(
            session.View().persistence.load_issue ==
                specforge::SpectralLineCacheLoadIssueKind::InvalidDocument,
            "schema-three unassigned identity/flag mismatch must fail before canonicalization");
        RequireApplied(
            session.Submit(
                specforge::CatalogUserStateIntent::SetMarkerVisibility(
                    "h_alpha",
                    false)),
            "schema-three flag corruption should create a replacement attempt");
        Require(
            !session.Flush(),
            "schema-three unassigned identity/flag mismatch must fail closed");
        Require(
            ReadFile(path) == corrupted &&
                session.View().persistence.save_diagnostic_detail.find(
                    "unassigned group identity/flag mismatch") != std::string::npos,
            "schema-three flag corruption must preserve bytes and expose a diagnostic");
    }
    RemoveTestCache(path);
}

void TestSameTaskNewGroupReorderPreservesFinalOrder()
{
    const std::filesystem::path path =
        TestCachePath("same_task_new_group_reorder");
    RemoveTestCache(path);
    SeedMultiProcessCacheWithThreeGroups(path);

    specforge::SpectralLinesPanelController session(
        GroupedCatalog(),
        specforge::PublicSpectralLineCatalogIdentity(),
        path);
    RequireApplied(
        session.Submit(
            specforge::CatalogUserStateIntent::AddUserGroup("view-2")),
        "same-task ordering test should create group-4");
    RequireApplied(
        session.Submit(
            specforge::CatalogUserStateIntent::ReorderUserGroupBefore(
                "view-2",
                "group-4",
                "group-1")),
        "same-task ordering test should move the new group before group-1");
    Require(
        session.Flush(),
        "same-task new-group reorder should flush");

    const auto loaded = specforge::LoadCatalogUserStateCache(path);
    const auto& views = loaded.cache.catalogs.at("specforge.public").grouping_views;
    const auto view = std::find_if(
        views.begin(),
        views.end(),
        [](const auto& candidate) { return candidate.id == "view-2"; });
    Require(view != views.end(), "same-task ordering test should retain view-2");
    Require(
        view->groups.size() == 5 &&
            view->groups[0].id == "group-4" &&
            view->groups[1].id == "group-1" &&
            view->groups[2].id == "group-2" &&
            view->groups[3].id == "group-3" &&
            view->groups[4].id == specforge::UnassignedUserGroupId(),
        "a new group explicitly moved before an existing group must retain that final order");
    RemoveTestCache(path);
}

void TestLegacyMultiCatalogMigrationFailsClosed()
{
    const std::filesystem::path path =
        TestCachePath("legacy_multi_catalog_migration");
    for (const int schema_version : {1, 2, 3}) {
        RemoveTestCache(path);
        SeedLegacyMultiCatalogCache(path, schema_version);
        const std::string legacy = ReadFile(path);

        {
            specforge::SpectralLinesPanelController session(
                GroupedCatalog(),
                specforge::PublicSpectralLineCatalogIdentity(),
                path);
            Require(
                !session.Flush(),
                "legacy multi-catalog migration must fail closed without a foreign catalog definition");
            Require(
                session.View().persistence.save_diagnostic_detail.find(
                    "unrelated catalog") != std::string::npos,
                "legacy multi-catalog rejection must explain the unsafe migration");
        }
        Require(
            ReadFile(path) == legacy,
            "legacy multi-catalog migration failure must preserve the original file");
    }
    RemoveTestCache(path);
}

void TestConcurrentCatalogStateReconciliation()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("specforge_catalog_user_state_multiprocess_" +
         std::to_string(GetCurrentProcessId()));
    const std::filesystem::path path = root / "state.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    SeedMultiProcessCache(path);
    RunConcurrentCatalogChildren(path, "set_alpha_hidden", "set_beta_hidden");
    {
        const specforge::CatalogUserStateCacheLoadResult loaded =
            specforge::LoadCatalogUserStateCache(path);
        const specforge::CatalogUserState& state =
            loaded.cache.catalogs.at("specforge.public");
        Require(
            !specforge::IsMarkerVisible(state, "h_alpha") &&
                !specforge::IsMarkerVisible(state, "h_beta"),
            "concurrent disjoint marker updates must both survive");
    }

    // A restart reads the reconciled durable snapshot, not either stale
    // controller's original startup snapshot.
    {
        specforge::SpectralLinesPanelController restarted(
            GroupedCatalog(),
            specforge::PublicSpectralLineCatalogIdentity(),
            path);
        Require(
            !restarted.View().grouping_views.empty(),
            "restarted controller should read the reconciled grouping state");
        const specforge::SpectralLinePlotView restarted_plot =
            restarted.PlotView(Snapshot(true));
        const auto marker_is_visible =
            [&restarted_plot](std::string_view marker_id) {
                return std::any_of(
                    restarted_plot.visible_markers.begin(),
                    restarted_plot.visible_markers.end(),
                    [marker_id](const specforge::SpectralLineMarker* marker) {
                        return marker != nullptr && marker->id == marker_id;
                    });
            };
        Require(
            !marker_is_visible("h_alpha") && !marker_is_visible("h_beta"),
            "restarted plot projection must retain both child processes' hidden markers");
        Require(restarted.Flush(), "a clean restart should remain flushable");
    }

    SeedMultiProcessCache(path);
    RunConcurrentCatalogChildren(path, "set_alpha_hidden", "set_alpha_visible");
    {
        const auto loaded = specforge::LoadCatalogUserStateCache(path);
        const auto& state = loaded.cache.catalogs.at("specforge.public");
        const auto alpha = state.marker_visibility.find("h_alpha");
        Require(
            alpha != state.marker_visibility.end(),
            "same-field visibility conflict must retain one explicit winner");
    }

    SeedMultiProcessCache(path);
    RunConcurrentCatalogChildren(
        path,
        "rename_view_1_local",
        "rename_view_1_remote");
    {
        const auto loaded = specforge::LoadCatalogUserStateCache(path);
        const auto& views = loaded.cache.catalogs.at("specforge.public").grouping_views;
        const auto view = std::find_if(
            views.begin(),
            views.end(),
            [](const auto& candidate) { return candidate.id == "view-1"; });
        Require(
            view != views.end() &&
                (view->name == "Local view 1" || view->name == "Remote view 1"),
            "same-field view rename conflict must retain one explicit winner");
    }

    SeedMultiProcessCache(path);
    RunConcurrentCatalogChildren(path, "create_view", "create_view");
    {
        const specforge::CatalogUserStateCacheLoadResult loaded =
            specforge::LoadCatalogUserStateCache(path);
        const auto& views = loaded.cache.catalogs.at("specforge.public").grouping_views;
        std::unordered_set<std::string> ids;
        for (const specforge::GroupingView& view : views) {
            ids.insert(view.id);
        }
        Require(
            views.size() == 4 && ids.size() == views.size(),
            "concurrent grouping-view additions must retain both colliding ids under unique identities");
    }

    RunCreateDeleteViewRace(path);
    RunExplicitSelectionRoundTripRace(path);
    RunMissingAllocatorHistoryFailsClosedRace(path);
    RunCreateDeleteGroupRace(path);

    SeedMultiProcessCache(path);
    RunConcurrentCatalogChildren(
        path,
        "create_view_with_group",
        "create_view_with_group");
    {
        const auto loaded = specforge::LoadCatalogUserStateCache(path);
        const auto& views = loaded.cache.catalogs.at("specforge.public").grouping_views;
        std::unordered_set<std::string> view_ids;
        std::unordered_set<std::string> ordinary_group_ids;
        for (const auto& view : views) {
            view_ids.insert(view.id);
            for (const auto& group : view.groups) {
                if (!group.is_unassigned &&
                    group.id != specforge::UnassignedUserGroupId()) {
                    ordinary_group_ids.insert(group.id);
                }
            }
        }
        Require(
            views.size() == 4 && view_ids.size() == views.size() &&
                ordinary_group_ids.size() == 4,
            "concurrent new-view and new-group tasks must remap group identities globally");
    }

    SeedMultiProcessCache(path);
    RunConcurrentCatalogChildren(
        path,
        "create_view_with_group",
        "add_group_view_2");
    {
        const auto loaded = specforge::LoadCatalogUserStateCache(path);
        const auto& views = loaded.cache.catalogs.at("specforge.public").grouping_views;
        std::unordered_set<std::string> view_ids;
        std::unordered_set<std::string> ordinary_group_ids;
        std::size_t view_two_group_count = 0;
        bool found_new_view = false;
        for (const auto& view : views) {
            view_ids.insert(view.id);
            found_new_view = found_new_view || view.id == "view-3";
            if (view.id == "view-2") {
                view_two_group_count = view.groups.size();
            }
            for (const auto& group : view.groups) {
                if (!group.is_unassigned &&
                    group.id != specforge::UnassignedUserGroupId()) {
                    ordinary_group_ids.insert(group.id);
                }
            }
        }
        Require(
            views.size() == 3 && view_ids.size() == views.size() &&
                found_new_view && view_two_group_count == 4 &&
                ordinary_group_ids.size() == 4,
            "a new view/group and an existing-view group addition must not reuse a global group identity");
    }

    SeedMultiProcessCache(path);
    RunConcurrentCatalogChildren(path, "delete_view_1", "rename_view_1_local");
    {
        const auto loaded = specforge::LoadCatalogUserStateCache(path);
        const auto& views = loaded.cache.catalogs.at("specforge.public").grouping_views;
        Require(
            std::none_of(
                views.begin(),
                views.end(),
                [](const auto& view) { return view.id == "view-1"; }) &&
                std::any_of(
                    views.begin(),
                    views.end(),
                    [](const auto& view) {
                        return view.id == "view-2" &&
                               view.name == "Grouping 2";
                    }),
            "same-entity deletion must win over a stale edit without changing another view");
    }

    SeedMultiProcessCache(path);
    RunConcurrentCatalogChildren(path, "add_group_view_2", "add_group_view_2");
    {
        const auto loaded = specforge::LoadCatalogUserStateCache(path);
        const auto& views = loaded.cache.catalogs.at("specforge.public").grouping_views;
        const auto view = std::find_if(
            views.begin(),
            views.end(),
            [](const auto& candidate) { return candidate.id == "view-2"; });
        std::unordered_set<std::string> group_ids;
        if (view != views.end()) {
            for (const auto& group : view->groups) {
                group_ids.insert(group.id);
            }
        }
        Require(
            view != views.end() && view->groups.size() == 5 &&
                group_ids.size() == view->groups.size(),
            "concurrent group additions with one requested id must retain unique identities");
    }

    SeedMultiProcessCache(path);
    RunConcurrentCatalogChildren(path, "delete_group_1_view_2", "rename_view_2");
    {
        const auto loaded = specforge::LoadCatalogUserStateCache(path);
        const auto& views = loaded.cache.catalogs.at("specforge.public").grouping_views;
        const auto view = std::find_if(
            views.begin(),
            views.end(),
            [](const auto& candidate) { return candidate.id == "view-2"; });
        Require(view != views.end(), "group deletion should retain view-2");
        Require(
            view->name == "Remote view 2" &&
                std::none_of(
                    view->groups.begin(),
                    view->groups.end(),
                    [](const auto& group) { return group.id == "group-1"; }) &&
                std::any_of(
                    view->groups.begin(),
                    view->groups.end(),
                    [](const auto& group) { return group.id == "group-2"; }),
            "group deletion must win for that group while an unrelated view edit survives");
    }

    SeedMultiProcessCacheWithReferences(path);
    RunConcurrentCatalogChildren(
        path,
        "remove_reference_group_1",
        "copy_reference_group_1_to_group_2");
    {
        const auto loaded = specforge::LoadCatalogUserStateCache(path);
        const auto& views = loaded.cache.catalogs.at("specforge.public").grouping_views;
        const auto view = std::find_if(
            views.begin(),
            views.end(),
            [](const auto& candidate) { return candidate.id == "view-2"; });
        Require(
            view != views.end(),
            "reference reconciliation should retain view-2");
        const auto group_one = std::find_if(
            view->groups.begin(),
            view->groups.end(),
            [](const auto& group) { return group.id == "group-1"; });
        const auto group_two = std::find_if(
            view->groups.begin(),
            view->groups.end(),
            [](const auto& group) { return group.id == "group-2"; });
        const auto has_alpha = [](const auto& group) {
            return std::any_of(
                group.marker_references.begin(),
                group.marker_references.end(),
                [](const auto& reference) {
                    return reference.marker_id == "h_alpha";
                });
        };
        Require(
            group_one != view->groups.end() &&
                group_two != view->groups.end() && !has_alpha(*group_one) &&
                has_alpha(*group_two),
            "concurrent reference removal and addition must preserve task-level ownership");
    }

    RunDeterministicOrderingRace(path);

    SeedMultiProcessCache(path);
    RunConcurrentCatalogChildren(path, "select_view_1", "select_view_2");
    {
        const auto loaded = specforge::LoadCatalogUserStateCache(path);
        const std::string& active =
            loaded.cache.catalogs.at("specforge.public").active_view_id;
        Require(
            active == "view-1" || active == "view-2",
            "a selection conflict must resolve to one valid explicit task selection");
    }

    SeedMultiProcessCache(path);
    RunConcurrentCatalogChildren(path, "select_view_1", "delete_view_1");
    {
        const auto loaded = specforge::LoadCatalogUserStateCache(path);
        const std::string& active =
            loaded.cache.catalogs.at("specforge.public").active_view_id;
        Require(
            active != "view-1" &&
                (active == specforge::CatalogGroupingViewId() || active == "view-2"),
            "selection of a concurrently deleted view must fall back to a surviving view");
    }

    RunDeleteActiveViewWithStaleSelectionRace(path);

    SeedMultiProcessCache(path);
    const std::filesystem::path interruption_root = root / "interruption";
    std::filesystem::create_directories(interruption_root);
    ChildProcess interrupted = StartCatalogChild(
        "hold_after_mutation",
        path,
        interruption_root / "ready",
        interruption_root / "go",
        interruption_root / "done");
    Require(
        WaitForMarkerFile(interruption_root / "ready", 10s),
        "interrupted child should publish its startup snapshot");
    WriteMarkerFile(interruption_root / "go");
    Require(
        WaitForMarkerFile(interruption_root / "done", 10s),
        "interrupted child should publish its pre-flush mutation");
    Require(
        TerminateProcess(interrupted.process, 9) != FALSE &&
            WaitForSingleObject(interrupted.process, 5'000) == WAIT_OBJECT_0,
        "interrupted child should terminate within the bounded test cleanup");
    specforge::SpectralLinesPanelController after_interrupt(
        GroupedCatalog(),
        specforge::PublicSpectralLineCatalogIdentity(),
        path);
    RequireApplied(
        after_interrupt.Submit(
            specforge::CatalogUserStateIntent::SetMarkerVisibility(
                "h_beta",
                false)),
        "a fresh controller should still be able to write after an interrupted peer");
    Require(
        after_interrupt.Flush(),
        "an interrupted stale controller must not block the next atomic write");
    const auto after_interrupt_cache =
        specforge::LoadCatalogUserStateCache(path);
    Require(
        after_interrupt_cache.warning.empty() &&
            !specforge::IsMarkerVisible(
                after_interrupt_cache.cache.catalogs.at("specforge.public"),
                "h_beta"),
        "an interrupted peer must leave a valid cache for the next instance");

    SeedMultiProcessCache(path);
    const std::filesystem::path replace_interruption_root =
        root / "atomic-replace-interruption";
    std::filesystem::create_directories(replace_interruption_root);
    ChildProcess interrupted_replace = StartCatalogChild(
        "interrupt_during_atomic_replace",
        path,
        replace_interruption_root / "ready",
        replace_interruption_root / "go",
        replace_interruption_root / "done");
    Require(
        WaitForMarkerFile(replace_interruption_root / "ready", 10s),
        "atomic interruption child should publish its startup snapshot");
    WriteMarkerFile(replace_interruption_root / "go");
    Require(
        WaitForMarkerFile(replace_interruption_root / "done", 10s),
        "atomic interruption child should publish its pre-replace checkpoint");
    Require(
        ReadFile(replace_interruption_root / "done").find("before-replace") !=
            std::string::npos,
        "atomic interruption evidence should identify the pre-replace boundary");
    Require(
        TerminateProcess(interrupted_replace.process, 11) != FALSE &&
            WaitForSingleObject(interrupted_replace.process, 5'000) == WAIT_OBJECT_0,
        "atomic interruption child should terminate while waiting before replacement");

    const auto after_atomic_interrupt =
        specforge::LoadCatalogUserStateCache(path);
    Require(
        after_atomic_interrupt.issue_kind ==
                specforge::CatalogUserStateCacheLoadIssueKind::None &&
            specforge::IsMarkerVisible(
                after_atomic_interrupt.cache.catalogs.at("specforge.public"),
                "h_alpha"),
        "an interrupted atomic writer must leave the previous durable cache valid");
    specforge::SpectralLinesPanelController after_atomic_replace(
        GroupedCatalog(),
        specforge::PublicSpectralLineCatalogIdentity(),
        path);
    RequireApplied(
        after_atomic_replace.Submit(
            specforge::CatalogUserStateIntent::SetMarkerVisibility(
                "h_beta",
                false)),
        "a fresh controller should recover after atomic replacement interruption");
    Require(
        after_atomic_replace.Flush(),
        "the next controller should atomically replace the recovered cache");
    const auto recovered_after_atomic_interrupt =
        specforge::LoadCatalogUserStateCache(path);
    Require(
        recovered_after_atomic_interrupt.issue_kind ==
                specforge::CatalogUserStateCacheLoadIssueKind::None &&
            !specforge::IsMarkerVisible(
                recovered_after_atomic_interrupt.cache.catalogs.at("specforge.public"),
                "h_beta"),
        "atomic replacement recovery should preserve a valid target and new task delta");

    std::filesystem::remove_all(root, error);
}

}  // namespace

int main(int argc, char* argv[])
{
    if (argc > 1 && std::string_view(argv[1]) == "--catalog-reconcile-child") {
        return RunCatalogReconciliationChild(argc, argv);
    }
    try {
        TestCatalogCommitLeasePathPreservesNativePath();
        TestForeignIdentitiesAreRejectedWithoutPersistence();
        TestPlotViewProjectsOnlyPlotOverlayState();
        TestCacheLoadUsesDomainCanonicalizationAndPreservesUnresolvedMarkers();
        TestPersistentIntentUsesDomainCanonicalizationForSelection();
        TestGeneratedNamesPreserveStoredValuesAndOrigins();
        TestExplicitStoredValueRenamesClearGeneratedMetadata();
        TestLegacyExactShapeNamesRemainUserOwnedAcrossRestart();
        TestMalformedLegacyCacheBodyIsNotSilentlyOverwritten();
        TestModificationSelectionAndPersistenceRoundTrip();
        TestPersistenceViewReportsLoadWarningRetryAndRecovery();
        TestStartupCanonicalizationIsNotAnExplicitReconciliationDelta();
        TestLegacySchemaFirstExplicitWriteMigratesBeforeReconciliation();
        TestGeneratedCatalogIdsRemainMonotonicAcrossDeletionAndRestart();
        TestCorruptLatestCatalogStateFailsClosed();
        TestSemanticallyInvalidLatestCatalogStateFailsClosed();
        TestEmptyLatestCatalogIdentitiesFailClosed();
        TestDuplicateLatestJsonKeysFailClosed();
        TestCurrentSchemaSemanticCorruptionFailsClosedWithoutMaintenanceRewrite();
        TestLegacyIdentityCorruptionFailsClosedBeforeMigration();
        TestCurrentSchemaAllocatorHistoryCorruptionFailsClosed();
        TestLegacyReferenceAndUnassignedCorruptionFailsClosedBeforeMigration();
        TestSameTaskNewGroupReorderPreservesFinalOrder();
        TestLegacyMultiCatalogMigrationFailsClosed();
        TestConcurrentCatalogStateReconciliation();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
