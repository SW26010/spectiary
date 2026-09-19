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
#ifndef SPECFORGE_EXPECTED_CFITSIO_VERSION
#error "SPECFORGE_EXPECTED_CFITSIO_VERSION must be configured."
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
        .product_name = "Spectiary",
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

std::string Schema6Metadata(
    std::string_view completed_at_json =
        R"("2026-08-05T09:21:32Z")",
    std::string_view artifact_json =
        R"({"file":"Spectiary.exe","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})",
    bool include_completed_at = true,
    bool include_artifact = true,
    std::string_view deployment = {})
{
    if (completed_at_json.empty()) {
        completed_at_json = R"("2026-08-05T09:21:32Z")";
    }
    if (artifact_json.empty()) {
        artifact_json =
            R"({"file":"Spectiary.exe","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})";
    }
    std::string document =
        "{\n"
        "  \"schema_version\": 6,\n"
        "  \"application_id\": \"0238d5bf7b34bb99c006f9807537d31234ca2e3d\",\n"
        "  \"product\": {\n"
        "    \"name\": \"Spectiary\",\n"
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
        "    \"cfitsio\": \"4.6.4\",\n"
        "    \"yaml_cpp\": \"0.9.0\",\n"
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

std::string CurrentMetadata(std::string_view deployment = {}, std::string_view compiler_version = "19.44")
{
    std::string document = Schema6Metadata({}, {}, true, true, deployment);
    const auto offset = document.find("19.44");
    document.replace(offset, 5, compiler_version);
    return document;
}

void TestIdentitySeparationAndSchemaCutover()
{
    const auto root = std::filesystem::temp_directory_path() / "spectiary-identity-cutover";
    std::filesystem::create_directories(root);
    const auto path = root / "spectiary_metadata.json";
    auto identity = WorkingTreeIdentity();
    Require(identity.application_id == "0238d5bf7b34bb99c006f9807537d31234ca2e3d",
        "application identity must retain the entire founding seed");
    WriteTextFile(path, Schema6Metadata());
    identity.product_name = "A completely different display name";
    Require(specforge::ReadSpecForgeMetadata(path, identity).build_metadata.status ==
        specforge::BuildMetadataStatus::Available, "display name must not control machine identity");
    identity.application_id = "wrong-application";
    Require(specforge::ReadSpecForgeMetadata(path, identity).build_metadata.status ==
        specforge::BuildMetadataStatus::Mismatch, "application identity must be matched independently");
    auto renamed = Schema6Metadata();
    renamed.replace(renamed.find("Spectiary\""), 9, "Other product");
    WriteTextFile(path, renamed);
    Require(specforge::ReadSpecForgeMetadata(path, WorkingTreeIdentity()).build_metadata.status ==
        specforge::BuildMetadataStatus::Available, "sidecar display text is not machine identity");
    for (const auto invalid_identity : {"", "SpecForge", "0238d5bf7b34bb99"}) {
        auto document = CurrentMetadata(R"({"distribution":"portable","storage_profile":"portable"})");
        document.replace(document.find(specforge::project_identity::kApplicationId), 40, invalid_identity);
        WriteTextFile(path, document);
        const auto result = specforge::ReadSpecForgeMetadata(path, WorkingTreeIdentity());
        Require(!result.startup_error && result.deployment.storage_profile == specforge::StorageProfile::Portable &&
            result.build_metadata.status != specforge::BuildMetadataStatus::Available,
            "invalid application identity must reject provenance independently of deployment");
    }
    for (const int version : {3, 4, 5, 7}) {
        auto document = Schema6Metadata();
        const auto offset = document.find("\"schema_version\": 6");
        document.replace(offset, std::string("\"schema_version\": 6").size(),
            "\"schema_version\": " + std::to_string(version));
        WriteTextFile(path, document);
        Require(specforge::ReadSpecForgeMetadata(path, WorkingTreeIdentity()).startup_error.has_value(),
            "retired and unknown metadata schemas must be rejected");
    }
    std::filesystem::remove(path);
    WriteTextFile(root / "specforge_build_metadata.json", Schema6Metadata());
    WriteTextFile(root / "specforge_metadata.json", Schema6Metadata());
    Require(specforge::ReadAdjacentSpecForgeMetadata(root, WorkingTreeIdentity()).metadata_path.empty(),
        "retired filenames must not be active aliases");
    WriteTextFile(path, Schema6Metadata());
    Require(specforge::ReadAdjacentSpecForgeMetadata(root, WorkingTreeIdentity()).metadata_path == path,
        "discovery must use the explicit sidecar contract");
    std::filesystem::remove_all(root);
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

void TestCurrentDeploymentSelection()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-metadata-schema4-deployment";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path path = root / "spectiary_metadata.json";

    WriteTextFile(path, CurrentMetadata());
    const specforge::SpecForgeMetadataReadResult standalone =
        specforge::ReadSpecForgeMetadata(
            path,
            WorkingTreeIdentity());
    RequireDefaultDeployment(
        standalone,
        "schema 6 without deployment");
    Require(
        standalone.build_metadata.status ==
                specforge::BuildMetadataStatus::Available,
        "schema 6 without deployment should still expose build provenance");
    Require(
        standalone.build_metadata.metadata &&
            standalone.build_metadata.metadata->finalized_artifact.has_value(),
        "schema 6 should not invent schema 6 finalized-artifact fields");

    WriteTextFile(
        path,
        CurrentMetadata(
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
        "schema 6 Portable should select package-local Data");

    WriteTextFile(
        path,
        CurrentMetadata(
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
        "schema 6 WinGet should retain LocalAppData storage");

    for (const auto& [value, expected] :
         std::vector<std::pair<std::string_view, specforge::Distribution>>{
             {"installer", specforge::Distribution::Installer},
             {"scoop", specforge::Distribution::Scoop},
         }) {
        WriteTextFile(
            path,
            CurrentMetadata(
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

void TestSchema6StrictParsing()
{
    const specforge::SpecForgeMetadataReadResult fixture =
        specforge::ReadSpecForgeMetadata(
            FixturePath("available-schema6-working-tree.json"),
            WorkingTreeIdentity());
    RequireDefaultDeployment(fixture, "schema 6 fixture");
    Require(
        fixture.build_metadata.status ==
                specforge::BuildMetadataStatus::Available &&
            fixture.build_metadata.metadata,
        "matching schema 6 build provenance should be available");
    const specforge::BuildMetadata& metadata =
        *fixture.build_metadata.metadata;
    Require(
        metadata.cfitsio_version == "4.6.4" &&
            metadata.yaml_cpp_version == "0.9.0" &&
            metadata.finalized_artifact &&
            metadata.finalized_artifact->completed_at_utc ==
                "2026-08-05T09:21:32Z" &&
            metadata.finalized_artifact->artifact.file == "Spectiary.exe" &&
            metadata.finalized_artifact->artifact.sha256 ==
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "schema 6 should expose dependency, timestamp, and artifact identity");

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "specforge-metadata-schema6-strict";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    const std::filesystem::path path = root / "metadata.json";

    std::string legacy_schema6 = Schema6Metadata();
    const std::size_t yaml_cpp_begin =
        legacy_schema6.find("    \"yaml_cpp\": ");
    Require(
        yaml_cpp_begin != std::string::npos,
        "schema 6 compatibility fixture should contain yaml_cpp before mutation");
    const std::size_t yaml_cpp_end =
        legacy_schema6.find('\n', yaml_cpp_begin);
    Require(
        yaml_cpp_end != std::string::npos,
        "schema 6 yaml_cpp fixture line should terminate");
    legacy_schema6.erase(
        yaml_cpp_begin,
        yaml_cpp_end - yaml_cpp_begin + 1U);
    WriteTextFile(path, legacy_schema6);
    const specforge::SpecForgeMetadataReadResult legacy_without_yaml_cpp =
        specforge::ReadSpecForgeMetadata(path, WorkingTreeIdentity());
    Require(!legacy_without_yaml_cpp.startup_error &&
        legacy_without_yaml_cpp.build_metadata.status == specforge::BuildMetadataStatus::Unavailable,
        "schema 6 requires yaml_cpp");

    std::string legacy_without_cfitsio_document = Schema6Metadata();
    const std::size_t cfitsio_begin =
        legacy_without_cfitsio_document.find("    \"cfitsio\": ");
    Require(
        cfitsio_begin != std::string::npos,
        "schema 6 compatibility fixture should contain cfitsio before mutation");
    const std::size_t cfitsio_end =
        legacy_without_cfitsio_document.find('\n', cfitsio_begin);
    Require(
        cfitsio_end != std::string::npos,
        "schema 6 cfitsio fixture line should terminate");
    legacy_without_cfitsio_document.erase(
        cfitsio_begin,
        cfitsio_end - cfitsio_begin + 1U);
    WriteTextFile(path, legacy_without_cfitsio_document);
    const specforge::SpecForgeMetadataReadResult legacy_without_cfitsio =
        specforge::ReadSpecForgeMetadata(path, WorkingTreeIdentity());
    Require(!legacy_without_cfitsio.startup_error &&
        legacy_without_cfitsio.build_metadata.status == specforge::BuildMetadataStatus::Unavailable,
        "schema 6 requires cfitsio");

    for (std::string_view malformed_value : {
             std::string_view{"\"\""},
             std::string_view{"\"4.6.x\""},
         }) {
        std::string malformed_cfitsio = Schema6Metadata();
        const std::size_t cfitsio_value =
            malformed_cfitsio.find("\"cfitsio\": \"4.6.4\"");
        Require(
            cfitsio_value != std::string::npos,
            "schema 6 malformed cfitsio fixture should contain its value");
        malformed_cfitsio.replace(
            cfitsio_value,
            std::string_view("\"cfitsio\": \"4.6.4\"").size(),
            "\"cfitsio\": " + std::string(malformed_value));
        WriteTextFile(path, malformed_cfitsio);
        const specforge::SpecForgeMetadataReadResult malformed_cfitsio_result =
            specforge::ReadSpecForgeMetadata(path, WorkingTreeIdentity());
        Require(
            !malformed_cfitsio_result.startup_error &&
                malformed_cfitsio_result.build_metadata.status ==
                    specforge::BuildMetadataStatus::Unavailable &&
                !malformed_cfitsio_result.build_metadata.metadata,
            "present but empty or malformed cfitsio provenance must be unavailable");
    }

    std::string malformed_yaml_cpp = Schema6Metadata();
    const std::size_t yaml_cpp_value =
        malformed_yaml_cpp.find("\"yaml_cpp\": \"0.9.0\"");
    Require(
        yaml_cpp_value != std::string::npos,
        "schema 6 malformed yaml_cpp fixture should contain its value");
    malformed_yaml_cpp.replace(
        yaml_cpp_value,
        std::string_view("\"yaml_cpp\": \"0.9.0\"").size(),
        "\"yaml_cpp\": false");
    WriteTextFile(path, malformed_yaml_cpp);
    const specforge::SpecForgeMetadataReadResult malformed_yaml_cpp_result =
        specforge::ReadSpecForgeMetadata(path, WorkingTreeIdentity());
    Require(
        !malformed_yaml_cpp_result.startup_error &&
            malformed_yaml_cpp_result.build_metadata.status ==
                specforge::BuildMetadataStatus::Unavailable &&
            !malformed_yaml_cpp_result.build_metadata.metadata,
        "present but malformed yaml_cpp provenance must remain unavailable");

    WriteTextFile(
        path,
        Schema6Metadata(R"("2024-02-29T23:59:59Z")"));
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
        "schema 6 should accept the valid leap day 2024-02-29");

    const std::vector<std::string> invalid_documents = {
        Schema6Metadata({}, {}, false, true),
        Schema6Metadata({}, {}, true, false),
        Schema6Metadata("false"),
        Schema6Metadata(R"("2026-02-29T00:00:00Z")"),
        Schema6Metadata(R"("2024-04-31T00:00:00Z")"),
        Schema6Metadata(R"("2024-01-01T24:00:00Z")"),
        Schema6Metadata(R"("2024-01-01T00:00:00+00:00")"),
        Schema6Metadata(R"("2024-01-01T00:00:00.000Z")"),
        Schema6Metadata(
            R"("2024-02-29T23:59:59Z")",
            R"({"file":"specforge.exe","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})"),
        Schema6Metadata(
            {},
            R"({"file":"Spectiary.exe","sha256":"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"})"),
        Schema6Metadata(
            {},
            R"({"file":"Spectiary.exe","sha256":"0123456789abcdef"})"),
        Schema6Metadata(
            {},
            R"({"file":"Spectiary.exe","sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg"})"),
        Schema6Metadata(
            {},
            R"({"file":false,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})"),
        Schema6Metadata({}, "false"),
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
            "malformed schema 6 build fields should make provenance unavailable without changing deployment");
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
    const std::filesystem::path path = root / "spectiary_metadata.json";

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
        WriteTextFile(path, CurrentMetadata(deployment));
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
    const std::filesystem::path path = root / "spectiary_metadata.json";
    const std::string portable_deployment =
        R"({"distribution":"portable","storage_profile":"portable"})";

    WriteTextFile(path, CurrentMetadata(portable_deployment));
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
        CurrentMetadata(portable_deployment, "latest"));
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
        Schema6Metadata(
            {},
            R"({"file":"Spectiary.exe","sha256":"0000000000000000000000000000000000000000000000000000000000000000"})",
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
        "storage profile names should match schema 6 values");
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
        root / "Spectiary.exe";
    const std::filesystem::path state_root =
        root / "local-user-state";
    WriteTextFile(
        root / "spectiary_metadata.json",
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
        root / "spectiary_metadata.json",
        cleanup_error);
    const specforge::SpecForgeStartup startup =
        specforge::PrepareSpecForgeStartup({
            .executable_path = executable_path,
            .local_app_data_user_state_root = state_root,
        });
    Require(
        startup.runtime_paths().executable_path ==
                executable_path &&
            startup.runtime_paths().application_data_root ==
                state_root &&
            startup.runtime_paths().source_session_state_path ==
                state_root / "state" / "source-session.json" &&
            startup.runtime_paths().legacy_spectrum_view_state_path.empty() &&
            startup.runtime_paths().spectrum_plot_preferences_path ==
                state_root / "config" / "spectrum-plot-preferences.json" &&
            startup.runtime_paths().spectrum_viewport_state_path ==
                state_root / "state" / "spectrum-viewport-state.json" &&
            startup.runtime_paths().sample_workflow_state_path ==
                state_root / "state" / "sample-workflow-state.json" &&
            startup.runtime_paths().spectral_line_user_state_path ==
                state_root / "state" /
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


    TestCurrentDeploymentSelection();
    TestSchema6StrictParsing();
    TestInvalidDeploymentFailsClosed();
    TestBuildProvenanceDoesNotControlDeployment();
    TestIdentitySeparationAndSchemaCutover();
    TestMetadataNames();
    TestStartupPreflightRejectsInvalidMetadataBeforeStateConstruction();
    std::cout << "SpecForge metadata tests passed\n";
    return 0;
}
