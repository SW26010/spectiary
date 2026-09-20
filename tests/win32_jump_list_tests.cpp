#include "platform/win32_jump_list.h"
#include "platform/win32_process_launcher.h"
#include "platform/win32_external_open_router.h"
#include "automation/automation_startup.h"
#include "ui/source_collection_session_state_cache_io.h"
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>
#include <algorithm>

namespace {
using namespace spectiary;
using Microsoft::WRL::ComPtr;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

std::wstring Arguments(IShellLinkW& link)
{
    std::wstring arguments(32768, L'\0');
    Require(SUCCEEDED(link.GetArguments(arguments.data(), static_cast<int>(arguments.size()))), "read native shell-link arguments");
    arguments.resize(wcslen(arguments.c_str()));
    return arguments;
}

// Exercise the publisher's actual COM protocol without replacing the desktop's
// Jump List. Links and object collections themselves are real Windows objects.
struct DestinationList : ICustomDestinationList {
    UINT slots = 10;
    HRESULT append_result = S_OK;
    HRESULT commit_result = S_OK;
    bool committed = false;
    bool aborted = false;
    bool explicit_id_requested = false;
    std::function<void()> on_begin;
    std::wstring category;
    ComPtr<IObjectCollection> removed;
    std::vector<ComPtr<IShellLinkW>> links;
    DestinationList() { Require(SUCCEEDED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&removed))), "create removed collection"); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(ICustomDestinationList)) return E_NOINTERFACE;
        *output = static_cast<ICustomDestinationList*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetAppID(LPCWSTR) override { explicit_id_requested = true; return E_UNEXPECTED; }
    HRESULT STDMETHODCALLTYPE BeginList(UINT* count, REFIID iid, void** output) override {
        *count = slots; committed = false; aborted = false; links.clear(); category.clear();
        if (on_begin) on_begin();
        return removed->QueryInterface(iid, output);
    }
    HRESULT STDMETHODCALLTYPE AppendCategory(LPCWSTR name, IObjectArray* items) override {
        category = name;
        if (FAILED(append_result)) return append_result;
        UINT count = 0; items->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IShellLinkW> link;
            Require(SUCCEEDED(items->GetAt(i, IID_PPV_ARGS(&link))), "published entry must be a shell link");
            links.push_back(std::move(link));
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE AppendKnownCategory(KNOWNDESTCATEGORY) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddUserTasks(IObjectArray*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CommitList() override {
        if (FAILED(commit_result)) return commit_result;
        committed = true; removed->Clear(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRemovedDestinations(REFIID iid, void** output) override { return removed->QueryInterface(iid, output); }
    HRESULT STDMETHODCALLTYPE DeleteList(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AbortList() override { aborted = true; return S_OK; }
};

void TestPublication(const std::filesystem::path& root)
{
    const auto executable = ResolveCurrentExecutablePath().path;
    const auto file = root / L"光谱 source.csv";
    const auto folder = root / L"folder 目录";
    std::ofstream(file) << "wav,flux\n5000,1\n5001,2\n";
    std::filesystem::create_directories(folder);
    const std::vector<JumpListSource> sources{
        {{}, L"memory"}, {L"relative.csv", L"relative"}, {root / "missing.csv", L"stale"},
        {file, L"光谱"}, {folder, L"目录"}, {file, L"duplicate"}};
    const auto eligible = EligibleJumpListSources(sources);
    Require(eligible.size() == 2 && eligible[0].path == file && eligible[1].path == folder,
        "only durable existing filesystem entries, in Files order, without duplicates");
    DestinationList list;
    const auto preferences = root / "exclusions.txt";
    const auto publish = [&] { return PublishSourceJumpList(executable, sources, L"数据源", preferences, list); };
    Require(SUCCEEDED(publish()) && list.committed && list.links.size() == 2 && !list.explicit_id_requested && list.category == L"数据源",
        "publish custom category without overriding Windows shell identity");
    for (std::size_t i = 0; i < list.links.size(); ++i) {
        auto& link = *list.links[i].Get();
        std::wstring target(32768, L'\0');
        Require(SUCCEEDED(link.GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)), "link target path");
        target.resize(wcslen(target.c_str()));
        Require(target == executable.wstring(), "destination must execute current binary");
        const auto command = QuoteWindowsCommandLineArgument(executable.wstring()) + L" " + Arguments(link);
        int argc = 0;
        auto argv = CommandLineToArgvW(command.c_str(), &argc);
        Require(argv != nullptr, "parse actual shell-link launch command");
        std::vector<std::wstring> arguments(argv, argv + argc);
        LocalFree(argv);
        const auto parsed = ParseSpectiaryCommandLine(arguments);
        Require(parsed.error_message.empty() && parsed.force_new_instance && !parsed.automation &&
            parsed.initial_source == eligible[i].path, "link must request an ordinary GUI with explicit new-instance routing and exact Unicode source");
        ComPtr<IPropertyStore> properties;
        Require(SUCCEEDED(list.links[i].As(&properties)), "link properties");
        PROPVARIANT value{};
        Require(SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_EMPTY,
            "source link must not override Windows shell identity");
        PropVariantClear(&value);
        Require(SUCCEEDED(properties->GetValue(PKEY_Title, &value)) && value.vt == VT_LPWSTR && eligible[i].title == value.pwszVal,
            "native shell title must retain Files display name");
        PropVariantClear(&value);
    }
    list.removed->AddObject(list.links[0].Get());
    Require(SUCCEEDED(publish()) && list.links.size() == 1 && Arguments(*list.links[0].Get()) == NewInstanceSourceArguments(folder),
        "removed destination must be omitted");
    Require(SUCCEEDED(publish()) && list.links.size() == 1,
        "removal survives Windows clearing removed destinations after CommitList");
    DestinationList restarted;
    Require(SUCCEEDED(PublishSourceJumpList(executable, sources, L"Sources", preferences, restarted)) && restarted.links.size() == 1,
        "user removal persists across publisher instances");
    DestinationList other_namespace;
    Require(SUCCEEDED(PublishSourceJumpList(executable, sources, L"Sources",
        root / "other-exclusions.txt", other_namespace)) && other_namespace.links.size() == 2, "removal preference belongs to its configuration file");
    list.slots = 0;
    Require(SUCCEEDED(publish()) && list.links.empty() && list.committed, "zero Windows slots commits an empty projection");
    other_namespace.slots = 1;
    Require(SUCCEEDED(PublishSourceJumpList(executable, sources, L"Sources",
        root / "other-exclusions.txt", other_namespace)) && other_namespace.links.size() == 1 &&
        Arguments(*other_namespace.links[0].Get()) == NewInstanceSourceArguments(file), "Windows cap preserves Files ordering");
    list.slots = 10;
    list.append_result = E_ACCESSDENIED;
    Require(SUCCEEDED(publish()) && list.committed, "privacy-disabled category is nonfatal and still commits");
    list.append_result = E_FAIL;
    Require(FAILED(publish()) && list.aborted && !list.committed, "failed update aborts the shell transaction");
    list.append_result = S_OK;
    std::filesystem::remove_all(folder);
    Require(SUCCEEDED(publish()) && list.committed && list.links.empty(), "deleted sources disappear on refresh");
    std::ofstream(preferences) << "corrupt preferences";
    list.committed = false;
    Require(FAILED(publish()) && !list.committed, "unreadable removal preferences cannot resurrect hidden destinations");
}

void TestShutdownDoesNotWaitForPublication(const std::filesystem::path& root)
{
    struct BlockedPublication {
        std::promise<void> entered;
        std::promise<void> release;
        std::atomic<unsigned> calls = 0;
        std::atomic<bool> stopped = false;
        std::atomic<bool> snapshot_alive = false;
    };
    auto blocked = std::make_shared<BlockedPublication>();
    auto entered = blocked->entered.get_future();
    const auto release = blocked->release.get_future().share();
    // Destruction of this capture proves the worker has released its state,
    // rather than merely returning from the injected I/O operation.
    struct WorkerLifetime {
        std::promise<void> finished;
        ~WorkerLifetime() { finished.set_value(); }
    };
    auto lifetime = std::make_shared<WorkerLifetime>();
    auto finished = lifetime->finished.get_future();
    auto list = std::make_unique<Win32JumpList>(root,
        [blocked, release, lifetime = std::move(lifetime)](std::span<const JumpListSource> sources,
            const std::wstring& category, std::stop_token stop) {
            if (++blocked->calls == 1) blocked->entered.set_value();
            release.wait(); // Deterministic stand-in for a blocked network/COM call.
            blocked->stopped = stop.stop_requested();
            blocked->snapshot_alive = sources.size() == 1 && sources[0].title == L"active" && category == L"Sources";
        });
    list->Refresh({{root / "active.csv", L"active"}}, L"Sources");
    const bool started = entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!started) {
        blocked->release.set_value();
        Require(false, "publication worker must start");
    }
    list->Refresh({{root / "queued.csv", L"queued"}}, L"Pending");
    auto destroyed = std::async(std::launch::async, [list = std::move(list)]() mutable { list.reset(); });
    const bool prompt = destroyed.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    // Always unblock even on regression so the test reports instead of hanging.
    blocked->release.set_value();
    destroyed.get();
    Require(finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "stopped worker must release owned state");
    Require(prompt, "destruction must not join blocked publication I/O");
    Require(blocked->calls == 1, "shutdown must discard the pending publication");
    Require(blocked->stopped && blocked->snapshot_alive, "in-flight publication owns its snapshot and observes shutdown");
}

void TestPublicationCancellation(const std::filesystem::path& root)
{
    DestinationList list;
    std::stop_source stop;
    list.on_begin = [&] { stop.request_stop(); };
    const auto result = PublishSourceJumpList(ResolveCurrentExecutablePath().path, {}, L"Sources",
        root / "cancelled-exclusions.txt", list, stop.get_token());
    Require(result == HRESULT_FROM_WIN32(ERROR_CANCELLED) && list.aborted && !list.committed,
        "shutdown after BeginList must abort, never commit an empty replacement");
}

void TestNativeShell(const std::filesystem::path& root)
{
    const auto executable = ResolveCurrentExecutablePath().path;
    PWSTR process_id = nullptr;
    const auto explicit_id = GetCurrentProcessExplicitAppUserModelID(&process_id);
    CoTaskMemFree(process_id);
    Require(FAILED(explicit_id), "native test must exercise implicit process identity");
    ComPtr<ICustomDestinationList> destinations;
    Require(SUCCEEDED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&destinations))),
        "create native Windows destination list");
    struct ListCleanup { ICustomDestinationList* list; ~ListCleanup() { list->DeleteList(nullptr); } } cleanup{destinations.Get()};
    const auto file = root / "native.csv";
    std::ofstream(file) << "wav,flux\n5000,1\n";
    const std::vector<JumpListSource> sources{{file, L"Native source"}};
    const auto hr = PublishSourceJumpList(executable, sources, L"Sources", root / "native-exclusions.txt", *destinations.Get());
    if (FAILED(hr)) std::cerr << "Native publication HRESULT: " << std::hex << static_cast<unsigned long>(hr) << '\n';
    Require(SUCCEEDED(hr), "publish native shell list using test executable implicit identity");
}

// Optional real-GUI smoke: use a disposable Portable deployment so ordinary
// (non-automation) startup and routing execute with disposable source/configuration state.
// Windows owns the copied executable's implicit shell identity and list lifetime.
void TestGuiRouting(const std::filesystem::path& root)
{
    const auto build = ResolveCurrentExecutablePath().path.parent_path();
    const auto executable = root / "Spectiary.exe";
    std::filesystem::copy_file(build / "Spectiary.exe", executable);
    for (const auto& entry : std::filesystem::directory_iterator(build)) {
        if (entry.path().extension() == ".dll") std::filesystem::copy_file(entry.path(), root / entry.path().filename());
    }
    auto metadata = nlohmann::json::parse(std::ifstream(build / "spectiary_metadata.json"));
    metadata["deployment"] = {{"distribution", "portable"}, {"storage_profile", "portable"}};
    std::ofstream(root / "spectiary_metadata.json") << metadata.dump();
    std::filesystem::create_directories(root / "config");
    std::filesystem::copy_file(build / "config" / "spectral_lines.public.json", root / "config" / "spectral_lines.public.json");
    std::ofstream(root / "config" / "external-source-settings.json") << R"({"format_kind":"spectiary.external_source.settings","schema_version":1,"open_external_source_as_folder":false,"instance_policy":"recent_instance"})";
    const auto old_source = root / L"old.csv";
    const auto routed_source = root / L"routed.csv";
    const auto selected_source = root / L"selected 光谱.csv";
    for (const auto& source : {old_source, routed_source, selected_source}) std::ofstream(source) << "wav,flux\n5000,1\n5001,2\n";
    RuntimePathInputs runtime_inputs;
    runtime_inputs.executable_path = executable;
    const auto startup_context = PrepareSpectiaryStartup(std::move(runtime_inputs));
    const auto& runtime_paths = startup_context.runtime_paths();
    Require(SaveSourceCollectionSessionStateCache(runtime_paths, runtime_paths.source_session_state_path,
        {.sources = {{.path = old_source}, {.path = selected_source}}, .active_source_index = 0}),
        "seed a two-source Portable roster before actual GUI startup");
    const auto title_for = [](DWORD pid) {
        struct Observation { DWORD pid; HWND window = nullptr; std::wstring title; } observation{pid};
        EnumWindows([](HWND window, LPARAM data) -> BOOL {
            auto& o = *reinterpret_cast<Observation*>(data);
            DWORD process = 0; GetWindowThreadProcessId(window, &process);
            if (process != o.pid || GetWindow(window, GW_OWNER)) return TRUE;
            wchar_t title[1024]{};
            GetWindowTextW(window, title, 1024);
            if (std::wstring_view(title).starts_with(L"Spectiary ")) { o.window = window; o.title = title; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&observation));
        return std::pair{observation.window, observation.title};
    };
    std::vector<HANDLE> processes;
    struct Cleanup {
        std::vector<HANDLE>& processes;
        const decltype(title_for)& observe;
        ~Cleanup() {
            for (auto process : processes) {
                const auto [window, title] = observe(GetProcessId(process));
                if (window) PostMessageW(window, WM_CLOSE, 0, 0);
            }
            for (auto process : processes) {
                if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) { TerminateProcess(process, 1); WaitForSingleObject(process, 1000); }
                CloseHandle(process);
            }
        }
    } cleanup{processes, title_for};
    const auto start = [&](const std::wstring& arguments) {
        auto command = QuoteWindowsCommandLineArgument(executable.wstring()) + L" " + arguments;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNOACTIVATE;
        PROCESS_INFORMATION process{};
        Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, root.c_str(), &startup, &process),
            "start isolated ordinary GUI");
        CloseHandle(process.hThread); processes.push_back(process.hProcess); return process.hProcess;
    };
    const auto wait_title = [&](HANDLE process, const std::wstring& expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) return false;
            if (title_for(GetProcessId(process)).second.find(expected) != std::wstring::npos) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };
    const auto first = start(QuoteWindowsCommandLineArgument(old_source.wstring()));
    Require(wait_title(first, L"old.csv | 1/1"), "first ordinary GUI must display initial source");
    // A deterministic receiver avoids overriding Windows foreground restrictions
    // on unattended desktops. Its wire protocol is the production router.
    std::atomic<unsigned> forwarded = 0;
    std::promise<bool> ready;
    auto ready_result = ready.get_future();
    std::jthread receiver([&](std::stop_token stop) {
        Win32ExternalOpenRouter router;
        ready.set_value(router.Start(root / "config", [&] { ++forwarded; return true; }, {}));
        while (!stop.stop_requested()) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            (void)router.TakeRequests();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    Require(ready_result.get(), "register deterministic recent-instance receiver");
    const auto ordinary = start(QuoteWindowsCommandLineArgument(routed_source.wstring()));
    Require(WaitForSingleObject(ordinary, 15000) == WAIT_OBJECT_0 && forwarded == 1,
        "control invocation must actually forward under recent-instance policy");
    const auto activate_link = [&](const std::filesystem::path& source, const wchar_t* filename) {
        ComPtr<IShellLinkW> link;
        Require(SUCCEEDED(CreateSourceJumpListLink(executable, {source, source.filename().wstring()}, &link)), "create production source link");
        ComPtr<IPersistFile> persistence;
        Require(SUCCEEDED(link.As(&persistence)), "persist source link");
        const auto shortcut = root / filename;
        Require(SUCCEEDED(persistence->Save(shortcut.c_str(), TRUE)), "save production source link");
        SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info); info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.lpFile = shortcut.c_str(); info.nShow = SW_SHOWNOACTIVATE;
        Require(ShellExecuteExW(&info) && info.hProcess, "Windows shell activates production source destination");
        processes.push_back(info.hProcess); return info.hProcess;
    };
    const auto selected = activate_link(selected_source, L"selected.lnk");
    Require(GetProcessId(selected) != GetProcessId(first) && wait_title(selected, L"selected 光谱.csv | 1/1"),
        "Jump List link must create new ordinary GUI with selected source despite recent-instance policy");
    Require(forwarded == 1 && title_for(GetProcessId(first)).second.find(L"old.csv | 1/1") != std::wstring::npos,
        "Jump List action must not forward or mutate prior GUI");
    const auto stale = activate_link(root / "deleted.csv", L"stale.lnk");
    Require(wait_title(stale, L"Spectiary "), "stale source still launches a separate ordinary GUI");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    Require(forwarded == 1 && WaitForSingleObject(stale, 0) == WAIT_TIMEOUT && title_for(GetProcessId(first)).second.find(L"old.csv | 1/1") != std::wstring::npos &&
        title_for(GetProcessId(selected)).second.find(L"selected 光谱.csv | 1/1") != std::wstring::npos,
        "stale destination must not route to or alter unrelated GUIs");
    PostMessageW(title_for(GetProcessId(selected)).first, WM_CLOSE, 0, 0);
    Require(WaitForSingleObject(selected, 5000) == WAIT_OBJECT_0, "selected-source GUI should close and flush its session");
    const auto retained = LoadSourceCollectionSessionStateCache(runtime_paths, runtime_paths.source_session_state_path).cache;
    Require(retained.sources.size() == 2 &&
        std::any_of(retained.sources.begin(), retained.sources.end(), [&](const auto& source) { return source.path == old_source; }) &&
        std::any_of(retained.sources.begin(), retained.sources.end(), [&](const auto& source) { return source.path == selected_source; }),
        "real Jump List child must persist both sources instead of collapsing the roster");

}
}

int main(int argc, char** argv)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    const auto root = std::filesystem::temp_directory_path() / (L"spectiary-jump-list-test-" + std::to_wstring(GetCurrentProcessId()));
    int exit_code = 0;
    try {
        std::filesystem::create_directories(root);
        if (argc == 2 && std::string_view(argv[1]) == "--native-shell") TestNativeShell(root);
        else if (argc == 2 && std::string_view(argv[1]) == "--gui-routing") TestGuiRouting(root);
        else {
            TestPublication(root);
            TestShutdownDoesNotWaitForPublication(root);
            TestPublicationCancellation(root);
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; exit_code = 1; }
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    CoUninitialize();
    return exit_code;
}
