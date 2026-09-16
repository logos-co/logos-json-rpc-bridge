// RetryScheduler: keyed delayed jobs on one thread, stopped before the pump.

#include <logos_test.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "retry_scheduler.h"

using namespace bridge;
using namespace std::chrono_literals;

namespace {

struct Log {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> ran;

    void add(const std::string& s) {
        std::lock_guard<std::mutex> lock(mu);
        ran.push_back(s);
        cv.notify_all();
    }
    bool waitFor(size_t n) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, 5s, [&] { return ran.size() >= n; });
    }
};

} // namespace

LOGOS_TEST(scheduler_runs_due_jobs_in_deadline_order) {
    Log log;
    RetryScheduler s;
    s.start();
    LOGOS_ASSERT_TRUE(s.schedule("late", 60ms, [&] { log.add("late"); }));
    LOGOS_ASSERT_TRUE(s.schedule("now", 0ms, [&] { log.add("now"); }));
    LOGOS_ASSERT_TRUE(log.waitFor(2));
    LOGOS_ASSERT_EQ(log.ran[0], std::string("now"));
    LOGOS_ASSERT_EQ(log.ran[1], std::string("late"));
    s.stop();
}

// One timer per key: re-arming replaces the earlier job and its deadline.
LOGOS_TEST(scheduler_replaces_a_pending_job_with_the_same_key) {
    Log log;
    RetryScheduler s;
    s.start();
    LOGOS_ASSERT_TRUE(s.schedule("m1", 10min, [&] { log.add("old"); }));
    LOGOS_ASSERT_TRUE(s.schedule("m1", 0ms, [&] { log.add("new"); }));
    LOGOS_ASSERT_TRUE(log.waitFor(1));
    LOGOS_ASSERT_EQ(log.ran[0], std::string("new"));
    LOGOS_ASSERT_EQ(s.pending(), static_cast<size_t>(0));
    s.stop();
}

LOGOS_TEST(scheduler_stop_drops_pending_jobs_and_refuses_new_ones) {
    std::atomic<int> runs{0};
    RetryScheduler s;
    LOGOS_ASSERT_FALSE(s.schedule("early", 0ms, [&] { ++runs; }));   // not started
    s.start();
    LOGOS_ASSERT_TRUE(s.schedule("m1", 10min, [&] { ++runs; }));
    LOGOS_ASSERT_EQ(s.pending(), static_cast<size_t>(1));
    s.stop();
    LOGOS_ASSERT_EQ(s.pending(), static_cast<size_t>(0));
    LOGOS_ASSERT_FALSE(s.schedule("m2", 0ms, [&] { ++runs; }));
    s.stop();   // idempotent
    LOGOS_ASSERT_EQ(runs.load(), 0);
}
