#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace spectiary::tests {

class SourceStage {
public:
    static SourceStage Between(
        const std::filesystem::path& source_path,
        std::string_view anchor,
        std::string_view begin_marker,
        std::string_view end_marker)
    {
        std::ifstream source(source_path);
        if (!source) {
            throw std::runtime_error(
                "could not open cancellation-stage source file");
        }

        bool found_anchor = false;
        bool found_begin = false;
        DWORD begin_line = 0;
        DWORD end_line = 0;
        std::string line;
        DWORD line_number = 0;
        while (std::getline(source, line)) {
            ++line_number;
            if (!found_anchor && line.find(anchor) != std::string::npos) {
                found_anchor = true;
                continue;
            }
            if (!found_anchor) {
                continue;
            }
            if (!found_begin &&
                line.find(begin_marker) != std::string::npos) {
                found_begin = true;
                begin_line = line_number;
                continue;
            }
            if (found_begin &&
                line.find(end_marker) != std::string::npos) {
                end_line = line_number;
                break;
            }
        }
        if (!found_anchor || !found_begin || end_line == 0) {
            throw std::runtime_error(
                "could not resolve cancellation-stage source range");
        }
        return SourceStage(
            source_path.filename().string(),
            begin_line,
            end_line);
    }

    [[nodiscard]] bool Contains(
        std::string_view source_file,
        DWORD line_number) const
    {
        return std::filesystem::path(source_file).filename().string() ==
                source_filename_ &&
            line_number >= begin_line_ && line_number <= end_line_;
    }

private:
    SourceStage(
        std::string source_filename,
        DWORD begin_line,
        DWORD end_line)
        : source_filename_(std::move(source_filename))
        , begin_line_(begin_line)
        , end_line_(end_line)
    {
    }

    std::string source_filename_;
    DWORD begin_line_ = 0;
    DWORD end_line_ = 0;
};

class CancellationStageProbe {
public:
    explicit CancellationStageProbe(SourceStage stage)
        : stage_(std::move(stage))
    {
        if (!EnsureSymbols()) {
            throw std::runtime_error(
                "could not initialize symbols for cancellation-stage tests");
        }
    }

    bool Poll()
    {
        std::array<void*, 64> frames = {};
        const USHORT frame_count = CaptureStackBackTrace(
            0,
            static_cast<DWORD>(frames.size()),
            frames.data(),
            nullptr);
        HANDLE process = GetCurrentProcess();
        std::scoped_lock symbol_lock(SymbolMutex());
        for (USHORT index = 0; index < frame_count; ++index) {
            if (AddressIsWithinStage(
                    process,
                    reinterpret_cast<DWORD64>(frames[index]))) {
                stage_observed_ = true;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool stage_observed() const noexcept
    {
        return stage_observed_;
    }

private:
    [[nodiscard]] bool AddressIsWithinStage(
        HANDLE process,
        DWORD64 address) const
    {
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD displacement = 0;
        if (SymGetLineFromAddr64(
                process,
                address,
                &displacement,
                &line) != FALSE &&
            line.FileName != nullptr &&
            stage_.Contains(line.FileName, line.LineNumber)) {
            return true;
        }

        const DWORD inline_frame_count =
            SymAddrIncludeInlineTrace(process, address);
        if (inline_frame_count == 0) {
            return false;
        }

        DWORD inline_context = INLINE_FRAME_CONTEXT_INIT;
        DWORD inline_frame_index = 0;
        if (SymQueryInlineTrace(
                process,
                address,
                INLINE_FRAME_CONTEXT_INIT,
                address,
                address,
                &inline_context,
                &inline_frame_index) == FALSE) {
            return false;
        }

        for (DWORD index = 0; index < inline_frame_count;
             ++index, ++inline_context) {
            line = {};
            line.SizeOfStruct = sizeof(line);
            displacement = 0;
            if (SymGetLineFromInlineContext(
                    process,
                    address,
                    inline_context,
                    0,
                    &displacement,
                    &line) != FALSE &&
                line.FileName != nullptr &&
                stage_.Contains(line.FileName, line.LineNumber)) {
                return true;
            }
        }
        return false;
    }

    static std::mutex& SymbolMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    static bool EnsureSymbols()
    {
        static std::once_flag initialize_once;
        static bool initialized = false;
        std::scoped_lock symbol_lock(SymbolMutex());
        std::call_once(
            initialize_once,
            []() {
                SymSetOptions(
                    SymGetOptions() |
                    SYMOPT_DEFERRED_LOADS |
                    SYMOPT_LOAD_LINES |
                    SYMOPT_UNDNAME);
                initialized = SymInitialize(
                                  GetCurrentProcess(),
                                  nullptr,
                                  TRUE) != FALSE;
                if (!initialized &&
                    GetLastError() == ERROR_INVALID_PARAMETER) {
                    initialized = true;
                }
            });
        return initialized;
    }

    SourceStage stage_;
    bool stage_observed_ = false;
};

}  // namespace spectiary::tests
