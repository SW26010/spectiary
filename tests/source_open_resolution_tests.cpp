#include "domain/source_open_resolution.h"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto suffix =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        path_ = std::filesystem::temp_directory_path() /
            ("spectiary-source-open-resolution-tests-" +
             std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class CurrentDirectory {
public:
    explicit CurrentDirectory(const std::filesystem::path& path)
        : original_path_(std::filesystem::current_path())
    {
        std::error_code error;
        std::filesystem::current_path(path, error);
        Require(!error, "source-open test should change current directory");
    }

    ~CurrentDirectory()
    {
        std::error_code ignored;
        std::filesystem::current_path(original_path_, ignored);
    }

private:
    std::filesystem::path original_path_;
};

void WriteFixture(const std::filesystem::path& path)
{
    std::ofstream stream(path);
    Require(stream.good(), "source-open fixture should be writable");
    stream << "fixture";
}

std::string PathText(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

spectiary::SourceOpenResolution Resolve(
    const std::filesystem::path& path,
    spectiary::SourceOpenOrigin origin =
        spectiary::SourceOpenOrigin::ExternalStartup,
    bool open_external_source_as_folder = true)
{
    const spectiary::SourceOpenRequest request{
        .source_path = path,
        .origin = origin,
        .open_external_source_as_folder =
            open_external_source_as_folder,
    };
    return spectiary::ResolveSourceOpenRequest(
        request,
        spectiary::ProbeSourceOpenRequest(request));
}

void TestSupportedFitsExtensionsExpandAndRetainPreferredMember()
{
    TemporaryDirectory temporary;
    std::size_t index = 0;
    for (const std::string_view suffix :
         {".fits", ".fit", ".fts", ".fits.gz", ".fit.gz", ".fts.gz",
          ".FITS", ".FIT", ".FTS", ".FITS.GZ"}) {
        const std::filesystem::path member =
            temporary.path() /
            ("selected-" + std::to_string(index++) +
             std::string(suffix));
        WriteFixture(member);

        const spectiary::SourceOpenResolution resolution =
            Resolve(member);
        Require(
            resolution.kind ==
                spectiary::SourceOpenResolutionKind::Folder,
            "supported FITS suffix should resolve to a folder source");
        Require(
            resolution.source_path == temporary.path(),
            "folder resolution should use the FITS parent folder");
        Require(
            resolution.preferred_member_path == member,
            "folder resolution should retain the requested FITS member");
        Require(!resolution.failed(), "successful resolution must not fail");
    }
}

void TestSupportedCsvExtensionsExpandAndRetainPreferredMember()
{
    TemporaryDirectory temporary;
    std::size_t index = 0;
    for (const std::string_view suffix : {".csv", ".CSV", ".CsV"}) {
        const std::filesystem::path member =
            temporary.path() /
            ("selected-csv-" + std::to_string(index++) +
             std::string(suffix));
        WriteFixture(member);

        const spectiary::SourceOpenResolution resolution =
            Resolve(member);
        Require(
            resolution.kind ==
                spectiary::SourceOpenResolutionKind::Folder &&
                resolution.source_path == temporary.path() &&
                resolution.preferred_member_path == member &&
                !resolution.failed(),
            "supported CSV suffix should resolve to a folder source and retain the requested member");
    }
}

void TestRelativeFitsUsesCurrentDirectoryAsParent()
{
    TemporaryDirectory temporary;
    CurrentDirectory current_directory(temporary.path());
    const std::filesystem::path relative_member = "selected.fits";
    WriteFixture(relative_member);

    const spectiary::SourceOpenResolution resolution =
        Resolve(relative_member);
    Require(
        resolution.kind == spectiary::SourceOpenResolutionKind::Folder &&
            resolution.source_path == temporary.path() &&
            resolution.preferred_member_path == relative_member,
        "relative FITS should use the current directory and retain its member path");
}

void TestNpyStaysDirect()
{
    TemporaryDirectory temporary;
    const std::filesystem::path member =
        temporary.path() / "selected.npy";
    WriteFixture(member);

    const spectiary::SourceOpenResolution resolution = Resolve(member);
    Require(
        resolution.kind == spectiary::SourceOpenResolutionKind::Direct &&
            resolution.source_path == member &&
            !resolution.preferred_member_path,
        "external NPY source should retain single-file semantics");
}

void TestTxtStaysDirect()
{
    TemporaryDirectory temporary;
    const std::filesystem::path member =
        temporary.path() / "selected.txt";
    WriteFixture(member);

    const spectiary::SourceOpenResolution resolution = Resolve(member);
    Require(
        resolution.kind == spectiary::SourceOpenResolutionKind::Direct &&
            resolution.source_path == member &&
            !resolution.preferred_member_path,
        "external TXT source should retain single-file semantics");
}

void TestDisabledCsvPreferenceStaysDirect()
{
    TemporaryDirectory temporary;
    const std::filesystem::path member =
        temporary.path() / "selected.CSV";
    WriteFixture(member);

    const spectiary::SourceOpenResolution resolution =
        Resolve(member, spectiary::SourceOpenOrigin::ExternalStartup, false);
    Require(
        resolution.kind == spectiary::SourceOpenResolutionKind::Direct &&
            resolution.source_path == member &&
            !resolution.preferred_member_path,
        "disabled external CSV preference should retain single-file semantics");
}

void TestDisabledPreferenceStaysDirect()
{
    TemporaryDirectory temporary;
    const std::filesystem::path member =
        temporary.path() / "selected.fits";
    WriteFixture(member);

    const spectiary::SourceOpenResolution resolution =
        Resolve(member, spectiary::SourceOpenOrigin::ExternalStartup, false);
    Require(
        resolution.kind == spectiary::SourceOpenResolutionKind::Direct &&
            resolution.source_path == member &&
            !resolution.preferred_member_path,
        "disabled external FITS preference should retain single-file semantics");
}

void TestInAppOriginStaysDirect()
{
    TemporaryDirectory temporary;
    const std::filesystem::path member =
        temporary.path() / "selected.fits";
    WriteFixture(member);

    const spectiary::SourceOpenResolution resolution =
        Resolve(member, spectiary::SourceOpenOrigin::InApp);
    Require(
        resolution.kind == spectiary::SourceOpenResolutionKind::Direct &&
            resolution.source_path == member &&
            !resolution.preferred_member_path,
        "in-app FITS open should retain single-file semantics");
}

void TestAutomationOriginStaysDirect()
{
    TemporaryDirectory temporary;
    const std::filesystem::path member =
        temporary.path() / "selected.fits";
    WriteFixture(member);

    const spectiary::SourceOpenResolution resolution =
        Resolve(member, spectiary::SourceOpenOrigin::Automation);
    Require(
        resolution.kind == spectiary::SourceOpenResolutionKind::Direct &&
            resolution.source_path == member &&
            !resolution.preferred_member_path,
        "automation FITS open should retain single-file semantics");
}

void TestInAppCsvOriginStaysDirect()
{
    TemporaryDirectory temporary;
    const std::filesystem::path member =
        temporary.path() / "selected.CSV";
    WriteFixture(member);

    const spectiary::SourceOpenResolution resolution =
        Resolve(member, spectiary::SourceOpenOrigin::InApp);
    Require(
        resolution.kind == spectiary::SourceOpenResolutionKind::Direct &&
            resolution.source_path == member &&
            !resolution.preferred_member_path,
        "in-app CSV open should retain single-file semantics");
}

void TestAutomationCsvOriginStaysDirect()
{
    TemporaryDirectory temporary;
    const std::filesystem::path member =
        temporary.path() / "selected.CSV";
    WriteFixture(member);

    const spectiary::SourceOpenResolution resolution =
        Resolve(member, spectiary::SourceOpenOrigin::Automation);
    Require(
        resolution.kind == spectiary::SourceOpenResolutionKind::Direct &&
            resolution.source_path == member &&
            !resolution.preferred_member_path,
        "automation CSV open should retain single-file semantics");
}

void TestEmptySourcePathFailsWithDiagnostic()
{
    const std::filesystem::path empty_path;
    const spectiary::SourceOpenResolution resolution =
        Resolve(empty_path);
    Require(
        resolution.failed() &&
            resolution.failure ==
                spectiary::SourceOpenResolutionFailure::EmptySourcePath &&
            !resolution.diagnostic.empty(),
        "empty source path should fail with a diagnostic");
}

void TestMissingTargetFailsWithoutExpansion()
{
    TemporaryDirectory temporary;
    const std::filesystem::path missing =
        temporary.path() / "missing.fits";

    const spectiary::SourceOpenResolution resolution = Resolve(missing);
    Require(
        resolution.failed() &&
            resolution.kind == spectiary::SourceOpenResolutionKind::Failed,
        "missing source should fail resolution");
    Require(
            resolution.failure ==
                spectiary::SourceOpenResolutionFailure::SourcePathUnavailable &&
            !resolution.preferred_member_path &&
            resolution.diagnostic.find(PathText(missing)) !=
                std::string::npos,
        "missing source failure should be typed, diagnosed, and unexpanded");
}

void TestMissingTargetWithDisabledPreferenceFailsDirectly()
{
    TemporaryDirectory temporary;
    const std::filesystem::path missing =
        temporary.path() / "missing.fits";

    const spectiary::SourceOpenResolution resolution =
        Resolve(missing, spectiary::SourceOpenOrigin::ExternalStartup, false);
    Require(
        resolution.failed() &&
            resolution.failure ==
                spectiary::SourceOpenResolutionFailure::SourcePathUnavailable &&
            !resolution.preferred_member_path &&
            resolution.diagnostic.find(PathText(missing)) !=
                std::string::npos,
        "disabled external FITS missing target should fail as a direct source");
}

void TestMissingTargetWithInAppOriginFailsDirectly()
{
    TemporaryDirectory temporary;
    const std::filesystem::path missing =
        temporary.path() / "missing.fits";

    const spectiary::SourceOpenResolution resolution =
        Resolve(missing, spectiary::SourceOpenOrigin::InApp);
    Require(
        resolution.failed() &&
            resolution.failure ==
                spectiary::SourceOpenResolutionFailure::SourcePathUnavailable &&
            !resolution.preferred_member_path &&
            resolution.diagnostic.find(PathText(missing)) !=
                std::string::npos,
        "in-app FITS missing target should fail as a direct source");
}

void TestMissingTargetWithAutomationOriginFailsDirectly()
{
    TemporaryDirectory temporary;
    const std::filesystem::path missing =
        temporary.path() / "missing.fits";

    const spectiary::SourceOpenResolution resolution =
        Resolve(missing, spectiary::SourceOpenOrigin::Automation);
    Require(
        resolution.failed() &&
            resolution.failure ==
                spectiary::SourceOpenResolutionFailure::SourcePathUnavailable &&
            !resolution.preferred_member_path &&
            resolution.diagnostic.find(PathText(missing)) !=
                std::string::npos,
        "automation FITS missing target should fail as a direct source");
}

void TestNonRegularFitsTargetFailsWithDiagnostic()
{
    TemporaryDirectory temporary;
    const std::filesystem::path member =
        temporary.path() / "selected.fits";
    Require(
        std::filesystem::create_directory(member),
        "non-regular FITS fixture directory should be created");

    const spectiary::SourceOpenResolution resolution = Resolve(member);
    Require(
        resolution.failed() &&
            resolution.failure ==
                spectiary::SourceOpenResolutionFailure::SourcePathNotRegularFile &&
            !resolution.preferred_member_path &&
            resolution.diagnostic.find(PathText(member)) !=
                std::string::npos,
        "non-regular FITS target should fail with its path in the diagnostic");
}

void TestMissingParentFailsWithDiagnostic()
{
    TemporaryDirectory temporary;
    const std::filesystem::path missing_parent =
        temporary.path() / "missing-parent";
    const std::filesystem::path missing =
        missing_parent / "selected.fits";

    const spectiary::SourceOpenResolution resolution = Resolve(missing);
    Require(
        resolution.failed() &&
            resolution.failure ==
                spectiary::SourceOpenResolutionFailure::ParentPathUnavailable &&
            !resolution.preferred_member_path &&
            resolution.diagnostic.find(PathText(missing_parent)) !=
                std::string::npos,
        "missing FITS parent should fail with the parent path in the diagnostic");
}

void TestInvalidParentFailsWithoutChoosingAnotherMember()
{
    TemporaryDirectory temporary;
    const std::filesystem::path parent_file =
        temporary.path() / "not-a-folder";
    WriteFixture(parent_file);
    const std::filesystem::path missing =
        parent_file / "member.fits";

    const spectiary::SourceOpenResolution resolution = Resolve(missing);

    Require(
        resolution.failed() &&
            resolution.failure ==
                spectiary::SourceOpenResolutionFailure::ParentPathNotDirectory &&
            !resolution.preferred_member_path &&
            resolution.diagnostic.find(PathText(parent_file)) !=
                std::string::npos,
        "invalid parent should fail without selecting a sorted member");
}

void TestInvalidCsvParentFailsWithoutChoosingAnotherMember()
{
    TemporaryDirectory temporary;
    const std::filesystem::path parent_file =
        temporary.path() / "not-a-csv-folder";
    WriteFixture(parent_file);
    const std::filesystem::path missing =
        parent_file / "member.CSV";

    const spectiary::SourceOpenResolution resolution = Resolve(missing);

    Require(
        resolution.failed() &&
            resolution.failure ==
                spectiary::SourceOpenResolutionFailure::ParentPathNotDirectory &&
            !resolution.preferred_member_path &&
            resolution.diagnostic.find(PathText(parent_file)) !=
                std::string::npos,
        "invalid CSV parent should fail without selecting a sorted member");
}

}  // namespace

int main()
{
    TestSupportedFitsExtensionsExpandAndRetainPreferredMember();
    TestSupportedCsvExtensionsExpandAndRetainPreferredMember();
    TestRelativeFitsUsesCurrentDirectoryAsParent();
    TestNpyStaysDirect();
    TestTxtStaysDirect();
    TestDisabledPreferenceStaysDirect();
    TestDisabledCsvPreferenceStaysDirect();
    TestInAppOriginStaysDirect();
    TestAutomationOriginStaysDirect();
    TestInAppCsvOriginStaysDirect();
    TestAutomationCsvOriginStaysDirect();
    TestEmptySourcePathFailsWithDiagnostic();
    TestMissingTargetFailsWithoutExpansion();
    TestMissingTargetWithDisabledPreferenceFailsDirectly();
    TestMissingTargetWithInAppOriginFailsDirectly();
    TestMissingTargetWithAutomationOriginFailsDirectly();
    TestNonRegularFitsTargetFailsWithDiagnostic();
    TestMissingParentFailsWithDiagnostic();
    TestInvalidParentFailsWithoutChoosingAnotherMember();
    TestInvalidCsvParentFailsWithoutChoosingAnotherMember();
    return 0;
}
