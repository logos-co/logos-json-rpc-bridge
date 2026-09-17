// HttpBodyWriter: the slices ws_server.cpp writes an HTTP response in; and
// RequestFraming: when a request's body follows its headers.

#include <logos_test.h>

#include <string>

#include "http_body.h"

using namespace bridge;

namespace {

RequestFraming withLength(const std::string& length) {
    RequestFraming f;
    f.hasLength = true;
    f.length = length;
    return f;
}

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

LOGOS_TEST(without_a_length_only_a_post_put_or_patch_has_a_body) {
    RequestFraming get;
    LOGOS_ASSERT_FALSE(bodyFollows(get));
    RequestFraming post;
    post.bodyMethod = true;
    LOGOS_ASSERT_TRUE(bodyFollows(post));   // lws reads it as 100 MiB
    LOGOS_ASSERT_FALSE(usableLength(post));
}

LOGOS_TEST(a_zero_length_is_no_body_and_a_usable_one) {
    for (const char* zero : {"0", "00"}) {
        RequestFraming f = withLength(zero);
        f.bodyMethod = true;
        LOGOS_ASSERT_FALSE(bodyFollows(f));
        LOGOS_ASSERT_TRUE(usableLength(f));
    }
}

// Anything but zeros may be followed by bytes lws reads as a body.
LOGOS_TEST(any_other_length_declares_a_body) {
    for (const char* length : {"5", "1048577", "5, 5", "0, 0", "abc", "-1", ""}) {
        LOGOS_ASSERT_TRUE(bodyFollows(withLength(length)));
    }
    LOGOS_ASSERT_TRUE(usableLength(withLength("5")));
    LOGOS_ASSERT_TRUE(usableLength(withLength("1048577")));   // the size cap is checked as it arrives
    for (const char* length : {"5, 5", "0, 0", "abc", "-1", "+5", " 5", ""}) {
        LOGOS_ASSERT_FALSE(usableLength(withLength(length)));
    }
}

// Chunked or not, and whatever Content-Length says beside it.
LOGOS_TEST(a_transfer_encoding_always_declares_a_body_the_bridge_does_not_read) {
    RequestFraming get;
    get.transferEncoding = true;
    LOGOS_ASSERT_TRUE(bodyFollows(get));
    RequestFraming both = withLength("0");
    both.transferEncoding = true;
    LOGOS_ASSERT_TRUE(bodyFollows(both));
    LOGOS_ASSERT_FALSE(usableLength(both));
}
