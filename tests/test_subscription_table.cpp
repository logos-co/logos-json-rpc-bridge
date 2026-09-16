// Per-connection subscription ids. A failed or lost subscription frees exactly
// its own id, and only while it still holds it.

#include <logos_test.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "subscription_table.h"

using namespace bridge;
using Claim = SubscriptionTable::Claim;

namespace {

constexpr int kLimit = 256;   // limits.max_subscriptions_per_connection default
const std::string kModule = "provider_module";
const std::string kTick = "tick";
const std::string kTock = "tock";

// The bridge keys a client-assigned id by its JSON dump.
std::string key(const nlohmann::json& id) { return id.dump(); }

} // namespace

LOGOS_TEST(a_failed_subscribe_frees_its_id_so_a_retry_is_fresh) {
    SubscriptionTable t;
    LOGOS_ASSERT_TRUE(t.claim(key("s1"), kModule, kTick, 1, kLimit) == Claim::Fresh);
    LOGOS_ASSERT_TRUE(t.release(key("s1"), 1));   // hub.add failed, or the pump refused the job
    LOGOS_ASSERT_EQ(t.size(), static_cast<std::size_t>(0));
    LOGOS_ASSERT_TRUE(t.claim(key("s1"), kModule, kTick, 2, kLimit) == Claim::Fresh);
}

LOGOS_TEST(a_live_id_is_a_duplicate_and_keeps_its_first_owner) {
    SubscriptionTable t;
    LOGOS_ASSERT_TRUE(t.claim(key("s1"), kModule, kTick, 1, kLimit) == Claim::Fresh);
    LOGOS_ASSERT_TRUE(t.claim(key("s1"), kModule, kTick, 2, kLimit) == Claim::Duplicate);
    LOGOS_ASSERT_FALSE(t.release(key("s1"), 2));
    LOGOS_ASSERT_EQ(t.size(), static_cast<std::size_t>(1));
    LOGOS_ASSERT_TRUE(t.release(key("s1"), 1));
}

LOGOS_TEST(a_stale_release_cannot_free_a_newer_claim_on_the_same_id) {
    SubscriptionTable t;
    LOGOS_ASSERT_TRUE(t.claim(key("s1"), kModule, kTick, 1, kLimit) == Claim::Fresh);
    SubscriptionTable::Entry e;
    LOGOS_ASSERT_TRUE(t.take(key("s1"), &e));      // unsubscribed while 1 is still in flight
    LOGOS_ASSERT_TRUE(t.claim(key("s1"), kModule, kTick, 2, kLimit) == Claim::Fresh);

    LOGOS_ASSERT_FALSE(t.release(key("s1"), 1));   // 1 fails late
    LOGOS_ASSERT_TRUE(t.claim(key("s1"), kModule, kTick, 3, kLimit) == Claim::Duplicate);
    LOGOS_ASSERT_TRUE(t.release(key("s1"), 2));
}

// "beta" sorts after "alpha", so a first-match release would free the wrong id.
LOGOS_TEST(losing_one_of_two_ids_on_the_same_event_frees_exactly_that_id) {
    SubscriptionTable t;
    LOGOS_ASSERT_TRUE(t.claim(key("alpha"), kModule, kTick, 1, kLimit) == Claim::Fresh);
    LOGOS_ASSERT_TRUE(t.claim(key("beta"), kModule, kTick, 2, kLimit) == Claim::Fresh);

    LOGOS_ASSERT_TRUE(t.release(key("beta"), 2));
    LOGOS_ASSERT_EQ(t.size(), static_cast<std::size_t>(1));
    LOGOS_ASSERT_TRUE(t.claim(key("alpha"), kModule, kTick, 3, kLimit) == Claim::Duplicate);
    LOGOS_ASSERT_TRUE(t.claim(key("beta"), kModule, kTick, 4, kLimit) == Claim::Fresh);
}

LOGOS_TEST(the_limit_counts_live_ids_and_a_release_makes_room) {
    SubscriptionTable t;
    const int limit = 2;
    LOGOS_ASSERT_TRUE(t.claim(key("a"), kModule, kTick, 1, limit) == Claim::Fresh);
    LOGOS_ASSERT_TRUE(t.claim(key("b"), kModule, kTock, 2, limit) == Claim::Fresh);
    LOGOS_ASSERT_TRUE(t.claim(key("c"), kModule, kTick, 3, limit) == Claim::Full);

    LOGOS_ASSERT_TRUE(t.release(key("a"), 1));
    LOGOS_ASSERT_TRUE(t.claim(key("c"), kModule, kTick, 4, limit) == Claim::Fresh);
    LOGOS_ASSERT_EQ(t.size(), static_cast<std::size_t>(2));
}

LOGOS_TEST(re_subscribing_a_live_id_at_the_limit_is_still_idempotent) {
    SubscriptionTable t;
    const int limit = 2;
    LOGOS_ASSERT_TRUE(t.claim(key("a"), kModule, kTick, 1, limit) == Claim::Fresh);
    LOGOS_ASSERT_TRUE(t.claim(key("b"), kModule, kTick, 2, limit) == Claim::Fresh);
    LOGOS_ASSERT_TRUE(t.claim(key("a"), kModule, kTick, 3, limit) == Claim::Duplicate);
    LOGOS_ASSERT_TRUE(t.claim(key("b"), kModule, kTick, 4, limit) == Claim::Duplicate);
    LOGOS_ASSERT_EQ(t.size(), static_cast<std::size_t>(2));
}

LOGOS_TEST(unsubscribe_takes_the_id_and_reports_its_target) {
    SubscriptionTable t;
    LOGOS_ASSERT_TRUE(t.claim(key(42), kModule, kTock, 7, kLimit) == Claim::Fresh);

    SubscriptionTable::Entry e;
    LOGOS_ASSERT_TRUE(t.take(key(42), &e));
    LOGOS_ASSERT_EQ(e.module, kModule);
    LOGOS_ASSERT_EQ(e.event, kTock);
    LOGOS_ASSERT_EQ(e.owner, static_cast<std::uint64_t>(7));

    LOGOS_ASSERT_FALSE(t.take(key(42), &e));
    LOGOS_ASSERT_FALSE(t.release(key(42), 7));
    LOGOS_ASSERT_TRUE(t.claim(key(42), kModule, kTock, 8, kLimit) == Claim::Fresh);
}

LOGOS_TEST(a_refused_claim_changes_nothing) {
    SubscriptionTable t;
    const int limit = 1;
    LOGOS_ASSERT_TRUE(t.claim(key("a"), kModule, kTick, 1, limit) == Claim::Fresh);
    LOGOS_ASSERT_TRUE(t.claim(key("a"), "other_module", kTock, 2, limit) == Claim::Duplicate);
    LOGOS_ASSERT_TRUE(t.claim(key("b"), kModule, kTick, 3, limit) == Claim::Full);
    LOGOS_ASSERT_EQ(t.size(), static_cast<std::size_t>(1));
    LOGOS_ASSERT_FALSE(t.release(key("b"), 3));
    LOGOS_ASSERT_FALSE(t.release(key("a"), 2));

    SubscriptionTable::Entry e;
    LOGOS_ASSERT_TRUE(t.take(key("a"), &e));
    LOGOS_ASSERT_EQ(e.module, kModule);
    LOGOS_ASSERT_EQ(e.event, kTick);
    LOGOS_ASSERT_EQ(e.owner, static_cast<std::uint64_t>(1));
}
