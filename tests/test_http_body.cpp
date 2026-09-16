// HttpBodyWriter: the slices ws_server.cpp writes an HTTP response in.

#include <logos_test.h>

#include <string>

#include "http_body.h"

using namespace bridge;

namespace {

// Every slice in order, joined; `count` and `lastFlags` say how they came.
std::string drain(HttpBodyWriter& w, size_t* count, size_t* lastFlags) {
    std::string out;
    *count = 0;
    *lastFlags = 0;
    bool last = false;
    while (!last) {
        const auto s = w.next(&last);
        out.append(s.first, s.second);
        ++*count;
        if (last) ++*lastFlags;
        if (*count > 1000) throw LogosTestFailure("the writer never finished");
    }
    return out;
}

} // namespace

LOGOS_TEST(slices_cover_the_body_once_in_order) {
    std::string body;
    for (int i = 0; i < 100000; ++i) body.push_back(static_cast<char>('a' + i % 26));
    HttpBodyWriter w(body);
    size_t count = 0, lastFlags = 0;
    LOGOS_ASSERT_EQ(drain(w, &count, &lastFlags), body);
    LOGOS_ASSERT_EQ(count, static_cast<size_t>(7));   // 6 full 16 KiB slices and the rest
    LOGOS_ASSERT_EQ(lastFlags, static_cast<size_t>(1));
    LOGOS_ASSERT_TRUE(w.done());
}

LOGOS_TEST(a_body_of_exact_slices_ends_on_its_last_full_slice) {
    HttpBodyWriter w(std::string(3 * 4, 'x'), 4);
    size_t count = 0, lastFlags = 0;
    LOGOS_ASSERT_EQ(drain(w, &count, &lastFlags).size(), static_cast<size_t>(12));
    LOGOS_ASSERT_EQ(count, static_cast<size_t>(3));
}

LOGOS_TEST(a_small_body_is_one_final_slice) {
    HttpBodyWriter w("{\"status\":\"ok\"}");
    bool last = false;
    const auto s = w.next(&last);
    LOGOS_ASSERT_TRUE(last);
    LOGOS_ASSERT_EQ(std::string(s.first, s.second), std::string("{\"status\":\"ok\"}"));
}

// A 404 with no body still ends the transaction.
LOGOS_TEST(an_empty_body_is_one_empty_final_slice) {
    HttpBodyWriter w("");
    LOGOS_ASSERT_FALSE(w.done());
    bool last = false;
    const auto s = w.next(&last);
    LOGOS_ASSERT_TRUE(last);
    LOGOS_ASSERT_EQ(s.second, static_cast<size_t>(0));
    LOGOS_ASSERT_TRUE(w.done());
}
