#include "app/specforge_metadata.h"
#include "app/runtime_paths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef SPECFORGE_METADATA_FIXTURE_DIR
#error "SPECFORGE_METADATA_FIXTURE_DIR must be configured."
#endif

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

specforge::BuildIdentity WorkingTreeIdentity()
{
    return {
        .product_name = "SpecForge",
        .specforge_version = "0.4.1",
        .configuration = "Debug",
        .target_architecture = "amd64",
        .source_mode = "working_tree",
        .source_revision = "",
    };
}

std::filesystem::path FixturePath(std::string_view name)
{
    return std::filesystem::path(
               SPECFORGE_METADATA_FIXTURE_DIR) /
        name;
}

void WriteTextFile(
    const std::filesystem::path& path,
    std::string_view contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    Require(stream.good(), "could not create metadata fixture");
    stream << contents;
    Require(stream.good(), "could not write metadata fixture");
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "could not read metadata fixture");
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

std::string Schema4Metadata(
    std::string_view deployment = {},
    std::string_view compiler_version = "19.44")
{
    std::string document =
        "{\n"
        "  \"schema_version\": 4,\n"
        "  \"product\": {\n"
        "    \"name\": \"SpecForge\",\n"
        "    \"version\": \"0.4.1\"\n"
        "  },\n"
        "  \"build\": {\n"
        "    \"source_mode\": \"working_tree\",\n"
        "    \"source_revision\": null,\n"
        "    \"configuration\": \"Debug\",\n"
        "    \"compiler_id\": \"MSVC\",\n"
        "    \"compiler_version\": \"" +
        std::string(compiler_version) +
        "\",\n"
        "    \"cmake_version\": \"4.1.0\",\n"
        "    \"generator\": \"Ninja\",\n"
        "    \"target_architecture\": \"amd64\",\n"
        "    \"windows_sdk_version\": null,\n"
        "    \"dear_imgui\": \"1.92.5\",\n"
        "    \"implot\": \"0.17\",\n"
        "    \"zlib\": \"1.3.1\"\n"
        "  }";
    if (!deployment.empty()) {
        document += ",\n  \"deployment\": ";
        document += deployment;
    }
    document += "\n}\n";
    return document;
}

std::string Schema5Metadata(
    std::string_view completed_at_json =
        R"("2026-08-05T09:21:32Z")",
    std::string_view artifact_json =
        R"({"file":"SpecForge.exe","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})",
    bool include_completed_at = true,
    bool include_artifact = true,
    std::string_view deployment = {})
{
    if (completed_at_json.empty()) {
        completed_at_json = R"("2026-08-05T09:21:32Z")";
    }
    if (artifact_json.empty()) {
        artifact_json =
            R"({"file":"SpecForge.exe","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})";
    }
    std::string document =
        "{\n"
        "  \"schema_version\": 5,\n"
        "  \"product\": {\n"
        "    \"name\": \"SpecForge\",\n"
        "    \"version\": \"0.4.1\"\n"
        "  },\n"
        "  \"build\": {\n"
        "    \"source_mode\": \"working_tree\",\n"
        "    \"source_revision\": null,\n"
        "    \"configuration\": \"Debug\",\n"
        "    \"compiler_id\": \"MSVC\",\n"
        "    \"compiler_version\": \"19.44\",\n"
        "    \"cmake_version\": \"4.1.0\",\n"
        "    \"generator\": \"Ninja\",\n"
        "    \"target_architecture\": \"amd64\",\n"
        "    \"windows_sdk_version\": null,\n"
        "    \"dear_imgui\": \"1.92.5\",\n"
        "    \"implot\": \"0.17\",\n"
        "    \"zlib\": \"1.3.1\"";
    if (include_completed_at) {
        document +=
            ",\n    \"completed_at_utc\": " +
            std::string(completed_at_json);
    }
    document += "\n  }";
    if (include_artifact) {
        document +=
            ",\n  \"artifact\": " +
            std::string(artifact_json);
    }
    if (!deployment.empty()) {
        document +=
            ",\n  \"deployment\": " +
            std::string(deployment);
    }
    document += "\n}\n";
    return document;
}

void RequireDefaultDeployment(
    const specforge::SpecForgeMetadataReadResult& result,
    std::string_view description)
{
    Require(
        !result.startup_error,
        std::string(description) +
            " should not produce a startup error");
    Require(
        result.deployment.distribution ==
                specforge::Distribution::Standalone &&
            result.deployment.storage_profile ==
                specforge::StorageProfile::LocalAppData,
        std::string(description) +
            " should select Standalone with LocalAppData storage");
}

void TestMissingMetadataDefaultsToStandalone()
{
    const specforge::SpecForgeMetadataReadResult result =
        specforge::ReadSpecForgeMetadata(
            FixturePath("does-not-exist.json"),
            WorkingTreeIdentity());
    RequireDefaultDeployment(result, "missing metadata");
    Require(
        result.build_metadata.status ==
                specforge::BuildMetadataStatus::Unavailable &&
            !result.build_metadata.metadata,
        "missing metadata should make build provenance unavailable");
}

void TestSchema3Compatibility()
{
    const specforge::SpecForgeMetadataReadResult portable =
        specforge::ReadSpecForgeMetadata(
            FixturePath("available-working-tree.json"),
            WorkingTreeIdentity());
    Require(
        !portable.startup_error &&
            portable.deployment.distribution ==
                specforge::Distribution::Standalone &&
            portable.deployment.storage_profile ==
                specforge::StorageProfile::Portable,
        "schema 3 Portable should retain package-local storage "
        "without inventing a distribution");
    Require(
        portable.build_metadata.status ==
                specforge::BuildMetadataStatus::Available &&
            portable.build_metadata.metadata,
        "matching schema 3 build provenance should normalize legacy x64 "
        "to amd64");

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-metadata-schema3-installed";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path path = root / "metadata.json";
    const std::string installed =
        "{\n"
        "  \"schema_version\": 3,\n"
        "  \"source_mode\": \"working_tree\",\n"
        "  \"source_revision\": null,\n"
        "  \"specforge_version\": \"0.4.1\",\n"
        "  \"release_profile\": \"Installed\",\n"
        "  \"configuration\": \"Debug\",\n"
        "  \"compiler_id\": \"MSVC\",\n"
        "  \"compiler_version\": \"19.44\",\n"
        "  \"cmake_version\": \"4.1.0\",\n"
        "  \"generator\": \"Ninja\",\n"
        "  \"target_architecture\": \"x64\",\n"
        "  \"windows_sdk_version\": null,\n"
        "  \"dear_imgui\": \"1.92.5\",\n"
        "  \"implot\": \"0.17\",\n"
        "  \"zlib\": \"1.3.1\"\n"
        "}\n";
    WriteTextFile(path, installed);
    const specforge::SpecForgeMetadataReadResult local_app_data =
        specforge::ReadSpecForgeMetadata(
            path,
            WorkingTreeIdentity());
    RequireDefaultDeployment(
        local_app_data,
        "schema 3 Installed metadata");
    std::filesystem::remove_all(root, cleanup_error);
}

void TestSchema3BuildValidationRemainsIndependent()
{
    struct Case {
        std::string_view fixture;
        bool startup_error;
    };
    const std::vector<Case> cases = {
        {"invalid-json.json", true},
        {"unsupported-schema.json", true},
        {"missing-field.json", false},
        {"wrong-type.json", false},
        {"working-tree-empty-revision.json", false},
        {"empty-required-string.json", false},
        {"whitespace-required-string.json", false},
        {"padded-required-string.json", false},
        {"invalid-toolchain-format.json", false},
        {"working-tree-nonempty-revision.json", false},
        {"unknown-source-mode.json", false},
        {"head-null-revision.json", false},
        {"head-short-revision.json", false},
        {"head-invalid-revision.json", false},
    };

    for (const Case& test_case : cases) {
        const specforge::SpecForgeMetadataReadResult result =
            specforge::ReadSpecForgeMetadata(
                FixturePath(test_case.fixture),
                WorkingTreeIdentity());
        Require(
            result.startup_error.has_value() ==
                test_case.startup_error,
            std::string(test_case.fixture) +
                " should classify startup validity correctly");
        Require(
            result.build_metadata.status ==
                    specforge::BuildMetadataStatus::Unavailable &&
                !result.build_metadata.metadata,
            std::string(test_case.fixture) +
                " should make malformed build provenance unavailable");
        if (!test_case.startup_error) {
            Require(
                result.deployment.storage_profile ==
                    specforge::StorageProfile::Portable,
                std::string(test_case.fixture) +
                    " should preserve its valid schema 3 storage mapping");
        }
    }
}

void TestSchema4DeploymentSelection()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-metadata-schema4-deployment";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path path = root / "specforge_metadata.json";

    WriteTextFile(path, Schema4Metadata());
    const specforge::SpecForgeMetadataReadResult standalone =
        specforge::ReadSpecForgeMetadata(
            path,
            WorkingTreeIdentity());
    RequireDefaultDeployment(
        standalone,
        "schema 4 without deployment");
    Require(
        standalone.build_metadata.status ==
                specforge::BuildMetadataStatus::Available,
        "schema 4 without deployment should still expose build provenance");
    Require(
        standalone.build_metadata.metadata &&
            !standalone.build_metadata.metadata->finalized_artifact,
        "schema 4 should not invent schema 5 finalized-artifact fields");

    WriteTextFile(
        path,
        Schema4Metadata(
            R"({"distribution":"portable","storage_profile":"portable"})"));
    const specforge::SpecForgeMetadataReadResult portable =
        specforge::ReadSpecForgeMetadata(
            path,
            WorkingTreeIdentity());
    Require(
        !portable.startup_error &&
            portable.deployment.distribution ==
                specforge::Distribution::Portable &&
            portable.deployment.storage_profile ==
                specforge::StorageProfile::Portable,
        "schema 4 Portable should select package-local Data");

    WriteTextFile(
        path,
        Schema4Metadata(
            R"({"distribution":"winget","storage_profile":"local_app_data"})"));
    const specforge::SpecForgeMetadataReadResult winget =
        specforge::ReadSpecForgeMetadata(
            path,
            WorkingTreeIdentity());
    Require(
        !winget.startup_error &&
            winget.deployment.distribution ==
                specforge::Distribution::WinGet &&
            winget.deployment.storage_profile ==
                specforge::StorageProfile::LocalAppData,
        "schema 4 WinGet should retain LocalAppData storage");

    for (const auto& [value, expected] :
         std::vector<std::pair<std::string_view, specforge::Distribution>>{
             {"installer", specforge::Distribution::Installer},
             {"scoop", specforge::Distribution::Scoop},
         }) {
        WriteTextFile(
            path,
            Schema4Metadata(
                "{\"distribution\":\"" + std::string(value) +
                "\",\"storage_profile\":\"local_app_data\"}"));
        const specforge::SpecForgeMetadataReadResult result =
            specforge::ReadSpecForgeMetadata(
                path,
                WorkingTreeIdentity());
        Require(
            !result.startup_error &&
                result.deployment.distribution == expected,
            std::string(value) +
                " should map to its display identity");
    }

    std::filesystem::remove_all(root, cleanup_error);
}

void TestSchema5StrictParsing()
{
    const specforge::SpecForgeMetadataReadResult fixture =
        specforge::ReadSpecForgeMetadata(
            FixturePath("available-schema5-working-tree.json"),
            WorkingTreeIdentity());
    RequireDefaultDeployment(fixture, "schema 5 fixture");
    Require(
        fixture.build_metadata.status ==
                specforge::BuildMetadataStatus::Available &&
            fixture.build_metadata.metadata,
        "matching schema 5 build provenance should be available");
    const specforge::BuildMetadata& metadata =
        *fixture.build_metadata.metadata;
    Require(
        metadata.finalized_artifact &&
            metadata.finalized_artifact->completed_at_utc ==
                "2026-08-05T09:21:32Z" &&
            metadata.finalized_artifact->artifact.file == "SpecForge.exe" &&
            metadata.finalized_artifact->artifact.sha256 ==
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "schema 5 should expose the finalized timestamp and artifact identity");

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-metadata-schema5-strict";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path path = root / "metadata.json";

    WriteTextFile(
        path,
        Schema5Metadata(R"("2024-02-29T23:59:59Z")"));
    const specforge::SpecForgeMetadataReadResult leap_day =
        specforge::ReadSpecForgeMetadata(
            path,
            WorkingTreeIdentity());
    Require(
        !leap_day.startup_error &&
            leap_day.build_metadata.status ==
                specforge::BuildMetadataStatus::Available &&
            leap_day.build_metadata.metadata &&
            leap_day.build_metadata.metadata->finalized_artifact &&
            leap_day.build_metadata.metadata->finalized_artifact
                    ->completed_at_utc ==
                "2024-02-29T23:59:59Z",
        "schema 5 should accept the valid leap day 2024-02-29");

    const std::vector<std::string> invalid_documents = {
        Schema5Metadata({}, {}, false, true),
        Schema5Metadata({}, {}, true, false),
        Schema5Metadata("false"),
        Schema5Metadata(R"("2026-02-29T00:00:00Z")"),
        Schema5Metadata(R"("2024-04-31T00:00:00Z")"),
        Schema5Metadata(R"("2024-01-01T24:00:00Z")"),
        Schema5Metadata(R"("2024-01-01T00:00:00+00:00")"),
        Schema5Metadata(R"("2024-01-01T00:00:00.000Z")"),
        Schema5Metadata(
            R"("2024-02-29T23:59:59Z")",
            R"({"file":"specforge.exe","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})"),
        Schema5Metadata(
            {},
            R"({"file":"SpecForge.exe","sha256":"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"})"),
        Schema5Metadata(
            {},
            R"({"file":"SpecForge.exe","sha256":"0123456789abcdef"})"),
        Schema5Metadata(
            {},
            R"({"file":"SpecForge.exe","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg"})"),
        Schema5Metadata(
            {},
            R"({"file":false,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})"),
        Schema5Metadata({}, "false"),
    };
    for (const std::string& document : invalid_documents) {
        WriteTextFile(path, document);
        const specforge::SpecForgeMetadataReadResult result =
            specforge::ReadSpecForgeMetadata(
                path,
                WorkingTreeIdentity());
        Require(
            !result.startup_error &&
                result.build_metadata.status ==
                    specforge::BuildMetadataStatus::Unavailable &&
                !result.build_metadata.metadata,
            "malformed schema 5 build fields should make provenance unavailable without changing deployment");
    }

    std::filesystem::remove_all(root, cleanup_error);
}

void TestInvalidDeploymentFailsClosed()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-metadata-invalid-deployment";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path path = root / "specforge_metadata.json";

    const std::vector<std::string> invalid_deployments = {
        "null",
        "{}",
        R"({"storage_profile":"portable"})",
        R"({"distribution":"portable"})",
        R"({"distribution":7,"storage_profile":"portable"})",
        R"({"distribution":"portable","storage_profile":false})",
        R"({"distribution":"unknown","storage_profile":"portable"})",
        R"({"distribution":"portable","storage_profile":"unknown"})",
    };
    for (const std::string& deployment : invalid_deployments) {
        WriteTextFile(path, Schema4Metadata(deployment));
        const specforge::SpecForgeMetadataReadResult result =
            specforge::ReadSpecForgeMetadata(
                path,
                WorkingTreeIdentity());
        Require(
            result.startup_error.has_value(),
            "invalid deployment should fail closed: " + deployment);
        Require(
            result.startup_error->find("Invalid SpecForge metadata") !=
                std::string::npos,
            "invalid deployment should produce a diagnostic error");
    }

    std::filesystem::remove_all(root, cleanup_error);
}

void TestBuildProvenanceDoesNotControlDeployment()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-metadata-provenance-boundary";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path path = root / "specforge_metadata.json";
    const std::string portable_deployment =
        R"({"distribution":"portable","storage_profile":"portable"})";

    WriteTextFile(path, Schema4Metadata(portable_deployment));
    specforge::BuildIdentity mismatched_identity =
        WorkingTreeIdentity();
    mismatched_identity.specforge_version = "9.9.9";
    const specforge::SpecForgeMetadataReadResult mismatch =
        specforge::ReadSpecForgeMetadata(
            path,
            mismatched_identity);
    Require(
        !mismatch.startup_error &&
            mismatch.deployment.storage_profile ==
                specforge::StorageProfile::Portable &&
            mismatch.build_metadata.status ==
                specforge::BuildMetadataStatus::Mismatch,
        "build identity mismatch must not discard valid storage selection");

    WriteTextFile(
        path,
        Schema4Metadata(portable_deployment, "latest"));
    const specforge::SpecForgeMetadataReadResult unavailable =
        specforge::ReadSpecForgeMetadata(
            path,
            WorkingTreeIdentity());
    Require(
        !unavailable.startup_error &&
            unavailable.deployment.storage_profile ==
                specforge::StorageProfile::Portable &&
            unavailable.build_metadata.status ==
                specforge::BuildMetadataStatus::Unavailable,
        "malformed build provenance must not discard valid storage selection");

    WriteTextFile(
        path,
        Schema5Metadata(
            {},
            R"({"file":"SpecForge.exe","sha256":"0000000000000000000000000000000000000000000000000000000000000000"})",
            true,
            true,
            portable_deployment));
    const specforge::SpecForgeMetadataReadResult artifact_mismatch =
        specforge::ReadSpecForgeMetadata(
            path,
            WorkingTreeIdentity());
    Require(
        !artifact_mismatch.startup_error &&
            artifact_mismatch.deployment.storage_profile ==
                specforge::StorageProfile::Portable &&
            artifact_mismatch.build_metadata.status ==
                specforge::BuildMetadataStatus::Available &&
            artifact_mismatch.build_metadata.metadata &&
            artifact_mismatch.build_metadata.metadata->finalized_artifact &&
            artifact_mismatch.build_metadata.metadata->finalized_artifact
                    ->artifact.sha256 ==
                "0000000000000000000000000000000000000000000000000000000000000000",
        "an artifact digest mismatch must not discard valid Portable deployment selection");

    std::filesystem::remove_all(root, cleanup_error);
}

void TestAdjacentMetadataSelectionAndLegacyFallback()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-adjacent-metadata-selection";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);

    WriteTextFile(
        root / "specforge_build_metadata.json",
        ReadTextFile(
            FixturePath("available-working-tree.json")));
    const specforge::SpecForgeMetadataReadResult legacy =
        specforge::ReadAdjacentSpecForgeMetadata(
            root,
            WorkingTreeIdentity());
    Require(
        !legacy.startup_error &&
            legacy.deployment.storage_profile ==
                specforge::StorageProfile::Portable &&
            legacy.metadata_path.filename() ==
                "specforge_build_metadata.json",
        "legacy filename should preserve old Portable packages");

    WriteTextFile(
        root / "specforge_metadata.json",
        Schema4Metadata());
    const specforge::SpecForgeMetadataReadResult current =
        specforge::ReadAdjacentSpecForgeMetadata(
            root,
            WorkingTreeIdentity());
    Require(
        !current.startup_error &&
            current.deployment.storage_profile ==
                specforge::StorageProfile::LocalAppData &&
            current.metadata_path.filename() ==
                "specforge_metadata.json",
        "current metadata filename should take precedence over legacy metadata");

    WriteTextFile(
        root / "specforge_metadata.json",
        "{ invalid");
    const specforge::SpecForgeMetadataReadResult invalid_current =
        specforge::ReadAdjacentSpecForgeMetadata(
            root,
            WorkingTreeIdentity());
    Require(
        invalid_current.startup_error.has_value() &&
            invalid_current.metadata_path.filename() ==
                "specforge_metadata.json",
        "invalid current metadata must not fall back to a legacy declaration");

    WriteTextFile(
        root / "specforge_metadata.json",
        ReadTextFile(
            FixturePath("available-working-tree.json")));
    const specforge::SpecForgeMetadataReadResult schema3_current =
        specforge::ReadAdjacentSpecForgeMetadata(
            root,
            WorkingTreeIdentity());
    Require(
        schema3_current.startup_error.has_value() &&
            schema3_current.startup_error->find("schema 4") !=
                std::string::npos,
        "the current metadata filename should reject legacy schema 3");

    WriteTextFile(
        root / "specforge_metadata.json",
        ReadTextFile(
            FixturePath("available-schema5-working-tree.json")));
    const specforge::SpecForgeMetadataReadResult schema5_current =
        specforge::ReadAdjacentSpecForgeMetadata(
            root,
            WorkingTreeIdentity());
    Require(
        !schema5_current.startup_error &&
            schema5_current.build_metadata.status ==
                specforge::BuildMetadataStatus::Available,
        "the current metadata filename should accept schema 5");

    std::filesystem::remove(
        root / "specforge_metadata.json",
        cleanup_error);
    WriteTextFile(
        root / "specforge_build_metadata.json",
        Schema4Metadata());
    const specforge::SpecForgeMetadataReadResult schema4_legacy =
        specforge::ReadAdjacentSpecForgeMetadata(
            root,
            WorkingTreeIdentity());
    Require(
        schema4_legacy.startup_error.has_value() &&
            schema4_legacy.startup_error->find("schema 3") !=
                std::string::npos,
        "the legacy metadata filename should accept only schema 3");

    std::filesystem::remove_all(root, cleanup_error);
}

void TestMetadataNames()
{
    Require(
        std::string_view(specforge::DistributionName(
            specforge::Distribution::Standalone)) == "Standalone" &&
            std::string_view(specforge::DistributionName(
                specforge::Distribution::Installer)) == "Installer" &&
            std::string_view(specforge::DistributionName(
                specforge::Distribution::WinGet)) == "WinGet" &&
            std::string_view(specforge::DistributionName(
                specforge::Distribution::Portable)) == "Portable" &&
            std::string_view(specforge::DistributionName(
                specforge::Distribution::Scoop)) == "Scoop",
        "distribution display names should match the product vocabulary");
    Require(
        std::string_view(specforge::StorageProfileName(
            specforge::StorageProfile::Portable)) == "portable" &&
            std::string_view(specforge::StorageProfileName(
                specforge::StorageProfile::LocalAppData)) ==
                "local_app_data",
        "storage profile names should match schema 4 values");
}

void TestStartupPreflightRejectsInvalidMetadataBeforeStateConstruction()
{
    static_assert(
        !std::is_default_constructible_v<
            specforge::SpecForgeStartup>);

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-startup-preflight-tests";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);

    const std::filesystem::path executable_path =
        root / "SpecForge.exe";
    const std::filesystem::path state_root =
        root / "local-user-state";
    WriteTextFile(
        root / "specforge_metadata.json",
        "{ invalid metadata");

    bool startup_rejected = false;
    try {
        (void)specforge::PrepareSpecForgeStartup({
            .executable_path = executable_path,
            .local_app_data_user_state_root = state_root,
        });
    } catch (const std::runtime_error&) {
        startup_rejected = true;
    }

    Require(
        startup_rejected,
        "invalid adjacent metadata must not produce validated "
        "startup facts");
    Require(
        !std::filesystem::exists(state_root),
        "startup preflight must not enter local-state "
        "construction");

    std::filesystem::remove(
        root / "specforge_metadata.json",
        cleanup_error);
    const specforge::SpecForgeStartup startup =
        specforge::PrepareSpecForgeStartup({
            .executable_path = executable_path,
            .local_app_data_user_state_root = state_root,
        });
    Require(
        startup.runtime_paths().executable_path ==
                executable_path &&
            startup.runtime_paths().local_user_state_root ==
                state_root &&
            startup.runtime_paths().source_session_state_path ==
                state_root / "source-session.json" &&
            startup.runtime_paths().spectrum_view_state_path ==
                state_root / "spectrum-view-state.json" &&
            startup.runtime_paths().sample_workflow_state_path ==
                state_root / "sample-workflow-state.json" &&
            startup.runtime_paths().spectral_line_user_state_path ==
                state_root /
                    "spectral-line-grouping-views.json",
        "validated startup facts should retain the one resolved "
        "executable and complete state-path decision");
    Require(
        !std::filesystem::exists(state_root),
        "preflight should decide paths without materializing "
        "local state");

    std::filesystem::remove_all(root, cleanup_error);
}

}  // namespace

int main()
{
    TestMissingMetadataDefaultsToStandalone();
    TestSchema3Compatibility();
    TestSchema3BuildValidationRemainsIndependent();
    TestSchema4DeploymentSelection();
    TestSchema5StrictParsing();
    TestInvalidDeploymentFailsClosed();
    TestBuildProvenanceDoesNotControlDeployment();
    TestAdjacentMetadataSelectionAndLegacyFallback();
    TestMetadataNames();
    TestStartupPreflightRejectsInvalidMetadataBeforeStateConstruction();
    std::cout << "SpecForge metadata tests passed\n";
    return 0;
}
