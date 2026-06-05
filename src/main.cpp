#include "app/specforge_app.h"

#include <Windows.h>

#include <exception>
#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command)
{
    try {
        specforge::SpecForgeApp app;
        return app.Run(instance, show_command);
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "SpecForge startup error", MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxA(nullptr, "Unknown startup error.", "SpecForge startup error", MB_OK | MB_ICONERROR);
    }

    return 1;
}
