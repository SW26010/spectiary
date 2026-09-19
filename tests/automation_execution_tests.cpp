#include "app/automation_execution.h"

#include <iostream>
#include <map>
#include <stdexcept>

using namespace spectiary;

namespace {
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

struct Fixture {
    AutomationExecution execution;
    std::map<std::string, bool> active;
    std::map<std::string, bool> claimed;
    std::map<std::string, AutomationCommandResult> results;
    std::map<std::string, std::string> failures;
    std::map<int, ShellAutomationSourceOutcome> sources;
    ShellAutomationView view{.source_id = "source", .spectrum = {.present = true, .index = 2}};
    AutomationExecution::Presentation presented{10, 1, "source", 2};
    ShellAutomationLabelAssignmentResult assignment{
        .source_id = "source", .task_id = "task", .spectrum = {.present = true, .index = 2},
        .previous_code = -1, .new_code = 5, .changed = true,
        .state_save_scheduled = true, .navigation_pending = true};
    std::uint64_t generation = 1;
    bool idle = true;
    bool claim = true;
    bool connected = true;
    int opens = 0;
    int writes = 0;
    int terminal_count = 0;
    int published_terminals = 0;
    int frame_requests = 0;
    ShellAutomationNavigationError navigation_error = ShellAutomationNavigationError::None;

    AutomationExecution::Callbacks Callbacks()
    {
        return {
            .is_request_active = [this](std::string_view id) { return active[std::string(id)]; },
            .try_claim_execution = [this](const auto& command) {
                if (!claim || !connected || !active[command.request_id]) return false;
                claimed[command.request_id] = true;
                return true;
            },
            .complete = [this](const auto& command, const auto& result) {
                Require(active[command.request_id], "duplicate terminal completion");
                active[command.request_id] = false;
                results.emplace(command.request_id, result);
                ++terminal_count;
                if (connected) ++published_terminals;
            },
            .fail = [this](const auto& command, auto code, auto) {
                Require(active[command.request_id], "duplicate terminal failure");
                active[command.request_id] = false;
                failures.emplace(command.request_id, code);
                ++terminal_count;
                if (connected) ++published_terminals;
            },
            .open_source = [this](const auto&) -> AutomationExecution::SourceOperation {
                const int id = ++opens;
                sources[id] = {};
                return [this, id]() { return sources.at(id); };
            },
            .goto_spectrum = [this](auto index, auto) {
                return ShellAutomationNavigationResult{.error = navigation_error,
                    .source_id = view.source_id,
                    .target = {.present = true, .index = index.value_or(2)}, .changed = true};
            },
            .assign_label = [this](int) { ++writes; return assignment; },
            .activation_generation = [this]() { return generation; },
            .presented_spectrum = [this]() { return presented; },
            .view = [this]() { return view; },
            .shell_idle = [this]() { return idle; },
            .request_frame = [this]() { ++frame_requests; },
        };
    }

    AutomationQueuedCommand Command(std::string id, AutomationCommandKind kind,
                                    AutomationCommandParameters parameters)
    {
        active[id] = true;
        return {std::move(id), kind, std::move(parameters), 1};
    }
    void Goto(std::string id = "goto")
    {
        execution.BeginAutomationSpectrumGoto(Command(id, AutomationCommandKind::SpectrumGoto,
            AutomationSpectrumGotoParameters{{2, std::nullopt}}), Callbacks());
    }
    void Label(bool target = true)
    {
        execution.BeginAutomationLabelAssign(Command("label", AutomationCommandKind::LabelAssign,
            AutomationLabelAssignParameters{5, target ? std::optional<AutomationSpectrumTarget>{{2, std::nullopt}} : std::nullopt}), Callbacks());
    }
    void Open(std::string id)
    {
        const auto path = std::filesystem::current_path().u8string();
        execution.BeginAutomationSourceOpen(Command(id, AutomationCommandKind::SourceOpen,
            AutomationSourceOpenParameters{std::string(path.begin(), path.end())}), Callbacks());
    }
    void Poll() { execution.Poll(Callbacks()); }
    void Disconnect()
    {
        connected = false;
        for (auto& [id, is_active] : active) {
            if (!claimed[id]) is_active = false;
        }
    }
};

void SourceLifecycle()
{
    Fixture f;
    f.Open("old");
    f.Open("new");
    f.Poll();
    Require(f.terminal_count == 0 && f.execution.NextDeadline().has_value(), "pending source must schedule observation");
    f.sources[1].state = ShellAutomationSourceOutcome::State::Canceled;
    f.sources[2] = {ShellAutomationSourceOutcome::State::Succeeded,
        std::filesystem::current_path(), "replacement", 4, 1, "sample"};
    f.Poll();
    Require(f.failures.at("old") == "operation_canceled", "replacement cancels old source");
    const auto& result = std::get<AutomationSourceOpenResult>(f.results.at("new"));
    Require(result.source_id == "replacement", "source terminal retains Shell outcome");
    Require(f.execution.Idle() && !f.execution.NextDeadline(), "terminal source operations retire");
    f.Poll();
    Require(f.terminal_count == 2, "source terminal must be emitted exactly once");

    f.Open("failed");
    f.sources[3].state = ShellAutomationSourceOutcome::State::Failed;
    f.Poll();
    Require(f.failures.at("failed") == "source_load_failed", "source failure classification");
    f.Open("canceled");
    f.active["canceled"] = false;
    f.Poll();
    Require(f.execution.Idle() && f.terminal_count == 3, "inactive source must retire silently");
}

void NavigationPresentation()
{
    Fixture f;
    f.Goto();
    const auto deadline = f.execution.NextDeadline();
    Require(deadline && f.execution.NextDeadline() == deadline, "deadline queries must not postpone polling");
    f.Poll();
    Require(f.terminal_count == 0, "old or failed Present cannot complete navigation");
    ++f.presented.sequence;
    f.presented.activation_generation = 0;
    f.Poll();
    Require(f.terminal_count == 0, "wrong presented activation cannot complete navigation");
    f.presented.activation_generation = 1;
    f.presented.source_id = "other";
    f.Poll();
    Require(f.terminal_count == 0, "wrong presented source cannot complete navigation");
    f.presented.source_id = "source";
    f.presented.spectrum_index = 1;
    f.Poll();
    Require(f.terminal_count == 0, "wrong presented sample cannot complete navigation");
    f.presented.spectrum_index = 2;
    f.idle = false;
    f.Poll();
    Require(f.terminal_count == 0, "busy Shell cannot complete navigation");
    f.idle = true;
    f.Poll();
    Require(f.results.contains("goto") && f.execution.Idle(), "matching presentation completes navigation");
    f.Poll();
    Require(f.terminal_count == 1, "navigation terminal exactly once");
}

void NavigationFailures()
{
    for (int scenario = 0; scenario != 5; ++scenario) {
        Fixture f;
        f.Goto();
        if (scenario == 0) ++f.generation;
        if (scenario == 1) f.view.source_id = "replacement";
        if (scenario == 2) f.view.spectrum.present = false;
        if (scenario == 3) f.view.spectrum.index = 1;
        if (scenario == 4) f.active["goto"] = false;
        f.Poll();
        Require(f.execution.Idle(), "failed or inactive navigation retires");
        if (scenario < 2) Require(f.failures.at("goto") == "operation_canceled", "source replacement cancels navigation");
        else if (scenario < 4) Require(f.failures.at("goto") == "spectrum_load_failed", "failed load cannot complete navigation");
        else Require(f.terminal_count == 0, "inactive navigation retires without a second terminal");
    }
    Fixture unclaimed;
    unclaimed.claim = false;
    unclaimed.Goto();
    Require(unclaimed.execution.Idle() && unclaimed.frame_requests == 0, "unclaimed request cannot execute");
}

void LabelLifecycle()
{
    Fixture f;
    f.Label();
    f.Poll();
    Require(f.writes == 0, "label target requires new successful Present before mutation");
    ++f.presented.sequence;
    f.Poll();
    Require(f.writes == 1 && f.terminal_count == 0, "target presentation triggers one label write");
    f.Poll();
    Require(f.writes == 1 && f.terminal_count == 0, "auto-advance waits for a subsequent presentation");
    f.view.spectrum.index = 3;
    f.presented.spectrum_index = 3;
    ++f.presented.sequence;
    f.Poll();
    Require(f.results.contains("label") && f.execution.Idle(), "auto-advance completes committed assignment");
    const auto& result = std::get<AutomationLabelAssignResult>(f.results.at("label"));
    Require(result.source_id == "source" && result.task_id == "task" &&
        result.spectrum.index == 2 && result.new_code == 5 && result.previous_code == -1 &&
        result.persistence.state_save_scheduled && result.current_spectrum_after &&
        result.current_spectrum_after->index == 3,
        "label terminal separates the assigned sample and persistence facts from auto-advance");
    f.Poll();
    Require(f.writes == 1 && f.terminal_count == 1, "label mutation and settlement exactly once");

    Fixture replaced;
    replaced.Label(false);
    ++replaced.generation;
    replaced.view.source_id = "replacement";
    replaced.Poll();
    Require(replaced.results.contains("label"), "replacement after write must preserve successful label terminal");

    Fixture unchanged;
    unchanged.assignment.changed = false;
    unchanged.assignment.navigation_pending = false;
    unchanged.Label(false);
    unchanged.Poll();
    Require(unchanged.results.contains("label"), "unchanged assignment needs no new presentation");
}

void ClaimedDisconnectSettlement()
{
    Fixture source;
    source.Open("source");
    source.Disconnect();
    source.sources[1].state = ShellAutomationSourceOutcome::State::Succeeded;
    source.Poll();
    Require(source.execution.Idle() && source.terminal_count == 1 &&
        source.published_terminals == 0,
        "disconnected claimed source must settle locally without publishing");

    Fixture navigation;
    navigation.Goto();
    navigation.Disconnect();
    ++navigation.presented.sequence;
    navigation.Poll();
    Require(navigation.execution.Idle() && navigation.terminal_count == 1 &&
        navigation.published_terminals == 0,
        "disconnected claimed navigation must consume its matching presentation");

    Fixture label;
    label.Label();
    label.Disconnect();
    ++label.presented.sequence;
    label.Poll();
    Require(label.writes == 1 && label.terminal_count == 0,
        "disconnect after claim must not cancel a targeted label before its write");
    ++label.presented.sequence;
    label.Poll();
    label.Poll();
    Require(label.execution.Idle() && label.writes == 1 && label.terminal_count == 1 &&
        label.published_terminals == 0,
        "disconnected claimed label must commit and settle exactly once");
}

void LabelFailuresAndSettlement()
{
    for (int scenario = 0; scenario != 4; ++scenario) {
        Fixture f;
        f.Label();
        if (scenario == 0) ++f.generation;
        if (scenario == 1) f.view.spectrum.present = false;
        if (scenario == 2) f.active["label"] = false;
        if (scenario == 3) f.view.source_id = "replacement";
        f.Poll();
        Require(f.execution.Idle() && f.writes == 0, "failed or canceled target cannot mutate labels");
    }
    Fixture rejected;
    rejected.assignment.error = ShellAutomationLabelError::NoActiveTask;
    rejected.Label(false);
    Require(rejected.failures.at("label") == "no_active_label_task" && rejected.execution.Idle(), "assignment rejection settles immediately");

    Fixture busy;
    busy.Goto();
    busy.Label();
    Require(busy.failures.at("label") == "operation_busy" && busy.writes == 0, "navigation excludes overlapping label mutation");
    busy.Open("source");
    // The transport retires outstanding requests at shutdown; execution releases
    // observations without inventing a second response or undoing Shell work.
    busy.active["goto"] = false;
    busy.active["source"] = false;
    busy.execution.SettleForShutdown();
    busy.execution.SettleForShutdown();
    busy.Poll();
    Require(busy.execution.Idle() && !busy.execution.NextDeadline() && busy.terminal_count == 1,
        "shutdown settlement is idempotent and releases all observations");
}

void LabelShutdownSettlement()
{
    for (bool waiting_for_target : {true, false}) {
        Fixture f;
        f.Label(waiting_for_target);
        const int committed_writes = waiting_for_target ? 0 : 1;
        Require(!f.execution.Idle() && f.execution.NextDeadline() &&
            f.writes == committed_writes && f.terminal_count == 0,
            "shutdown fixture must retain a real label operation in the intended phase");
        const int scheduled_frames = f.frame_requests;
        // Match the application shutdown order: transport retires the request,
        // then the execution owner releases its pending operation.
        f.active["label"] = false;
        f.execution.SettleForShutdown();
        Require(f.execution.Idle() && !f.execution.NextDeadline(),
            "shutdown must retire label operations and their observation deadline");
        ++f.presented.sequence;
        f.execution.SettleForShutdown();
        f.Poll();
        Require(f.execution.Idle() && !f.execution.NextDeadline() &&
            f.writes == committed_writes && f.terminal_count == 0 &&
            f.frame_requests == scheduled_frames,
            "shutdown in either label phase must not write, publish or schedule again");
    }
}
} // namespace

int main()
{
    try {
        SourceLifecycle();
        NavigationPresentation();
        NavigationFailures();
        LabelLifecycle();
        ClaimedDisconnectSettlement();
        LabelFailuresAndSettlement();
        LabelShutdownSettlement();
        std::cout << "Automation execution tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
