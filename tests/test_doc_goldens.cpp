// Golden documents for the fixture contracts, which the docs-metaschema check validates.
// Contexts come from tests/fixtures/configs through the path the bridge and the docs
// CLI share (offlineView, docContext), so docs-golden can reproduce three of them.
// BRIDGE_TEST_FIXTURES_DIR/_GOLDENS_DIR come from CMake; BRIDGE_UPDATE_GOLDENS=1 rewrites.

#include <logos_test.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "discovery_model.h"
#include "doc_publisher.h"
#include "doc_test_util.h"

using namespace bridge::docs;
using namespace bridge_test;
using bridge::BridgeConfig;
using bridge::ExposedModule;
using bridge::ModuleView;

namespace {

// info.version in the goldens, pinned so a version bump does not rewrite them.
constexpr const char* kGoldenVersion = "0.1.0";

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    return static_cast<bool>(out);
}

std::string fixturePath(const std::string& rel) {
    return std::string(BRIDGE_TEST_FIXTURES_DIR) + "/" + rel;
}

std::string goldenPath(const std::string& context, const std::string& kind) {
    return std::string(BRIDGE_TEST_GOLDENS_DIR) + "/" + context + "." + kind + ".json";
}

json fixtureAst(const std::string& module) {
    return json::parse(readFile(fixturePath("ast/" + module + ".json")));
}

std::string fixtureLidl(const std::string& module) {
    return readFile(fixturePath("lidl/" + module + ".lidl"));
}

BridgeConfig parseConfig(const std::string& text) {
    bridge::ConfigParseResult r = bridge::parseBridgeConfig(text, "json_rpc_bridge");
    if (!r.ok) throw LogosTestFailure("bad config: " + r.error);
    return r.config;
}

// tests/fixtures/configs/<context>.json, which docs-golden hands to the CLI.
BridgeConfig contextConfig(const std::string& context) {
    return parseConfig(readFile(fixturePath("configs/" + context + ".json")));
}

// A fixture module as discovery reads it, and as json-rpc-bridge-docs does.
std::shared_ptr<const ModuleView> typedFixture(const ExposedModule& em) {
    bridge::OfflineView v = bridge::offlineView(em, fixtureLidl(em.name));
    if (!v.ok) throw LogosTestFailure(em.name + ": " + v.error);
    return std::make_shared<const ModuleView>(std::move(v.view));
}

bridge::LiveReport liveReport(const std::string& text) {
    bridge::LiveReport r;
    if (!bridge::parseLiveReport(json::parse(text), &r)) throw LogosTestFailure("bad live report");
    return r;
}

const std::vector<std::string> kGoldenContexts = {"full_api", "full_api_ext_policy", "storage", "multi"};
const std::vector<std::string> kFixtureModules = {"test_fullapi_cpp", "test_fullapi_ext_cpp",
                                                  "storage_module", "mini_module"};

// multi also holds what the CLI cannot render: untyped, invalid and pending modules.
std::shared_ptr<const ModuleView> multiView(const ExposedModule& em) {
    if (em.name == "mini_module") return typedFixture(em);
    ModuleView v = bridge::pendingView(em.name);
    if (em.name == "legacy_module") {
        v = bridge::untypedView(v, liveReport(
            R"([{"name":"ping","parameters":[]},)"
            R"({"name":"add","parameters":[{"name":"a"},{"name":"b"}]},)"
            R"({"type":"event","name":"ticked","parameters":[]}])"));
    } else if (em.name == "broken_module") {
        v = bridge::invalidView(v, liveReport(
            R"([{"name":"reset","parameters":[{"name":"hard"}]},{"name":"lidl","parameters":[]}])"),
            "3:17: expected ')' (lidl reader 1f54a2a)");
    }
    return std::make_shared<const ModuleView>(bridge::withExposure(em, std::move(v)));
}

DocContext goldenContext(const std::string& name) {
    const BridgeConfig cfg = contextConfig(name);
    std::vector<std::shared_ptr<const ModuleView>> views;
    for (const ExposedModule& em : cfg.modules)
        views.push_back(name == "multi" ? multiView(em) : typedFixture(em));
    return bridge::docContext(cfg, kGoldenVersion, views);
}

bool updateRequested() {
    const char* v = std::getenv("BRIDGE_UPDATE_GOLDENS");
    return v && std::string(v) == "1";
}

std::string firstDifference(const std::string& a, const std::string& b) {
    std::size_t i = 0, line = 1;
    while (i < a.size() && i < b.size() && a[i] == b[i]) {
        if (a[i] == '\n') ++line;
        ++i;
    }
    return "line " + std::to_string(line);
}

const json* findNamed(const json& list, const std::string& name) {
    for (const auto& item : list)
        if (item["name"] == name) return &item;
    return nullptr;
}

} // namespace

// The ASTs are `lidl json --identity` output: the canonical dump plus a newline,
// so re-dumping one is the canonical JSON its interface_sha256 is taken over.
LOGOS_TEST(fixture_asts_are_canonical_and_carry_the_built_ins) {
    for (const std::string& module : kFixtureModules) {
        const std::string text = readFile(fixturePath("ast/" + module + ".json"));
        LOGOS_ASSERT_FALSE(text.empty());
        const json ast = json::parse(text);
        LOGOS_ASSERT_EQ(ast.dump() + "\n", text);
        LOGOS_ASSERT_EQ(ast["name"], json(module));
        for (const char* builtin : {"name", "version", "lidl"}) {
            const json* m = findNamed(ast["methods"], builtin);
            LOGOS_ASSERT_TRUE(m != nullptr);
            LOGOS_ASSERT_EQ((*m)["derived"], json(true));
        }
        LOGOS_ASSERT_FALSE(fixtureLidl(module).empty());
    }
}

// The bridge's reader and `lidl json --identity` agree on every fixture, and the
// fixtures are canonical, so offline and served digests are the files' own.
LOGOS_TEST(every_fixture_reads_to_its_vendored_ast) {
    for (const std::string& module : kFixtureModules) {
        ExposedModule em;
        em.name = module;
        const std::string lidl = fixtureLidl(module);
        bridge::OfflineView v = bridge::offlineView(em, lidl);
        if (!v.ok) throw LogosTestFailure(module + ": " + v.error);
        LOGOS_ASSERT_TRUE(v.warnings.empty());
        const std::string ast = readFile(fixturePath("ast/" + module + ".json"));
        LOGOS_ASSERT_EQ(v.view.contract->interface, json::parse(ast));
        LOGOS_ASSERT_EQ(v.view.interfaceSha256, sha256Hex(ast.substr(0, ast.size() - 1)));
        LOGOS_ASSERT_EQ(v.view.contractSha256, sha256Hex(lidl));
        LOGOS_ASSERT_FALSE(v.view.crossChecked);
    }
}

// The policy goldens are only meaningful if the denied names exist, and if the
// record the denied method uses is used by nothing else that stays exposed.
LOGOS_TEST(the_policy_fixtures_declare_what_they_deny) {
    const json ext = fixtureAst("test_fullapi_ext_cpp");
    const json* echoWrapper = findNamed(ext["methods"], "echoWrapper");
    LOGOS_ASSERT_TRUE(echoWrapper != nullptr);
    LOGOS_ASSERT_EQ((*echoWrapper)["returnType"]["name"], json("Wrapper"));
    LOGOS_ASSERT_TRUE(findNamed(ext["events"], "blobEvent") != nullptr);
    LOGOS_ASSERT_TRUE(findNamed(ext["types"], "Wrapper") != nullptr);
    for (const auto& m : ext["methods"])
        if (m["name"] != "echoWrapper")
            LOGOS_ASSERT_TRUE(m.dump().find("\"Wrapper\"") == std::string::npos);
    const BridgeConfig extCfg = contextConfig("full_api_ext_policy");
    LOGOS_ASSERT_EQ(extCfg.modules.size(), static_cast<size_t>(1));
    LOGOS_ASSERT_FALSE(extCfg.modules[0].methodAllowed("echoWrapper"));
    LOGOS_ASSERT_FALSE(extCfg.modules[0].events.permits("blobEvent"));

    const json storage = fixtureAst("storage_module");
    LOGOS_ASSERT_TRUE(findNamed(storage["methods"], "destroy") != nullptr);
    const BridgeConfig storageCfg = contextConfig("storage");
    LOGOS_ASSERT_FALSE(storageCfg.modules[0].methodAllowed("destroy"));
    LOGOS_ASSERT_TRUE(storageCfg.modules[0].methodAllowed("lidl"));
}

// multi's one typed module stands in for full_api, so it must keep every shape the builders special-case.
LOGOS_TEST(the_mini_fixture_covers_what_multi_needs) {
    const json mini = fixtureAst("mini_module");
    const json* note = findNamed(mini["types"], "Note");
    LOGOS_ASSERT_TRUE(note != nullptr);
    bool optionalField = false, bytesField = false;
    for (const auto& f : (*note)["fields"]) {
        optionalField = optionalField || f["isOptional"].get<bool>();
        bytesField = bytesField || f["valueType"]["name"] == "bstr";
    }
    LOGOS_ASSERT_TRUE(optionalField && bytesField);

    const json* put = findNamed(mini["methods"], "put");
    LOGOS_ASSERT_TRUE(put != nullptr);
    LOGOS_ASSERT_EQ((*put)["params"][0]["type"]["name"], json("Note"));
    LOGOS_ASSERT_EQ((*put)["returnType"]["name"], json("Note"));
    bool noReturn = false, resultReturn = false, optionalParam = false;
    for (const auto& m : mini["methods"]) {
        noReturn = noReturn || !m.contains("returnType");
        resultReturn = resultReturn || (m.contains("returnType") && m["returnType"]["name"] == "result");
        for (const auto& p : m["params"]) optionalParam = optionalParam || p["isOptional"].get<bool>();
    }
    LOGOS_ASSERT_TRUE(noReturn && resultReturn && optionalParam);
    LOGOS_ASSERT_EQ(mini["events"].size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(mini["events"][0]["params"][0]["type"]["name"], json("Note"));
}

// Every status reaches the documents through the runtime's adapter.
LOGOS_TEST(the_multi_context_has_every_status) {
    const DocContext ctx = goldenContext("multi");
    std::vector<std::string> statuses;
    for (const auto& m : ctx.modules) statuses.push_back(m.status);
    LOGOS_ASSERT_TRUE(statuses == (std::vector<std::string>{"ok", "untyped", "invalid", "pending"}));
    LOGOS_ASSERT_TRUE(ctx.modules[1].exposedEvents == std::vector<std::string>{"ticked"});
    LOGOS_ASSERT_TRUE(ctx.modules[2].exposedMethods == (std::vector<std::string>{"reset", "lidl"}));
}

LOGOS_TEST(golden_documents_match) {
    const bool update = updateRequested();
    std::vector<std::string> stale;
    for (const std::string& name : kGoldenContexts) {
        const DocContext ctx = goldenContext(name);
        for (const std::string& kind : kDocKinds) {
            const std::string path = goldenPath(name, kind);
            const std::string built = buildDoc(kind, ctx).dump(2) + "\n";
            if (update) {
                LOGOS_ASSERT_TRUE(writeFile(path, built));
                continue;
            }
            const std::string onDisk = readFile(path);
            if (onDisk != built)
                stale.push_back(path + (onDisk.empty() ? " (missing)" : " (differs at " + firstDifference(onDisk, built) + ")"));
        }
    }
    if (!stale.empty()) {
        std::string msg = "stale goldens; regenerate them (tests/fixtures/SOURCES.md) and review the diff:";
        for (const std::string& s : stale) msg += "\n  " + s;
        throw LogosTestFailure(msg);
    }
}

LOGOS_TEST(every_golden_ref_resolves_inside_its_own_document) {
    for (const std::string& name : kGoldenContexts)
        for (const std::string& kind : kDocKinds) {
            const std::vector<std::string> bad = unresolvedRefs(buildDoc(kind, goldenContext(name)));
            if (!bad.empty()) throw LogosTestFailure(name + "." + kind + ": " + bad.front());
        }
}

LOGOS_TEST(golden_policies_leave_no_operation_for_denied_members) {
    const DocContext ext = goldenContext("full_api_ext_policy");
    const DocContext storage = goldenContext("storage");
    for (const std::string& kind : kDocKinds) {
        const json e = buildDoc(kind, ext);
        for (const auto& t : targets(e)) {
            LOGOS_ASSERT_NE(t.second, std::string("echoWrapper"));
            LOGOS_ASSERT_NE(t.second, std::string("blobEvent"));
        }
        LOGOS_ASSERT_TRUE(e["components"]["schemas"].contains("test_fullapi_ext_cpp.Wrapper"));
        LOGOS_ASSERT_TRUE(e["components"]["schemas"].contains("test_fullapi_ext_cpp.Blob"));
        LOGOS_ASSERT_EQ(e["x-logos-modules"][0]["interface_sha256"],
                        json(sha256Hex(fixtureAst("test_fullapi_ext_cpp").dump())));

        const json s = buildDoc(kind, storage);
        for (const auto& t : targets(s)) LOGOS_ASSERT_NE(t.second, std::string("destroy"));
        LOGOS_ASSERT_EQ(s["x-logos-modules"][0]["contract_sha256"],
                        json("9f6bd141a1401b14ec151b579fd1e1076ba7916929f54101fd6843501dee92ac"));
    }
    LOGOS_ASSERT_TRUE(buildOpenApi(ext)["paths"].contains("/modules/test_fullapi_ext_cpp/lidl"));
    LOGOS_ASSERT_TRUE(buildOpenApi(storage)["paths"].contains("/modules/storage_module/stop"));
    LOGOS_ASSERT_FALSE(buildOpenApi(storage)["paths"].contains("/modules/storage_module/destroy"));
}

// interface_sha256 is policy-independent: denying members does not change it.
LOGOS_TEST(the_interface_digest_ignores_policy) {
    const BridgeConfig open = parseConfig(R"({"expose":{"modules":["test_fullapi_ext_cpp"]}})");
    const BridgeConfig denied = contextConfig("full_api_ext_policy");
    const auto a = typedFixture(open.modules[0]);
    const auto b = typedFixture(denied.modules[0]);
    LOGOS_ASSERT_EQ(a->interfaceSha256, b->interfaceSha256);
    LOGOS_ASSERT_EQ(a->contract->interface, b->contract->interface);
    LOGOS_ASSERT_NE(a->exposure.methods.size(), b->exposure.methods.size());
    LOGOS_ASSERT_NE(a->exposure.events.size(), b->exposure.events.size());
}
