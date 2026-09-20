#include "platform/win32_external_open_router.h"
#include "app/project_identity.h"
#include "platform/win32_process_launcher.h"
#include "domain/source_path_identity.h"
#include "domain/stable_sha256.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace spectiary;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct State {
    volatile LONG count;
    volatile LONG mode;
    volatile LONG marked;
    LONG folder;
    DWORD pid;
    LONG restored;
    wchar_t path[32768];
};

int Child(const std::filesystem::path& root, const std::wstring& name)
{
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    auto* state = static_cast<State*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(State)));
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L".ready").c_str());
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, (name + L".stop").c_str());
    HANDLE mark = OpenEventW(SYNCHRONIZE, FALSE, (name + L".mark").c_str());
    if (!state || !ready || !stop || !mark) return 2;
    const HWND window = state->mode == 7 ? CreateWindowExW(0, L"STATIC", L"Spectiary routing test",
        WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    if (window) ShowWindow(window, SW_SHOWMINNOACTIVE);
    state->pid = GetCurrentProcessId();
    Win32ExternalOpenRouter router;
    Require(router.Start(root, [state, window, &router]() {
        if (state->mode == 1) Sleep(250);
        if (state->mode == 8) ExitProcess(0);
        if (state->mode == 9) router.Stop();
        if (state->mode == 7) {
            const bool activated = ActivateExternalOpenWindow(window);
            state->restored = !IsIconic(window);
            return activated;
        }
        return state->mode != 2;
    }, [state]() { if (state->mode == 6) Sleep(250); }), "child endpoint registration failed");
    if (state->mode == 3) {
        StableSha256 hash; hash.Append(SourcePathIdentityKey(root));
        const auto digest = hash.FinishHex();
        const auto class_name = project_identity::kExternalOpenNamespacePrefix + std::wstring(digest.begin(), digest.end());
        HWND hwnd = nullptr;
        while ((hwnd = FindWindowExW(HWND_MESSAGE, hwnd, class_name.c_str(), nullptr)) != nullptr) {
            DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId()) {
                SetPropW(hwnd, L"ExternalOpen.Protocol", reinterpret_cast<HANDLE>(2)); break;
            }
        }
    }
    if (state->mode == 4) router.Stop();
    SetEvent(ready);
    const HANDLE events[] = {stop, mark};
    bool done = false;
    while (!done) {
        const DWORD result = MsgWaitForMultipleObjects(2, events, FALSE, 100, QS_ALLINPUT);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_OBJECT_0 + 1) {
            router.MarkUsed();
            InterlockedIncrement(&state->marked);
        }
        if (state->mode == 5) { Sleep(250); continue; }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { done = true; break; }
            DispatchMessageW(&message);
        }
        for (const auto& request : router.TakeRequests()) {
            const auto path = request.path.wstring();
            std::copy(path.begin(), path.end(), state->path);
            state->path[path.size()] = L'\0';
            state->folder = request.as_folder;
            InterlockedIncrement(&state->count);
        }
    }
    router.Stop();
    if (window) DestroyWindow(window);
    UnmapViewOfFile(state);
    CloseHandle(mapping); CloseHandle(ready); CloseHandle(stop); CloseHandle(mark);
    return 0;
}

struct Instance {
    HANDLE mapping = nullptr, ready = nullptr, stop = nullptr, mark = nullptr, process = nullptr;
    State* state = nullptr;
    Instance(const std::filesystem::path& root, int mode = 0) {
        static unsigned sequence = 0;
        const auto name = L"Local\\Spectiary.Router.Test." + std::to_wstring(GetCurrentProcessId()) +
            L"." + std::to_wstring(++sequence);
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(State), name.c_str());
        state = static_cast<State*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(State)));
        Require(state != nullptr, "test mapping failed");
        state->mode = mode;
        ready = CreateEventW(nullptr, TRUE, FALSE, (name + L".ready").c_str());
        stop = CreateEventW(nullptr, TRUE, FALSE, (name + L".stop").c_str());
        mark = CreateEventW(nullptr, FALSE, FALSE, (name + L".mark").c_str());
        const auto executable = ResolveCurrentExecutablePath().path;
        auto command = QuoteWindowsCommandLineArgument(executable.wstring()) + L" --child " +
            QuoteWindowsCommandLineArgument(root.wstring()) + L" " + QuoteWindowsCommandLineArgument(name);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) != FALSE, "child launch failed");
        process = info.hProcess; CloseHandle(info.hThread);
        Require(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0, "child endpoint not ready");
    }
    ~Instance() {
        if (stop) SetEvent(stop);
        if (process) {
            if (WaitForSingleObject(process, 5000) == WAIT_TIMEOUT) TerminateProcess(process, 99);
            CloseHandle(process);
        }
        if (state) UnmapViewOfFile(state);
        if (mapping) CloseHandle(mapping);
        if (ready) CloseHandle(ready);
        if (stop) CloseHandle(stop);
        if (mark) CloseHandle(mark);
    }
    void WaitCount(LONG count) {
        const auto deadline = GetTickCount64() + 3000;
        while (state->count < count && GetTickCount64() < deadline) Sleep(5);
        Require(state->count == count, "accepted request count mismatch");
    }
    void Mark() {
        const auto previous = state->marked;
        SetEvent(mark);
        const auto deadline = GetTickCount64() + 3000;
        while (state->marked == previous && GetTickCount64() < deadline) Sleep(5);
        Require(state->marked != previous, "recency update did not complete");
    }
};

void Tests()
{
    const auto root = std::filesystem::temp_directory_path() /
        (L"spectiary-router-" + std::to_wstring(GetCurrentProcessId()));
    const ExternalOpenRequest request{root / L"数据 folder" / L"selected 光谱.fits", true};
    Require(!Win32ExternalOpenRouter::HasCompatibleInstance(root), "first source-free launch has no peer");
    {
        Win32ExternalOpenRouter self;
        Require(self.Start(root, [] { return true; }, {}), "self endpoint registration");
        Require(!Win32ExternalOpenRouter::HasCompatibleInstance(root), "discovery excludes this process");
    }
    Require(!Win32ExternalOpenRouter::Forward(root, request), "absent receiver must fall back");
    {
        Instance first(root);
        Require(Win32ExternalOpenRouter::HasCompatibleInstance(root), "subsequent launch discovers ordinary GUI peer");
        Require(!Win32ExternalOpenRouter::HasCompatibleInstance(root / L"other"), "clean-start discovery is namespace-local");
        Require(first.state->count == 0, "discovery must not send a source request");
        Require(Win32ExternalOpenRouter::Forward(root, request), "normal request should transfer");
        first.WaitCount(1);
        Require(first.state->path == request.path.wstring() && first.state->folder,
            "Unicode path and folder semantics must transfer intact");
        Require(!Win32ExternalOpenRouter::Forward(root / L"other", request), "namespace must isolate instances");
        Sleep(20);
        Instance second(root);
        Require(Win32ExternalOpenRouter::Forward(root, request), "newer instance should receive");
        second.WaitCount(1); Require(first.state->count == 1, "older instance must not receive");
        Sleep(20); first.Mark();
        Require(Win32ExternalOpenRouter::Forward(root, {root, false}), "recently used instance should receive");
        first.WaitCount(2);
        Require(!first.state->folder && first.state->path == root.wstring(), "directory open must remain direct");
        Require(!Win32ExternalOpenRouter::Forward(root, {L"relative.fits", false}), "relative wire paths are rejected");
        bool accepted_a = false, accepted_b = false;
        std::thread sender_a([&] { accepted_a = Win32ExternalOpenRouter::Forward(root, request); });
        std::thread sender_b([&] { accepted_b = Win32ExternalOpenRouter::Forward(root, request); });
        sender_a.join(); sender_b.join();
        Require(accepted_a && accepted_b, "concurrent invocations must retain independent request ownership");
        first.WaitCount(4);
    }
    Require(!Win32ExternalOpenRouter::Forward(root, request), "exited endpoint must disappear");
    Require(!Win32ExternalOpenRouter::HasCompatibleInstance(root), "clean-start classification is transient after process exit");
    for (const int mode : {1, 2, 3, 4, 5, 8, 9}) {
        Instance target(root, mode);
        Require(Win32ExternalOpenRouter::HasCompatibleInstance(root) == (mode != 3 && mode != 4),
            "discovery requires compatible live registration, without waiting for receiver responsiveness");
        const auto start = GetTickCount64();
        Require(!Win32ExternalOpenRouter::Forward(root, request, 50), "unavailable receiver must fall back");
        Require(GetTickCount64() - start < 1000, "failure must have bounded latency");
        Sleep(350);
        Require(target.state->count == 0, "timed-out/rejected request must not be applied later");
    }
    {
        Instance target(root, 6);
        Require(Win32ExternalOpenRouter::Forward(root, request, 100),
            "acceptance before a lost reply must suppress duplicate fallback");
        target.WaitCount(1);
    }
    {
        Instance compatible(root);
        Sleep(20); Instance incompatible(root, 3);
        Require(Win32ExternalOpenRouter::Forward(root, request), "new incompatible instance must not hide compatible target");
        compatible.WaitCount(1);
        Require(incompatible.state->count == 0, "incompatible endpoint must not receive");
    }
    {
        const HWND previous = GetForegroundWindow();
        Instance target(root, 7);
        Require(Win32ExternalOpenRouter::Forward(root, request), "native foreground activation should succeed");
        target.WaitCount(1);
        DWORD foreground_pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
        Require(target.state->restored && foreground_pid == target.state->pid,
            "native routing must restore a minimized window and activate its process");
        if (previous) SetForegroundWindow(previous);
    }
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc == 4 && std::wstring_view(argv[1]) == L"--child") return Child(argv[2], argv[3]);
        Tests();
        std::cout << "External-open routing tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
