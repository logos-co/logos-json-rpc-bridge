#pragma once

// One timer thread for keyed, delayed jobs; CallPump has no timers. A job runs
// on this thread with no lock held and should only hand work on to the pump.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace bridge {

class RetryScheduler {
public:
    using Clock = std::chrono::steady_clock;
    using Job = std::function<void()>;

    RetryScheduler() = default;
    RetryScheduler(const RetryScheduler&) = delete;
    RetryScheduler& operator=(const RetryScheduler&) = delete;
    ~RetryScheduler() { stop(); }

    void start() {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_thread.joinable()) return;
        m_stop = false;
        m_thread = std::thread([this] { loop(); });
    }

    // Drops every pending job and joins. Idempotent.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_stop = true;
            m_timers.clear();
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    // Run `job` after `delay`, replacing any timer already set for `key`.
    // False before start() and after stop().
    bool schedule(const std::string& key, std::chrono::milliseconds delay, Job job) {
        {
            std::lock_guard<std::mutex> lock(m_mu);
            if (m_stop) return false;
            m_timers[key] = Timer{Clock::now() + delay, std::move(job)};
        }
        m_cv.notify_all();
        return true;
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(m_mu);
        return m_timers.size();
    }

private:
    struct Timer {
        Clock::time_point due;
        Job job;
    };

    void loop() {
        std::unique_lock<std::mutex> lock(m_mu);
        while (!m_stop) {
            if (m_timers.empty()) {
                m_cv.wait(lock);
                continue;
            }
            auto next = std::min_element(m_timers.begin(), m_timers.end(),
                [](const auto& a, const auto& b) { return a.second.due < b.second.due; });
            const Clock::time_point due = next->second.due;   // the map may change while we wait
            if (due > Clock::now()) {
                m_cv.wait_until(lock, due);
                continue;
            }
            Job job = std::move(next->second.job);
            m_timers.erase(next);
            lock.unlock();
            if (job) job();
            lock.lock();
        }
    }

    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    std::map<std::string, Timer> m_timers;
    std::thread m_thread;
    bool m_stop = true;
};

} // namespace bridge
