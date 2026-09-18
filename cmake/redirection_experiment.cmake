# Explicitly built diagnostic executable; the standard target and dependency stay untouched.
FetchContent_GetProperties(specforge_widget_imgui_source)
if(NOT SPECFORGE_IMGUI_VERSION STREQUAL "1.92.8")
    message(FATAL_ERROR "Redirection experiment requires the reviewed ImGui 1.92.8 backend")
endif()
set(backend_source "${specforge_widget_imgui_source_SOURCE_DIR}/backends/imgui_impl_win32.cpp")
file(READ "${backend_source}" backend)
set(anchor "    if (flags & ImGuiViewportFlags_TopMost)\n        *out_ex_style |= WS_EX_TOPMOST;")
string(FIND "${backend}" "${anchor}" anchor_position)
if(anchor_position EQUAL -1)
    message(FATAL_ERROR "ImGui style function changed; review the experiment patch")
endif()
string(REPLACE "${anchor}" "${anchor}\n\n    // SpecForge isolated creation-time A/B experiment. Keep the bit on style updates too.\n    wchar_t experiment[2] = {};\n    if (::GetEnvironmentVariableW(L\"SPECFORGE_EXPERIMENT_NO_REDIRECTION_BITMAP\", experiment, 2) == 1 && experiment[0] == L'1')\n        *out_ex_style |= WS_EX_NOREDIRECTIONBITMAP;" backend "${backend}")
set(experiment_backend "${CMAKE_CURRENT_BINARY_DIR}/generated/experiments/imgui_impl_win32.cpp")
file(WRITE "${experiment_backend}" "${backend}")
get_target_property(native_sources specforge_native SOURCES)
get_target_property(native_libraries specforge_native LINK_LIBRARIES)
add_executable(specforge_redirection_experiment WIN32 EXCLUDE_FROM_ALL ${native_sources} "${experiment_backend}")
specforge_configure_production_target(specforge_redirection_experiment)
target_compile_definitions(specforge_redirection_experiment PRIVATE SPECFORGE_REDIRECTION_AB_BUILD=1)
target_link_libraries(specforge_redirection_experiment PRIVATE ${native_libraries})
set_target_properties(specforge_redirection_experiment PROPERTIES
    OUTPUT_NAME "${PROJECT_artifact_basename}" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/redirection-experiment")
if(MSVC)
    target_link_options(specforge_redirection_experiment PRIVATE /Brepro /INCREMENTAL:NO /DEBUG:FULL)
endif()
add_dependencies(specforge_redirection_experiment specforge_metadata_finalizer_tool)
add_custom_command(TARGET specforge_redirection_experiment POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:specforge_redirection_experiment>/config"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SPECFORGE_PUBLIC_SPECTRAL_LINES_TSV}"
        "$<TARGET_FILE_DIR:specforge_redirection_experiment>/config/spectral_lines.public.tsv"
    COMMAND "$<TARGET_FILE:specforge_metadata_finalizer_tool>" "$<TARGET_FILE:specforge_redirection_experiment>"
        "$<TARGET_FILE_DIR:specforge_redirection_experiment>/${PROJECT_metadata_filename}"
    VERBATIM)
add_executable(specforge_redirection_backend_tests
    tests/redirection_backend_tests.cpp "${experiment_backend}")
specforge_configure_production_target(specforge_redirection_backend_tests)
target_link_libraries(specforge_redirection_backend_tests PRIVATE imgui::imgui dwmapi imm32)
foreach(mode IN ITEMS baseline experiment)
    add_test(NAME specforge_redirection_backend_${mode} COMMAND specforge_redirection_backend_tests ${mode})
    set_tests_properties(specforge_redirection_backend_${mode} PROPERTIES TIMEOUT 15 LABELS fast)
endforeach()
