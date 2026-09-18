#include "app/local_user_state.h"
#include "app/runtime_paths.h"
#include "ui/source_collection_session_state_cache_io.h"
#include "ui/source_collection_session.h"
#include "ui/sample_workflow_preparation.h"

#include <nlohmann/json.hpp>
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace specforge;
namespace fs = std::filesystem;

void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

template<class F> void RequireFailure(F operation)
{
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    Require(failed, "invalid persistent root must fail explicitly");
}

void TestStorageContext(const fs::path& test_root)
{
    const auto package = test_root / "package";
    const auto local = test_root / "local";
    const auto disposable = test_root / "system-temp";
    int local_calls = 0;
    RuntimePathInputs inputs{
        .executable_path = package / "renamed.exe",
        .local_app_data_directory = [&] { ++local_calls; return local; },
        .system_temp_directory = [&] { return disposable; },
    };
    const auto installed = RuntimePathsForDeployment({}, inputs);
    Require(local_calls == 1, "resolve LocalAppData once");
    Require(installed.application_data_root == local / project_identity::kLocalAppDataLeaf,
            "use the explicit product storage leaf");
    Require(installed.application_data_root.filename() == "Spectiary", "final leaf is Spectiary");
    Require(installed.local_user_state_root == local / "SpecForge", "no business-file cutover");
    Require(!fs::exists(local), "path resolution must not create or migrate data");
    Require(installed.package_root == package, "package root is independent of executable name");
    inputs.local_app_data_directory = [&]() -> fs::path {
        ++local_calls;
        throw std::runtime_error("LocalAppData unavailable");
    };
    const DeploymentMetadata portable{.distribution = Distribution::Portable,
                                      .storage_profile = StorageProfile::Portable};
    const auto paths = RuntimePathsForDeployment(portable, inputs);
    Require(local_calls == 1, "Portable must not query LocalAppData");
    Require(paths.application_data_root == package, "Portable application root equals package root");
    Require(paths.local_user_state_root == package / "Data", "Portable business layout stays unchanged");
    for (const auto& p : {installed, paths}) {
        Require(p.config_root == p.application_data_root / "config", "config namespace");
        Require(p.state_root == p.application_data_root / "state", "state namespace");
        Require(p.logs_root == p.application_data_root / "logs", "logs namespace");
        Require(p.unsaved_root == p.application_data_root / "unsaved", "unsaved namespace");
        Require(p.temp_root == disposable / project_identity::kApplicationId / "temp", "system temp");
        Require(p.cache_root == disposable / project_identity::kApplicationId / "cache", "disposable cache");
    }
    RequireFailure([&] { (void)RuntimePathsForDeployment({}, inputs); });
    for (const auto& invalid : {fs::path{}, fs::path("relative")}) {
        inputs.local_app_data_directory = [=] { return invalid; };
        RequireFailure([&] { (void)RuntimePathsForDeployment({}, inputs); });
    }
    inputs.local_user_state_root_override = test_root / "injected";
    const auto overridden = RuntimePathsForDeployment({}, inputs);
    Require(overridden.application_data_root == *inputs.local_user_state_root_override,
            "override injects an already resolved root without platform lookup");
    Require(overridden.local_user_state_root == overridden.application_data_root, "isolated business writes");
    const auto portable_override = RuntimePathsForDeployment(portable, inputs);
    Require(portable_override.package_root == package && portable_override.application_data_root == package,
            "Portable override must not change package-relative identity");
    Require(portable_override.local_user_state_root == *inputs.local_user_state_root_override,
            "Portable runtime override isolates existing persistence");
    inputs.local_user_state_root_override = fs::path{};
    RequireFailure([&] { (void)RuntimePathsForDeployment({}, inputs); });
    inputs.local_user_state_root_override = "relative";
    RequireFailure([&] { (void)RuntimePathsForDeployment(portable, inputs); });
    const auto file = test_root / "not-directory";
    std::ofstream(file) << "sentinel";
    inputs.local_user_state_root_override = file / "nested";
    RequireFailure([&] { (void)RuntimePathsForDeployment({}, inputs); });
}

RuntimePaths Context(const fs::path& package, StorageProfile profile)
{
    return RuntimePathsForDeployment({.storage_profile = profile}, {
        .executable_path = package / "any.exe",
        .local_app_data_user_state_root = package / "local" / "Spectiary",
    });
}

void TestLocators(const fs::path& root)
{
    const auto portable = Context(root / "package-a", StorageProfile::Portable);
    const auto moved = Context(root / "package-b", StorageProfile::Portable);
    const auto local = Context(root / "package-a", StorageProfile::LocalAppData);
    Require(UserPathDisplayText({}, portable).empty(), "empty optional UI paths remain displayable");
    const auto file = portable.package_root / "work" / "source.npy";
    const auto encoded = PersistedPathReferenceJson(file, portable);
    Require(encoded.at("path_kind") == "package_relative", "Portable locator is relative");
    Require(ReadPersistedPathReference(encoded, moved) == moved.package_root / "work" / "source.npy",
            "explicit package-relative relocation uses supplied context");
    Require(!ReadPersistedPathReference(encoded, local), "contradictory local profile relative locator fails closed");
    for (const auto& p : {file, local.application_data_root / "work.npy"}) {
        const auto absolute = PersistedPathReferenceJson(p, local);
        Require(absolute.at("path_kind") == "absolute", "LocalAppData locators stay absolute");
        Require(ReadPersistedPathReference(absolute, moved) == p, "absolute paths never rebase");
    }
    for (const char* invalid : {"", "relative.npy", "C:relative.npy", "\\rooted.npy"}) {
        Require(!ReadPersistedPathReference({{"path_kind", "absolute"}, {"path", invalid}}, portable),
                "absolute locator must be genuinely absolute");
    }
    for (const char* invalid : {"", "..", "../file", "a/../../file", "a/../file", "C:relative.npy",
                               "C:/absolute.npy", "/rooted", "\\rooted", "\\\\server\\share\\file"}) {
        Require(!ReadPersistedPathReference({{"path_kind", "package_relative"}, {"path", invalid}}, portable),
                "package-relative locator must not contain root or traversal semantics");
    }
    for (const nlohmann::json malformed : {nlohmann::json{}, nlohmann::json(3),
            nlohmann::json{{"path_kind", "unknown"}, {"path", "x"}},
            nlohmann::json{{"path_kind", "absolute"}, {"path", 4}},
            nlohmann::json{{"path_kind", "package_relative"}, {"path", std::string("a\0b", 3)}}}) {
        Require(!ReadPersistedPathReference(malformed, portable), "malformed locator fails closed");
    }
    Require(ReadPersistedPathReference({{"path_kind", "package_relative"}, {"path", "."}}, portable)
            == portable.package_root, "package root locator");
    const auto old_path = root / "old" / portable.package_root.filename() / "source.npy";
    fs::create_directories(portable.package_root);
    std::ofstream(portable.package_root / "source.npy") << "different file";
    Require(ReadPersistedPathReference(LocalUserStatePathToUtf8(old_path), portable) == old_path,
            "matching basenames must never guess legacy relocation");
    Require(!ReadPersistedPathReference("relative.npy", portable), "legacy relative strings are not cwd locators");
    RequireFailure([&] { (void)PersistedPathReferenceJson({}, portable); });
    auto invalid_context = portable;
    invalid_context.package_root = "relative-package";
    RequireFailure([&] { (void)PersistedPathReferenceJson(file, invalid_context); });
    Require(!ReadPersistedPathReference(encoded, invalid_context), "unresolved context must not use cwd");

    // Independent contexts can be used concurrently; neither queries process metadata.
    auto worker = std::async(std::launch::async, [=] { return ReadPersistedPathReference(encoded, moved); });
    Require(ReadPersistedPathReference(encoded, portable) == file, "original context remains unchanged");
    Require(worker.get() == moved.package_root / "work" / "source.npy", "worker retains its injected context");
}

void TestReservedNamespaces(const fs::path& root)
{
    for (const auto profile : {StorageProfile::Portable, StorageProfile::LocalAppData}) {
        const auto paths = Context(root / "reserved", profile);
        for (const auto* name : {"config", "state", "logs", "unsaved", "CONFIG", "StAtE"}) {
            Require(CheckUserFilePath(paths.application_data_root / name, paths) == UserFilePathStatus::ReservedNamespace,
                    "reserved directory itself is rejected");
            Require(CheckUserFilePath(paths.application_data_root / name / "nested" / "file.npy", paths)
                    == UserFilePathStatus::ReservedNamespace, "reserved descendants are rejected");
        }
        for (const auto* name : {"user.npy", "workspace/file.npy", "config-other/file.npy", "workspace/state/file.npy",
                                "state/../user.npy"}) {
            Require(CheckUserFilePath(paths.application_data_root / name, paths) == UserFilePathStatus::Allowed,
                    "user files outside reserved namespaces are legal");
        }
        Require(CheckUserFilePath(paths.application_data_root / "workspace/../state/file.npy", paths)
                == UserFilePathStatus::ReservedNamespace, "normalized traversal cannot bypass reserved namespace");
        for (const auto* name : {"state./file.npy", "state /file.npy", "state:stream"}) {
            Require(CheckUserFilePath(paths.application_data_root / name, paths) != UserFilePathStatus::Allowed,
                    "Win32 path aliases fail closed");
        }
        Require(CheckUserFilePath("relative.npy", paths) == UserFilePathStatus::Invalid, "no cwd-dependent checks");
        Require(CheckUserFilePath(root / "outside.npy", paths) == UserFilePathStatus::Allowed, "outside user file");
    }
}

void TestConsumersKeepInjectedContext(const fs::path& root)
{
    const auto original = Context(root / "original", StorageProfile::Portable);
    const auto relocated = Context(root / "relocated", StorageProfile::Portable);
    const auto source_path = original.package_root / "workspace" / "source.npy";
    const auto session_cache = root / "session.json";
    SourceCollectionSessionStateCache cache;
    cache.sources.push_back({.path = source_path});
    cache.active_source_index = 0;
    Require(SaveSourceCollectionSessionStateCache(original, session_cache, cache), "write source locator");
    SourceCollectionSession session(session_cache, {}, {}, {},
        SampleLabelingStateCacheLoadPolicy::AllowPersistentOutputs, relocated);
    const auto restore = session.TakeDeferredRestorePlan();
    Require(restore && restore->sources.size() == 1 &&
            restore->sources.front().path == relocated.package_root / "workspace/source.npy",
            "session restoration must use injected package rather than current executable package");

    const auto workflow_cache = root / "workflow.json";
    SampleWorkflowStateCache workflow;
    workflow.sources_by_identity["source"].selected_sample_sort_source_id =
        "annotation:" + LocalUserStatePathToUtf8(source_path);
    Require(SaveSampleWorkflowStateCache(original, workflow_cache, workflow), "write annotation locator");
    SampleWorkflowPreparationPaths paths{
        .labeling_state_cache_path = root / "absent-labeling.json",
        .workflow_state_cache_path = workflow_cache,
        .navigation_state_cache_path = root / "absent-navigation.json",
        .runtime_paths = relocated,
    };
    auto worker = std::async(std::launch::async, [paths] {
        return LoadSampleWorkflowPreparationCacheBundle(paths);
    });
    const auto bundle = worker.get();
    Require(bundle.workflow.sources_by_identity.at("source").selected_sample_sort_source_id ==
            "annotation:" + LocalUserStatePathToUtf8(relocated.package_root / "workspace" / "source.npy"),
            "background preparation must carry startup locator context by value");
}
} // namespace

int main()
{
    const auto root = fs::temp_directory_path() / ("specforge-storage-context-" + std::to_string(GetCurrentProcessId()));
    try {
        fs::create_directories(root);
        TestStorageContext(root);
        TestLocators(root);
        TestReservedNamespaces(root);
        TestConsumersKeepInjectedContext(root);
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return 1;
    }
}
