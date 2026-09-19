#include "platform/atomic_file.h"
#include "helpers/temporary_directory.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool HasTemporarySibling(const std::filesystem::path& root)
{
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root, error)) {
        if (error) {
            return true;
        }
        if (entry.path().filename().string().find(".tmp.") !=
            std::string::npos) {
            return true;
        }
    }
    return false;
}

void TestWriterExceptionCleansTemporaryFile()
{
    const spectiary::test_support::TemporaryDirectory temporary;
    const std::filesystem::path& root = temporary.path();
    const std::filesystem::path target = root / "metadata.json";
    {
        std::ofstream stream(target, std::ios::binary);
        stream << "old metadata";
    }

    spectiary::AtomicFileWriteOptions options;
    options.target_description = "test metadata";
    std::string error;
    const bool written = spectiary::WriteFileAtomically(
        target,
        options,
        [](std::ostream& stream, std::string&) -> bool {
            stream << "partial metadata";
            throw std::runtime_error("serialization failed");
        },
        &error);

    Require(!written, "a writer exception should fail the atomic write");
    Require(
        error.find("serialization failed") != std::string::npos,
        "a writer exception should be reported");
    Require(
        std::filesystem::exists(target),
        "a writer exception should preserve the existing target");
    Require(
        !HasTemporarySibling(root),
        "a writer exception should remove the temporary file");
}

void TestTemporaryDirectoriesAreIsolatedAndCleaned()
{
    const spectiary::test_support::TemporaryDirectory survivor;
    const auto sentinel = survivor.path() / "keep.txt";
    {
        std::ofstream stream(sentinel);
        stream << "keep";
        Require(stream.good(), "surviving fixture should be writable");
    }

    std::filesystem::path normal_path;
    {
        const spectiary::test_support::TemporaryDirectory temporary;
        normal_path = temporary.path();
        Require(normal_path != survivor.path(), "temporary directory owners must be isolated");
        std::filesystem::create_directories(normal_path / "nested");
        std::ofstream stream(normal_path / "nested" / "fixture.txt");
        stream << "fixture";
        Require(stream.good(), "normal-scope fixture should be writable");
    }
    Require(!std::filesystem::exists(normal_path), "normal scope exit should clean all fixtures");

    struct ExpectedFailure {};
    std::filesystem::path failed_path;
    try {
        const spectiary::test_support::TemporaryDirectory temporary;
        failed_path = temporary.path();
        Require(failed_path != survivor.path(), "exception-scope owner must remain isolated");
        std::ofstream stream(failed_path / "fixture.txt");
        stream << "fixture";
        Require(stream.good(), "exception-scope fixture should be writable");
        throw ExpectedFailure{};
    } catch (const ExpectedFailure&) {
    }
    Require(!std::filesystem::exists(failed_path), "exception unwinding should clean fixtures");
    Require(std::filesystem::exists(sentinel), "cleanup must preserve another owner's fixtures");
}

}  // namespace

int main()
{
    try {
        TestTemporaryDirectoriesAreIsolatedAndCleaned();
        TestWriterExceptionCleansTemporaryFile();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
