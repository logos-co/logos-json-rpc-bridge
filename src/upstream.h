#pragma once

// Everything that touches logos-protocol: the client registry, the call pump,
// and the subscription hub. The ONLY file in the module that makes upstream
// calls, and the only one whose threading rules are load-bearing.
//
// Three invariants hold this together. Break any one and the failure is a
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

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <condition_variable>

#include <nlohmann/json.hpp>

#include <logos_lp_client.h>

namespace bridge {

// ---------------------------------------------------------------------------
// ClientRegistry — one LpClient per target module.
// ---------------------------------------------------------------------------
//
// Construction is cheap (LpClient stores two strings); the expensive
// lp_client_create happens lazily inside the client's own CAS-protected
// ensure(), on whichever thread calls first. So the mutex here covers the map
// and nothing else, which is what keeps invariant 1.
class ClientRegistry {
public:
    explicit ClientRegistry(std::string origin) : m_origin(std::move(origin)) {}

    std::shared_ptr<logos::LpClient> get(const std::string& target) {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_closed) return nullptr;
        auto it = m_clients.find(target);
        if (it != m_clients.end()) return it->second;
        auto c = std::make_shared<logos::LpClient>(target, m_origin);
        m_clients.emplace(target, c);
        return c;
    }

    // Refuse new clients and drop our references. Held clients stay alive until
    // their last shared_ptr goes, so an in-flight call never loses its client.
    void close() {
        std::map<std::string, std::shared_ptr<logos::LpClient>> dead;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_closed = true;
            dead.swap(m_clients);
        }
        dead.clear();   // destructors run with no lock held
    }

private:
    std::string m_origin;
    std::mutex m_mu;
    std::map<std::string, std::shared_ptr<logos::LpClient>> m_clients;
    bool m_closed = false;
};

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
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_stop.store(true);
            m_queue.clear();
        }
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

class SubscriptionHub {
public:
    // sink: called for each subscriber on each event. Runs on the upstream
    // delivery thread, so it must only enqueue.
    // onLost: called when an upstream subscription's provider went away, once
    // per affected subscriber.
    using Sink   = std::function<void(const Delivery&)>;
    using OnLost = std::function<void(std::uint64_t subscriberId,
                                      const std::string& module,
                                      const std::string& event)>;

    SubscriptionHub(ClientRegistry* clients, Sink sink, OnLost onLost)
        : m_clients(clients), m_sink(std::move(sink)), m_onLost(std::move(onLost)) {}

    ~SubscriptionHub() { clear(); }

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

        auto client = m_clients->get(module);
        if (!client) {
            std::lock_guard<std::mutex> lock(m_subMu);
            m_up.erase(key);
            return false;
        }

        // Captured by value: the callback may outlive any particular
        // subscriber, and must never reach back into the hub's map.
        std::weak_ptr<Up> weak = up;
        auto sink = m_sink;
        auto onEvent = [weak, sink](nlohmann::json payload) {
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

        // The loss watcher is per MODULE, so it is installed once per client
        // rather than once per subscription — the runtime reports a provider
        // dying at that granularity, and every event of that module is
        // affected together. Installing it per (module, event) would deliver N
        // copies of one loss and terminate the same client subscriptions N
        // times.
        //
        // Below protocol 0.9 there is no loss signal at all and a provider
        // restart resumes the stream silently. Documented in the README as the
        // reason for the minimum version.
        ensureLossWatcher(module, *client);

        auto handle = client->subscribe(event, std::move(onEvent));

        if (!handle.valid()) {
            std::lock_guard<std::mutex> lock(m_subMu);
            m_up.erase(key);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_subMu);
            auto it = m_up.find(key);
            if (it == m_up.end()) return false;   // torn down while we subscribed
            it->second->handle = std::move(handle);
        }
        return true;
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

    // Install this module's loss watcher exactly once.
    //
    // Capturing `this` is safe because clear() destroys every handle and the
    // hub outlives the registry it was built from; ~SubscriptionHub calls
    // clear() for exactly this reason.
    void ensureLossWatcher(const std::string& module, logos::LpClient& client) {
        {
            std::lock_guard<std::mutex> lock(m_subMu);
            if (!m_watched.insert(module).second) return;
        }
        client.onSubscriptionStatus(
            [this, module](logos::SubStatus state, std::uint64_t /*generation*/) {
                // Held is impossible here — the bridge never sets a manual
                // policy — but treating it as a loss anyway is the safe
                // reading: it means the same thing to a downstream client, and
                // a silent fall-through would resume a stream with a hole.
                if (state == logos::SubStatus::Lost || state == logos::SubStatus::Held ||
                    state == logos::SubStatus::Abandoned)
                    notifyModuleLost(module);
            });
    }

    // The provider for `module` went away, which takes every subscription to it
    // down at once. Terminate each affected event's subscribers.
    void notifyModuleLost(const std::string& module) {
        std::vector<std::string> events;
        {
            std::lock_guard<std::mutex> lock(m_subMu);
            for (const auto& kv : m_up)
                if (kv.second->module == module) events.push_back(kv.second->event);
        }
        for (const std::string& ev : events) notifyLost(module, ev);
    }

    // Called when the upstream reports the provider went away. Signals every
    // affected subscriber; the transport turns each into a
    // rpc.subscription_terminated and forgets the subscription.
    void notifyLost(const std::string& module, const std::string& event) {
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
        for (std::uint64_t id : subs->ids) m_onLost(id, mod, ev);   // no lock held
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
    struct Key {
        std::string module, event;
        bool operator<(const Key& o) const {
            return module != o.module ? module < o.module : event < o.event;
        }
    };
    struct Subscribers { std::vector<std::uint64_t> ids; };
    struct Up {
        std::string module, event;
        logos::LpSubscription handle;
        std::shared_ptr<const Subscribers> subscribers;
        std::atomic<std::uint64_t> generation{1};
    };

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

    ClientRegistry* m_clients;
    // Modules whose loss watcher is already installed. Guarded by m_subMu.
    std::set<std::string> m_watched;
    Sink m_sink;
    OnLost m_onLost;
    mutable std::mutex m_subMu;
    std::map<Key, std::shared_ptr<Up>> m_up;
    bool m_closed = false;
};

} // namespace bridge
