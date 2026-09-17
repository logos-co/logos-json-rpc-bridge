// Outcome -> JSON-RPC error mapping.
//
// The load-bearing test here is totality: every CallError code logos-protocol
// actually produces must map to something specific. An unmapped code silently
// becoming "internal error" is how a timeout ends up indistinguishable from a
// crash on the client side, and it is a regression that no other test notices.

#include <logos_test.h>

#include <set>
#include <string>
#include <vector>

#include "error_map.h"
#include "rpc_dispatcher.h"

using namespace bridge;

namespace {

// The vocabulary logos-protocol produces, from logos_call_error.h's documented
// set plus the two lp_invoke argument-parsing codes. If logos-protocol adds a
// code, this list is what should fail first.
const std::vector<std::string> kShippedCodes = {
    "object_unavailable",
    "timeout",
    "transport_error",
    "call_failed",
    "unauthorized",
    "invalid_args",
    "invalid_arg",
};

} // namespace

LOGOS_TEST(every_shipped_call_error_code_maps_to_something_specific) {
    for (const std::string& code : kShippedCodes) {
        const MappedError e = mapCallError(code, "detail");
        LOGOS_ASSERT_NE(e.jsonRpcCode, static_cast<int>(kInternalError));
        LOGOS_ASSERT_FALSE(e.message.empty());
    }
}

LOGOS_TEST(each_shipped_code_maps_to_a_distinct_enough_answer) {
    LOGOS_ASSERT_EQ(mapCallError("timeout", "").jsonRpcCode, static_cast<int>(kTimeout));
    LOGOS_ASSERT_EQ(mapCallError("object_unavailable", "").jsonRpcCode, static_cast<int>(kUnavailable));
    LOGOS_ASSERT_EQ(mapCallError("transport_error", "").jsonRpcCode, static_cast<int>(kTransportError));
    LOGOS_ASSERT_EQ(mapCallError("unauthorized", "").jsonRpcCode, static_cast<int>(kUnauthorized));
    LOGOS_ASSERT_EQ(mapCallError("call_failed", "").jsonRpcCode, static_cast<int>(kModuleError));
    LOGOS_ASSERT_EQ(mapCallError("invalid_args", "").jsonRpcCode, static_cast<int>(kInvalidParams));
}

// The set is strings rather than an enum precisely so it can grow, so a newer
// protocol WILL hand us something unlisted. Answering coarsely is correct;
// refusing to answer is not.
LOGOS_TEST(an_unknown_code_falls_back_rather_than_failing) {
    const MappedError e = mapCallError("something_new_in_0_11", "");
    LOGOS_ASSERT_EQ(e.jsonRpcCode, static_cast<int>(kInternalError));
    LOGOS_ASSERT_FALSE(e.message.empty());
}

// Every error carries the Logos taxonomy alongside the JSON-RPC code, so a
// Logos-aware client gets the real answer and a stock one still behaves.
LOGOS_TEST(errors_carry_the_logos_taxonomy_in_data) {
    const auto j = mapCallError("timeout", "").toJson();
    LOGOS_ASSERT_TRUE(j.contains("data"));
    LOGOS_ASSERT_EQ(j["data"]["logos_error_code"].get<int>(),
                    static_cast<int>(LogosErrorCode::Timeout));
    LOGOS_ASSERT_EQ(j["data"]["logos_error_name"].get<std::string>(), std::string("TIMEOUT"));
}

LOGOS_TEST(logos_error_names_match_the_spec_including_the_british_spelling) {
    LOGOS_ASSERT_EQ(std::string(logosErrorName(LogosErrorCode::NotAuthorised)),
                    std::string("NOT_AUTHORISED"));
    LOGOS_ASSERT_EQ(std::string(logosErrorName(LogosErrorCode::MethodNotFound)),
                    std::string("METHOD_NOT_FOUND"));
    LOGOS_ASSERT_EQ(static_cast<int>(LogosErrorCode::Cancelled), 9);
}

// Not-loaded, not-exposed, denied-by-config and does-not-exist must be
// byte-identical, or the bridge is an oracle for what a node is running and
// what it has been told to hide.
LOGOS_TEST(the_not_found_answer_is_a_single_constant) {
    const std::string a = notFound().toJson().dump();
    const std::string b = notFound().toJson().dump();
    LOGOS_ASSERT_EQ(a, b);
    LOGOS_ASSERT_EQ(notFound().jsonRpcCode, static_cast<int>(kMethodNotFound));
    LOGOS_ASSERT_EQ(notFound().message, std::string("method not found"));
}

// Upstream error text routinely carries nix store paths, socket paths and
// instance ids. None of it may reach an external client.
LOGOS_TEST(upstream_detail_is_never_echoed_into_the_response) {
    const std::string leaky =
        "failed to open /nix/store/abc123-module/lib/x.dylib via /run/user/501/logos.sock";
    for (const std::string& code : kShippedCodes) {
        const std::string rendered = mapCallError(code, leaky).toJson().dump();
        LOGOS_ASSERT_TRUE(rendered.find("/nix/store") == std::string::npos);
        LOGOS_ASSERT_TRUE(rendered.find("logos.sock") == std::string::npos);
    }
}

// invalid_params_detail is permitted ONLY alongside INVALID_PARAMS, and carries
// the spec's three reason values.
LOGOS_TEST(invalid_params_detail_rides_only_with_invalid_params) {
    const auto j = invalidParamsJson("schema-mismatch", "key");
    LOGOS_ASSERT_EQ(j["code"].get<int>(), static_cast<int>(kInvalidParams));
    LOGOS_ASSERT_EQ(j["data"]["invalid_params_detail"]["reason"].get<std::string>(),
                    std::string("schema-mismatch"));
    LOGOS_ASSERT_EQ(j["data"]["invalid_params_detail"]["path"].get<std::string>(),
                    std::string("key"));

    const auto noPath = invalidParamsJson("malformed-cbor", "");
    LOGOS_ASSERT_FALSE(noPath["data"]["invalid_params_detail"].contains("path"));
}

// The documents' `errors` must be exactly what the bridge can answer: mapCallError,
// the dispatcher, and json_rpc_bridge_impl.cpp's shutting-down and overloaded.
LOGOS_TEST(the_error_catalog_covers_every_code_the_bridge_emits) {
    std::set<int> produced;
    for (const std::string& code : kShippedCodes) produced.insert(mapCallError(code, "").jsonRpcCode);
    produced.insert(mapCallError("something_new", "").jsonRpcCode);
    produced.insert(notFound().jsonRpcCode);
    produced.insert(invalidParamsJson("schema-mismatch", "x")["code"].get<int>());

    const auto refusal = [&](bool accepted, const MappedError& e) {
        LOGOS_ASSERT_FALSE(accepted);
        produced.insert(e.jsonRpcCode);
    };
    const auto parse = [](const char* s) { return nlohmann::json::parse(s); };
    RpcRequest req;
    CallTarget call;
    SubscribeTarget sub;
    MappedError e;
    refusal(parseRequest(parse("[]"), &req, &e), e);
    refusal(parseRequest(parse(R"({"jsonrpc":"2.0","id":1,"method":"rpc.ping","params":3})"), &req, &e), e);
    refusal(parseCallTarget(parse("[]"), &call, &e), e);
    refusal(parseSubscribeTarget(parse("{}"), &sub, &e, true), e);
    std::vector<nlohmann::json> parts;
    bool single = true;
    nlohmann::json protocolError;
    LOGOS_ASSERT_FALSE(splitBody("{oops", &parts, &single, &protocolError));
    produced.insert(protocolError["error"]["code"].get<int>());
    LOGOS_ASSERT_FALSE(splitBody("[]", &parts, &single, &protocolError));
    produced.insert(protocolError["error"]["code"].get<int>());
    produced.insert(kShuttingDown);
    produced.insert(kOverloaded);

    std::set<int> listed;
    for (const CatalogError& entry : errorCatalog()) {
        LOGOS_ASSERT_TRUE(listed.insert(entry.error.jsonRpcCode).second);  // one entry per code
    }
    for (int code : produced) LOGOS_ASSERT_TRUE(listed.count(code) == 1);
    for (int code : listed) LOGOS_ASSERT_TRUE(produced.count(code) == 1);
    LOGOS_ASSERT_FALSE(listed.count(kCancelled));  // nothing emits it
}

LOGOS_TEST(error_catalog_entries_are_error_objects_with_the_logos_taxonomy) {
    const std::vector<CatalogError> catalog = errorCatalog();
    LOGOS_ASSERT_EQ(catalog.size(), static_cast<size_t>(12));
    std::set<std::string> keys;
    for (const CatalogError& entry : catalog) {
        const std::string key = entry.key;
        // Documents reference entries by key, so a key is a stable CamelCase component name.
        LOGOS_ASSERT_TRUE(keys.insert(key).second);
        LOGOS_ASSERT_TRUE(!key.empty() && key[0] >= 'A' && key[0] <= 'Z');
        for (char c : key)
            LOGOS_ASSERT_TRUE((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));

        const nlohmann::json j = entry.error.toJson();
        LOGOS_ASSERT_EQ(j.size(), static_cast<size_t>(3));
        LOGOS_ASSERT_TRUE(j["code"].is_number_integer());
        LOGOS_ASSERT_FALSE(j["message"].get<std::string>().empty());
        const int logosCode = j["data"]["logos_error_code"].get<int>();
        LOGOS_ASSERT_EQ(j["data"]["logos_error_name"].get<std::string>(),
                        std::string(logosErrorName(static_cast<LogosErrorCode>(logosCode))));
    }
    LOGOS_ASSERT_EQ(std::string(catalog[0].key), std::string("ParseError"));
    LOGOS_ASSERT_EQ(catalog[0].error.jsonRpcCode, static_cast<int>(kParseError));
    // The documented not-found answer is byte-identical to the one sent.
    LOGOS_ASSERT_EQ(std::string(catalog[2].key), std::string("MethodNotFound"));
    LOGOS_ASSERT_EQ(catalog[2].error.toJson().dump(), notFound().toJson().dump());
    LOGOS_ASSERT_EQ(std::string(catalog[7].key), std::string("UpstreamTimeout"));
    LOGOS_ASSERT_EQ(catalog[7].error.jsonRpcCode, static_cast<int>(kTimeout));
}
