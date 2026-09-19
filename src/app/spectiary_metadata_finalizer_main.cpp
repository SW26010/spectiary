#include "app/spectiary_metadata.h"
#include "app/spectiary_metadata_finalizer.h"
#include "spectiary/spectiary_build_identity.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace {

spectiary::BuildMetadata ConfiguredBuildMetadata()
{
    std::optional<std::string> windows_sdk_version;
    if (spectiary::build_info::kBuildWindowsSdkVersion[0] != '\0') {
        windows_sdk_version =
            spectiary::build_info::kBuildWindowsSdkVersion;
    }
    return {
        .compiler_id = spectiary::build_info::kBuildCompilerId,
        .compiler_version =
            spectiary::build_info::kBuildCompilerVersion,
        .cmake_version = spectiary::build_info::kBuildCMakeVersion,
        .generator = spectiary::build_info::kBuildGenerator,
        .windows_sdk_version = std::move(windows_sdk_version),
        .dear_imgui_version =
            spectiary::build_info::kBuildDearImguiVersion,
        .implot_version = spectiary::build_info::kBuildImPlotVersion,
        .cfitsio_version =
            spectiary::build_info::kBuildCfitsioVersion,
        .yaml_cpp_version =
            spectiary::build_info::kBuildYamlCppVersion,
        .zlib_version = spectiary::build_info::kBuildZlibVersion,
    };
}

template <typename Character>
int RunFinalizer(int argc, Character** argv)
{
    if (argc != 3) {
        std::cerr << "usage: spectiary_metadata_finalizer_tool "
                     "<Spectiary.exe> <spectiary_metadata.json>\n";
        return 2;
    }

    spectiary::SpectiaryMetadataFinalizerOptions options;
    options.executable_path = std::filesystem::path(argv[1]);
    options.metadata_path = std::filesystem::path(argv[2]);
    options.build_identity = spectiary::CompiledBuildIdentity();
    options.configured_build_metadata = ConfiguredBuildMetadata();

    std::string error;
    if (!spectiary::FinalizeSpectiaryMetadata(options, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    return RunFinalizer(argc, argv);
}
#else
int main(int argc, char** argv)
{
    return RunFinalizer(argc, argv);
}
#endif
