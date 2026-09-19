#pragma once

#include "ui/source_collection_load_dependencies.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>

namespace spectiary {

using SourceCollectionWorkflowCacheProvider =
    std::function<
        std::shared_ptr<const SampleWorkflowPreparationCacheBundle>(
            const SourceCollectionCancellationCheckpoint&)>;

class SourceCollectionPreparation {
public:
    SourceCollectionPreparation();
    explicit SourceCollectionPreparation(
        SourceCollectionLoadDependencies adapters);
    ~SourceCollectionPreparation();

    SourceCollectionPreparation(
        SourceCollectionPreparation&&) noexcept;
    SourceCollectionPreparation& operator=(
        SourceCollectionPreparation&&) noexcept;
    SourceCollectionPreparation(
        const SourceCollectionPreparation&) = delete;
    SourceCollectionPreparation& operator=(
        const SourceCollectionPreparation&) = delete;

    [[nodiscard]] std::shared_ptr<
        const SampleWorkflowPreparationCacheBundle>
    LoadWorkflowCache(
        const SourceCollectionCancellationCheckpoint& checkpoint);
    // Pair a cancellable checkpoint with its token so native registration
    // waits can wake without periodically invoking the checkpoint.
    [[nodiscard]] PreparedSourceCollection Prepare(
        std::uint64_t task_id,
        const SourceCollectionLoadRequest& request,
        const SourceCollectionCancellationCheckpoint& checkpoint,
        const SourceCollectionWorkflowCacheProvider&
            workflow_cache_provider = {},
        std::stop_token cancellation_token = {});

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spectiary
