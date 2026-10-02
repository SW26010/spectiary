option(SPECTIARY_ENABLE_ASAN "Instrument repository C++ code with MSVC AddressSanitizer." OFF)
option(SPECTIARY_ENABLE_STATIC_ANALYSIS "Analyze production C++ code with MSVC." OFF)

if(SPECTIARY_ENABLE_ASAN OR SPECTIARY_ENABLE_STATIC_ANALYSIS)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        message(FATAL_ERROR "Spectiary hardening presets require the MSVC compiler.")
    endif()
endif()

if(SPECTIARY_ENABLE_ASAN)
    # Apply to every repository translation unit (including test-owned sources),
    # but never change the independently built/imported vcpkg dependencies.
    # The ASan preset removes /RTC1 and incremental linking from Debug defaults.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/fsanitize=address>")
    add_link_options(/INCREMENTAL:NO /DEBUG:FULL)
    # vcpkg's prebuilt imgui_stdlib crosses the std::string boundary without
    # STL annotations. All objects must agree (LNK2038); keep ordinary ASan
    # instrumentation while foregoing size-vs-capacity checks in STL containers.
    add_compile_definitions(_DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
endif()

if(SPECTIARY_ENABLE_STATIC_ANALYSIS)
    # cl.exe can emit ruleset Action="Error" as a warning; /we makes the
    # selected diagnostics fatal without promoting unrelated dependency warnings.
    set(spectiary_analysis_options /analyze /analyze:external-
        /we6001 /we6011 /we6385 /we6386
        "/analyze:ruleset${CMAKE_SOURCE_DIR}/config/native-analysis.ruleset")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/config/native-analysis.ruleset")
    # A broken source must fail with the selected analyzer diagnostic, rather
    # than silently succeeding because a ruleset/tool installation was ignored.
    try_compile(spectiary_analysis_probe_passed
        "${CMAKE_BINARY_DIR}/analysis-probe"
        SOURCES "${CMAKE_SOURCE_DIR}/tests/fixtures/analysis_probe.cpp"
        COMPILE_DEFINITIONS ${spectiary_analysis_options}
        OUTPUT_VARIABLE spectiary_analysis_probe_output)
    file(WRITE "${CMAKE_BINARY_DIR}/analysis-probe.log" "${spectiary_analysis_probe_output}")
    if(spectiary_analysis_probe_passed OR NOT spectiary_analysis_probe_output MATCHES "error C6011")
        message(FATAL_ERROR "Static analysis must reject the null-pointer probe with C6011. See analysis-probe.log.")
    endif()
endif()

function(spectiary_enable_static_analysis target_name)
    if(SPECTIARY_ENABLE_STATIC_ANALYSIS)
        target_compile_options(${target_name} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:${spectiary_analysis_options}>")
        # Editing the rule policy must reanalyze existing local build objects.
        get_target_property(analysis_sources ${target_name} SOURCES)
        set_property(SOURCE ${analysis_sources} APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_SOURCE_DIR}/config/native-analysis.ruleset")
    endif()
endfunction()
