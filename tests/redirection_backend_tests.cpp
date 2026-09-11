#include <Windows.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    const bool experiment = argc == 2 && std::strcmp(argv[1], "experiment") == 0;
    SetEnvironmentVariableW(L"SPECFORGE_EXPERIMENT_NO_REDIRECTION_BITMAP", experiment ? L"1" : L"0");
    HWND main_window = CreateWindowExW(0, L"STATIC", L"redirection backend test",
        WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!main_window) return 1;
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    if (!ImGui_ImplWin32_Init(main_window)) return 2;
    bool passed = (GetWindowLongPtrW(main_window, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) == 0;
    auto& io = ImGui::GetPlatformIO();
    {
        ImGuiViewport viewport;
        viewport.ID = 0x12345;
        viewport.Pos = ImVec2(100, 100);
        viewport.Size = ImVec2(320, 240);
        io.Platform_CreateWindow(&viewport);
        HWND hwnd = static_cast<HWND>(viewport.PlatformHandleRaw);
        passed = passed && hwnd && (((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0) == experiment);
        viewport.Flags |= ImGuiViewportFlags_NoTaskBarIcon;
        io.Platform_UpdateWindow(&viewport);
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        passed = passed && (style & WS_EX_TOOLWINDOW) && (((style & WS_EX_NOREDIRECTIONBITMAP) != 0) == experiment);
        io.Platform_SetWindowSize(&viewport, ImVec2(400, 300));
        RECT rect{};
        passed = passed && GetClientRect(hwnd, &rect) && rect.right == 400 && rect.bottom == 300;
        io.Platform_DestroyWindow(&viewport);
        passed = passed && !IsWindow(hwnd) && !viewport.PlatformUserData;
    }
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(main_window);
    std::printf("redirection backend %s: %s\n", experiment ? "experiment" : "baseline", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
