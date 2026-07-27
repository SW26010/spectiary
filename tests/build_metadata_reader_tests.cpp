#include "app/build_metadata_reader.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifndef SPECFORGE_BUILD_METADATA_FIXTURE_DIR
#error "SPECFORGE_BUILD_METADATA_FIXTURE_DIR must be configured."
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
        .specforge_version = "0.4.1",
        .release_profile = "Portable",
        .configuration = "Debug",
        .target_architecture = "x64",
        .source_mode = "working_tree",
        .source_revision = "",
    };
}

std::filesystem::path FixturePath(std::string_view name)
{
    return std::filesystem::path(SPECFORGE_BUILD_METADATA_FIXTURE_DIR) /
        name;
}

void TestFileFixtures()
{
    struct Case {
        std::string_view fixture;
        specforge::BuildMetadataStatus expected_status;
    };
    const std::vector<Case> cases = {
        {"available-working-tree.json",
         specforge::BuildMetadataStatus::Available},
        {"invalid-json.json",
         specforge::BuildMetadataStatus::Unavailable},
        {"unsupported-schema.json",
         specforge::BuildMetadataStatus::Unavailable},
        {"missing-field.json",
         specforge::BuildMetadataStatus::Unavailable},
        {"wrong-type.json",
         specforge::BuildMetadataStatus::Unavailable},
        {"working-tree-empty-revision.json",
         specforge::BuildMetadataStatus::Unavailable},
        {"empty-required-string.json",
         specforge::BuildMetadataStatus::Unavailable},
        {"whitespace-required-string.json",
         specforge::BuildMetadataStatus::Unavailable},
        {"padded-required-string.json",
         specforge::BuildMetadataStatus::Unavailable},
        {"invalid-toolchain-format.json",
         specforge::BuildMetadataStatus::Unavailable},
        {"working-tree-nonempty-revision.json",
         specforge::BuildMetadataStatus::Mismatch},
    };

    for (const Case& test_case : cases) {
        const specforge::BuildMetadataReadResult result =
            specforge::ReadBuildMetadata(
                FixturePath(test_case.fixture),
                WorkingTreeIdentity());
        Require(
            result.status == test_case.expected_status,
            std::string(test_case.fixture) +
                " should produce the expected status");
        Require(
            result.metadata.has_value() ==
                (test_case.expected_status ==
                 specforge::BuildMetadataStatus::Available),
            std::string(test_case.fixture) +
                " should expose details only when available");
    }

    const specforge::BuildMetadataReadResult missing =
        specforge::ReadBuildMetadata(
            FixturePath("does-not-exist.json"),
            WorkingTreeIdentity());
    Require(
        missing.status == specforge::BuildMetadataStatus::Unavailable,
        "a missing metadata file should be unavailable");
    Require(
        !missing.metadata,
        "a missing metadata file should not expose details");
}

void TestAvailableDetails()
{
    const specforge::BuildMetadataReadResult result =
        specforge::ReadBuildMetadata(
            FixturePath("available-working-tree.json"),
            WorkingTreeIdentity());
    Require(
        result.status == specforge::BuildMetadataStatus::Available &&
            result.metadata,
        "matching metadata should be available");
    Require(
        result.metadata->compiler_id == "MSVC" &&
            result.metadata->compiler_version == "19.44" &&
            result.metadata->cmake_version == "4.1.0" &&
            result.metadata->generator == "Ninja" &&
            !result.metadata->windows_sdk_version &&
            result.metadata->dear_imgui_version == "1.92.5" &&
            result.metadata->implot_version == "0.17" &&
            result.metadata->zlib_version == "1.3.1",
        "available metadata should expose all diagnostic fields");
}

void TestEveryCoreIdentityMismatch()
{
    using IdentityMutation =
        std::pair<std::string_view, void (*)(specforge::BuildIdentity&)>;
    const std::vector<IdentityMutation> mutations = {
        {"version", [](specforge::BuildIdentity& value) {
             value.specforge_version = "9.9.9";
         }},
        {"release profile", [](specforge::BuildIdentity& value) {
             value.release_profile = "Installed";
         }},
        {"configuration", [](specforge::BuildIdentity& value) {
             value.configuration = "Release";
         }},
        {"architecture", [](specforge::BuildIdentity& value) {
             value.target_architecture = "arm64";
         }},
        {"source mode", [](specforge::BuildIdentity& value) {
             value.source_mode = "head";
             value.source_revision =
                 "0123456789abcdef0123456789abcdef01234567";
         }},
        {"source revision", [](specforge::BuildIdentity& value) {
             value.source_revision = "unexpected";
         }},
    };

    for (const auto& [description, mutate] : mutations) {
        specforge::BuildIdentity identity = WorkingTreeIdentity();
        mutate(identity);
        const specforge::BuildMetadataReadResult result =
            specforge::ReadBuildMetadata(
                FixturePath("available-working-tree.json"),
                identity);
        Require(
            result.status == specforge::BuildMetadataStatus::Mismatch,
            std::string(description) +
                " mismatch should reject otherwise valid metadata");
        Require(
            !result.metadata,
            std::string(description) +
                " mismatch should not expose diagnostic fields");
    }
}

void TestHeadRevisionMustMatchCompletely()
{
    specforge::BuildIdentity identity = {
        .specforge_version = "0.4.1",
        .release_profile = "Portable",
        .configuration = "Release",
        .target_architecture = "x64",
        .source_mode = "head",
        .source_revision =
            "0123456789abcdef0123456789abcdef01234567",
    };
    const specforge::BuildMetadataReadResult available =
        specforge::ReadBuildMetadata(
            FixturePath("available-head.json"),
            identity);
    Require(
        available.status == specforge::BuildMetadataStatus::Available,
        "HEAD metadata should accept the complete matching revision");

    identity.source_revision =
        "0123456789abcdef0123456789abcdef01234568";
    const specforge::BuildMetadataReadResult mismatch =
        specforge::ReadBuildMetadata(
            FixturePath("available-head.json"),
            identity);
    Require(
        mismatch.status == specforge::BuildMetadataStatus::Mismatch &&
            !mismatch.metadata,
        "HEAD metadata should reject any complete-revision difference");
}

}  // namespace

int main()
{
    TestFileFixtures();
    TestAvailableDetails();
    TestEveryCoreIdentityMismatch();
    TestHeadRevisionMustMatchCompletely();
    std::cout << "build metadata reader tests passed\n";
    return 0;
}
