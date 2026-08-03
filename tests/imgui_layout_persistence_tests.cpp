#include "app/imgui_layout_persistence.h"
#include "platform/atomic_file.h"

#include <Windows.h>
#include <imgui.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path TestRoot()
{
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("specforge-imgui-layout-tests-" +
            std::to_string(GetCurrentProcessId()) +
            "-" + std::to_string(timestamp));
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "layout target should be readable");
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

bool TryWriteTextFile(
    const std::filesystem::path& path,
    std::string_view contents)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.good()) {
        return false;
    }
    stream.write(
        contents.data(),
        static_cast<std::streamsize>(contents.size()));
    stream.close();
    return stream.good();
}

void WriteTextFile(
    const std::filesystem::path& path,
    std::string_view contents)
{
    Require(
        TryWriteTextFile(path, contents),
        "test marker should be writable");
}

bool FileExists(const std::filesystem::path& path)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    return exists && !error;
}

bool WaitForFile(
    const std::filesystem::path& path,
    std::chrono::milliseconds timeout)
{
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (FileExists(path)) {
            return true;
        }
        Sleep(5);
    }
    return FileExists(path);
}

std::filesystem::path TestExecutablePath()
{
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    Require(
        length > 0 && length < buffer.size(),
        "test executable path should be available");
    buffer.resize(length);
    return std::filesystem::path(std::move(buffer));
}

std::wstring QuoteCommandLineArgument(
    const std::filesystem::path& value)
{
    return L"\"" + value.wstring() + L"\"";
}

class ChildProcess {
public:
    explicit ChildProcess(HANDLE process) noexcept
        : process_(process)
    {
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    ChildProcess(ChildProcess&& other) noexcept
        : process_(other.process_)
    {
        other.process_ = nullptr;
    }

    ChildProcess& operator=(ChildProcess&& other) noexcept
    {
        if (this != &other) {
            Cleanup();
            process_ = other.process_;
            other.process_ = nullptr;
        }
        return *this;
    }

    ~ChildProcess()
    {
        Cleanup();
    }

    [[nodiscard]] DWORD Wait(DWORD timeout_ms) const noexcept
    {
        return WaitForSingleObject(process_, timeout_ms);
    }

    [[nodiscard]] DWORD ExitCode() const noexcept
    {
        DWORD exit_code = STILL_ACTIVE;
        (void)GetExitCodeProcess(process_, &exit_code);
        return exit_code;
    }

    void Terminate(DWORD exit_code) noexcept
    {
        if (process_ == nullptr) {
            return;
        }
        if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
            (void)TerminateProcess(process_, exit_code);
            (void)WaitForSingleObject(process_, 5000);
        }
    }

private:
    void Cleanup() noexcept
    {
        if (process_ == nullptr) {
            return;
        }
        Terminate(1);
        CloseHandle(process_);
        process_ = nullptr;
    }

    HANDLE process_ = nullptr;
};

ChildProcess StartChild(std::wstring command_line)
{
    std::vector<wchar_t> mutable_command(
        command_line.begin(),
        command_line.end());
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
        "child test process should start");
    CloseHandle(process.hThread);
    return ChildProcess(process.hProcess);
}

std::string ContentionSnapshot(int writer)
{
    return "[Window][process-writer-" + std::to_string(writer) + "]\n"
           "Pos=" + std::to_string(writer) + ",20\n"
           "Size=300,200\n"
           "Collapsed=0\n";
}

std::vector<std::filesystem::path> TemporarySiblings(
    const std::filesystem::path& target)
{
    std::vector<std::filesystem::path> temporary_paths;
    const std::wstring prefix = target.filename().wstring() + L".tmp.";
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        target.parent_path(),
        error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        const std::wstring name = iterator->path().filename().wstring();
        if (name.rfind(prefix, 0) == 0) {
            temporary_paths.push_back(iterator->path());
        }
        iterator.increment(error);
    }
    Require(
        !error,
        "temporary layout sibling enumeration should succeed");
    return temporary_paths;
}

void ConfigureImGuiForTest()
{
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1200.0f, 900.0f);
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Require(
        pixels != nullptr && width > 0 && height > 0,
        "ImGui test font atlas should build");
}

int RunContentionChild(int argc, wchar_t** argv)
{
    if (argc != 6) {
        return 2;
    }
    try {
        const std::filesystem::path target(argv[2]);
        const std::filesystem::path ready(argv[3]);
        const std::filesystem::path start(argv[4]);
        const int writer = std::stoi(argv[5]);
        if (!TryWriteTextFile(ready, "ready\n")) {
            return 3;
        }
        while (!FileExists(start)) {
            Sleep(1);
        }
        specforge::ImGuiLayoutPersistence persistence(target);
        std::string error;
        return persistence.SaveSnapshot(
                   ContentionSnapshot(writer),
                   &error)
                   ? 0
                   : 4;
    } catch (const std::exception& error) {
        std::cerr << "contention child failed: " << error.what() << '\n';
        return 5;
    }
}

int RunInterruptedWriteChild(int argc, wchar_t** argv)
{
    if (argc != 4) {
        return 2;
    }
    try {
        const std::filesystem::path target(argv[2]);
        const std::filesystem::path ready(argv[3]);
        specforge::AtomicFileWriteOptions options;
        options.open_mode = std::ios::binary | std::ios::trunc;
        options.target_description = "interrupted ImGui layout";
        std::string error;
        const bool completed = specforge::WriteFileAtomically(
            target,
            options,
            [&ready](std::ostream& stream, std::string& writer_error) {
                stream << "[Window][interrupted]\n"
                          "Pos=partial\n";
                stream.flush();
                if (!stream.good()) {
                    writer_error = "interrupted fixture could not flush";
                    return false;
                }
                if (!TryWriteTextFile(ready, "ready\n")) {
                    writer_error = "interrupted fixture could not signal readiness";
                    return false;
                }
                Sleep(INFINITE);
                return true;
            },
            &error);
        return completed ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << "interrupted child failed: " << error.what() << '\n';
        return 4;
    }
}

void TestContentionKeepsEveryCompletedSnapshotReadable(
    const std::filesystem::path& target)
{
    specforge::ImGuiLayoutPersistence persistence(target);
    const std::string seed =
        "[Window][seed]\n"
        "Pos=1,2\n"
        "Size=3,4\n"
        "Collapsed=0\n";
    std::string error;
    Require(
        persistence.SaveSnapshot(seed, &error),
        error.empty() ? "layout seed write failed" : error);

    constexpr int kWriterCount = 12;
    std::vector<std::string> snapshots;
    snapshots.reserve(kWriterCount);
    for (int writer = 0; writer < kWriterCount; ++writer) {
        snapshots.push_back(
            "[Window][writer-" + std::to_string(writer) + "]\n"
            "Pos=" + std::to_string(writer) + ",20\n"
            "Size=300,200\n"
            "Collapsed=0\n");
    }

    std::atomic<bool> start{false};
    std::vector<int> succeeded(kWriterCount, 0);
    std::vector<std::thread> writers;
    writers.reserve(kWriterCount);
    for (int writer = 0; writer < kWriterCount; ++writer) {
        writers.emplace_back([&, writer]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::string writer_error;
            succeeded[writer] = persistence.SaveSnapshot(
                snapshots[writer],
                &writer_error)
                                    ? 1
                                    : 0;
            if (succeeded[writer] == 0) {
                std::cerr << "contention writer failed: "
                          << writer_error << '\n';
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (std::thread& writer : writers) {
        writer.join();
    }
    for (int writer_succeeded : succeeded) {
        Require(
            writer_succeeded != 0,
            "every contention writer should complete atomically");
    }

    const std::string final_snapshot = ReadText(target);
    Require(
        specforge::ImGuiLayoutPersistence::IsWellFormedSnapshot(
            final_snapshot),
        "contention target should remain structurally valid");
    bool matched_writer = false;
    for (const std::string& snapshot : snapshots) {
        matched_writer = matched_writer || final_snapshot == snapshot;
    }
    Require(
        matched_writer,
        "contention target should be one complete last-completed snapshot");
}

void TestCrossProcessContentionKeepsEveryCompletedSnapshotReadable(
    const std::filesystem::path& target)
{
    specforge::ImGuiLayoutPersistence persistence(target);
    std::string error;
    Require(
        persistence.SaveSnapshot(
            "[Window][process-seed]\nPos=1,2\nSize=3,4\n",
            &error),
        error.empty() ? "process contention seed write failed" : error);

    constexpr int kWriterCount = 8;
    const std::filesystem::path executable = TestExecutablePath();
    const std::filesystem::path start = target.parent_path() /
                                        "process-contention-start.txt";
    std::vector<ChildProcess> children;
    children.reserve(kWriterCount);
    std::vector<std::string> snapshots;
    snapshots.reserve(kWriterCount);
    for (int writer = 0; writer < kWriterCount; ++writer) {
        const std::filesystem::path ready =
            target.parent_path() /
            ("process-contention-ready-" + std::to_string(writer) + ".txt");
        snapshots.push_back(ContentionSnapshot(writer));
        const std::wstring command_line =
            QuoteCommandLineArgument(executable) +
            L" --imgui-layout-contention-child " +
            QuoteCommandLineArgument(target) + L" " +
            QuoteCommandLineArgument(ready) + L" " +
            QuoteCommandLineArgument(start) + L" " +
            std::to_wstring(writer);
        children.emplace_back(StartChild(command_line));
    }

    for (int writer = 0; writer < kWriterCount; ++writer) {
        const std::filesystem::path ready =
            target.parent_path() /
            ("process-contention-ready-" + std::to_string(writer) + ".txt");
        Require(
            WaitForFile(ready, std::chrono::seconds(10)),
            "every process contention writer should reach the barrier");
    }
    WriteTextFile(start, "start\n");

    for (ChildProcess& child : children) {
        Require(
            child.Wait(30'000) == WAIT_OBJECT_0,
            "every process contention writer should exit");
        Require(
            child.ExitCode() == 0,
            "every process contention writer should save successfully");
    }

    const std::string final_snapshot = ReadText(target);
    Require(
        specforge::ImGuiLayoutPersistence::IsWellFormedSnapshot(
            final_snapshot),
        "cross-process contention target should remain structurally valid");
    bool matched_writer = false;
    for (const std::string& snapshot : snapshots) {
        matched_writer = matched_writer || final_snapshot == snapshot;
    }
    Require(
        matched_writer,
        "cross-process contention target should be one complete writer snapshot");
}

void TestInterruptedWritePreservesPreviousSnapshot(
    const std::filesystem::path& target)
{
    specforge::ImGuiLayoutPersistence persistence(target);
    const std::string stable =
        "[Window][stable]\n"
        "Pos=11,12\n"
        "Size=113,114\n"
        "Collapsed=0\n";
    std::string error;
    Require(
        persistence.SaveSnapshot(stable, &error),
        error.empty() ? "stable layout write failed" : error);

    specforge::AtomicFileWriteOptions options;
    options.open_mode = std::ios::binary | std::ios::trunc;
    options.target_description = "interrupted ImGui layout";
    Require(
        !specforge::WriteFileAtomically(
            target,
            options,
            [](std::ostream& stream, std::string& writer_error) {
                stream << "[Window][torn]\nPos=partial";
                writer_error = "simulated process interruption";
                return false;
            },
            &error),
        "interrupted layout write should fail before replacement");
    Require(
        ReadText(target) == stable,
        "interrupted layout write should preserve the previous target");
}

void TestInterruptedWriteByProcessTerminationRecoversOnRestart(
    const std::filesystem::path& target)
{
    specforge::ImGuiLayoutPersistence persistence(target);
    const std::string stable =
        "[Window][stable]\n"
        "Pos=11,12\n"
        "Size=113,114\n"
        "Collapsed=0\n";
    std::string error;
    Require(
        persistence.SaveSnapshot(stable, &error),
        error.empty() ? "process interruption seed write failed" : error);

    const std::filesystem::path ready =
        target.parent_path() / "process-interruption-ready.txt";
    const std::wstring command_line =
        QuoteCommandLineArgument(TestExecutablePath()) +
        L" --imgui-layout-interruption-child " +
        QuoteCommandLineArgument(target) + L" " +
        QuoteCommandLineArgument(ready);
    ChildProcess child = StartChild(command_line);
    Require(
        WaitForFile(ready, std::chrono::seconds(10)),
        "interrupted child should write its readiness marker");

    const std::vector<std::filesystem::path> temporary_paths =
        TemporarySiblings(target);
    Require(
        temporary_paths.size() == 1,
        "interrupted child should leave one temporary layout sibling");
    child.Terminate(73);
    Require(
        child.Wait(5000) == WAIT_OBJECT_0,
        "interrupted child should terminate");
    Require(
        child.ExitCode() == 73,
        "interrupted child should have the forced termination exit code");
    Require(
        FileExists(temporary_paths.front()),
        "interrupted child should leave its temporary layout sibling");
    Require(
        ReadText(temporary_paths.front()).find("[Window][interrupted]") !=
            std::string::npos,
        "leftover temporary layout should contain only the partial snapshot");
    Require(
        ReadText(target) == stable,
        "process interruption should preserve the previous target");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureImGuiForTest();
    const specforge::ImGuiLayoutLoadResult load = persistence.Load();
    Require(
        load.status == specforge::ImGuiLayoutLoadStatus::Loaded,
        "a fresh context should recover the complete target after interruption");
    ImGui::NewFrame();
    ImGui::Begin("stable");
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImGui::End();
    ImGui::EndFrame();
    Require(
        position.x == 11.0f && position.y == 12.0f,
        "restart recovery should restore the previous window position");
    Require(
        size.x == 113.0f && size.y == 114.0f,
        "restart recovery should restore the previous window size");
    ImGui::DestroyContext();
}

void TestSectionNamesMayContainClosingBrackets(
    const std::filesystem::path& target)
{
    const std::string layout =
        "[Window][name-with-]closing]\n"
        "Pos=31,32\n"
        "Size=333,334\n"
        "Collapsed=0\n";
    specforge::ImGuiLayoutPersistence persistence(target);
    std::string error;
    Require(
        persistence.SaveSnapshot(layout, &error),
        error.empty() ? "bracketed-name layout write failed" : error);
    Require(
        specforge::ImGuiLayoutPersistence::IsWellFormedSnapshot(layout),
        "section names containing ] should pass structural validation");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureImGuiForTest();
    const specforge::ImGuiLayoutLoadResult load = persistence.Load();
    Require(
        load.status == specforge::ImGuiLayoutLoadStatus::Loaded,
        "ImGui should load a section name containing ]");
    ImGui::NewFrame();
    ImGui::Begin("name-with-]closing");
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImGui::End();
    ImGui::EndFrame();
    Require(
        position.x == 31.0f && position.y == 32.0f,
        "a bracketed section name should restore its position");
    Require(
        size.x == 333.0f && size.y == 334.0f,
        "a bracketed section name should restore its size");
    ImGui::DestroyContext();
}

void TestMalformedLayoutRecoversOnNextSave(
    const std::filesystem::path& target)
{
    specforge::ImGuiLayoutPersistence persistence(target);
    const std::string malformed = "[Window][truncated\nPos=1,2\n";
    std::string error;
    Require(
        !persistence.SaveSnapshot(malformed, &error),
        "malformed snapshots should not replace a layout target");

    specforge::AtomicFileWriteOptions options;
    options.open_mode = std::ios::binary | std::ios::trunc;
    options.target_description = "malformed ImGui layout fixture";
    Require(
        specforge::WriteFileAtomically(
            target,
            options,
            [&](std::ostream& stream, std::string&) {
                stream << malformed;
                return true;
            },
            &error),
        error.empty() ? "malformed fixture write failed" : error);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureImGuiForTest();
    const specforge::ImGuiLayoutLoadResult load = persistence.Load();
    Require(
        load.status == specforge::ImGuiLayoutLoadStatus::Malformed,
        "malformed layout should fall back to defaults");
    ImGui::DestroyContext();

    const std::string repaired =
        "[Window][repaired]\n"
        "Pos=21,22\n"
        "Size=223,224\n"
        "Collapsed=0\n";
    Require(
        persistence.SaveSnapshot(repaired, &error),
        error.empty() ? "repaired layout write failed" : error);
    Require(
        ReadText(target) == repaired,
        "a successful later save should repair the malformed target");
}

void TestRestartLoadsTheSharedSnapshot(
    const std::filesystem::path& target)
{
    const std::string layout =
        "[Window][restart-window]\n"
        "Pos=123,234\n"
        "Size=321,243\n"
        "Collapsed=0\n";
    specforge::ImGuiLayoutPersistence persistence(target);
    std::string error;
    Require(
        persistence.SaveSnapshot(layout, &error),
        error.empty() ? "restart seed write failed" : error);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureImGuiForTest();
    const specforge::ImGuiLayoutLoadResult load = persistence.Load();
    Require(
        load.status == specforge::ImGuiLayoutLoadStatus::Loaded,
        "restart should load a complete shared layout");

    ImGui::NewFrame();
    ImGui::Begin("restart-window");
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImGui::End();
    ImGui::EndFrame();
    Require(
        position.x == 123.0f && position.y == 234.0f,
        "restart should restore the saved window position");
    Require(
        size.x == 321.0f && size.y == 243.0f,
        "restart should restore the saved window size");
    ImGui::GetIO().WantSaveIniSettings = true;
    Require(
        persistence.SaveIfRequested(&error),
        error.empty() ? "manual ImGui layout save failed" : error);
    Require(
        !ImGui::GetIO().WantSaveIniSettings,
        "successful manual layout save should clear the request flag");
    Require(
        specforge::ImGuiLayoutPersistence::IsWellFormedSnapshot(
            ReadText(target)),
        "manual ImGui layout save should produce a complete snapshot");
    ImGui::DestroyContext();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ConfigureImGuiForTest();
    const specforge::ImGuiLayoutLoadResult restarted = persistence.Load();
    Require(
        restarted.status == specforge::ImGuiLayoutLoadStatus::Loaded,
        "a fresh context should load the manually saved layout");
    ImGui::NewFrame();
    ImGui::Begin("restart-window");
    const ImVec2 restarted_position = ImGui::GetWindowPos();
    const ImVec2 restarted_size = ImGui::GetWindowSize();
    ImGui::End();
    ImGui::EndFrame();
    Require(
        restarted_position.x == 123.0f &&
            restarted_position.y == 234.0f,
        "a fresh context should restore the saved position");
    Require(
        restarted_size.x == 321.0f &&
            restarted_size.y == 243.0f,
        "a fresh context should restore the saved size");
    ImGui::DestroyContext();
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1) {
        const std::wstring_view mode(argv[1]);
        if (mode == L"--imgui-layout-contention-child") {
            return RunContentionChild(argc, argv);
        }
        if (mode == L"--imgui-layout-interruption-child") {
            return RunInterruptedWriteChild(argc, argv);
        }
    }

    try {
        const std::filesystem::path root = TestRoot();
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        std::filesystem::create_directories(root, cleanup_error);
        Require(
            !cleanup_error,
            "layout test root should be creatable");
        const std::filesystem::path target = root / "specforge-imgui-v2.ini";

        TestContentionKeepsEveryCompletedSnapshotReadable(target);
        TestCrossProcessContentionKeepsEveryCompletedSnapshotReadable(target);
        TestInterruptedWritePreservesPreviousSnapshot(target);
        TestInterruptedWriteByProcessTerminationRecoversOnRestart(target);
        TestMalformedLayoutRecoversOnNextSave(target);
        TestSectionNamesMayContainClosingBrackets(target);
        TestRestartLoadsTheSharedSnapshot(target);

        std::filesystem::remove_all(root, cleanup_error);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
