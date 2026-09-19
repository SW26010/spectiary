#pragma once

#include "app/panel_visibility_state.h"
#include "automation/automation_named_pipe.h"
#include "profile/profile_sink.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace spectiary {

struct AutomationShellState {
    bool idle = true;
    bool source_load_idle = true;
    bool pending_completion_idle = true;
    bool background_retirement_idle = true;
    std::size_t active_load_count = 0;
    std::size_t completed_load_count = 0;
    std::size_t pending_load_count = 0;
    std::size_t retirement_queued_count = 0;
    std::size_t retirement_in_flight_count = 0;
    std::string current_source_id;
    std::filesystem::path current_source_path;
};

struct AutomationWindowState {
    bool visible = true;
    bool minimized = false;
    unsigned int client_width = 0;
    unsigned int client_height = 0;
};

struct AutomationSettingsState {
    std::string language = "en";
    int ui_scale_percentage = 100;
};

struct AutomationSpectrumState {
    bool present = false;
    std::size_t index = 0;
    std::string name;
    std::size_t count = 0;
};

struct AutomationPresentedSourceState {
    bool present = false;
    std::string id;
    std::filesystem::path path;
};

struct AutomationLabelingState {
    bool has_active_task = false;
    std::string task_id;
    std::string task_name;
    std::vector<std::string> task_ids;
    int current_spectrum_code = -1;
};

struct AutomationCaptureState {
    bool pending = false;
    std::filesystem::path current_path;
    std::string last_result = "none";
    std::filesystem::path last_path;
};

struct AutomationProfileState {
    std::string status = "inactive";
    std::filesystem::path path;
    std::string stop_reason = "none";
    std::uint64_t dropped_events = 0;
};

struct AutomationRuntimeState {
    bool running = true;
    bool shutting_down = false;
    std::uint64_t frame_index = 0;
};

struct AutomationStateSnapshot {
    std::string instance_id;
    AutomationControlQueueSnapshot control;
    AutomationShellState shell;
    AutomationSettingsState settings;
    PanelVisibilityState panels;
    AutomationPresentedSourceState presented_source;
    AutomationSpectrumState spectrum;
    AutomationLabelingState labeling;
    AutomationCaptureState capture;
    AutomationProfileState profile;
    AutomationWindowState window;
    AutomationRuntimeState runtime;
};

// Publishes the terminal profile.stop response after the production sink has
// completed its asynchronous drain and final flush.
void CompleteAutomationProfileStopTerminal(
    AutomationNamedPipeServer& server,
    const AutomationQueuedCommand& command,
    ProfileSink::StopReason reason,
    const std::filesystem::path& path,
    std::uint64_t dropped_events);

}  // namespace spectiary
