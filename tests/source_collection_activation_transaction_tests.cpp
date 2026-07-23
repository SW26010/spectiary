#include "ui/source_collection_activation_transaction.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Transaction =
    specforge::SourceCollectionActivationTransaction;

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::vector<std::uint64_t> TaskIds(
    const std::vector<Transaction::PendingTask>& pending)
{
    std::vector<std::uint64_t> task_ids;
    task_ids.reserve(pending.size());
    for (const Transaction::PendingTask& task : pending) {
        task_ids.push_back(task.task_id);
    }
    std::sort(task_ids.begin(), task_ids.end());
    return task_ids;
}

void TestLaterExplicitOpenCancelsEarlierFollowUp()
{
    Transaction transaction;
    transaction.RegisterLoad(
        2,
        transaction.ReserveLoad(
            "B.csv",
            0,
            Transaction::Purpose::ExplicitOpen));
    transaction.RegisterLoad(
        3,
        transaction.ReserveLoad(
            "C.csv",
            0,
            Transaction::Purpose::ExplicitOpen));
    transaction.RegisterLoad(
        4,
        transaction.ReserveLoad(
            "restore.csv",
            0,
            Transaction::Purpose::DeferredRestore));
    transaction.RegisterLoad(
        6,
        transaction.ReserveLoad(
            "D.csv",
            0,
            Transaction::Purpose::ExplicitOpen));

    const auto first_completion =
        transaction.TakeCompletion(2, "B.csv", 0);
    Require(
        first_completion && first_completion->accepted,
        "the first explicit completion should be admitted");
    Require(
        Transaction::CompletionStartsActivationIntent(
            first_completion->ticket.purpose,
            true),
        "a successful explicit completion should start activation");
    Require(
        transaction.AdvanceIntent(true).empty(),
        "later explicit opens and deferred restore must remain pending");
    Require(
        transaction.ActivationEpoch() == 1,
        "the first successful explicit completion should advance the epoch");

    transaction.RegisterLoad(
        5,
        transaction.ReserveLoad(
            "A.csv",
            1,
            Transaction::Purpose::SessionFollowUp));
    const auto later_completion =
        transaction.TakeCompletion(3, "C.csv", 0);
    Require(
        later_completion && later_completion->accepted,
        "the later explicit completion should remain current");
    const std::vector<Transaction::PendingTask> superseded =
        transaction.AdvanceIntent(true);

    Require(
        transaction.ActivationEpoch() == 2,
        "the later explicit completion should advance the epoch again");
    Require(
        TaskIds(superseded) == std::vector<std::uint64_t>{5},
        "the later explicit completion should cancel the earlier follow-up");
    Require(
        transaction.HasPendingDeferredRestore(),
        "deferred restore should remain independent");
    const auto final_explicit =
        transaction.TakeCompletion(6, "D.csv", 0);
    Require(
        final_explicit && final_explicit->accepted,
        "an even later explicit open should be updated to the new epoch");
}

void TestFailedOrNonExplicitCompletionDoesNotAdvanceEpoch()
{
    Transaction transaction;

    if (Transaction::CompletionStartsActivationIntent(
            Transaction::Purpose::ExplicitOpen,
            false)) {
        (void)transaction.AdvanceIntent(true);
    }
    if (Transaction::CompletionStartsActivationIntent(
            Transaction::Purpose::SessionFollowUp,
            true)) {
        (void)transaction.AdvanceIntent(true);
    }
    if (Transaction::CompletionStartsActivationIntent(
            Transaction::Purpose::DeferredRestore,
            true)) {
        (void)transaction.AdvanceIntent(true);
    }

    Require(
        transaction.ActivationEpoch() == 0,
        "failed or non-explicit completions must not advance the epoch");
    Require(
        Transaction::CompletionStartsActivationIntent(
            Transaction::Purpose::ExplicitOpen,
            true),
        "a successful explicit completion should advance the epoch");
}

void TestStaleCompletionIsRejectedByEveryIdentityBoundary()
{
    Transaction transaction;

    transaction.RegisterLoad(
        1,
        transaction.ReserveLoad(
            "task.csv",
            0,
            Transaction::Purpose::SessionFollowUp));
    Require(
        !transaction.TakeCompletion(99, "task.csv", 0),
        "an unknown task must be rejected without consuming the real ticket");
    const auto task_current =
        transaction.TakeCompletion(1, "task.csv", 0);
    Require(
        task_current && task_current->accepted,
        "the real task should remain admissible after an unknown task");

    transaction.RegisterLoad(
        2,
        transaction.ReserveLoad(
            "path.csv",
            0,
            Transaction::Purpose::SessionFollowUp));
    const auto wrong_path =
        transaction.TakeCompletion(2, "other.csv", 0);
    Require(
        wrong_path && !wrong_path->accepted,
        "a completion with the wrong path must be rejected");

    transaction.RegisterLoad(
        3,
        transaction.ReserveLoad(
            "index.csv",
            4,
            Transaction::Purpose::SessionFollowUp));
    const auto wrong_index =
        transaction.TakeCompletion(3, "index.csv", 5);
    Require(
        wrong_index && !wrong_index->accepted,
        "a completion with the wrong spectrum index must be rejected");

    Transaction::Ticket stale_generation =
        transaction.ReserveLoad(
            "generation.csv",
            1,
            Transaction::Purpose::SessionFollowUp);
    (void)transaction.ReserveLoad(
        "generation.csv",
        2,
        Transaction::Purpose::SessionFollowUp);
    transaction.RegisterLoad(
        4,
        std::move(stale_generation));
    const auto wrong_generation =
        transaction.TakeCompletion(4, "generation.csv", 1);
    Require(
        wrong_generation && !wrong_generation->accepted,
        "a completion from an older path generation must be rejected");

    Transaction::Ticket stale_epoch =
        transaction.ReserveLoad(
            "epoch.csv",
            1,
            Transaction::Purpose::SessionFollowUp);
    Require(
        transaction.AdvanceIntent(false).empty(),
        "an unregistered reservation should not create a pending task");
    transaction.RegisterLoad(5, std::move(stale_epoch));
    const auto wrong_epoch =
        transaction.TakeCompletion(5, "epoch.csv", 1);
    Require(
        wrong_epoch && !wrong_epoch->accepted,
        "a non-deferred completion from an older epoch must be rejected");
}

void TestRemovedSourceCancelsOnlyItsDerivedTickets()
{
    Transaction transaction;
    transaction.RegisterLoad(
        1,
        transaction.ReserveLoad(
            "A.csv",
            1,
            Transaction::Purpose::SessionFollowUp));
    transaction.RegisterLoad(
        2,
        transaction.ReserveLoad(
            "B.csv",
            1,
            Transaction::Purpose::SessionFollowUp));
    transaction.RegisterLoad(
        3,
        transaction.ReserveLoad(
            "B.csv",
            2,
            Transaction::Purpose::DeferredRestore));
    transaction.RegisterLoad(
        4,
        transaction.ReserveLoad(
            "B.csv",
            0,
            Transaction::Purpose::ExplicitOpen));

    const std::vector<Transaction::PendingTask> canceled =
        transaction.CancelNonExplicitFollowUps("B.csv");

    Require(
        TaskIds(canceled) ==
            std::vector<std::uint64_t>{2, 3},
        "removing B should cancel B's derived tickets");
    Require(
        transaction.PendingLoadCount() == 2,
        "removing B must retain A's navigation and B's explicit open");
    Require(
        transaction.HasMatchingFollowUp("A.csv", 1),
        "another source's follow-up must remain pending");
    Require(
        !transaction.HasMatchingFollowUp("B.csv", 1) &&
            !transaction.HasMatchingFollowUp("B.csv", 2),
        "the removed source must have no derived ticket");
    Require(
        !transaction.HasPendingDeferredRestore(),
        "canceling B's restore should clear restore bookkeeping");
}

void TestDeferredRestoreIgnoresOrdinaryActivationEpoch()
{
    Transaction transaction;
    transaction.RegisterLoad(
        1,
        transaction.ReserveLoad(
            "restore.csv",
            3,
            Transaction::Purpose::DeferredRestore));
    transaction.RegisterLoad(
        2,
        transaction.ReserveLoad(
            "navigation.csv",
            1,
            Transaction::Purpose::SessionFollowUp));

    const std::vector<Transaction::PendingTask> superseded =
        transaction.AdvanceIntent(false);
    Require(
        TaskIds(superseded) ==
            std::vector<std::uint64_t>{2},
        "ordinary activation should cancel navigation but retain restore");
    Require(
        transaction.ActivationEpoch() == 1 &&
            transaction.HasPendingDeferredRestore(),
        "deferred restore should remain pending across an epoch change");

    const auto restore_completion =
        transaction.TakeCompletion(1, "restore.csv", 3);
    Require(
        restore_completion && restore_completion->accepted,
        "deferred restore should still validate generation, path, and index");
    Require(
        !transaction.HasPendingDeferredRestore(),
        "accepted restore completion should clear its bookkeeping");
}

void TestSamePathAndIndexFollowUpIsDeduplicated()
{
    Transaction transaction;
    transaction.RegisterLoad(
        1,
        transaction.ReserveLoad(
            "source.csv",
            7,
            Transaction::Purpose::ExplicitOpen));
    Require(
        !transaction.HasMatchingFollowUp("source.csv", 7),
        "an explicit open is not a derived follow-up");

    transaction.RegisterLoad(
        2,
        transaction.ReserveLoad(
            "source.csv",
            7,
            Transaction::Purpose::SessionFollowUp));
    Require(
        transaction.HasMatchingFollowUp("source.csv", 7),
        "the same path and index follow-up should be found");
    Require(
        !transaction.HasMatchingFollowUp("source.csv", 8) &&
            !transaction.HasMatchingFollowUp("other.csv", 7),
        "deduplication must match both path and spectrum index");
}

void TestRegisterOrReplaceReturnsSupersededTicket()
{
    Transaction transaction;
    transaction.RegisterLoad(
        1,
        transaction.ReserveLoad(
            "source.csv",
            1,
            Transaction::Purpose::ExplicitOpen));
    const std::vector<Transaction::PendingTask> replaced =
        transaction.RegisterOrReplaceLoad(
            2,
            transaction.ReserveLoad(
                "source.csv",
                2,
                Transaction::Purpose::ExplicitOpen));

    Require(
        TaskIds(replaced) == std::vector<std::uint64_t>{1},
        "replacing a path should return the superseded task to the caller");
    Require(
        transaction.PendingLoadCount() == 1,
        "replacement should retain only the new task");
    const auto completion =
        transaction.TakeCompletion(2, "source.csv", 2);
    Require(
        completion && completion->accepted,
        "the replacement task should be current");
}

}  // namespace

int main()
{
    try {
        TestLaterExplicitOpenCancelsEarlierFollowUp();
        TestFailedOrNonExplicitCompletionDoesNotAdvanceEpoch();
        TestStaleCompletionIsRejectedByEveryIdentityBoundary();
        TestRemovedSourceCancelsOnlyItsDerivedTickets();
        TestDeferredRestoreIgnoresOrdinaryActivationEpoch();
        TestSamePathAndIndexFollowUpIsDeduplicated();
        TestRegisterOrReplaceReturnsSupersededTicket();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
