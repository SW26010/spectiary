#include "platform/atomic_file.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
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
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "spectiary-atomic-file-writer-exception";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
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

    std::filesystem::remove_all(root, cleanup_error);
}

}  // namespace

int main()
{
    TestWriterExceptionCleansTemporaryFile();
    return 0;
}
