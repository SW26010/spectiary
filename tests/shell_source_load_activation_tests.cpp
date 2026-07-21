#include "ui/shell_ui.h"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace specforge {

struct ShellUiTestAccess {
    using Purpose = ShellUi::PendingSourceLoadPurpose;
    using PendingLoad = ShellUi::PendingSourceLoad;

    static bool CompletionStartsActivation(Purpose purpose, bool loaded)
    {
        return ShellUi::CompletionStartsSourceActivationIntent(purpose, loaded);
    }

    static void AdvanceActivation(
        std::uint64_t& activation_epoch,
        std::unordered_map<std::uint64_t, PendingLoad>& pending_loads,
        bool preserve_pending_explicit_opens,
        std::vector<std::uint64_t>& canceled)
    {
        ShellUi::AdvanceSourceActivationIntent(
            activation_epoch,
            pending_loads,
            preserve_pending_explicit_opens,
            [&canceled](std::uint64_t task_id) { canceled.push_back(task_id); });
    }
};

}  // namespace specforge

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

specforge::ShellUiTestAccess::PendingLoad Pending(
    std::string_view name,
    std::uint64_t activation_epoch,
    specforge::ShellUiTestAccess::Purpose purpose)
{
    return {
        .path = std::filesystem::path(name),
        .path_key = std::string(name),
        .activation_epoch = activation_epoch,
        .purpose = purpose,
    };
}

void TestLaterExplicitOpenCancelsEarlierFollowUp()
{
    using Access = specforge::ShellUiTestAccess;
    std::uint64_t activation_epoch = 10;
    std::unordered_map<std::uint64_t, Access::PendingLoad> pending_loads;
    pending_loads.emplace(2, Pending("B.csv", activation_epoch, Access::Purpose::ExplicitOpen));
    pending_loads.emplace(3, Pending("C.csv", activation_epoch, Access::Purpose::ExplicitOpen));
    pending_loads.emplace(4, Pending("restore.csv", activation_epoch, Access::Purpose::DeferredRestore));
    std::vector<std::uint64_t> canceled;

    Require(
        Access::CompletionStartsActivation(Access::Purpose::ExplicitOpen, true),
        "a successful explicit completion should advance source activation");
    Access::AdvanceActivation(activation_epoch, pending_loads, true, canceled);
    Require(activation_epoch == 11, "the first successful explicit completion should advance the epoch");
    Require(canceled.empty(), "later explicit opens and deferred restore must remain pending");
    Require(
        pending_loads.at(2).activation_epoch == 11 &&
            pending_loads.at(3).activation_epoch == 11,
        "later explicit opens should remain current");

    pending_loads.emplace(5, Pending("A.csv", activation_epoch, Access::Purpose::SessionFollowUp));
    pending_loads.erase(2);  // B is the explicit completion currently being applied.
    Access::AdvanceActivation(activation_epoch, pending_loads, true, canceled);

    Require(activation_epoch == 12, "the later explicit completion should advance the epoch again");
    Require(
        canceled == std::vector<std::uint64_t>{5},
        "the later explicit completion should cancel the earlier source follow-up");
    Require(!pending_loads.contains(5), "the canceled earlier follow-up should leave pending state");
    Require(
        pending_loads.contains(3) && pending_loads.at(3).activation_epoch == 12,
        "an even later explicit open should remain pending and current");
    Require(pending_loads.contains(4), "deferred restore should remain independent");
    Require(
        !Access::CompletionStartsActivation(Access::Purpose::ExplicitOpen, false) &&
            !Access::CompletionStartsActivation(Access::Purpose::SessionFollowUp, true),
        "failed or non-explicit completions must not supersede source activation");
}

}  // namespace

int main()
{
    TestLaterExplicitOpenCancelsEarlierFollowUp();
    return 0;
}
