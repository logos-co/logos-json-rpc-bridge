#pragma once

// Everything that touches logos-protocol: the client registry, the call pump,
// and the subscription hub. The ONLY file in the module that makes upstream
// calls, and the only one whose threading rules are load-bearing.
//
// Four invariants hold this together. Break any one and the failure is a
// deadlock or a use-after-free, not a wrong answer:
//
//  1. No lock is ever held across an upstream call. LpClient::ensure()
//     constructs on the Qt main thread and BLOCKS there; a worker holding a
//     lock across that waits for the main thread while the main thread waits
//     for the lock.
//  2. An LpSubscription is never destroyed while m_subMu is held. The
//     protocol's delivery callback holds its own per-subscription guard across
//     the entire user callback and lp_unsubscribe takes that same guard, so
//     destroying under our lock inverts against it.
//  3. The event callback does nothing but build a frame and hand it to a sink.
//     It runs on the publishing side's thread; blocking there stalls the
//     producer, and the payload buffer is borrowed for that call only.
//  4. A client is never destroyed inside one of its own callbacks: a replaced
//     client is released on the pump.
//
// The client types are template parameters only so the unit tests can drive
// this without IPC; the module uses logos::LpClient.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <condition_variable>

#include <nlohmann/json.hpp>

#include <logos_lp_client.h>

#include "recycle_policy.h"
#include "rpc_dispatcher.h"   // termination reasons

namespace bridge {

// ---------------------------------------------------------------------------
// ClientRegistry — one client per target module, replaced when it wedges.
// ---------------------------------------------------------------------------
//
// Construction is cheap (LpClient stores two strings); the expensive
// lp_client_create happens lazily inside the client's own CAS-protected
// ensure(), on whichever thread calls first. So the mutex here covers the
// bookkeeping and nothing else, which is what keeps invariant 1.
//
// Each module's client has an epoch, 1 for the first. Calls go through
// invoke(), which counts them per client: a replaced client stays alive until
// its calls are answered, because destroying it would drop their callbacks.
template <class Client>
class BasicClientRegistry : public std::enable_shared_from_this<BasicClientRegistry<Client>> {
public:
    using Clock = RecyclePolicy::Clock;
    using Job = std::function<void()>;
    using Done = std::function<void(nlohmann::json, const logos::CallError&)>;
    using OnReplaced = std::function<void(const std::string& module, std::uint64_t epoch)>;

    // A replaced client whose calls never all come back is released anyway after this.
    static constexpr std::chrono::minutes kRetiredDeadline{5};

    struct Lease {
        std::shared_ptr<Client> client;   // null once closed
        std::uint64_t epoch = 0;
        explicit operator bool() const { return client != nullptr; }
    };

    explicit BasicClientRegistry(std::string origin,
                                 std::function<Clock::time_point()> now = &Clock::now)
        : m_origin(std::move(origin)), m_now(std::move(now)) {}

    // Where replacements and releases run (the pump), and who hears of a replacement there.
    // Both are set once, before the first call.
    void setPost(std::function<bool(Job)> post) { m_post = std::move(post); }
    void setOnReplaced(OnReplaced hook) { m_onReplaced = std::move(hook); }

    Lease get(const std::string& module) {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_closed) return {};
        const std::shared_ptr<Slot>& slot = currentLocked(module);
        return Lease{slot->client, slot->epoch};
    }

    // 0 until the module's first client exists.
    std::uint64_t epoch(const std::string& module) const {
        std::lock_guard<std::mutex> lock(m_mu);
        auto it = m_modules.find(module);
        return it == m_modules.end() || !it->second.current ? 0 : it->second.current->epoch;
    }

    // An async call through the module's current client; `done` runs exactly once.
    // Call from the pump.
    void invoke(const std::string& module, const std::string& method, const nlohmann::json& args,
                Done done, int timeoutMs) {
        std::shared_ptr<Slot> slot;
        std::shared_ptr<Client> client;
        std::vector<std::shared_ptr<Client>> expired;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (!m_closed) {
                slot = currentLocked(module);
                client = slot->client;
                ++slot->inFlight;
                sweepLocked(&expired);
            }
        }
        expired.clear();
        if (!client) {
            done(nlohmann::json(), logos::callErrorObjectUnavailable(module, "the bridge is stopping"));
            return;
        }
        std::weak_ptr<BasicClientRegistry> self = this->weak_from_this();
        client->invokeAsyncResult(method, args,
            [self, slot, done = std::move(done)](nlohmann::json value, const logos::CallError& e) {
                if (auto registry = self.lock()) registry->finished(slot, e);
                done(std::move(value), e);
            },
            timeoutMs);
    }

    // Replace `module`'s client if `epoch` is still the current one. Call from the pump.
    bool replace(const std::string& module, std::uint64_t epoch) {
        std::shared_ptr<Client> dying;
        std::vector<std::shared_ptr<Client>> expired;
        std::uint64_t next = 0;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            auto it = m_modules.find(module);
            if (m_closed || it == m_modules.end() || !it->second.current ||
                it->second.current->epoch != epoch)
                return false;
            Module& m = it->second;
            std::shared_ptr<Slot> old = std::move(m.current);
            m.current = makeSlotLocked(module, m);
            next = m.current->epoch;
            ++m_replacements;
            old->retired = true;
            old->retiredAt = m_now();
            if (old->inFlight == 0) dying = std::move(old->client);
            else m_retired.push_back(std::move(old));
            sweepLocked(&expired);
        }
        dying.reset();
        expired.clear();
        if (m_onReplaced) m_onReplaced(module, next);
        return true;
    }

    std::uint64_t replacements() const {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_replacements;
    }

    // Replaced clients still waiting for their calls.
    std::size_t retiredCount() const {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_retired.size();
    }

    // Refuse new clients and destroy every client, replaced ones included, so
    // no callback can follow. A job still holding a lease releases its client when it ends.
    void close() {
        std::vector<std::shared_ptr<Client>> dead;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_closed = true;
            for (auto& kv : m_modules)
                if (kv.second.current && kv.second.current->client)
                    dead.push_back(std::move(kv.second.current->client));
            for (auto& slot : m_retired)
                if (slot->client) dead.push_back(std::move(slot->client));
            m_retired.clear();
            for (auto& client : m_unreleased) dead.push_back(std::move(client));
            m_unreleased.clear();
        }
        dead.clear();   // destructors run with no lock held
    }

private:
    struct Slot {
        std::string module;
        std::shared_ptr<Client> client;
        std::uint64_t epoch = 0;
        int inFlight = 0;
        bool retired = false;
        Clock::time_point retiredAt{};
    };
    struct Module {
        explicit Module(Clock::time_point created) : policy(created) {}
        std::shared_ptr<Slot> current;
        std::uint64_t lastEpoch = 0;
        RecyclePolicy policy;
    };

    const std::shared_ptr<Slot>& currentLocked(const std::string& module) {
        auto it = m_modules.find(module);
        if (it == m_modules.end()) it = m_modules.emplace(module, Module(m_now())).first;
        Module& m = it->second;
        if (!m.current) m.current = makeSlotLocked(module, m);
        return m.current;
    }

    std::shared_ptr<Slot> makeSlotLocked(const std::string& module, Module& m) {
        auto slot = std::make_shared<Slot>();
        slot->module = module;
        slot->client = std::make_shared<Client>(module, m_origin);
        slot->epoch = ++m.lastEpoch;
        return slot;
    }

    void sweepLocked(std::vector<std::shared_ptr<Client>>* out) {
        const Clock::time_point now = m_now();
        for (auto it = m_retired.begin(); it != m_retired.end();) {
            if (now - (*it)->retiredAt < kRetiredDeadline) { ++it; continue; }
            if ((*it)->client) out->push_back(std::move((*it)->client));
            it = m_retired.erase(it);
        }
    }

    bool post(Job job) { return m_post && m_post(std::move(job)); }

    // A call's answer, on the client's owner thread (invariant 4).
    void finished(const std::shared_ptr<Slot>& slot, const logos::CallError& e) {
        std::shared_ptr<Client> dying;
        std::uint64_t replaceEpoch = 0;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            --slot->inFlight;
            auto it = m_modules.find(slot->module);
            if (!m_closed && it != m_modules.end() && it->second.current == slot) {
                if (e.code == "object_unavailable") {
                    if (it->second.policy.onUnavailable(m_now())) replaceEpoch = slot->epoch;
                } else if (e.ok() || e.code == "unauthorized") {
                    it->second.policy.onReached();
                }
            }
            if (slot->retired && slot->inFlight == 0 && slot->client) {
                dying = std::move(slot->client);
                m_retired.erase(std::remove(m_retired.begin(), m_retired.end(), slot), m_retired.end());
            }
        }
        if (dying) {
            // Held through a box, so a refused post leaves the client with us rather than in the dead job.
            auto box = std::make_shared<std::shared_ptr<Client>>(std::move(dying));
            if (!post([box] { box->reset(); })) {
                std::lock_guard<std::mutex> lock(m_mu);
                m_unreleased.push_back(std::move(*box));
            }
        }
        if (replaceEpoch != 0) {
            std::weak_ptr<BasicClientRegistry> self = this->weak_from_this();
            const std::string module = slot->module;
            post([self, module, replaceEpoch] {
                if (auto registry = self.lock()) registry->replace(module, replaceEpoch);
            });
        }
    }

    std::string m_origin;
    std::function<Clock::time_point()> m_now;
    std::function<bool(Job)> m_post;
    OnReplaced m_onReplaced;
    mutable std::mutex m_mu;
    std::map<std::string, Module> m_modules;
    std::vector<std::shared_ptr<Slot>> m_retired;
    std::vector<std::shared_ptr<Client>> m_unreleased;   // released by close() or the destructor
    std::uint64_t m_replacements = 0;
    bool m_closed = false;
};

using ClientRegistry = BasicClientRegistry<logos::LpClient>;

// ---------------------------------------------------------------------------
// CallPump — a small pool that owns every upstream submission.
// ---------------------------------------------------------------------------
//
// It exists so the socket thread never blocks. A cold LpClient's first call
// marshals construction onto the Qt main thread and can sit there for seconds;
// on the pump that costs one pump thread, on the socket thread it would cost
// every connected client.
class CallPump {
public:
    using Job = std::function<void()>;

    void start(int threads) {
        m_stop.store(false);
        for (int i = 0; i < threads; ++i)
            m_threads.emplace_back([this] { loop(); });
    }

    bool submit(Job job) {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_stop.load()) return false;
            m_queue.push_back(std::move(job));
        }
        m_cv.notify_one();
        return true;
    }

    // Drain and join. Queued-but-unstarted jobs are dropped: the connections
    // they would answer are going away in the same teardown.
    void stop() {
        // Dropped jobs die unlocked: one may hold a client's last reference, and
        // lp_client_destroy waits for callbacks that may be inside submit().
        std::deque<Job> dropped;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_stop.store(true);
            dropped.swap(m_queue);
        }
        dropped.clear();
        m_cv.notify_all();
        for (auto& t : m_threads)
            if (t.joinable()) t.join();
        m_threads.clear();
    }

    bool stopping() const { return m_stop.load(); }

private:
    void loop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m_mu);
                m_cv.wait(lock, [this] { return m_stop.load() || !m_queue.empty(); });
                if (m_stop.load()) return;
                job = std::move(m_queue.front());
                m_queue.pop_front();
            }
            job();   // never under the lock
        }
    }

    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<Job> m_queue;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_stop{true};
};

// ---------------------------------------------------------------------------
// SubscriptionHub — one upstream subscription per (module, event), fanned out.
// ---------------------------------------------------------------------------

// What a delivered event is handed to. `subscriberId` is the bridge-internal
// per-subscriber handle; the transport maps it back to a connection.
struct Delivery {
    std::uint64_t subscriberId = 0;
    std::string module;
    std::string event;
    nlohmann::json data;
    std::uint64_t generation = 0;
};

template <class Client>
class BasicSubscriptionHub {
public:
    using Registry = BasicClientRegistry<Client>;
    // sink: called for each subscriber on each event. Runs on the upstream
    // delivery thread, so it must only enqueue.
    // onLost: called once per affected subscriber when a module's subscriptions
    // end (the provider went away, or was replaced), with the reason.
    using Sink   = std::function<void(const Delivery&)>;
    using OnLost = std::function<void(std::uint64_t subscriberId,
                                      const std::string& module,
                                      const std::string& event,
                                      const char* why)>;
    using OnModuleStatus = std::function<void(const std::string& module,
                                              logos::SubStatus state,
                                              std::uint64_t generation)>;

    BasicSubscriptionHub(Registry* clients, Sink sink, OnLost onLost)
        : m_clients(clients), m_sink(std::move(sink)), m_onLost(std::move(onLost)) {}

    ~BasicSubscriptionHub() { clear(); }

    // Also told every status of a watched module (discovery rediscovers on it).
    // Set once, before the first subscribe; it runs on the delivery thread.
    void setModuleStatusHook(OnModuleStatus hook) { m_statusHook = std::move(hook); }

    // Add a subscriber to (module, event), creating the upstream subscription
    // on first use. MUST be called from the pump: lp_subscribe blocks on the
    // Qt main thread.
    //
    // Returns false only if the client could not be obtained.
    bool add(const std::string& module, const std::string& event,
             std::uint64_t subscriberId) {
        const Key key{module, event};
        std::shared_ptr<Up> up;
        bool needsSubscribe = false;
        {
            std::lock_guard<std::mutex> lock(m_subMu);
            if (m_closed) return false;
            auto it = m_up.find(key);
            if (it == m_up.end()) {
                up = std::make_shared<Up>();
                up->module = module;
                up->event = event;
                up->subscribers = std::make_shared<const Subscribers>();
                m_up.emplace(key, up);
                needsSubscribe = true;
            } else {
                up = it->second;
            }
            addSubscriberLocked(up, subscriberId);
        }

        if (!needsSubscribe) return true;
        if (bind(up)) return true;
        std::lock_guard<std::mutex> lock(m_subMu);
        auto it = m_up.find(key);
        if (it != m_up.end() && it->second == up && up->epoch == 0) m_up.erase(it);
        return false;
    }

    // Drop one subscriber. The upstream subscription is deliberately KEPT even
    // at zero subscribers: tearing it down and re-creating it on the next
    // subscribe leaks a dead callback into the shared upstream event helper
    // every cycle (the protocol's cancel does not detach the callback, only
    // gates it), which client churn would turn into an unbounded leak and a
    // growing per-event cost on the process's only event loop.
    void remove(const std::string& module, const std::string& event,
                std::uint64_t subscriberId) {
        std::lock_guard<std::mutex> lock(m_subMu);
        auto it = m_up.find(Key{module, event});
        if (it == m_up.end()) return;
        removeSubscriberLocked(it->second, subscriberId);
    }

    void removeSubscriberEverywhere(std::uint64_t subscriberId) {
        std::lock_guard<std::mutex> lock(m_subMu);
        for (auto& kv : m_up) removeSubscriberLocked(kv.second, subscriberId);
    }

    // `module`'s client was replaced by `epoch`: its predecessor's status reports no longer count.
    void retire(const std::string& module, std::uint64_t epoch) {
        std::lock_guard<std::mutex> lock(m_subMu);
        Watch& w = m_watch[module];
        w.accept = std::max(w.accept, epoch);
    }

    // Move `module`'s upstream subscriptions onto its current client, keeping their subscribers.
    // Call from the pump, after retire().
    void rebind(const std::string& module) {
        std::vector<std::shared_ptr<Up>> ups;
        {
            std::lock_guard<std::mutex> lock(m_subMu);
            if (m_closed) return;
            for (const auto& kv : m_up)
                if (kv.second->module == module) ups.push_back(kv.second);
        }
        for (const auto& up : ups) bind(up);
    }

    // The provider for `module` went away or was replaced, which ends every
    // subscription to it at once. Terminate each affected event's subscribers.
    void notifyModuleLost(const std::string& module, const char* why) {
        std::vector<std::string> events;
        {
            std::lock_guard<std::mutex> lock(m_subMu);
            for (const auto& kv : m_up)
                if (kv.second->module == module) events.push_back(kv.second->event);
        }
        for (const std::string& ev : events) notifyLost(module, ev, why);
    }

    // Signals every subscriber of (module, event); the transport turns each into
    // rpc.subscription_terminated. The upstream stays armed for re-subscribes.
    void notifyLost(const std::string& module, const std::string& event, const char* why) {
        std::shared_ptr<const Subscribers> subs;
        std::string mod, ev;
        {
            std::lock_guard<std::mutex> lock(m_subMu);
            auto it = m_up.find(Key{module, event});
            if (it == m_up.end()) return;
            subs = std::atomic_load(&it->second->subscribers);
            it->second->generation.fetch_add(1, std::memory_order_acq_rel);
            mod = it->second->module;
            ev = it->second->event;
            it->second->subscribers = std::make_shared<const Subscribers>();
            std::atomic_store(&it->second->subscribers, it->second->subscribers);
        }
        if (!subs) return;
        for (std::uint64_t id : subs->ids) m_onLost(id, mod, ev, why);   // no lock held
    }

    std::size_t upstreamCount() const {
        std::lock_guard<std::mutex> lock(m_subMu);
        return m_up.size();
    }

    // Tear down every upstream subscription. Handles are moved OUT under the
    // lock and destroyed after it is released — invariant 2.
    void clear() {
        std::vector<std::shared_ptr<Up>> dead;
        {
            std::lock_guard<std::mutex> lock(m_subMu);
            m_closed = true;
            for (auto& kv : m_up) dead.push_back(std::move(kv.second));
            m_up.clear();
        }
        dead.clear();
    }

private:
    using Lease = typename Registry::Lease;
    using Handle = decltype(std::declval<Client&>().subscribe(
        std::declval<const std::string&>(), std::declval<std::function<void(nlohmann::json)>>()));

    // A replacement racing a subscribe makes bind() go round again; replacements are seconds apart.
    static constexpr int kBindRounds = 4;

    struct Key {
        std::string module, event;
        bool operator<(const Key& o) const {
            return module != o.module ? module < o.module : event < o.event;
        }
    };
    struct Subscribers { std::vector<std::uint64_t> ids; };
    struct Up {
        std::string module, event;
        Handle handle;
        std::uint64_t epoch = 0;   // the client `handle` came from; 0 until bound
        std::shared_ptr<const Subscribers> subscribers;
        std::atomic<std::uint64_t> generation{1};
    };
    // Per module: the client epoch whose watcher is installed, and the one whose reports count.
    struct Watch {
        std::uint64_t installed = 0;
        std::uint64_t accept = 0;
    };

    bool liveLocked(const std::shared_ptr<Up>& up) const {
        auto it = m_up.find(Key{up->module, up->event});
        return it != m_up.end() && it->second == up;
    }

    // Subscribe `up` on its module's current client. True once it is bound to one.
    bool bind(const std::shared_ptr<Up>& up) {
        for (int round = 0; round < kBindRounds; ++round) {
            const Lease lease = m_clients->get(up->module);
            if (!lease) return false;
            {
                std::lock_guard<std::mutex> lock(m_subMu);
                if (m_closed || !liveLocked(up)) return false;
                if (up->epoch >= lease.epoch) return true;
            }
            ensureLossWatcher(up->module, lease);
            Handle handle = lease.client->subscribe(up->event, deliveryFor(up));
            if (!handle.valid()) {
                std::lock_guard<std::mutex> lock(m_subMu);
                return up->epoch != 0;
            }
            Handle old;   // outlives the lock below (invariant 2)
            {
                std::lock_guard<std::mutex> lock(m_subMu);
                if (m_closed || !liveLocked(up)) {
                    old = std::move(handle);
                    return false;
                }
                if (up->epoch >= lease.epoch) {
                    old = std::move(handle);
                } else {
                    old = std::move(up->handle);
                    up->handle = std::move(handle);
                    up->epoch = lease.epoch;
                }
            }
            if (m_clients->epoch(up->module) == lease.epoch) return true;
        }
        return true;
    }

    // Captured by value: the callback may outlive any particular subscriber,
    // and must never reach back into the hub's map.
    std::function<void(nlohmann::json)> deliveryFor(const std::shared_ptr<Up>& up) {
        std::weak_ptr<Up> weak = up;
        auto sink = m_sink;
        return [weak, sink](nlohmann::json payload) {
            auto up = weak.lock();
            if (!up) return;
            // Lock-free read of the published snapshot — invariant 2's other
            // half: delivery never takes m_subMu, so it can never be the thread
            // an unsubscribe is waiting behind.
            auto subs = std::atomic_load(&up->subscribers);
            if (!subs || subs->ids.empty()) return;
            const std::uint64_t gen = up->generation.load(std::memory_order_acquire);
            for (std::uint64_t id : subs->ids)
                sink(Delivery{id, up->module, up->event, payload, gen});
        };
    }

    // Install this client's loss watcher, once per client.
    //
    // The loss watcher is per MODULE, so it is installed once per client
    // rather than once per subscription — the runtime reports a provider
    // dying at that granularity, and every event of that module is affected
    // together. Installing it per (module, event) would deliver N copies of
    // one loss and terminate the same client subscriptions N times.
    //
    // Below protocol 0.9 there is no loss signal at all and a provider restart
    // resumes the stream silently. Documented in the README as the reason for
    // the minimum version.
    //
    // Capturing `this` is safe because the registry is closed, which destroys
    // every client and so every watcher, before the hub goes.
    void ensureLossWatcher(const std::string& module, const Lease& lease) {
        {
            std::lock_guard<std::mutex> lock(m_subMu);
            Watch& w = m_watch[module];
            if (w.installed >= lease.epoch) return;
            w.installed = lease.epoch;
            w.accept = std::max(w.accept, lease.epoch);
        }
        const std::uint64_t epoch = lease.epoch;
        // The ONE status callback this client can hold: a second installer
        // would replace it, so every other listener goes through m_statusHook.
        lease.client->onSubscriptionStatus(
            [this, module, epoch](logos::SubStatus state, std::uint64_t generation) {
                {
                    std::lock_guard<std::mutex> lock(m_subMu);
                    auto it = m_watch.find(module);
                    if (m_closed || it == m_watch.end() || it->second.accept != epoch) return;
                }
                // Held is impossible here — the bridge never sets a manual
                // policy — but treating it as a loss anyway is the safe
                // reading: it means the same thing to a downstream client, and
                // a silent fall-through would resume a stream with a hole.
                if (state == logos::SubStatus::Lost || state == logos::SubStatus::Held ||
                    state == logos::SubStatus::Abandoned)
                    notifyModuleLost(module, reason::kProviderUnavailable);
                if (m_statusHook) m_statusHook(module, state, generation);
            });
    }

    void addSubscriberLocked(const std::shared_ptr<Up>& up, std::uint64_t id) {
        auto next = std::make_shared<Subscribers>(*std::atomic_load(&up->subscribers));
        for (std::uint64_t existing : next->ids)
            if (existing == id) return;
        next->ids.push_back(id);
        std::atomic_store(&up->subscribers,
                          std::shared_ptr<const Subscribers>(std::move(next)));
    }

    void removeSubscriberLocked(const std::shared_ptr<Up>& up, std::uint64_t id) {
        auto cur = std::atomic_load(&up->subscribers);
        if (!cur) return;
        auto next = std::make_shared<Subscribers>();
        for (std::uint64_t existing : cur->ids)
            if (existing != id) next->ids.push_back(existing);
        std::atomic_store(&up->subscribers,
                          std::shared_ptr<const Subscribers>(std::move(next)));
    }

    Registry* m_clients;
    std::map<std::string, Watch> m_watch;   // guarded by m_subMu
    Sink m_sink;
    OnLost m_onLost;
    OnModuleStatus m_statusHook;   // written once before any watcher exists
    mutable std::mutex m_subMu;
    std::map<Key, std::shared_ptr<Up>> m_up;
    bool m_closed = false;
};

using SubscriptionHub = BasicSubscriptionHub<logos::LpClient>;

} // namespace bridge
