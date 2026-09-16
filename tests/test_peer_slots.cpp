// Per-peer connection slots. The bound counts connections, not HTTP requests:
// lws keeps one wsi across keep-alive requests and a WebSocket upgrade.

#include <logos_test.h>

#include <string>

#include "peer_slots.h"

using namespace bridge;

namespace {

constexpr int kLimit = 8;   // limits.max_connections_per_peer default
const std::string kPeer = "127.0.0.1";
const std::string kOtherPeer = "127.0.0.2";

// Distinct addresses standing in for lws wsi pointers.
int g_wsi[16];
const void* wsi(int i) { return &g_wsi[i]; }

} // namespace

LOGOS_TEST(a_keep_alive_connection_holds_one_slot_however_many_requests) {
    PeerSlots s;
    LOGOS_ASSERT_TRUE(s.acquire(wsi(0), kPeer));
    for (int i = 0; i < 11; ++i) LOGOS_ASSERT_FALSE(s.acquire(wsi(0), kPeer));
    LOGOS_ASSERT_EQ(s.count(kPeer), 1);
}

LOGOS_TEST(closing_a_keep_alive_connection_frees_its_slot) {
    PeerSlots s;
    for (int i = 0; i < 12; ++i) s.acquire(wsi(0), kPeer);
    LOGOS_ASSERT_TRUE(s.release(wsi(0)));
    LOGOS_ASSERT_EQ(s.count(kPeer), 0);
    LOGOS_ASSERT_TRUE(s.admits(kPeer, kLimit));
}

LOGOS_TEST(the_ninth_connection_is_refused_not_the_ninth_request) {
    PeerSlots s;
    for (int i = 0; i < 20; ++i) s.acquire(wsi(0), kPeer);
    LOGOS_ASSERT_TRUE(s.admits(kPeer, kLimit));

    for (int h = 1; h < kLimit; ++h) LOGOS_ASSERT_TRUE(s.acquire(wsi(h), kPeer));
    LOGOS_ASSERT_EQ(s.count(kPeer), kLimit);
    LOGOS_ASSERT_FALSE(s.admits(kPeer, kLimit));

    LOGOS_ASSERT_TRUE(s.release(wsi(3)));
    LOGOS_ASSERT_TRUE(s.admits(kPeer, kLimit));
}

LOGOS_TEST(an_upgrade_on_a_kept_alive_socket_keeps_its_one_slot) {
    PeerSlots s;
    LOGOS_ASSERT_TRUE(s.acquire(wsi(0), kPeer));    // LWS_CALLBACK_HTTP
    LOGOS_ASSERT_FALSE(s.acquire(wsi(0), kPeer));   // LWS_CALLBACK_ESTABLISHED, same wsi
    LOGOS_ASSERT_EQ(s.count(kPeer), 1);
    LOGOS_ASSERT_TRUE(s.release(wsi(0)));
    LOGOS_ASSERT_EQ(s.count(kPeer), 0);
}

// The upgrade filter admits holds(wsi) || admits(peer, limit).
LOGOS_TEST(an_upgrade_that_already_holds_its_slot_is_admitted_at_the_limit) {
    PeerSlots s;
    for (int h = 0; h < kLimit; ++h) LOGOS_ASSERT_TRUE(s.acquire(wsi(h), kPeer));
    LOGOS_ASSERT_FALSE(s.admits(kPeer, kLimit));
    LOGOS_ASSERT_TRUE(s.holds(wsi(1)));
    LOGOS_ASSERT_TRUE(s.holds(wsi(1)) || s.admits(kPeer, kLimit));    // kept-alive wsi(1) upgrades
    LOGOS_ASSERT_FALSE(s.holds(wsi(8)) || s.admits(kPeer, kLimit));   // a new connection does not

    LOGOS_ASSERT_FALSE(s.acquire(wsi(1), kPeer));
    LOGOS_ASSERT_EQ(s.count(kPeer), kLimit);
}

LOGOS_TEST(holds_is_false_after_release) {
    PeerSlots s;
    LOGOS_ASSERT_FALSE(s.holds(wsi(0)));
    s.acquire(wsi(0), kPeer);
    LOGOS_ASSERT_TRUE(s.holds(wsi(0)));
    LOGOS_ASSERT_TRUE(s.release(wsi(0)));
    LOGOS_ASSERT_FALSE(s.holds(wsi(0)));
    LOGOS_ASSERT_FALSE(s.release(wsi(0)));
    LOGOS_ASSERT_FALSE(s.holds(wsi(0)));
}

LOGOS_TEST(unknown_and_repeated_releases_are_ignored) {
    PeerSlots s;
    LOGOS_ASSERT_FALSE(s.release(wsi(9)));
    LOGOS_ASSERT_EQ(s.count(kPeer), 0);

    s.acquire(wsi(0), kPeer);
    s.acquire(wsi(1), kPeer);
    LOGOS_ASSERT_TRUE(s.release(wsi(0)));
    // A close callback followed by WSI_DESTROY must not free another connection's slot.
    LOGOS_ASSERT_FALSE(s.release(wsi(0)));
    LOGOS_ASSERT_EQ(s.count(kPeer), 1);

    LOGOS_ASSERT_TRUE(s.release(wsi(1)));
    LOGOS_ASSERT_FALSE(s.release(wsi(1)));
    LOGOS_ASSERT_EQ(s.count(kPeer), 0);
    LOGOS_ASSERT_TRUE(s.admits(kPeer, kLimit));
}

LOGOS_TEST(peers_are_counted_independently) {
    PeerSlots s;
    for (int h = 0; h < kLimit; ++h) s.acquire(wsi(h), kPeer);
    LOGOS_ASSERT_FALSE(s.admits(kPeer, kLimit));
    LOGOS_ASSERT_TRUE(s.admits(kOtherPeer, kLimit));

    LOGOS_ASSERT_TRUE(s.acquire(wsi(8), kOtherPeer));
    LOGOS_ASSERT_EQ(s.count(kOtherPeer), 1);
    LOGOS_ASSERT_EQ(s.count(kPeer), kLimit);

    LOGOS_ASSERT_TRUE(s.release(wsi(8)));
    LOGOS_ASSERT_EQ(s.count(kOtherPeer), 0);
    LOGOS_ASSERT_EQ(s.count(kPeer), kLimit);
}

// lws frees a wsi and may hand the same address to the next connection.
LOGOS_TEST(a_reused_handle_address_counts_as_a_new_connection) {
    PeerSlots s;
    LOGOS_ASSERT_TRUE(s.acquire(wsi(0), kPeer));
    LOGOS_ASSERT_TRUE(s.release(wsi(0)));
    LOGOS_ASSERT_TRUE(s.acquire(wsi(0), kPeer));
    LOGOS_ASSERT_EQ(s.count(kPeer), 1);
    LOGOS_ASSERT_TRUE(s.release(wsi(0)));
    LOGOS_ASSERT_EQ(s.count(kPeer), 0);
}

LOGOS_TEST(a_slot_returns_to_the_peer_it_was_taken_from) {
    PeerSlots s;
    LOGOS_ASSERT_TRUE(s.acquire(wsi(0), kPeer));
    LOGOS_ASSERT_FALSE(s.acquire(wsi(0), kOtherPeer));
    LOGOS_ASSERT_EQ(s.count(kOtherPeer), 0);
    LOGOS_ASSERT_EQ(s.count(kPeer), 1);

    LOGOS_ASSERT_TRUE(s.release(wsi(0)));
    LOGOS_ASSERT_EQ(s.count(kPeer), 0);
    LOGOS_ASSERT_EQ(s.count(kOtherPeer), 0);
}
