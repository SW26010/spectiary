#pragma once

#include <filesystem>
#include <memory>

namespace specforge {

// A one-shot observation boundary for a directory. Once any relevant
// first-level entry changes, this generation remains invalid forever.
class DirectoryChangeGeneration {
public:
    virtual ~DirectoryChangeGeneration() = default;

    [[nodiscard]] virtual bool IsCurrent() const noexcept = 0;
};

using DirectoryChangeGenerationHandle = std::shared_ptr<const DirectoryChangeGeneration>;

[[nodiscard]] DirectoryChangeGenerationHandle BeginDirectoryChangeGeneration(
    const std::filesystem::path& path);

}  // namespace specforge
