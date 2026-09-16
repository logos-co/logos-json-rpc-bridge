// SHA-256 and the interface digest.
//
// tests/fixtures/interface-digest-vectors.json is the digest contract with the
// Python SDK: [{name, value, canonical, sha256}], where canonical and sha256
// come from Python's json.dumps(value, sort_keys=True, separators=(",", ":"),
// ensure_ascii=False). The bridge must produce the same bytes.

#include <logos_test.h>

#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "discovery_model.h"
#include "lidl_contract.h"
#include "sha256.h"

using namespace bridge;

namespace {

std::string fixture(const std::string& name) {
    const std::string path = std::string(BRIDGE_TEST_FIXTURES) + "/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw LogosTestFailure("missing fixture " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

nlohmann::json vectors() {
    return nlohmann::json::parse(fixture("interface-digest-vectors.json"));
}

} // namespace

// ── SHA-256 ─────────────────────────────────────────────────────────────────

LOGOS_TEST(sha256_matches_the_nist_short_messages) {
    LOGOS_ASSERT_EQ(sha256Hex(""),
        std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    LOGOS_ASSERT_EQ(sha256Hex("abc"),
        std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    LOGOS_ASSERT_EQ(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
        std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    LOGOS_ASSERT_EQ(sha256Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                              "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
        std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
}

LOGOS_TEST(sha256_matches_the_nist_million_a) {
    LOGOS_ASSERT_EQ(sha256Hex(std::string(1000000, 'a')),
        std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

// Lengths around the 56-byte padding boundary and the block edge.
LOGOS_TEST(sha256_pads_correctly_at_block_boundaries) {
    const struct { size_t n; const char* hex; } cases[] = {
        {55, "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072"},
        {56, "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e"},
        {63, "75220b47218278e656f2013bb8f0c455a25eaf01e86c64924e9d48d89776d6f2"},
        {64, "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c"},
        {65, "9537c5fdf120482f7d58d25e9ed583f52c02b4e304ea814db1633ad565aed7e9"},
        {119, "000b48d4edf0fa7bee3c6236ecd2785baa5db4eeb8bb54341b029e0d9fa5fb0c"},
        {120, "13f05a0b594787f5ecd315edc96141bd3243203d1b7d4f0836f37308b276ba98"},
    };
    for (const auto& c : cases) LOGOS_ASSERT_EQ(sha256Hex(std::string(c.n, 'x')), std::string(c.hex));
}

LOGOS_TEST(sha256_is_the_same_fed_in_pieces) {
    const std::string msg(1000, 'q');
    for (size_t split : {size_t(0), size_t(1), size_t(55), size_t(64), size_t(127), size_t(999)}) {
        Sha256 h;
        h.update(msg.substr(0, split));
        h.update(msg.substr(split));
        std::string hex;
        for (unsigned b : h.finish()) {
            static const char* d = "0123456789abcdef";
            hex += d[b >> 4];
            hex += d[b & 15];
        }
        LOGOS_ASSERT_EQ(hex, sha256Hex(msg));
    }
}

// Digests are over bytes: UTF-8 in, UTF-8 hashed, nothing transcoded.
LOGOS_TEST(sha256_hashes_utf8_bytes) {
    LOGOS_ASSERT_EQ(sha256Hex("\xe2\x80\x93"),
        std::string("d4f85d36757c12f0c6dab57721287f327efbfc46ea8c0f820301067f8627fc0f"));
    LOGOS_ASSERT_EQ(sha256Hex(fixture("contracts/test_fullapi_cpp.lidl")),
        std::string("6e92054db23a3b3bfb78179647a0d67315c48a2efd6e0a515b5f0b9bf6b7687c"));
    LOGOS_ASSERT_EQ(sha256Hex(fixture("contracts/storage_module.lidl")),
        std::string("9f6bd141a1401b14ec151b579fd1e1076ba7916929f54101fd6843501dee92ac"));
}

// ── canonical JSON and interface_sha256 ─────────────────────────────────────

LOGOS_TEST(every_digest_vector_matches_python) {
    const nlohmann::json all = vectors();
    LOGOS_ASSERT_GE(all.size(), static_cast<size_t>(10));
    for (const auto& v : all) {
        const std::string name = v.at("name").get<std::string>();
        const std::string canonical = v.at("canonical").get<std::string>();
        const std::string sha = v.at("sha256").get<std::string>();
        if (canonicalJson(v.at("value")) != canonical)
            throw LogosTestFailure("canonical JSON differs from Python for vector " + name);
        LOGOS_ASSERT_EQ(sha256Hex(canonical), sha);
        LOGOS_ASSERT_EQ(interfaceDigest(v.at("value")), sha);
    }
}

// The escapes that differ between JSON writers, pinned by name.
LOGOS_TEST(the_vectors_pin_the_escapes_that_writers_disagree_on) {
    const nlohmann::json all = vectors();
    auto canonicalOf = [&](const std::string& name) {
        for (const auto& v : all)
            if (v["name"] == name) return v["canonical"].get<std::string>();
        throw LogosTestFailure("no vector " + name);
    };
    LOGOS_ASSERT_CONTAINS(canonicalOf("u001f_lowercase_hex"), "\\u001f");
    LOGOS_ASSERT_CONTAINS(canonicalOf("en_dash"), "\xe2\x80\x93");
    LOGOS_ASSERT_CONTAINS(canonicalOf("raw_u2028_u2029"), "\xe2\x80\xa8");
    LOGOS_ASSERT_CONTAINS(canonicalOf("del_and_slash_unescaped"), "\x7f/usr/bin</script>");
    LOGOS_ASSERT_CONTAINS(canonicalOf("short_escapes_and_nul"), "\\b\\f\\r\\t\\u0000\\u0001");
    LOGOS_ASSERT_CONTAINS(canonicalOf("newline_quote_backslash"), "\\n");
    LOGOS_ASSERT_CONTAINS(canonicalOf("large_integers"), "18446744073709551615");
    LOGOS_ASSERT_CONTAINS(canonicalOf("large_integers"), "-9223372036854775808");
    // Code-point order puts U+FF61 before U+1F600; UTF-16 order would not.
    LOGOS_ASSERT_EQ(canonicalOf("code_point_not_utf16_order").rfind("{\"\xef\xbd\xa1\"", 0),
                    static_cast<size_t>(0));
}

// The served interface of a real contract hashes to the Python vector.
LOGOS_TEST(the_served_storage_interface_matches_its_vector) {
    const ContractResult c = readContract(fixture("contracts/storage_module.lidl"), "storage_module");
    LOGOS_ASSERT_TRUE(c.ok);
    const auto typed = typedContract(c);
    LOGOS_ASSERT_TRUE(typed != nullptr);
    std::string expected;
    for (const auto& v : vectors())
        if (v["name"] == "storage_module_identity_ast") expected = v["sha256"].get<std::string>();
    LOGOS_ASSERT_EQ(typed->interfaceSha256, expected);
    LOGOS_ASSERT_EQ(typed->interfaceSha256, interfaceDigest(typed->interface));
}
