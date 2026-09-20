#pragma once

#include <Windows.h>
#include <shobjidl.h>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace spectiary {

struct JumpListSource {
    std::filesystem::path path;
    std::wstring title;
};

[[nodiscard]] std::vector<JumpListSource> EligibleJumpListSources(
    std::span<const JumpListSource> sources);
[[nodiscard]] HRESULT CreateSourceJumpListLink(
    const std::filesystem::path& executable, const JumpListSource& source,
    const std::wstring& app_id, IShellLinkW** link);
[[nodiscard]] HRESULT PublishSourceJumpList(
    const std::filesystem::path& executable, std::span<const JumpListSource> sources,
    const std::wstring& category, const std::wstring& app_id,
    const std::filesystem::path& exclusions_path, ICustomDestinationList& destinations);

// Shell I/O and filesystem availability checks stay off the GUI thread.
// Requests replace pending derived state; no source registry is persisted here.
class Win32JumpList {
public:
    Win32JumpList(std::wstring app_id, std::filesystem::path config_root);
    ~Win32JumpList();
    Win32JumpList(const Win32JumpList&) = delete;
    Win32JumpList& operator=(const Win32JumpList&) = delete;
    void Refresh(std::vector<JumpListSource> sources, std::wstring category);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace spectiary
