#pragma once

#include <Windows.h>
#include <shobjidl.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace spectiary {

struct JumpListSource {
    std::filesystem::path path;
    std::wstring title;
};

[[nodiscard]] std::vector<JumpListSource> EligibleJumpListSources(
    std::span<const JumpListSource> sources, std::stop_token stop = {});
[[nodiscard]] HRESULT CreateSourceJumpListLink(
    const std::filesystem::path& executable, const JumpListSource& source,
    IShellLinkW** link);
[[nodiscard]] HRESULT PublishSourceJumpList(
    const std::filesystem::path& executable, std::span<const JumpListSource> sources,
    const std::wstring& category,
    const std::filesystem::path& exclusions_path, ICustomDestinationList& destinations,
    std::stop_token stop = {});

// Shell I/O and filesystem availability checks stay off the GUI thread.
// Requests replace pending derived state; no source registry is persisted here.
class Win32JumpList {
public:
    // An injected publisher must own its captures: an in-flight call can outlive
    // this facade. The default publisher uses only copied paths and snapshots.
    using Publisher = std::function<void(std::span<const JumpListSource>,
        const std::wstring&, std::stop_token)>;
    explicit Win32JumpList(std::filesystem::path config_root, Publisher publisher = {});
    ~Win32JumpList();
    Win32JumpList(const Win32JumpList&) = delete;
    Win32JumpList& operator=(const Win32JumpList&) = delete;
    void Refresh(std::vector<JumpListSource> sources, std::wstring category);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace spectiary
