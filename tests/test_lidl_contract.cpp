// readContract: the one place logos-lidl is called.
//
// Fixtures (tests/fixtures/contracts), taken from real builds:
//   storage_module.lidl            #lidl of logos-storage-module on builder 5a962b8
//                                  (= its lidl() answer and LGX asset, sha256 9f6bd141...)
//   test_fullapi_cpp.legacy.lidl   #lidl of logos-test-modules test_fullapi_cpp from an
//                                  older writer (`doVoid() -> void`), so non-canonical
//   test_fullapi_cpp.lidl          `lidl fmt` of the legacy file (lidl-cli 1f54a2a)
//   test_fullapi_cpp.identity.json `lidl json --identity` of the canonical file

#include <logos_test.h>

#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "lidl_contract.h"

using namespace bridge;

namespace {

std::string fixture(const std::string& name) {
    const std::string path = std::string(BRIDGE_TEST_FIXTURES) + "/contracts/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw LogosTestFailure("missing fixture " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool hasWarning(const ContractResult& r, const std::string& code) {
    for (const auto& w : r.warnings)
        if (w == code) return true;
    return false;
}

const char* kSmall = "module m {\n  depends []\n\n  method get(key: tstr, ttl: ? int) -> tstr\n\n"
                     "  event changed(key: tstr)\n}\n";

} // namespace

LOGOS_TEST(a_canonical_contract_reads_clean) {
    const ContractResult r = readContract(fixture("test_fullapi_cpp.lidl"), "test_fullapi_cpp");
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_TRUE(r.warnings.empty());
    LOGOS_ASSERT_TRUE(r.lint.empty());
    LOGOS_ASSERT_EQ(r.moduleName, std::string("test_fullapi_cpp"));
    LOGOS_ASSERT_EQ(r.contractVersion, std::string("1.0.0"));
    LOGOS_ASSERT_EQ(r.events.size(), static_cast<size_t>(15));
    LOGOS_ASSERT_EQ(r.methods.front().name, std::string("whoAmI"));
    LOGOS_ASSERT_FALSE(r.methods.front().derived);
}

// Injection appends name, version and lidl, in that order, marked derived.
LOGOS_TEST(the_built_ins_are_injected_last_and_derived) {
    const ContractResult r = readContract(kSmall, "m");
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_EQ(r.methods.size(), static_cast<size_t>(4));
    const char* builtins[] = {"name", "version", "lidl"};
    for (int i = 0; i < 3; ++i) {
        LOGOS_ASSERT_EQ(r.methods[i + 1].name, std::string(builtins[i]));
        LOGOS_ASSERT_TRUE(r.methods[i + 1].derived);
        LOGOS_ASSERT_TRUE(r.methods[i + 1].params.empty());
    }
}

LOGOS_TEST(params_carry_names_and_optionality) {
    const ContractResult r = readContract(kSmall, "m");
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_EQ(r.methods[0].params.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(r.methods[0].params[0].name, std::string("key"));
    LOGOS_ASSERT_FALSE(r.methods[0].params[0].optional);
    LOGOS_ASSERT_TRUE(r.methods[0].params[1].optional);
    LOGOS_ASSERT_EQ(r.events.at(0).name, std::string("changed"));
    LOGOS_ASSERT_EQ(r.events.at(0).params.at(0).name, std::string("key"));
}

// The served AST is exactly what `lidl json --identity` prints for the file.
LOGOS_TEST(the_ast_equals_lidl_json_identity) {
    const ContractResult r = readContract(fixture("test_fullapi_cpp.lidl"), "test_fullapi_cpp");
    LOGOS_ASSERT_TRUE(r.ok);
    const auto served = nlohmann::json::parse(r.astJson);
    const auto cli = nlohmann::json::parse(fixture("test_fullapi_cpp.identity.json"));
    LOGOS_ASSERT_TRUE(served == cli);
    LOGOS_ASSERT_EQ(served.dump() + "\n", fixture("test_fullapi_cpp.identity.json"));
}

LOGOS_TEST(a_real_storage_contract_reads_clean) {
    const ContractResult r = readContract(fixture("storage_module.lidl"), "storage_module");
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_TRUE(r.warnings.empty());
    LOGOS_ASSERT_EQ(r.contractVersion, std::string("2.1.3"));
    LOGOS_ASSERT_CONTAINS(r.astJson, "\xe2\x80\x93");   // U+2013 stays raw UTF-8
}

// An older writer's `-> void` still parses; it just does not re-serialize to itself.
LOGOS_TEST(a_legacy_void_contract_is_accepted_but_non_canonical) {
    const ContractResult r =
        readContract(fixture("test_fullapi_cpp.legacy.lidl"), "test_fullapi_cpp");
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_TRUE(hasWarning(r, "non_canonical"));
    const auto ast = nlohmann::json::parse(r.astJson);
    bool sawDoVoid = false;
    for (const auto& m : ast["methods"]) {
        if (m["name"] != "doVoid") continue;
        sawDoVoid = true;
        LOGOS_ASSERT_FALSE(m.contains("returnType"));
        LOGOS_ASSERT_FALSE(m.contains("returnValueType"));
    }
    LOGOS_ASSERT_TRUE(sawDoVoid);
    // Same contract, canonical spelling: the served ASTs agree.
    const ContractResult canonical =
        readContract(fixture("test_fullapi_cpp.lidl"), "test_fullapi_cpp");
    LOGOS_ASSERT_EQ(r.astJson, canonical.astJson);
}

LOGOS_TEST(whitespace_drift_is_non_canonical) {
    std::string text = kSmall;
    text.insert(text.find("method"), " ");
    const ContractResult r = readContract(text, "m");
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_TRUE(hasWarning(r, "non_canonical"));
}

LOGOS_TEST(a_parse_error_reports_line_and_column) {
    const ContractResult r = readContract("module m {\n  depends []\n  method (\n}\n", "m");
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_EQ(r.errorLine, 3);
    LOGOS_ASSERT_GT(r.errorCol, 0);
    LOGOS_ASSERT_FALSE(r.error.empty());
    LOGOS_ASSERT_TRUE(r.astJson.empty());
}

LOGOS_TEST(a_contract_for_another_module_is_refused) {
    const ContractResult r = readContract(kSmall, "storage_module");
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, "'m'");
    LOGOS_ASSERT_CONTAINS(r.error, "'storage_module'");
    LOGOS_ASSERT_EQ(r.errorLine, 0);
}

// lidl() is generator-owned: an authored one means the text is not a built artifact.
LOGOS_TEST(an_authored_lidl_method_is_refused) {
    const ContractResult r =
        readContract("module m {\n  depends []\n\n  method lidl() -> tstr\n}\n", "m");
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, "lidl");
}

LOGOS_TEST(an_identity_method_with_the_wrong_signature_is_refused) {
    const ContractResult r =
        readContract("module m {\n  depends []\n\n  method version(n: int) -> tstr\n}\n", "m");
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, "version");
}

// An author's exact-signature name()/version() is kept, not duplicated.
LOGOS_TEST(an_authored_exact_identity_method_is_kept) {
    const ContractResult r =
        readContract("module m {\n  depends []\n\n  method version() -> tstr\n}\n", "m");
    LOGOS_ASSERT_TRUE(r.ok);
    LOGOS_ASSERT_EQ(r.methods.size(), static_cast<size_t>(3));
    LOGOS_ASSERT_FALSE(r.methods[0].derived);
}

LOGOS_TEST(a_contract_failing_validation_is_refused) {
    const ContractResult r =
        readContract("module m {\n  depends []\n\n  method get() -> Missing\n}\n", "m");
    LOGOS_ASSERT_FALSE(r.ok);
    LOGOS_ASSERT_CONTAINS(r.error, "invalid contract");
    LOGOS_ASSERT_CONTAINS(r.error, "Missing");
}

LOGOS_TEST(the_reader_version_has_the_cli_shape) {
    const std::string v = lidlReaderVersion();
    LOGOS_ASSERT_EQ(v.rfind("lidl ", 0), static_cast<size_t>(0));
    LOGOS_ASSERT_CONTAINS(v, " (" + lidlReaderRev() + ")");
    LOGOS_ASSERT_FALSE(lidlReaderRev().empty());
}
