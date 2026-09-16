// json-rpc-bridge-docs, run in-process: arguments, exit codes, and that its output
// is what the running bridge builds from the same views. Packages with contracts
// need the lgx CLI, so the docs-golden check covers --lgx reading.

#include <logos_test.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>   // mkdtemp

#include <lgx.h>
#include <nlohmann/json.hpp>

#include "discovery_model.h"
#include "doc_publisher.h"
#include "json_rpc_bridge_docs.h"

using namespace bridge;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const char* kM1 = "module m1 {\n  version \"1.0.0\"\n  depends []\n\n"
                  "  method greet(who: tstr) -> tstr\n  method reset()\n\n"
                  "  event greeted(who: tstr)\n}\n";
const char* kM2 = "module m2 {\n  depends []\n\n  method ping() -> bool\n}\n";

// A scratch directory for one test, removed afterwards.
struct Scratch {
    fs::path dir;
    Scratch() {
        std::string pattern = (fs::temp_directory_path() / "bridge-docs-cli.XXXXXX").string();
        std::vector<char> buf(pattern.begin(), pattern.end());
        buf.push_back('\0');
        if (!::mkdtemp(buf.data())) throw LogosTestFailure("mkdtemp failed");
        dir = buf.data();
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string write(const std::string& name, const std::string& content) const {
        const fs::path p = dir / name;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << content;
        return p.string();
    }
    std::string path(const std::string& name) const { return (dir / name).string(); }
};

struct Run {
    int code = -1;
    std::string out, err;
};

Run cli(std::initializer_list<std::string> args) {
    std::vector<std::string> owned{"json-rpc-bridge-docs"};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<const char*> argv;
    for (const auto& a : owned) argv.push_back(a.c_str());
    std::ostringstream out, err;
    Run r;
    r.code = bridge_docs::runDocsCli(static_cast<int>(argv.size()), argv.data(), out, err);
    r.out = out.str();
    r.err = err.str();
    return r;
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void expectCode(const Run& r, int code) {
    if (r.code != code)
        throw LogosTestFailure("exit " + std::to_string(r.code) + ", expected " + std::to_string(code) +
                               "; stderr: " + r.err);
}

const char* kOneModule = R"({"expose":{"modules":["m1"]}})";

// What a running bridge builds for these (name, contract) pairs under this config.
docs::DocContext servedContext(const std::string& configText,
                               const std::vector<std::pair<std::string, std::string>>& contracts,
                               const std::string& version = JSON_RPC_BRIDGE_VERSION) {
    const ConfigParseResult cfg = parseBridgeConfig(configText, "json_rpc_bridge");
    if (!cfg.ok) throw LogosTestFailure("bad test config: " + cfg.error);
    std::vector<std::shared_ptr<const ModuleView>> views;
    for (const auto& c : contracts) {
        OfflineView v = offlineView(*cfg.config.find(c.first), c.second);
        if (!v.ok) throw LogosTestFailure("fixture contract: " + v.error);
        views.push_back(std::make_shared<const ModuleView>(v.view));
    }
    return docContext(cfg.config, version, views);
}

} // namespace

// ── usage ───────────────────────────────────────────────────────────────────

LOGOS_TEST(cli_version_and_help_exit_zero) {
    Run v = cli({"--version"});
    expectCode(v, 0);
    LOGOS_ASSERT_TRUE(v.out.rfind(std::string("json-rpc-bridge-docs ") + JSON_RPC_BRIDGE_VERSION + " (lidl ", 0) == 0);
    LOGOS_ASSERT_TRUE(has(v.out, "lgx "));
    Run h = cli({"--help"});
    expectCode(h, 0);
    LOGOS_ASSERT_TRUE(has(h.out, "usage: json-rpc-bridge-docs --config PATH"));
    LOGOS_ASSERT_TRUE(has(h.out, "Exit codes: 0 ok, 2 usage"));
}

LOGOS_TEST(cli_usage_errors_exit_two) {
    Scratch s;
    const std::string cfg = s.write("c.json", R"({"expose":{"modules":["m1","m2"]}})");
    const std::string m1 = s.write("m1.lidl", kM1);
    const std::string in = "m1=" + m1;
    const std::vector<std::vector<std::string>> cases = {
        {},
        {"--format", "openrpc", "--lidl", in},
        {"--config", cfg, "--lidl", in},
        {"--config", cfg, "--format", "yaml", "--lidl", in},
        {"--config", cfg, "--format", "openrpc"},
        {"--config", cfg, "--format", "openrpc", "--lidl", in, "--bogus"},
        {"--config", cfg, "--format", "openrpc", "--lidl", in, "stray"},
        {"--config", cfg, "--format", "openrpc", "--lidl", m1},
        {"--config", cfg, "--format", "openrpc", "--lidl", "=" + m1},
        {"--config", cfg, "--format", "openrpc", "--lidl=", "--lidl", in},
        {"--config", cfg, "--config", cfg, "--format", "openrpc", "--lidl", in},
        {"--config", cfg, "--format", "openrpc", "--lidl", in, "--server-port", "0"},
        {"--config", cfg, "--format", "openrpc", "--lidl", in, "--server-port", "70000"},
        {"--config", cfg, "--format", "openrpc", "--lidl", in, "--server-port", "80a"},
        {"--config", cfg, "--format", "openrpc", "--lidl", in, "--server-host", "a b"},
        {"--config", cfg, "--format", "openrpc", "--lidl", in, "--server-host", "h/x"},
        {"--config", cfg, "--format", "openrpc", "--lidl", in, "--pretty=yes"},
        {"--config", cfg, "--format", "openrpc", "--lidl", in, "--module"},
        {"--config", cfg, "--format", "openrpc", "--lidl", in, "--module", "m3"},
        {"--config", cfg, "--format", "openrpc", "--lidl", "m3=" + m1},
        {"--config", cfg, "--format", "openrpc", "--lidl", in, "--lidl", in},
        {"--config", cfg, "--format", "interface", "--lidl", in},
    };
    for (const auto& args : cases) {
        std::vector<std::string> owned{"json-rpc-bridge-docs"};
        owned.insert(owned.end(), args.begin(), args.end());
        std::vector<const char*> argv;
        for (const auto& a : owned) argv.push_back(a.c_str());
        std::ostringstream out, err;
        const int code = bridge_docs::runDocsCli(static_cast<int>(argv.size()), argv.data(), out, err);
        std::string joined;
        for (const auto& a : args) joined += a + " ";
        if (code != 2) throw LogosTestFailure("exit " + std::to_string(code) + " for: " + joined + "\n" + err.str());
        LOGOS_ASSERT_TRUE(out.str().empty());
        LOGOS_ASSERT_TRUE(has(err.str(), "Try 'json-rpc-bridge-docs --help'."));
    }
}

// ── config ──────────────────────────────────────────────────────────────────

LOGOS_TEST(cli_a_rejected_config_exits_three) {
    Scratch s;
    const std::string in = "m1=" + s.write("m1.lidl", kM1);
    const std::vector<std::pair<std::string, std::string>> configs = {
        {"not json", "config is not valid JSON"},
        {R"({"http":{"host":"0.0.0.0"},"expose":{"modules":["m1"]}})", "loopback"},
        {R"({"expose":{"modules":["rpc"]}})", "rpc"},
        {R"({"expose":{"modules":[{"name":"m1","methods":{"deny":["lidl"]}}]}})", "must not name 'lidl'"},
        {R"({"expose":{"modules":["json_rpc_bridge"]}})", "this module itself"},
    };
    for (const auto& c : configs) {
        const std::string path = s.write("c.json", c.first);
        Run r = cli({"--config", path, "--format", "openrpc", "--lidl", in});
        expectCode(r, 3);
        LOGOS_ASSERT_TRUE(has(r.err, "config rejected: "));
        LOGOS_ASSERT_TRUE(has(r.err, c.second));
    }
    Run missing = cli({"--config", s.path("absent.json"), "--format", "openrpc", "--lidl", in});
    expectCode(missing, 3);
    LOGOS_ASSERT_TRUE(has(missing.err, "absent.json: no such file"));
}

// ── contracts ───────────────────────────────────────────────────────────────

LOGOS_TEST(cli_unreadable_or_invalid_contracts_exit_four_with_discovery_messages) {
    Scratch s;
    const std::string cfg = s.write("c.json", kOneModule);
    const auto run = [&](const std::string& contract) {
        const std::string file = s.write("in/m1.lidl", contract);
        return cli({"--config", cfg, "--format", "openapi", "--lidl", "m1=" + file});
    };
    Run malformed = run("module m1 {\n  method greet(who: tstr -> tstr\n}\n");
    expectCode(malformed, 4);
    LOGOS_ASSERT_TRUE(has(malformed.err, "m1: "));
    LOGOS_ASSERT_TRUE(has(malformed.err, "2:"));
    LOGOS_ASSERT_TRUE(has(malformed.err, "(lidl reader " + lidlReaderRev() + ")"));

    Run mismatch = run(kM2);
    expectCode(mismatch, 4);
    LOGOS_ASSERT_TRUE(has(mismatch.err, "the contract declares module 'm2', not 'm1'"));

    Run authored = run("module m1 {\n  depends []\n\n  method lidl() -> tstr\n}\n");
    expectCode(authored, 4);
    const ContractResult direct = readContract("module m1 {\n  depends []\n\n  method lidl() -> tstr\n}\n", "m1");
    LOGOS_ASSERT_FALSE(direct.ok);
    LOGOS_ASSERT_TRUE(has(authored.err, direct.error));

    Run utf8 = run(std::string("module m1 {\n  description \"\xC3\x28\"\n  depends []\n}\n"));
    expectCode(utf8, 4);
    LOGOS_ASSERT_TRUE(has(utf8.err, "not valid UTF-8"));

    Run large = run(std::string(kMaxContractBytes + 1, ' '));
    expectCode(large, 4);
    LOGOS_ASSERT_TRUE(has(large.err, "larger than 4 MiB"));

    Run absent = cli({"--config", cfg, "--format", "openapi", "--lidl", "m1=" + s.path("nope.lidl")});
    expectCode(absent, 4);
    LOGOS_ASSERT_TRUE(has(absent.err, "nope.lidl: no such file"));

    Run notDir = cli({"--config", cfg, "--format", "openapi", "--lidl-dir", s.write("file", "x")});
    expectCode(notDir, 4);
    LOGOS_ASSERT_TRUE(has(notDir.err, "not a directory"));
}

LOGOS_TEST(cli_disagreeing_sources_exit_four_and_agreeing_ones_are_fine) {
    Scratch s;
    const std::string cfg = s.write("c.json", kOneModule);
    const std::string file = s.write("a/m1.lidl", kM1);
    s.write("b/m1.lidl", std::string(kM1) + "\n");
    Run agree = cli({"--config", cfg, "--format", "openrpc", "--lidl", "m1=" + file, "--lidl-dir", s.path("a")});
    expectCode(agree, 0);
    Run differ = cli({"--config", cfg, "--format", "openrpc", "--lidl", "m1=" + file, "--lidl-dir", s.path("b")});
    expectCode(differ, 4);
    LOGOS_ASSERT_TRUE(has(differ.err, " differ"));
}

LOGOS_TEST(cli_packages_that_cannot_be_read_or_carry_nothing_exit_four) {
    Scratch s;
    const std::string cfg = s.write("c.json", kOneModule);
    Run missing = cli({"--config", cfg, "--format", "openrpc", "--lgx", s.path("none.lgx")});
    expectCode(missing, 4);
    LOGOS_ASSERT_TRUE(has(missing.err, "none.lgx: "));
    Run garbage = cli({"--config", cfg, "--format", "openrpc", "--lgx", s.write("bad.lgx", "not a package")});
    expectCode(garbage, 4);
    LOGOS_ASSERT_TRUE(has(garbage.err, "bad.lgx: "));

    // A well-formed package with no assets/lidl at all, made through the same C API.
    const std::string pkgPath = s.path("m1.lgx");
    const std::string payload = s.write("payload/m1_plugin.so", "not really a plugin");
    lgx_result_t made = lgx_create(pkgPath.c_str(), "m1");
    LOGOS_ASSERT_TRUE(made.success);
    lgx_package_t pkg = lgx_load(pkgPath.c_str());
    LOGOS_ASSERT_TRUE(pkg != nullptr);
    LOGOS_ASSERT_TRUE(lgx_add_variant(pkg, lgx_host_variant(), payload.c_str(), nullptr).success);
    LOGOS_ASSERT_TRUE(lgx_save(pkg, pkgPath.c_str()).success);
    lgx_free_package(pkg);
    Run empty = cli({"--config", cfg, "--format", "openrpc", "--lgx", pkgPath});
    expectCode(empty, 4);
    LOGOS_ASSERT_TRUE(has(empty.err, "carries no contract for m1 (assets/lidl has none)"));
    // It does not help when the contract comes from elsewhere either.
    Run mixed = cli({"--config", cfg, "--format", "openrpc", "--lgx", pkgPath,
                     "--lidl", "m1=" + s.write("m1.lidl", kM1)});
    expectCode(mixed, 4);
}

// ── untyped modules ─────────────────────────────────────────────────────────

LOGOS_TEST(cli_a_module_without_a_contract_exits_five_unless_allowed) {
    Scratch s;
    const std::string two = R"({"expose":{"modules":["m1","m2"]}})";
    const std::string cfg = s.write("c.json", two);
    const std::string in = "m1=" + s.write("m1.lidl", kM1);
    Run strict = cli({"--config", cfg, "--format", "openrpc", "--lidl", in});
    expectCode(strict, 5);
    LOGOS_ASSERT_TRUE(has(strict.err, "m2: no contract given"));

    Run allowed = cli({"--config", cfg, "--format", "openrpc", "--lidl", in, "--allow-untyped"});
    expectCode(allowed, 0);
    LOGOS_ASSERT_TRUE(has(allowed.err, "m2: no contract; left out"));
    const json doc = json::parse(allowed.out);
    LOGOS_ASSERT_EQ(doc["x-logos-modules"].size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(doc["x-logos-modules"][0]["name"], json("m1"));
    LOGOS_ASSERT_EQ(allowed.out, compactJson(docs::buildOpenRpc(servedContext(R"({"expose":{"modules":["m1"]}})", {{"m1", kM1}}))));

    Run list = cli({"--config", cfg, "--format", "schema", "--lidl", in, "--allow-untyped"});
    expectCode(list, 0);
    LOGOS_ASSERT_EQ(json::parse(list.out).size(), static_cast<size_t>(1));

    // A single module cannot be left out of its own rendering.
    expectCode(cli({"--config", cfg, "--format", "interface", "--module", "m2", "--lidl", in, "--allow-untyped"}), 5);
    expectCode(cli({"--config", cfg, "--format", "schema", "--module", "m2", "--lidl", in, "--allow-untyped"}), 5);
    // --module narrows what must have a contract.
    expectCode(cli({"--config", cfg, "--format", "openrpc", "--module", "m1", "--lidl", in}), 0);
}

// ── output ──────────────────────────────────────────────────────────────────

LOGOS_TEST(cli_documents_equal_what_the_bridge_builds) {
    Scratch s;
    const std::string config = R"({"http":{"port":9123},"expose":{"modules":[)"
                               R"({"name":"m1","methods":{"deny":["reset"]}},"m2"]}})";
    const std::string cfg = s.write("c.json", config);
    const std::string m1 = s.write("dir/m1.lidl", kM1);
    const std::string m2 = s.write("dir/m2.lidl", kM2);
    const docs::DocContext served = servedContext(config, {{"m1", kM1}, {"m2", kM2}});
    const std::vector<std::pair<std::string, json>> kinds = {
        {"openrpc", docs::buildOpenRpc(served)},
        {"openapi", docs::buildOpenApi(served)},
        {"asyncapi", docs::buildAsyncApi(served)},
    };
    for (const auto& k : kinds) {
        Run byFile = cli({"--config", cfg, "--format", k.first, "--lidl", "m1=" + m1, "--lidl", "m2=" + m2});
        expectCode(byFile, 0);
        LOGOS_ASSERT_TRUE(byFile.err.empty());
        LOGOS_ASSERT_EQ(byFile.out, compactJson(k.second));
        Run byDir = cli({"--config", cfg, "--format=" + k.first, "--lidl-dir", s.path("dir")});
        expectCode(byDir, 0);
        LOGOS_ASSERT_EQ(byDir.out, byFile.out);
        Run pretty = cli({"--config", cfg, "--format", k.first, "--lidl-dir", s.path("dir"), "--pretty"});
        expectCode(pretty, 0);
        LOGOS_ASSERT_EQ(pretty.out, k.second.dump(2) + "\n");
    }
    const json openapi = json::parse(cli({"--config", cfg, "--format", "openapi", "--lidl-dir", s.path("dir")}).out);
    LOGOS_ASSERT_FALSE(openapi["paths"].contains("/modules/m1/reset"));
    LOGOS_ASSERT_TRUE(openapi["paths"].contains("/modules/m1/lidl"));
    LOGOS_ASSERT_EQ(openapi["servers"][0]["url"], json("http://127.0.0.1:9123"));
}

LOGOS_TEST(cli_server_and_version_overrides_reach_the_documents) {
    Scratch s;
    const std::string cfg = s.write("c.json", kOneModule);
    const std::string in = "m1=" + s.write("m1.lidl", kM1);
    Run r = cli({"--config", cfg, "--format", "openrpc", "--lidl", in, "--server-host", "::1",
                 "--server-port", "18645", "--info-version", "9.9.9-test"});
    expectCode(r, 0);
    const json doc = json::parse(r.out);
    LOGOS_ASSERT_EQ(doc["info"]["version"], json("9.9.9-test"));
    LOGOS_ASSERT_EQ(doc["servers"][0]["url"], json("http://[::1]:18645/rpc"));
    LOGOS_ASSERT_EQ(doc["servers"][1]["url"], json("ws://[::1]:18645/ws"));
    Run async = cli({"--config", cfg, "--format", "asyncapi", "--lidl", in, "--server-host", "localhost"});
    LOGOS_ASSERT_EQ(json::parse(async.out)["servers"]["bridge"]["host"], json("localhost:8645"));
}

// sha256(interface output) is interface_sha256, and the schema view is rpc.schema's.
LOGOS_TEST(cli_interface_bytes_hash_to_the_schema_digest) {
    Scratch s;
    const std::string cfg = s.write("c.json", R"({"expose":{"modules":["m1","m2"]}})");
    const std::string dir = s.path("d");
    s.write("d/m1.lidl", kM1);
    s.write("d/m2.lidl", kM2);
    Run iface = cli({"--config", cfg, "--format", "interface", "--module", "m1", "--lidl-dir", dir});
    expectCode(iface, 0);
    Run schema = cli({"--config", cfg, "--format", "schema", "--module", "m1", "--lidl-dir", dir});
    expectCode(schema, 0);
    const json view = json::parse(schema.out);
    LOGOS_ASSERT_EQ(json(sha256Hex(iface.out)), view["interface_sha256"]);
    LOGOS_ASSERT_EQ(json(sha256Hex(kM1)), view["contract_sha256"]);
    LOGOS_ASSERT_EQ(json::parse(iface.out), view["interface"]);
    LOGOS_ASSERT_EQ(iface.out, json::parse(iface.out).dump());
    LOGOS_ASSERT_EQ(view["interface_status"], json("ok"));
    LOGOS_ASSERT_EQ(view["source"], json("lidl"));
    LOGOS_ASSERT_TRUE(view["cross_check"].is_null());
    LOGOS_ASSERT_EQ(view["exposure"]["methods"], json({"greet", "reset", "name", "version", "lidl"}));
    Run pretty = cli({"--config", cfg, "--format", "interface", "--module", "m1", "--lidl-dir", dir, "--pretty"});
    LOGOS_ASSERT_EQ(pretty.out, view["interface"].dump(2) + "\n");

    Run list = cli({"--config", cfg, "--format", "schema", "--lidl-dir", dir});
    expectCode(list, 0);
    const json views = json::parse(list.out);
    LOGOS_ASSERT_EQ(views.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(views[1]["module"], json("m2"));
    LOGOS_ASSERT_FALSE(views[0].contains("interface"));
    LOGOS_ASSERT_EQ(views[0]["interface_sha256"], view["interface_sha256"]);
}

LOGOS_TEST(cli_out_writes_the_file_or_exits_six) {
    Scratch s;
    const std::string cfg = s.write("c.json", kOneModule);
    const std::string in = "m1=" + s.write("m1.lidl", kM1);
    const std::string target = s.path("out.json");
    Run ok = cli({"--config", cfg, "--format", "openapi", "--lidl", in, "--out", target});
    expectCode(ok, 0);
    LOGOS_ASSERT_TRUE(ok.out.empty());
    std::ifstream file(target, std::ios::binary);
    std::string written((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    LOGOS_ASSERT_EQ(written, cli({"--config", cfg, "--format", "openapi", "--lidl", in}).out);
    Run bad = cli({"--config", cfg, "--format", "openapi", "--lidl", in, "--out", s.path("no/such/dir/x.json")});
    expectCode(bad, 6);
    LOGOS_ASSERT_TRUE(has(bad.err, "cannot be written"));
}

LOGOS_TEST(cli_a_non_canonical_contract_renders_with_a_warning) {
    Scratch s;
    const std::string cfg = s.write("c.json", kOneModule);
    const std::string file = s.write("m1.lidl", "module m1 {\n  depends []\n  method greet(who: tstr) -> tstr\n}\n");
    Run r = cli({"--config", cfg, "--format", "schema", "--module", "m1", "--lidl", "m1=" + file});
    expectCode(r, 0);
    LOGOS_ASSERT_TRUE(has(r.err, "warning: m1: "));
    LOGOS_ASSERT_TRUE(has(r.err, "not in canonical form"));
    LOGOS_ASSERT_TRUE(json::parse(r.out)["cross_check"].is_null());
}
