#pragma once

#include "app/specforge_metadata.h"
#include "platform/atomic_file.h"

#include <chrono>
#include <functional>
#include <string>

namespace specforge {

using MetadataFinalizerClock = std::function<
    std::chrono::system_clock::time_point()>;

struct SpecForgeMetadataFinalizerOptions {
    std::filesystem::path executable_path;
    std::filesystem::path metadata_path;
    BuildIdentity build_identity;
    BuildMetadata configured_build_metadata;
    MetadataFinalizerClock utc_now;
    AtomicFileReplaceRetryPolicy replace_retry_policy;
    AtomicFileWriteCheckpoint before_replace;
};

[[nodiscard]] bool FinalizeSpecForgeMetadata(
    const SpecForgeMetadataFinalizerOptions& options,
    std::string* error_message = nullptr);

}  // namespace specforge
