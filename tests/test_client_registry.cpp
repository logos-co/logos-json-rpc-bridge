// ClientRegistry and SubscriptionHub over a fake client: calls complete when the
// test says so, posted jobs run by hand, and the clock is the test's.

#include <logos_test.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "recycle_policy.h"
#include "upstream.h"

using namespace bridge;
using namespace std::chrono_literals;

namespace {

using Clock = RecyclePolicy::Clock;
using Json = nlohmann::json;
using Done = std::function<void(Json, const logos::CallError&)>;

struct FakeClient;

struct FakeSub {
    int client = 0;
    std::string event;
    std::function<void(Json)> deliver;
    bool cancelled = false;
};

// Move-only, like logos::LpSubscription: destroying it cancels the subscription.
struct FakeHandle {
    FakeHandle() = default;
    explicit FakeHandle(std::shared_ptr<FakeSub> s) : sub(std::move(s)) {}
    FakeHandle(FakeHandle&& o) noexcept : sub(std::move(o.sub)) {}
    FakeHandle& operator=(FakeHandle&& o) noexcept {
        if (this != &o) { reset(); sub = std::move(o.sub); }
        return *this;
    }
    ~FakeHandle() { reset(); }
    bool valid() const { return sub != nullptr; }
    void reset() {
        if (sub) sub->cancelled = true;
        sub.reset();
    }
    std::shared_ptr<FakeSub> sub;
};

struct Call {
    int client = 0;
    std::string method;
    Done done;
};

// Everything the fake clients did, in one place.
struct World {
    Clock::time_point now = Clock::time_point() + 1h;
    int nextId = 1;
    std::map<int, FakeClient*> alive;
    std::vector<int> destroyed;
    int destroyedInsideCallback = 0;
    int inCallbackOf = 0;
    std::vector<Call> calls;
    std::vector<std::shared_ptr<FakeSub>> subs;
    std::map<int, std::function<void(logos::SubStatus, std::uint64_t)>> watchers;
    std::function<void(int client)> onSubscribe;   // runs inside subscribe()

    std::vector<CallPump::Job> posted;
    bool postAccepts = true;

    size_t pending(int client) const {
        size_t n = 0;
        for (const auto& c : calls)
            if (c.client == client) ++n;
        return n;
    }

    // Answers the oldest call to `client`, as the protocol would: only while the client lives.
    void complete(int client, Json value, logos::CallError e = {}) {
        for (auto it = calls.begin(); it != calls.end(); ++it) {
            if (it->client != client) continue;
            Done done = std::move(it->done);
            calls.erase(it);
            if (!alive.count(client)) return;
            inCallbackOf = client;
            done(std::move(value), e);
            inCallbackOf = 0;
            return;
        }
        throw LogosTestFailure("no outstanding call to client " + std::to_string(client));
    }

    void unavailable(int client) { complete(client, Json(), {"object_unavailable", "gone", "m1"}); }

    size_t runPosted() {
        size_t n = 0;
        while (!posted.empty()) {
            CallPump::Job job = std::move(posted.front());
            posted.erase(posted.begin());
            job();
            ++n;
        }
        return n;
    }

    void publish(int client, const std::string& event, Json payload) {
        for (const auto& s : subs)
            if (s->client == client && s->event == event && !s->cancelled) s->deliver(payload);
    }

    void status(int client, logos::SubStatus state, std::uint64_t generation) {
        auto it = watchers.find(client);
        if (it != watchers.end() && alive.count(client)) it->second(state, generation);
    }

    size_t liveSubs(int client) const {
        size_t n = 0;
        for (const auto& s : subs)
            if (s->client == client && !s->cancelled) ++n;
        return n;
    }

    bool isDestroyed(int client) const {
        for (int d : destroyed)
            if (d == client) return true;
        return false;
    }
};

World* g_world = nullptr;

struct FakeClient {
    FakeClient(std::string target, std::string origin)
        : id(g_world->nextId++), target(std::move(target)), origin(std::move(origin)) {
        g_world->alive[id] = this;
    }
    ~FakeClient() {
        g_world->alive.erase(id);
        g_world->destroyed.push_back(id);
        if (g_world->inCallbackOf == id) ++g_world->destroyedInsideCallback;
    }
    void invokeAsyncResult(const std::string& method, const Json&, Done cb, int) {
        g_world->calls.push_back(Call{id, method, std::move(cb)});
    }
    FakeHandle subscribe(const std::string& event, std::function<void(Json)> cb) {
        auto s = std::make_shared<FakeSub>();
        s->client = id;
        s->event = event;
        s->deliver = std::move(cb);
        g_world->subs.push_back(s);
        if (g_world->onSubscribe) g_world->onSubscribe(id);
        return FakeHandle(s);
    }
    void onSubscriptionStatus(std::function<void(logos::SubStatus, std::uint64_t)> cb) {
        g_world->watchers[id] = std::move(cb);
    }

    int id;
    std::string target, origin;
};

using Registry = BasicClientRegistry<FakeClient>;
using Hub = BasicSubscriptionHub<FakeClient>;

struct Fixture {
    World world;
    std::shared_ptr<Registry> reg;
    std::vector<std::pair<std::string, std::uint64_t>> replaced;

    Fixture() {
        g_world = &world;
        reg = std::make_shared<Registry>("json_rpc_bridge", [this] { return world.now; });
        reg->setPost([this](CallPump::Job job) {
            if (!world.postAccepts) return false;
            world.posted.push_back(std::move(job));
            return true;
        });
        reg->setOnReplaced([this](const std::string& m, std::uint64_t e) { replaced.emplace_back(m, e); });
    }
    // Pending calls and posted jobs hold clients too: drop them while the world still exists.
    ~Fixture() {
        world.calls.clear();
        world.posted.clear();
        reg.reset();
        world.subs.clear();
        world.watchers.clear();
        g_world = nullptr;
    }

    // Starts a call and returns where its answer will go.
    std::shared_ptr<std::vector<std::string>> call(const std::string& module) {
        auto answers = std::make_shared<std::vector<std::string>>();
        reg->invoke(module, "ping", Json::array(),
                    [answers](Json, const logos::CallError& e) {
                        answers->push_back(e.ok() ? "ok" : e.code);
                    },
                    5000);
        return answers;
    }
};

logos::CallError reached() { return {}; }

} // namespace

// ── RecyclePolicy ───────────────────────────────────────────────────────────

LOGOS_TEST(recycle_policy_waits_for_the_first_gap_and_then_backs_off) {
    const Clock::time_point t0 = Clock::time_point() + 1h;
    RecyclePolicy p(t0);
    LOGOS_ASSERT_FALSE(p.onUnavailable(t0 + 1999ms));
    LOGOS_ASSERT_TRUE(p.onUnavailable(t0 + 2s));
    LOGOS_ASSERT_EQ(p.gap().count(), 4000L);
    LOGOS_ASSERT_FALSE(p.onUnavailable(t0 + 5s));
    LOGOS_ASSERT_TRUE(p.onUnavailable(t0 + 6s));
    LOGOS_ASSERT_EQ(p.gap().count(), 8000L);
    Clock::time_point t = t0 + 6s;
    for (int i = 0; i < 10; ++i) {
        t += p.gap();
        LOGOS_ASSERT_TRUE(p.onUnavailable(t));
    }
    LOGOS_ASSERT_EQ(p.gap().count(), 60000L);
}

LOGOS_TEST(recycle_policy_starts_over_once_the_provider_answers) {
    const Clock::time_point t0 = Clock::time_point() + 1h;
    RecyclePolicy p(t0);
    LOGOS_ASSERT_TRUE(p.onUnavailable(t0 + 10s));
    LOGOS_ASSERT_TRUE(p.onUnavailable(t0 + 14s));
    p.onReached();
    LOGOS_ASSERT_EQ(p.gap().count(), 2000L);
    LOGOS_ASSERT_FALSE(p.onUnavailable(t0 + 15s));
    LOGOS_ASSERT_TRUE(p.onUnavailable(t0 + 16s));
}

// ── ClientRegistry ──────────────────────────────────────────────────────────

LOGOS_TEST(registry_keeps_one_client_per_module) {
    Fixture f;
    const auto a = f.reg->get("m1");
    const auto b = f.reg->get("m1");
    const auto c = f.reg->get("m2");
    LOGOS_ASSERT_TRUE(a.client == b.client);
    LOGOS_ASSERT_EQ(a.epoch, static_cast<std::uint64_t>(1));
    LOGOS_ASSERT_TRUE(a.client != c.client);
    LOGOS_ASSERT_EQ(c.epoch, static_cast<std::uint64_t>(1));
    LOGOS_ASSERT_EQ(a.client->target, std::string("m1"));
    LOGOS_ASSERT_EQ(a.client->origin, std::string("json_rpc_bridge"));
    LOGOS_ASSERT_EQ(f.reg->epoch("m3"), static_cast<std::uint64_t>(0));
}

LOGOS_TEST(registry_replaces_a_client_that_keeps_finding_its_module_unavailable) {
    Fixture f;
    auto early = f.call("m1");
    f.world.now += 1s;
    f.world.unavailable(1);   // inside the first gap: nothing to do yet
    LOGOS_ASSERT_EQ(early->at(0), std::string("object_unavailable"));
    LOGOS_ASSERT_TRUE(f.world.posted.empty());

    auto late = f.call("m1");
    f.world.now += 2s;
    f.world.unavailable(1);
    LOGOS_ASSERT_EQ(f.world.posted.size(), static_cast<size_t>(1));
    LOGOS_ASSERT_FALSE(f.world.isDestroyed(1));
    LOGOS_ASSERT_EQ(f.world.runPosted(), static_cast<size_t>(1));

    LOGOS_ASSERT_EQ(f.reg->epoch("m1"), static_cast<std::uint64_t>(2));
    LOGOS_ASSERT_EQ(f.reg->get("m1").client->id, 2);
    LOGOS_ASSERT_EQ(f.reg->replacements(), static_cast<std::uint64_t>(1));
    LOGOS_ASSERT_EQ(f.replaced.size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(f.replaced[0].first, std::string("m1"));
    LOGOS_ASSERT_EQ(f.replaced[0].second, static_cast<std::uint64_t>(2));
    LOGOS_ASSERT_TRUE(f.world.isDestroyed(1));
    LOGOS_ASSERT_EQ(f.world.destroyedInsideCallback, 0);

    auto next = f.call("m1");
    LOGOS_ASSERT_EQ(f.world.pending(2), static_cast<size_t>(1));
    f.world.complete(2, Json("pong"), reached());
    LOGOS_ASSERT_EQ(next->at(0), std::string("ok"));
}

LOGOS_TEST(registry_replaces_once_for_a_burst_of_unavailable_answers) {
    Fixture f;
    f.call("m1");
    f.call("m1");
    f.call("m1");
    f.world.now += 3s;
    f.world.unavailable(1);
    f.world.unavailable(1);
    f.world.unavailable(1);
    LOGOS_ASSERT_EQ(f.world.runPosted(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(f.reg->replacements(), static_cast<std::uint64_t>(1));
}

LOGOS_TEST(registry_keeps_a_replaced_client_until_its_calls_are_answered) {
    Fixture f;
    auto a = f.call("m1");
    auto b = f.call("m1");
    f.world.now += 10s;
    LOGOS_ASSERT_TRUE(f.reg->replace("m1", 1));
    LOGOS_ASSERT_FALSE(f.world.isDestroyed(1));
    LOGOS_ASSERT_EQ(f.reg->retiredCount(), static_cast<size_t>(1));

    // The old client's answers are still delivered, and no longer judge the module.
    f.world.unavailable(1);
    LOGOS_ASSERT_EQ(a->at(0), std::string("object_unavailable"));
    LOGOS_ASSERT_TRUE(f.world.posted.empty());
    f.world.complete(1, Json(1), reached());
    LOGOS_ASSERT_EQ(b->at(0), std::string("ok"));

    // Released on the pump, never inside its own callback.
    LOGOS_ASSERT_FALSE(f.world.isDestroyed(1));
    LOGOS_ASSERT_EQ(f.world.runPosted(), static_cast<size_t>(1));
    LOGOS_ASSERT_TRUE(f.world.isDestroyed(1));
    LOGOS_ASSERT_EQ(f.world.destroyedInsideCallback, 0);
    LOGOS_ASSERT_EQ(f.reg->retiredCount(), static_cast<size_t>(0));
}

LOGOS_TEST(registry_ignores_a_replacement_for_a_client_already_replaced) {
    Fixture f;
    f.reg->get("m1");
    LOGOS_ASSERT_TRUE(f.reg->replace("m1", 1));
    LOGOS_ASSERT_FALSE(f.reg->replace("m1", 1));
    LOGOS_ASSERT_FALSE(f.reg->replace("m9", 1));
    LOGOS_ASSERT_EQ(f.reg->epoch("m1"), static_cast<std::uint64_t>(2));
    LOGOS_ASSERT_EQ(f.reg->replacements(), static_cast<std::uint64_t>(1));
    LOGOS_ASSERT_EQ(f.replaced.size(), static_cast<size_t>(1));
}

LOGOS_TEST(registry_answer_resets_the_backoff) {
    Fixture f;
    f.call("m1");
    f.world.now += 2s;
    f.world.unavailable(1);
    f.world.runPosted();                       // client 2, next gap 4 s
    f.call("m1");
    f.world.now += 1s;
    f.world.complete(2, Json(1), reached());   // back to 2 s
    f.call("m1");
    f.world.now += 1500ms;
    f.world.unavailable(2);                    // 2.5 s after the replacement
    LOGOS_ASSERT_EQ(f.world.runPosted(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(f.reg->epoch("m1"), static_cast<std::uint64_t>(3));
}

LOGOS_TEST(registry_releases_an_old_client_whose_calls_never_return) {
    Fixture f;
    f.call("m1");
    f.reg->replace("m1", 1);
    f.world.now += Registry::kRetiredDeadline - 1s;
    f.call("m1");
    LOGOS_ASSERT_FALSE(f.world.isDestroyed(1));
    f.world.now += 2s;
    f.call("m1");   // the sweep runs here, on the pump
    LOGOS_ASSERT_TRUE(f.world.isDestroyed(1));
    LOGOS_ASSERT_EQ(f.reg->retiredCount(), static_cast<size_t>(0));
    LOGOS_ASSERT_EQ(f.world.destroyedInsideCallback, 0);
}

LOGOS_TEST(registry_close_destroys_current_and_replaced_clients) {
    Fixture f;
    auto a = f.call("m1");
    f.reg->replace("m1", 1);
    f.reg->get("m2");
    f.reg->close();
    LOGOS_ASSERT_TRUE(f.world.alive.empty());
    LOGOS_ASSERT_FALSE(static_cast<bool>(f.reg->get("m1")));
    f.world.unavailable(1);   // a destroyed client never answers
    LOGOS_ASSERT_TRUE(a->empty());
    auto after = f.call("m1");
    LOGOS_ASSERT_EQ(after->size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(after->at(0), std::string("object_unavailable"));
    LOGOS_ASSERT_FALSE(f.reg->replace("m1", 2));
}

LOGOS_TEST(registry_keeps_an_old_client_when_the_pump_refuses_its_release) {
    Fixture f;
    f.call("m1");
    f.reg->replace("m1", 1);
    f.world.postAccepts = false;
    f.world.complete(1, Json(1), reached());
    LOGOS_ASSERT_FALSE(f.world.isDestroyed(1));
    f.reg->close();
    LOGOS_ASSERT_TRUE(f.world.isDestroyed(1));
    LOGOS_ASSERT_EQ(f.world.destroyedInsideCallback, 0);
}

// ── SubscriptionHub ─────────────────────────────────────────────────────────

namespace {

struct HubFixture : Fixture {
    std::vector<std::string> delivered;   // "<subscriber>:<payload>:<generation>"
    std::vector<std::string> lost;        // "<subscriber>:<event>:<reason>"
    std::vector<std::string> statuses;    // "<state>:<generation>"
    std::unique_ptr<Hub> hub;

    HubFixture() {
        hub = std::make_unique<Hub>(
            reg.get(),
            [this](const Delivery& d) {
                delivered.push_back(std::to_string(d.subscriberId) + ":" + d.data.dump() + ":" +
                                    std::to_string(d.generation));
            },
            [this](std::uint64_t id, const std::string&, const std::string& event, const char* why) {
                lost.push_back(std::to_string(id) + ":" + event + ":" + why);
            });
        hub->setModuleStatusHook([this](const std::string&, logos::SubStatus s, std::uint64_t g) {
            statuses.push_back(std::to_string(static_cast<int>(s)) + ":" + std::to_string(g));
        });
        reg->setOnReplaced([this](const std::string& m, std::uint64_t e) {
            replaced.emplace_back(m, e);
            hub->retire(m, e);
            hub->rebind(m);
        });
    }
    ~HubFixture() {
        hub->clear();
        reg->close();
        hub.reset();
    }
};

} // namespace

LOGOS_TEST(hub_moves_subscriptions_to_the_new_client_and_keeps_their_subscribers) {
    HubFixture f;
    LOGOS_ASSERT_TRUE(f.hub->add("m1", "ev", 7));
    LOGOS_ASSERT_TRUE(f.hub->add("m1", "other", 8));
    LOGOS_ASSERT_EQ(f.world.liveSubs(1), static_cast<size_t>(2));
    f.world.publish(1, "ev", Json::array({1}));
    LOGOS_ASSERT_EQ(f.delivered.back(), std::string("7:[1]:1"));

    LOGOS_ASSERT_TRUE(f.reg->replace("m1", 1));
    LOGOS_ASSERT_EQ(f.world.liveSubs(1), static_cast<size_t>(0));
    LOGOS_ASSERT_EQ(f.world.liveSubs(2), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(f.hub->upstreamCount(), static_cast<size_t>(2));

    f.world.publish(2, "ev", Json::array({2}));
    f.world.publish(2, "other", Json::array({3}));
    LOGOS_ASSERT_EQ(f.delivered.size(), static_cast<size_t>(3));
    LOGOS_ASSERT_EQ(f.delivered[1], std::string("7:[2]:1"));
    LOGOS_ASSERT_EQ(f.delivered[2], std::string("8:[3]:1"));
    LOGOS_ASSERT_TRUE(f.lost.empty());
}

LOGOS_TEST(hub_ignores_the_replaced_client_and_listens_to_the_new_one) {
    HubFixture f;
    LOGOS_ASSERT_TRUE(f.hub->add("m1", "ev", 7));
    f.world.status(1, logos::SubStatus::Armed, 1);
    LOGOS_ASSERT_EQ(f.statuses.size(), static_cast<size_t>(1));

    auto pending = f.call("m1");   // keeps client 1 alive past its replacement
    f.reg->replace("m1", 1);
    f.world.status(1, logos::SubStatus::Lost, 1);
    LOGOS_ASSERT_TRUE(f.lost.empty());
    LOGOS_ASSERT_EQ(f.statuses.size(), static_cast<size_t>(1));

    f.world.status(2, logos::SubStatus::Armed, 1);
    LOGOS_ASSERT_EQ(f.statuses.back(), std::string("1:1"));
    f.world.status(2, logos::SubStatus::Lost, 1);
    LOGOS_ASSERT_EQ(f.lost.size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(f.lost[0], std::string("7:ev:provider_unavailable"));

    f.world.complete(1, Json(1), reached());
    LOGOS_ASSERT_EQ(pending->at(0), std::string("ok"));
    LOGOS_ASSERT_EQ(f.world.runPosted(), static_cast<size_t>(1));
    LOGOS_ASSERT_TRUE(f.world.isDestroyed(1));
}

LOGOS_TEST(hub_subscribe_racing_a_replacement_ends_on_the_new_client) {
    HubFixture f;
    f.reg->setOnReplaced([&f](const std::string& m, std::uint64_t e) {
        f.replaced.emplace_back(m, e);
        f.hub->retire(m, e);   // no rebind: the subscribe in flight has to catch up on its own
    });
    bool once = false;
    f.world.onSubscribe = [&](int client) {
        if (client == 1 && !once) {
            once = true;
            f.reg->replace("m1", 1);
        }
    };
    LOGOS_ASSERT_TRUE(f.hub->add("m1", "ev", 7));
    LOGOS_ASSERT_EQ(f.world.liveSubs(1), static_cast<size_t>(0));
    LOGOS_ASSERT_EQ(f.world.liveSubs(2), static_cast<size_t>(1));
    f.world.publish(2, "ev", Json::array({5}));
    LOGOS_ASSERT_EQ(f.delivered.size(), static_cast<size_t>(1));
}

LOGOS_TEST(hub_add_fails_cleanly_once_the_registry_is_closed) {
    HubFixture f;
    f.reg->close();
    LOGOS_ASSERT_FALSE(f.hub->add("m1", "ev", 7));
    LOGOS_ASSERT_EQ(f.hub->upstreamCount(), static_cast<size_t>(0));
}
