#include "ui/source_collection_preparation_internal.h"

#include "domain/sample_labeling_source_compatibility.h"
#include "domain/source_path_identity.h"
#include "domain/spectrum_loader.h"
#include "ui/sample_labeling_state_cache_io.h"
#include "ui/sample_workflow_state_cache_io.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace specforge {
namespace {

void ValidateDecodedSnapshot(const SpectrumSnapshotHandle& snapshot)
{
    if (!snapshot) {
        throw std::runtime_error(
            "The source decoder returned no result.");
    }
    const auto error = std::find_if(
        snapshot->diagnostics.begin(),
        snapshot->diagnostics.end(),
        [](const SpectrumDiagnostic& diagnostic) {
            return diagnostic.severity ==
                SpectrumDiagnosticSeverity::Error;
        });
    if (error != snapshot->diagnostics.end()) {
        throw std::runtime_error(
            error->message.empty()
                ? "The source decoder rejected this input."
                : error->message);
    }
}

void LoadRestoredAnnotations(
    SourceCollectionContext& context,
    const SpectrumSnapshot& snapshot,
    const std::vector<std::filesystem::path>& annotation_paths,
    const SourceCollectionCancellationCheckpoint& checkpoint)
{
    const SampleLabelingCanonicalSourceDescriptor source =
        BuildSampleLabelingCanonicalSourceDescriptor(
            snapshot,
            context);
    SampleAnnotationSourceCompatibility compatibility =
        SampleLabelingCompatibilityView(source);
    if (compatibility.source_kind == "unknown") {
        compatibility.source_kind = {};
    }
    for (const std::filesystem::path& annotation_path :
         annotation_paths) {
        checkpoint();
        if (SourceCollectionManifestContainsAnnotation(
                context.manifest,
                annotation_path)) {
            continue;
        }
        (void)IngestReadOnlySampleAnnotationCancelable(
            context.manifest,
            annotation_path,
            compatibility,
            checkpoint);
    }
}

bool CanReusePreparedWorkflow(
    const SourceCollectionIdentity& actual,
    const SourceCollectionReuseCandidate* reuse)
{
    return reuse && actual.id == reuse->identity().id &&
           actual.context_fingerprint ==
               reuse->identity().context_fingerprint &&
           actual.spectrum_count ==
               reuse->identity().spectrum_count;
}

bool SourceCollectionIdentitiesMatchExactly(
    const SourceCollectionIdentity& left,
    const SourceCollectionIdentity& right)
{
    return left.id == right.id &&
           left.source_name == right.source_name &&
           left.source_fingerprint == right.source_fingerprint &&
           left.context_fingerprint ==
               right.context_fingerprint &&
           left.spectrum_count == right.spectrum_count;
}

bool SourceCollectionBaseIdentitiesMatch(
    const SourceCollectionIdentity& left,
    const SourceCollectionIdentity& right)
{
    return left.id == right.id &&
           left.source_name == right.source_name &&
           left.source_fingerprint == right.source_fingerprint &&
           left.spectrum_count == right.spectrum_count;
}

bool AllWorkflowCachePathsEmpty(
    const SampleWorkflowPreparationPaths& paths)
{
    return paths.labeling_state_cache_path.empty() &&
           paths.workflow_state_cache_path.empty() &&
           paths.navigation_state_cache_path.empty();
}

std::string PathText(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

bool AllWorkflowCachePathsPresent(
    const SampleWorkflowPreparationPaths& paths)
{
    return !paths.labeling_state_cache_path.empty() &&
           !paths.workflow_state_cache_path.empty() &&
           !paths.navigation_state_cache_path.empty();
}

SourceCollectionPreparationAdapters DefaultAdapters(
    SampleWorkflowPreparationPaths workflow_cache_paths)
{
    SourceCollectionPreparationAdapters adapters;
    adapters.snapshot_loader =
        LoadSpectrumSnapshotFromPathCancelable;
    adapters.folder_snapshot_loader =
        LoadFolderSpectrumSnapshotFromListingCancelable;
    adapters.folder_scanner =
        [](const std::filesystem::path& path,
           const SourceCollectionCancellationCheckpoint& checkpoint) {
            return ScanSourceCollectionFolder(
                path,
                {},
                checkpoint);
        };
    adapters.source_open_probe =
        ProbeSourceOpenRequest;
    adapters.workflow_cache_loader =
        LoadSampleWorkflowPreparationCacheBundle;
    adapters.file_context_builder =
        [](const SpectrumSnapshot& snapshot,
           const SourceCollectionSingleFileState& file_state,
           const SourceCollectionCancellationCheckpoint& checkpoint) {
            return LoadSourceCollectionContextCancelable(
                snapshot,
                file_state,
                checkpoint);
        };
    adapters.folder_context_builder =
        [](const SpectrumSnapshot& snapshot,
           const SourceCollectionFolderListing& listing,
           const SourceCollectionCancellationCheckpoint& checkpoint) {
            return BuildFolderSourceCollectionContextCancelable(
                snapshot,
                listing,
                checkpoint);
        };
    if (AllWorkflowCachePathsEmpty(workflow_cache_paths)) {
        workflow_cache_paths = {
            DefaultSampleLabelingStateCachePath(),
            DefaultSampleWorkflowStateCachePath(),
            DefaultSampleNavigationStateCachePath(),
        };
    }
    adapters.workflow_cache_paths =
        std::move(workflow_cache_paths);
    return adapters;
}

void FillMissingAdapters(
    SourceCollectionPreparationAdapters& adapters)
{
    if (!AllWorkflowCachePathsEmpty(
            adapters.workflow_cache_paths) &&
        !AllWorkflowCachePathsPresent(
            adapters.workflow_cache_paths)) {
        throw std::invalid_argument(
            "Workflow cache paths must be either all empty "
            "or all explicit.");
    }

    SourceCollectionPreparationAdapters defaults =
        DefaultAdapters(adapters.workflow_cache_paths);
    if (!adapters.snapshot_loader) {
        adapters.snapshot_loader =
            std::move(defaults.snapshot_loader);
    }
    if (!adapters.folder_snapshot_loader) {
        adapters.folder_snapshot_loader =
            std::move(defaults.folder_snapshot_loader);
    }
    if (!adapters.folder_scanner) {
        adapters.folder_scanner =
            std::move(defaults.folder_scanner);
    }
    if (!adapters.source_open_probe) {
        adapters.source_open_probe =
            std::move(defaults.source_open_probe);
    }
    if (!adapters.workflow_cache_loader) {
        adapters.workflow_cache_loader =
            std::move(defaults.workflow_cache_loader);
    }
    if (!adapters.file_context_builder) {
        adapters.file_context_builder =
            std::move(defaults.file_context_builder);
    }
    if (!adapters.folder_context_builder) {
        adapters.folder_context_builder =
            std::move(defaults.folder_context_builder);
    }
    adapters.workflow_cache_paths =
        std::move(defaults.workflow_cache_paths);
}

}  // namespace

SourceCollectionReuseCandidate::SourceCollectionReuseCandidate(
    SourceCollectionIdentity identity,
    std::uint64_t live_workflow_revision,
    SourceCollectionFolderListingGenerationHandle
        folder_listing_generation,
    std::optional<SourceCollectionContextReuseProof>
        context_reuse_proof,
    std::optional<SourceCollectionResidentSnapshot>
        resident_snapshot)
    : identity_(std::move(identity)),
      live_workflow_revision_(live_workflow_revision),
      folder_listing_generation_(
          std::move(folder_listing_generation)),
      context_reuse_proof_(std::move(context_reuse_proof)),
      resident_snapshot_(std::move(resident_snapshot))
{
}

SourceCollectionReuseCandidate
SourceCollectionReuseCandidate::Known(
    SourceCollectionIdentity identity,
    std::uint64_t live_workflow_revision,
    SourceCollectionFolderListingGenerationHandle
        folder_listing_generation)
{
    return SourceCollectionReuseCandidate(
        std::move(identity),
        live_workflow_revision,
        std::move(folder_listing_generation),
        std::nullopt,
        std::nullopt);
}

SourceCollectionReuseCandidate
SourceCollectionReuseCandidate::Verified(
    SourceCollectionContextReuseProof proof,
    std::uint64_t live_workflow_revision,
    SourceCollectionFolderListingGenerationHandle
        folder_listing_generation,
    std::optional<SourceCollectionResidentSnapshot>
        resident_snapshot)
{
    if (resident_snapshot &&
        (resident_snapshot->context_reuse_proof != proof ||
         resident_snapshot->folder_listing_generation !=
             folder_listing_generation)) {
        throw std::invalid_argument(
            "resident snapshot does not match its verified reuse boundary");
    }
    SourceCollectionIdentity identity = proof.identity;
    return SourceCollectionReuseCandidate(
        std::move(identity),
        live_workflow_revision,
        std::move(folder_listing_generation),
        std::move(proof),
        std::move(resident_snapshot));
}

const SourceCollectionIdentity&
SourceCollectionReuseCandidate::identity() const noexcept
{
    return identity_;
}

std::uint64_t
SourceCollectionReuseCandidate::live_workflow_revision() const noexcept
{
    return live_workflow_revision_;
}

const std::optional<SourceCollectionContextReuseProof>&
SourceCollectionReuseCandidate::context_reuse_proof() const noexcept
{
    return context_reuse_proof_;
}

const SourceCollectionFolderListingGenerationHandle&
SourceCollectionReuseCandidate::folder_listing_generation() const noexcept
{
    return folder_listing_generation_;
}

const std::optional<SourceCollectionResidentSnapshot>&
SourceCollectionReuseCandidate::resident_snapshot() const noexcept
{
    return resident_snapshot_;
}

SourceCollectionPreparationCanceled::
    SourceCollectionPreparationCanceled()
    : std::runtime_error("source loading was canceled")
{
}

SourceCollectionPreparationStale::
    SourceCollectionPreparationStale()
    : std::runtime_error(
          "The speculative snapshot no longer matches the active "
          "verified context.")
{
}

class SourceCollectionPreparation::Impl {
public:
    explicit Impl(SourceCollectionPreparationAdapters adapters)
        : adapters_(std::move(adapters))
    {
        FillMissingAdapters(adapters_);
        if (!adapters_.folder_change_generation_factory) {
            if (adapters_
                    .folder_change_generation_registration_factory) {
                directory_change_generation_monitor_ =
                    std::make_unique<
                        DirectoryChangeGenerationMonitor>(
                        std::move(
                            adapters_
                                .folder_change_generation_registration_factory));
            } else {
                directory_change_generation_monitor_ =
                    std::make_unique<
                        DirectoryChangeGenerationMonitor>();
            }
            adapters_.folder_change_generation_factory =
                [monitor =
                     directory_change_generation_monitor_.get()](
                    const std::filesystem::path& path,
                    const SourceCollectionCancellationCheckpoint&
                        checkpoint) {
                    return monitor->Begin(path, checkpoint);
                };
        }
    }

    struct Work {
        std::uint64_t task_id = 0;
        const SourceCollectionLoadRequest& request;
        const SourceCollectionCancellationCheckpoint& checkpoint;
        const SourceCollectionWorkflowCacheProvider&
            workflow_cache_provider;
        std::size_t spectrum_index = 0;
    };

    std::shared_ptr<const SampleWorkflowPreparationCacheBundle>
    LoadWorkflowCache(
        const SourceCollectionCancellationCheckpoint& checkpoint)
    {
        return std::make_shared<
            SampleWorkflowPreparationCacheBundle>(
            adapters_.workflow_cache_loader(
                adapters_.workflow_cache_paths,
                checkpoint));
    }

    PreparedSourceCollection Prepare(
        std::uint64_t task_id,
        const SourceCollectionLoadRequest& request,
        const SourceCollectionCancellationCheckpoint& checkpoint,
        const SourceCollectionWorkflowCacheProvider&
            workflow_cache_provider)
    {
        checkpoint();
        SourceCollectionLoadRequest resolved_request =
            request;
        if (request.source_open_request) {
            const SourceOpenFilesystemProbe probe =
                adapters_.source_open_probe(
                    *request.source_open_request,
                    checkpoint);
            const SourceOpenResolution resolution =
                ResolveSourceOpenRequest(
                    *request.source_open_request,
                    probe);
            if (resolution.failed()) {
                throw std::runtime_error(
                    resolution.diagnostic);
            }
            resolved_request.path = resolution.source_path;
            resolved_request.preferred_member_path =
                resolution.preferred_member_path;
            resolved_request.source_open_request.reset();
        }
        if (request.snapshot_only &&
            (!resolved_request.reuse ||
             !resolved_request.reuse->context_reuse_proof())) {
            throw SourceCollectionPreparationStale();
        }
        const Work work{
            task_id,
            resolved_request,
            checkpoint,
            workflow_cache_provider,
            request.spectrum_index,
        };
        std::error_code directory_error;
        const bool is_directory =
            std::filesystem::is_directory(
                resolved_request.path,
                directory_error);
        if (!directory_error && is_directory) {
            return PrepareFolder(work);
        }
        if (resolved_request.preferred_member_path) {
            throw std::runtime_error(
                "The external source folder is no longer "
                "available as a directory: " +
                PathText(resolved_request.path));
        }
        return PrepareFile(work);
    }

private:
    std::shared_ptr<const SampleWorkflowPreparationCacheBundle>
    WorkflowCache(const Work& work)
    {
        if (work.workflow_cache_provider) {
            return work.workflow_cache_provider(
                work.checkpoint);
        }
        return LoadWorkflowCache(work.checkpoint);
    }

    PreparedSampleWorkflowState PrepareWorkflow(
        const Work& work,
        const SpectrumSnapshot& snapshot,
        const SourceCollectionContext& context)
    {
        const std::shared_ptr<
            const SampleWorkflowPreparationCacheBundle> cache =
            WorkflowCache(work);
        PreparedSampleWorkflowState prepared =
            PrepareSampleWorkflowStateFromCache(
                snapshot,
                context,
                work.spectrum_index,
                *cache,
                nullptr,
                nullptr,
                work.checkpoint);
        prepared.preparation_cache = cache;
        return prepared;
    }

    void FinalizeContext(
        const Work& work,
        const SpectrumSnapshot& snapshot,
        SourceCollectionContext& context)
    {
        LoadRestoredAnnotations(
            context,
            snapshot,
            work.request.annotation_paths,
            work.checkpoint);
        FinalizeSourceCollectionAnnotationContextFingerprint(
            context,
            work.request.annotation_paths,
            work.checkpoint);
    }

    bool CanReuseKnownContext(
        const Work& work,
        const SpectrumSnapshot& snapshot,
        const SourceCollectionSingleFileState& initial_state,
        bool folder_generation_proven) const
    {
        const SourceCollectionReuseCandidate* reuse =
            work.request.reuse
            ? &*work.request.reuse
            : nullptr;
        const std::optional<SourceCollectionContextReuseProof>*
            proof =
                reuse ? &reuse->context_reuse_proof() : nullptr;
        if (!proof || !*proof ||
            !SourceCollectionIdentitiesMatchExactly(
                (*proof)->identity,
                reuse->identity()) ||
            !SourceCollectionSingleFileStatesMatch(
                (*proof)->dependency_state,
                initial_state) ||
            snapshot.collection.spectrum_count !=
                (*proof)->identity.spectrum_count ||
            SourcePathIdentityKey(snapshot.source.path) !=
                SourcePathIdentityKey(work.request.path)) {
            return false;
        }

        if (folder_generation_proven) {
            return true;
        }

        std::error_code directory_error;
        const bool snapshot_is_directory =
            std::filesystem::is_directory(
                snapshot.source.path,
                directory_error);
        if (directory_error || snapshot_is_directory) {
            return false;
        }
        const SourceCollectionIdentity actual_base =
            BuildSourceCollectionIdentity(
                snapshot,
                initial_state);
        return SourceCollectionBaseIdentitiesMatch(
            actual_base,
            (*proof)->identity);
    }

    bool CanReuseResidentSnapshot(
        const Work& work,
        const SourceCollectionResidentSnapshot& resident,
        const SourceCollectionSingleFileState& initial_state,
        bool folder_generation_proven) const
    {
        const SourceCollectionReuseCandidate* reuse =
            work.request.reuse
            ? &*work.request.reuse
            : nullptr;
        return reuse &&
               reuse->context_reuse_proof() &&
               resident.snapshot &&
               resident.spectrum_index ==
                   work.spectrum_index &&
               resident.snapshot->collection.current_index ==
                   work.spectrum_index &&
               resident.context_reuse_proof ==
                   *reuse->context_reuse_proof() &&
               resident.folder_listing_generation ==
                   reuse->folder_listing_generation() &&
               CanReuseKnownContext(
                   work,
                   *resident.snapshot,
                   initial_state,
                   folder_generation_proven);
    }

    PreparedSourceCollection BuildReusedPrepared(
        const Work& work,
        SpectrumSnapshotHandle snapshot,
        const SourceCollectionSingleFileState& verified_state,
        SourceCollectionFolderListingGenerationHandle
            folder_listing_generation = {},
        std::optional<SourceCollectionResidentSnapshotOrigin>
            snapshot_cache_origin = std::nullopt,
        std::uint64_t snapshot_prefetch_id = 0,
        std::uint64_t snapshot_prefetch_task_id = 0,
        std::int64_t snapshot_prefetch_scheduled_ns = 0,
        SampleNavigationDirection snapshot_prefetch_direction =
            SampleNavigationDirection::Next)
    {
        const SourceCollectionIdentity identity =
            work.request.reuse->context_reuse_proof()->identity;
        if (work.request.latency_attempt) {
            work.request.latency_attempt
                ->MarkWorkflowReused(true);
        }
        PreparedSourceCollection prepared{
            work.task_id,
            work.request.path,
            work.spectrum_index,
            std::move(snapshot),
            PreparedSourceCollectionReuse{identity},
        };
        prepared.context_reuse_proof =
            SourceCollectionContextReuseProof{
                identity,
                verified_state};
        prepared.folder_listing_generation =
            std::move(folder_listing_generation);
        prepared.snapshot_cache_hit =
            snapshot_cache_origin.has_value();
        prepared.snapshot_cache_origin =
            snapshot_cache_origin;
        prepared.snapshot_prefetch_id = snapshot_prefetch_id;
        prepared.snapshot_prefetch_task_id =
            snapshot_prefetch_task_id;
        prepared.snapshot_prefetch_scheduled_ns =
            snapshot_prefetch_scheduled_ns;
        prepared.snapshot_prefetch_direction =
            snapshot_prefetch_direction;
        return prepared;
    }

    PreparedSourceCollection BuildPrepared(
        const Work& work,
        SpectrumSnapshotHandle snapshot,
        SourceCollectionContext context,
        const SourceCollectionSingleFileState& verified_state,
        SourceCollectionFolderListingGenerationHandle
            folder_listing_generation = {})
    {
        const SourceCollectionReuseCandidate* reuse =
            work.request.reuse
            ? &*work.request.reuse
            : nullptr;
        if (!work.request.preferred_member_path &&
            CanReusePreparedWorkflow(
                context.identity,
                reuse)) {
            if (work.request.latency_attempt) {
                work.request.latency_attempt
                    ->MarkWorkflowReused(true);
            }
            SourceCollectionContextReuseProof reuse_proof{
                context.identity,
                verified_state};
            PreparedSourceCollection prepared{
                work.task_id,
                work.request.path,
                work.spectrum_index,
                std::move(snapshot),
                PreparedSourceCollectionReuse{
                    reuse_proof.identity},
            };
            prepared.context_reuse_proof =
                std::move(reuse_proof);
            prepared.folder_listing_generation =
                std::move(folder_listing_generation);
            return prepared;
        }
        if (work.request.latency_attempt) {
            work.request.latency_attempt
                ->MarkWorkflowReused(false);
        }
        PreparedSampleWorkflowState workflow =
            PrepareWorkflow(work, *snapshot, context);
        if (work.request.preferred_member_path &&
            workflow.filter_evaluation.active &&
            (work.spectrum_index >=
                 workflow.filter_evaluation.included_samples.size() ||
             !workflow.filter_evaluation
                   .included_samples[work.spectrum_index])) {
            throw std::runtime_error(
                "The requested external source member is excluded by "
                "the active sample filter: " +
                PathText(
                    *work.request.preferred_member_path));
        }
        SourceCollectionContextReuseProof reuse_proof{
            context.identity,
            verified_state};
        PreparedSourceCollection prepared{
            work.task_id,
            work.request.path,
            work.spectrum_index,
            std::move(snapshot),
            PreparedSourceCollectionPlan{
                std::move(context),
                std::move(workflow),
                reuse
                    ? std::optional<std::uint64_t>{
                          reuse->live_workflow_revision()}
                    : std::nullopt,
                work.request.preferred_member_path},
        };
        prepared.context_reuse_proof =
            std::move(reuse_proof);
        prepared.folder_listing_generation =
            std::move(folder_listing_generation);
        return prepared;
    }

    SourceCollectionFolderListingGenerationHandle
    ScanFolderListingGeneration(
        const std::filesystem::path& path,
        const SourceCollectionCancellationCheckpoint& checkpoint)
    {
        DirectoryChangeGenerationHandle change_generation =
            adapters_.folder_change_generation_factory(
                path,
                checkpoint);
        SourceCollectionFolderListing listing =
            adapters_.folder_scanner(path, checkpoint);
        return std::make_shared<
            const SourceCollectionFolderListingGeneration>(
            SourceCollectionFolderListingGeneration{
                std::move(listing),
                std::move(change_generation),
            });
    }

    PreparedSourceCollection PrepareFolder(Work work)
    {
        constexpr std::size_t kMaximumAttempts = 2;
        const SourceCollectionReuseCandidate* reuse =
            work.request.reuse
            ? &*work.request.reuse
            : nullptr;
        SourceCollectionFolderListingGenerationHandle
            listing_generation =
                reuse
                ? reuse->folder_listing_generation()
                : SourceCollectionFolderListingGenerationHandle{};
        bool resident_candidate_available =
            reuse && reuse->resident_snapshot().has_value();
        for (std::size_t attempt = 0;
             attempt < kMaximumAttempts;
             ++attempt) {
            work.checkpoint();
            const bool hint_present =
                listing_generation != nullptr;
            const bool generation_current_at_start =
                listing_generation &&
                listing_generation->change_generation &&
                listing_generation->IsCurrent();
            bool listing_scan_performed = false;
            if (listing_generation &&
                ((listing_generation->change_generation &&
                  !generation_current_at_start) ||
                 (!work.request.preferred_member_path &&
                  (work.spectrum_index >=
                       listing_generation->listing.spectra.size() ||
                   !SourceCollectionFolderSpectrumFileMatchesCurrentState(
                       listing_generation
                           ->listing
                           .spectra[work.spectrum_index]))))) {
                listing_generation.reset();
            }
            if (work.request.snapshot_only &&
                (!listing_generation ||
                 !generation_current_at_start)) {
                throw SourceCollectionPreparationStale();
            }
            if (!listing_generation) {
                listing_generation =
                    ScanFolderListingGeneration(
                        work.request.path,
                        work.checkpoint);
                listing_scan_performed = true;
            }
            const SourceCollectionFolderListing& listing =
                listing_generation->listing;
            if (work.request.preferred_member_path) {
                if (!listing.readable) {
                    const std::string diagnostic =
                        listing.error_message.empty()
                            ? "Could not enumerate the input folder: " +
                                  PathText(work.request.path)
                            : listing.error_message;
                    throw std::runtime_error(
                        diagnostic);
                }
                const std::string preferred_member_key =
                    SourcePathIdentityKey(
                        *work.request.preferred_member_path);
                std::optional<std::size_t> preferred_index;
                for (std::size_t index = 0;
                     index < listing.spectra.size();
                     ++index) {
                    if ((index & 0xffU) == 0U) {
                        work.checkpoint();
                    }
                    if (SourcePathIdentityKey(
                            listing.spectra[index].path) ==
                        preferred_member_key) {
                        preferred_index = index;
                        break;
                    }
                }
                if (!preferred_index) {
                    if (listing_generation &&
                        !listing_scan_performed) {
                        listing_generation.reset();
                        continue;
                    }
                    throw std::runtime_error(
                        "The requested external source member was not "
                        "found in the source folder after the first-level "
                        "scan: " +
                        PathText(
                            *work.request.preferred_member_path));
                }
                if (!SourceCollectionFolderSpectrumFileMatchesCurrentState(
                        listing.spectra[*preferred_index])) {
                    if (listing_generation &&
                        !listing_scan_performed) {
                        listing_generation.reset();
                        continue;
                    }
                    throw std::runtime_error(
                        "The requested external source member is no longer "
                        "a valid member of the source folder: " +
                        PathText(
                            *work.request.preferred_member_path));
                }
                work.spectrum_index = *preferred_index;
            } else {
                work.spectrum_index =
                    work.request.spectrum_index;
            }
            const SourceCollectionSingleFileState initial_state =
                CaptureSourceCollectionSingleFileState(
                    work.request.path,
                    work.request.annotation_paths,
                    work.checkpoint);
            if (resident_candidate_available) {
                const SourceCollectionResidentSnapshot& resident =
                    *reuse->resident_snapshot();
                const bool resident_current =
                    !work.request.preferred_member_path &&
                    !listing_scan_performed &&
                    generation_current_at_start &&
                    listing_generation ==
                        resident.folder_listing_generation &&
                    CanReuseResidentSnapshot(
                        work,
                        resident,
                        initial_state,
                        true);
                resident_candidate_available = false;
                if (resident_current) {
                    if (work.request.latency_attempt) {
                        work.request.latency_attempt
                            ->MarkFolderSnapshotLoadStarted({
                                .hint_present = hint_present,
                                .generation_current_at_start =
                                    generation_current_at_start,
                                .listing_scan_performed = false,
                            });
                    }
                    SpectrumSnapshotHandle snapshot =
                        resident.snapshot;
                    work.checkpoint();
                    ValidateDecodedSnapshot(snapshot);
                    if (work.request.latency_attempt) {
                        work.request.latency_attempt
                            ->MarkSnapshotLoadFinished();
                        work.request.latency_attempt
                            ->MarkContextPrepared(true);
                    }
                    const SourceCollectionSingleFileState
                        verified_state =
                            CaptureSourceCollectionSingleFileState(
                                work.request.path,
                                work.request.annotation_paths,
                                work.checkpoint);
                    const bool listing_generation_is_current =
                        listing_generation->IsCurrent();
                    const bool revalidation_succeeded =
                        SourceCollectionSingleFileStatesMatch(
                            initial_state,
                            verified_state) &&
                        listing_generation_is_current;
                    if (work.request.latency_attempt) {
                        work.request.latency_attempt
                            ->MarkSourceRevalidated(
                                revalidation_succeeded);
                    }
                    if (revalidation_succeeded) {
                        return BuildReusedPrepared(
                            work,
                            std::move(snapshot),
                            verified_state,
                            std::move(listing_generation),
                            resident.origin,
                            resident.prefetch_id,
                            resident.prefetch_task_id,
                            resident.prefetch_scheduled_ns,
                            resident.prefetch_direction);
                    }
                    if (!listing_generation_is_current) {
                        listing_generation.reset();
                    }
                    continue;
                }
            }
            if (work.request.latency_attempt) {
                work.request.latency_attempt
                    ->MarkFolderSnapshotLoadStarted({
                        .hint_present = hint_present,
                        .generation_current_at_start =
                            generation_current_at_start,
                        .listing_scan_performed =
                            listing_scan_performed,
                    });
            }
            SpectrumSnapshotHandle snapshot =
                adapters_.folder_snapshot_loader(
                    work.request.path,
                    work.spectrum_index,
                    listing,
                    [&checkpoint = work.checkpoint]() {
                        try {
                            checkpoint();
                            return false;
                        } catch (
                            const SourceCollectionPreparationCanceled&) {
                            return true;
                        }
                    });
            work.checkpoint();
            ValidateDecodedSnapshot(snapshot);
            if (work.request.latency_attempt) {
                work.request.latency_attempt
                    ->MarkSnapshotLoadFinished();
            }
            const bool can_reuse_context =
                CanReuseKnownContext(
                    work,
                    *snapshot,
                    initial_state,
                    generation_current_at_start &&
                        !listing_scan_performed &&
                        reuse &&
                        listing_generation ==
                        reuse->folder_listing_generation());
            const bool reuse_context =
                can_reuse_context &&
                !work.request.preferred_member_path;
            std::optional<SourceCollectionContext> context;
            if (!reuse_context) {
                if (work.request.snapshot_only) {
                    throw SourceCollectionPreparationStale();
                }
                context.emplace(
                    adapters_.folder_context_builder(
                        *snapshot,
                        listing,
                        work.checkpoint));
                FinalizeContext(work, *snapshot, *context);
            }
            if (work.request.latency_attempt) {
                work.request.latency_attempt
                    ->MarkContextPrepared(reuse_context);
            }
            const SourceCollectionSingleFileState verified_state =
                CaptureSourceCollectionSingleFileState(
                    work.request.path,
                    work.request.annotation_paths,
                    work.checkpoint);
            SourceCollectionFolderListingGenerationHandle
                verified_generation = listing_generation;
            bool listing_generation_is_current = false;
            if (listing_generation->change_generation) {
                listing_generation_is_current =
                    listing_generation->IsCurrent();
            } else {
                verified_generation =
                    ScanFolderListingGeneration(
                        work.request.path,
                        work.checkpoint);
                if (work.request.latency_attempt) {
                    work.request.latency_attempt
                        ->MarkFolderListingScanPerformed();
                }
                listing_generation_is_current =
                    SourceCollectionFolderListingsMatch(
                        listing,
                        verified_generation->listing,
                        work.checkpoint) &&
                    (!verified_generation->change_generation ||
                     verified_generation->IsCurrent());
            }
            const bool revalidation_succeeded =
                SourceCollectionSingleFileStatesMatch(
                    initial_state,
                    verified_state) &&
                listing_generation_is_current;
            if (work.request.latency_attempt) {
                work.request.latency_attempt
                    ->MarkSourceRevalidated(
                        revalidation_succeeded);
            }
            if (revalidation_succeeded) {
                if (reuse_context) {
                    return BuildReusedPrepared(
                        work,
                        std::move(snapshot),
                        verified_state,
                        std::move(verified_generation));
                }
                return BuildPrepared(
                    work,
                    std::move(snapshot),
                    std::move(*context),
                    verified_state,
                    std::move(verified_generation));
            }
            if (listing_generation_is_current) {
                listing_generation =
                    std::move(verified_generation);
            } else if (!listing_generation->change_generation &&
                       verified_generation &&
                       (!verified_generation->change_generation ||
                        verified_generation->IsCurrent())) {
                listing_generation =
                    std::move(verified_generation);
            } else {
                listing_generation.reset();
            }
        }
        throw std::runtime_error(
            "The source folder kept changing while it was loaded; "
            "try again after synchronization settles.");
    }

    PreparedSourceCollection PrepareFile(const Work& work)
    {
        constexpr std::size_t kMaximumAttempts = 2;
        const SourceCollectionReuseCandidate* reuse =
            work.request.reuse
            ? &*work.request.reuse
            : nullptr;
        bool resident_candidate_available =
            reuse && reuse->resident_snapshot().has_value();
        for (std::size_t attempt = 0;
             attempt < kMaximumAttempts;
             ++attempt) {
            work.checkpoint();
            const SourceCollectionSingleFileState initial_state =
                CaptureSourceCollectionSingleFileState(
                    work.request.path,
                    work.request.annotation_paths,
                    work.checkpoint);
            if (resident_candidate_available) {
                const SourceCollectionResidentSnapshot& resident =
                    *reuse->resident_snapshot();
                const bool resident_current =
                    CanReuseResidentSnapshot(
                        work,
                        resident,
                        initial_state,
                        false);
                resident_candidate_available = false;
                if (resident_current) {
                    if (work.request.latency_attempt) {
                        work.request.latency_attempt
                            ->MarkSnapshotLoadStarted(false);
                    }
                    SpectrumSnapshotHandle snapshot =
                        resident.snapshot;
                    work.checkpoint();
                    ValidateDecodedSnapshot(snapshot);
                    if (work.request.latency_attempt) {
                        work.request.latency_attempt
                            ->MarkSnapshotLoadFinished();
                        work.request.latency_attempt
                            ->MarkContextPrepared(true);
                    }
                    const SourceCollectionSingleFileState
                        verified_state =
                            CaptureSourceCollectionSingleFileState(
                                work.request.path,
                                work.request.annotation_paths,
                                work.checkpoint);
                    const bool revalidation_succeeded =
                        SourceCollectionSingleFileStatesMatch(
                            initial_state,
                            verified_state);
                    if (work.request.latency_attempt) {
                        work.request.latency_attempt
                            ->MarkSourceRevalidated(
                                revalidation_succeeded);
                    }
                    if (revalidation_succeeded) {
                        return BuildReusedPrepared(
                            work,
                            std::move(snapshot),
                            verified_state,
                            {},
                            resident.origin,
                            resident.prefetch_id,
                            resident.prefetch_task_id,
                            resident.prefetch_scheduled_ns,
                            resident.prefetch_direction);
                    }
                    continue;
                }
            }
            if (work.request.latency_attempt) {
                work.request.latency_attempt
                    ->MarkSnapshotLoadStarted(false);
            }
            SpectrumSnapshotHandle snapshot =
                adapters_.snapshot_loader(
                    work.request.path,
                    work.spectrum_index,
                    [&checkpoint = work.checkpoint]() {
                        try {
                            checkpoint();
                            return false;
                        } catch (
                            const SourceCollectionPreparationCanceled&) {
                            return true;
                        }
                    });
            work.checkpoint();
            ValidateDecodedSnapshot(snapshot);
            if (work.request.latency_attempt) {
                work.request.latency_attempt
                    ->MarkSnapshotLoadFinished();
            }
            const bool can_reuse_context =
                CanReuseKnownContext(
                    work,
                    *snapshot,
                    initial_state,
                    false);
            std::optional<SourceCollectionContext> context;
            if (!can_reuse_context) {
                if (work.request.snapshot_only) {
                    throw SourceCollectionPreparationStale();
                }
                context.emplace(
                    adapters_.file_context_builder(
                        *snapshot,
                        initial_state,
                        work.checkpoint));
                FinalizeContext(work, *snapshot, *context);
            }
            if (work.request.latency_attempt) {
                work.request.latency_attempt
                    ->MarkContextPrepared(can_reuse_context);
            }
            const SourceCollectionSingleFileState verified_state =
                CaptureSourceCollectionSingleFileState(
                    work.request.path,
                    work.request.annotation_paths,
                    work.checkpoint);
            const bool revalidation_succeeded =
                SourceCollectionSingleFileStatesMatch(
                    initial_state,
                    verified_state);
            if (work.request.latency_attempt) {
                work.request.latency_attempt
                    ->MarkSourceRevalidated(
                        revalidation_succeeded);
            }
            if (revalidation_succeeded) {
                if (can_reuse_context) {
                    return BuildReusedPrepared(
                        work,
                        std::move(snapshot),
                        verified_state);
                }
                return BuildPrepared(
                    work,
                    std::move(snapshot),
                    std::move(*context),
                    verified_state);
            }
        }
        throw std::runtime_error(
            "The source file or one of its companions kept changing "
            "while it was loaded; try again after synchronization "
            "settles.");
    }

    std::unique_ptr<DirectoryChangeGenerationMonitor>
        directory_change_generation_monitor_;
    SourceCollectionPreparationAdapters adapters_;
};

SourceCollectionPreparation::SourceCollectionPreparation()
    : SourceCollectionPreparation(
          SourceCollectionPreparationAdapters{})
{
}

SourceCollectionPreparation::SourceCollectionPreparation(
    SourceCollectionPreparationAdapters adapters)
    : impl_(std::make_unique<Impl>(std::move(adapters)))
{
}

SourceCollectionPreparation::~SourceCollectionPreparation() =
    default;
SourceCollectionPreparation::SourceCollectionPreparation(
    SourceCollectionPreparation&&) noexcept = default;
SourceCollectionPreparation&
SourceCollectionPreparation::operator=(
    SourceCollectionPreparation&&) noexcept = default;

std::shared_ptr<const SampleWorkflowPreparationCacheBundle>
SourceCollectionPreparation::LoadWorkflowCache(
    const SourceCollectionCancellationCheckpoint& checkpoint)
{
    return impl_->LoadWorkflowCache(checkpoint);
}

PreparedSourceCollection SourceCollectionPreparation::Prepare(
    std::uint64_t task_id,
    const SourceCollectionLoadRequest& request,
    const SourceCollectionCancellationCheckpoint& checkpoint,
    const SourceCollectionWorkflowCacheProvider&
        workflow_cache_provider)
{
    return impl_->Prepare(
        task_id,
        request,
        checkpoint,
        workflow_cache_provider);
}

}  // namespace specforge
