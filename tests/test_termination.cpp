// Ending a client subscription: the notice a client receives, and the id
// bookkeeping that lets it subscribe again (SubscriptionTable, as the
// bridge's loss and provider-changed paths use it).

#include <logos_test.h>

#include <string>

#include "rpc_dispatcher.h"
#include "subscription_table.h"

using namespace bridge;

namespace {

const std::string kId = nlohmann::json("s1").dump();   // the table key: the id's JSON dump

} // namespace

LOGOS_TEST(a_termination_notice_names_the_subscription_and_the_reason) {
    const nlohmann::json n =
        terminationNotice(nlohmann::json("s1"), "m1", "tick", reason::kProviderChanged);
    LOGOS_ASSERT_FALSE(n.contains("id"));
    LOGOS_ASSERT_EQ(n["method"].get<std::string>(), std::string("rpc.subscription_terminated"));
    LOGOS_ASSERT_EQ(n["params"].dump(), std::string(
        R"({"event":"tick","module":"m1","reason":"provider_changed","subscription":"s1"})"));
    // The loss path's wire form is unchanged.
    LOGOS_ASSERT_EQ(terminationNotice(nlohmann::json(7), "m1", "tick", reason::kProviderUnavailable)
                        ["params"].dump(),
                    std::string(R"({"event":"tick","module":"m1","reason":"provider_unavailable",)"
                                R"("subscription":7})"));
}

// Termination releases the exact (id, owner); the client's next subscribe is Fresh.
LOGOS_TEST(a_resubscribe_after_termination_is_fresh) {
    SubscriptionTable subs;
    LOGOS_ASSERT_TRUE(subs.claim(kId, "m1", "tick", 7, 8) == SubscriptionTable::Claim::Fresh);
    LOGOS_ASSERT_TRUE(subs.claim(kId, "m1", "tick", 8, 8) == SubscriptionTable::Claim::Duplicate);
    LOGOS_ASSERT_TRUE(subs.release(kId, 7));   // what ending subscriber 7 does
    LOGOS_ASSERT_TRUE(subs.claim(kId, "m1", "tick", 9, 8) == SubscriptionTable::Claim::Fresh);
    SubscriptionTable::Entry held;
    LOGOS_ASSERT_TRUE(subs.take(kId, &held));
    LOGOS_ASSERT_EQ(held.owner, static_cast<std::uint64_t>(9));
}

// Ending one module's subscriber leaves the connection's other ids alone.
LOGOS_TEST(termination_frees_only_the_ended_id) {
    SubscriptionTable subs;
    const std::string other = nlohmann::json("s2").dump();
    subs.claim(kId, "m1", "tick", 7, 8);
    subs.claim(other, "m2", "tick", 8, 8);
    LOGOS_ASSERT_TRUE(subs.release(kId, 7));
    LOGOS_ASSERT_EQ(subs.size(), static_cast<size_t>(1));
    LOGOS_ASSERT_TRUE(subs.claim(other, "m2", "tick", 10, 8) == SubscriptionTable::Claim::Duplicate);
}

// A client that re-subscribed before a second notice for its old subscriber keeps its new one.
LOGOS_TEST(a_late_termination_cannot_end_the_resubscription) {
    SubscriptionTable subs;
    subs.claim(kId, "m1", "tick", 7, 8);
    LOGOS_ASSERT_TRUE(subs.release(kId, 7));
    LOGOS_ASSERT_TRUE(subs.claim(kId, "m1", "tick", 9, 8) == SubscriptionTable::Claim::Fresh);
    LOGOS_ASSERT_FALSE(subs.release(kId, 7));
    LOGOS_ASSERT_TRUE(subs.claim(kId, "m1", "tick", 10, 8) == SubscriptionTable::Claim::Duplicate);
}
