#include "app/automation_panel_command_coordinator.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using specforge::ApplicationPanel;
using specforge::AutomationCommandKind;
using specforge::AutomationPanelCommandCoordinator;
using specforge::AutomationPanelGetParameters;
using specforge::AutomationPanelSetParameters;
using specforge::AutomationQueuedCommand;
using specforge::PanelVisibilityState;
using specforge::ShellAutomationPanelPresentation;
using specforge::ShellAutomationPanelPresentationStatus;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

struct Terminal {
    std::string request_id;
    std::string status;
    std::string code;
    std::string body;
};

struct Fixture {
    PanelVisibilityState visibility;
    std::unordered_map<std::string, bool> active;
    std::vector<Terminal> terminals;
    int poll_requests = 0;

    AutomationPanelCommandCoordinator::Callbacks Callbacks()
    {
        return {
            .is_request_active = [this](std::string_view request_id) {
                const auto found = active.find(std::string(request_id));
                return found != active.end() && found->second;
            },
            .try_claim_execution = [this](
                                        const AutomationQueuedCommand& command) {
                return active.contains(command.request_id) &&
                       active.at(command.request_id);
            },
            .current_visibility = [this]() {
                return visibility;
            },
            .apply_visibility = [this](ApplicationPanel panel, bool visible) {
                const bool previous =
                    specforge::ApplicationPanelVisible(visibility, panel);
                if (previous == visible) {
                    return AutomationPanelCommandCoordinator::ApplyResult{
                        .outcome =
                            AutomationPanelCommandCoordinator::ApplyOutcome::
                                Unchanged,
                    };
                }
                specforge::SetApplicationPanelVisible(
                    visibility,
                    panel,
                    visible);
                return AutomationPanelCommandCoordinator::ApplyResult{
                    .outcome =
                        AutomationPanelCommandCoordinator::ApplyOutcome::Applied,
                };
            },
            .complete = [this](
                            const AutomationQueuedCommand& command,
                            const specforge::AutomationCommandResult& body) {
                active[command.request_id] = false;
                terminals.push_back({
                    .request_id = command.request_id,
                    .status = "completed",
                    .body = specforge::SerializeAutomationCommandResultBody(body),
                });
            },
            .fail = [this](
                        const AutomationQueuedCommand& command,
                        std::string_view code,
                        std::string_view message) {
                active[command.request_id] = false;
                terminals.push_back({
                    .request_id = command.request_id,
                    .status = "failed",
                    .code = std::string(code),
                    .body = std::string(message),
                });
            },
            .request_poll = [this]() {
                ++poll_requests;
            },
        };
    }

    AutomationQueuedCommand Set(
        std::string request_id,
        bool visible,
        std::uint64_t sequence = 1)
    {
        active[request_id] = true;
        return {
            .request_id = std::move(request_id),
            .command = AutomationCommandKind::PanelSet,
            .parameters = AutomationPanelSetParameters{
                .name = "files",
                .visible = visible,
            },
            .sequence = sequence,
        };
    }

    Terminal LastTerminal() const
    {
        Require(!terminals.empty(), "expected a panel terminal");
        return terminals.back();
    }
};

ShellAutomationPanelPresentation Presented(
    bool files_visible,
    std::uint64_t frame_index)
{
    ShellAutomationPanelPresentation presented;
    presented.visibility.files = files_visible;
    presented.frame_indices[
        static_cast<std::size_t>(ApplicationPanel::Files)] = frame_index;
    return presented;
}

ShellAutomationPanelPresentationStatus ViewportBlocked(
    std::uint64_t frame_index)
{
    ShellAutomationPanelPresentationStatus status;
    status.frame_indices[
        static_cast<std::size_t>(ApplicationPanel::Files)] = frame_index;
    status.blocked[
        static_cast<std::size_t>(ApplicationPanel::Files)] = true;
    return status;
}

void TestPresentationAndSupersession()
{
    Fixture fixture;
    AutomationPanelCommandCoordinator coordinator;
    const auto first = fixture.Set("first", false, 1);
    coordinator.ServicePanelSet(
        first,
        10,
        {},
        fixture.Callbacks());
    Require(!fixture.visibility.files, "first panel mutation should apply");
    Require(fixture.poll_requests == 1, "accepted mutation should request polling");

    const auto second = fixture.Set("second", true, 2);
    coordinator.ServicePanelSet(
        second,
        11,
        {},
        fixture.Callbacks());
    Require(fixture.visibility.files, "superseding panel mutation should apply");

    coordinator.Poll(
        Presented(true, 20),
        {},
        {},
        fixture.Callbacks());
    Require(
        fixture.terminals.size() == 2 &&
            fixture.terminals[0].request_id == "first" &&
            fixture.terminals[0].code == "operation_canceled" &&
            fixture.terminals[1].request_id == "second" &&
            fixture.terminals[1].status == "completed" &&
            fixture.terminals[1].body.find("frame_index") != std::string::npos,
        "a newer presented generation should cancel the older command and complete the newer one");
    Require(coordinator.Idle(), "presented supersession should leave the coordinator idle");
}

void TestViewportRollbackAndShutdownSettlement()
{
    {
        Fixture fixture;
        AutomationPanelCommandCoordinator coordinator;
        const auto command = fixture.Set("blocked", false, 1);
        coordinator.ServicePanelSet(
            command,
            10,
            {},
            fixture.Callbacks());
        coordinator.Poll(
            Presented(false, 0),
            ViewportBlocked(20),
            {},
            fixture.Callbacks());
        Require(
            fixture.visibility.files &&
                fixture.LastTerminal().code ==
                    "panel_viewport_not_renderable",
            "a blocked detached viewport should rollback the complete chain and report its factual error");
        Require(coordinator.Idle(), "viewport rollback should clear coordinator state");
    }

    {
        Fixture fixture;
        AutomationPanelCommandCoordinator coordinator;
        const auto command = fixture.Set("shutdown", false, 1);
        coordinator.ServicePanelSet(
            command,
            10,
            {},
            fixture.Callbacks());
        Require(
            coordinator.SettleForShutdown(fixture.Callbacks()) &&
                fixture.visibility.files &&
                fixture.LastTerminal().code == "app_shutdown",
            "shutdown should rollback an unpresented mutation and settle its command");
        Require(coordinator.Idle(), "shutdown settlement should clear coordinator state");
    }
}

void TestRenderabilityGatesAndPanelGet()
{
    Fixture fixture;
    AutomationPanelCommandCoordinator coordinator;

    const auto hidden = fixture.Set("hidden", false, 1);
    coordinator.ServicePanelSet(
        hidden,
        10,
        {.window_renderable = false},
        fixture.Callbacks());
    Require(
        fixture.LastTerminal().code == "window_not_renderable" &&
            fixture.visibility.files,
        "hidden window should reject panel.set without production mutation");

    const auto immersive = fixture.Set("immersive", false, 2);
    coordinator.ServicePanelSet(
        immersive,
        10,
        {.immersive_plot_mode = true},
        fixture.Callbacks());
    Require(
        fixture.LastTerminal().code == "panel_not_renderable" &&
            fixture.visibility.files,
        "immersive mode should reject panel.set without production mutation");

    fixture.active["get"] = true;
    const AutomationQueuedCommand get{
        .request_id = "get",
        .command = AutomationCommandKind::PanelGet,
        .parameters = AutomationPanelGetParameters{.name = "files"},
    };
    coordinator.ServicePanelGet(get, fixture.Callbacks());
    Require(
        fixture.LastTerminal().request_id == "get" &&
            fixture.LastTerminal().status == "completed" &&
            fixture.LastTerminal().body.find("\"visible\":true") !=
                std::string::npos,
        "panel.get should remain a stable read of production visibility");
}

}  // namespace

int main()
{
    try {
        TestPresentationAndSupersession();
        TestViewportRollbackAndShutdownSettlement();
        TestRenderabilityGatesAndPanelGet();
        std::cout << "automation panel command coordinator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
