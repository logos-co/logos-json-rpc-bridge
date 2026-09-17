// The served documents: views mapped onto the builders' input, and the publisher's
// coalesced, off-thread rebuilds of immutable snapshots.

#include <logos_test.h>

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "doc_publisher.h"

using namespace bridge;

namespace {

using nlohmann::json;

BridgeConfig config(const std::string& text) {
    ConfigParseResult r = parseBridgeConfig(text, "json_rpc_bridge");
    if (!r.ok) throw LogosTestFailure("bad test config: " + r.error);
    return r.config;
}

LiveReport live(const std::string& text) {
    LiveReport r;
    if (!parseLiveReport(json::parse(text), &r)) throw LogosTestFailure("bad live report");
    return r;
}

const char* kIface = R"([{"name":"greet","parameters":[{"name":"who"}]},)"
                     R"({"name":"reset","parameters":[]},{"name":"lidl","parameters":[]},)"
                     R"({"type":"event","name":"greeted","parameters":[{"name":"who"}]}])";

const char* kContract =
    "module m1 {\n  version \"1.0.0\"\n  depends []\n\n  method greet(who: tstr) -> tstr\n"
    "  method reset()\n\n  event greeted(who: tstr)\n}\n";

ModuleView typedView(const ExposedModule& em) {
    const ContractResult c = readContract(kContract, "m1");
    if (!c.ok) throw LogosTestFailure("fixture contract: " + c.error);
    ModuleView v = okView(pendingView("m1"), live(kIface), typedContract(c));
    v.contractSha256 = sha256Hex(kContract);
    return withExposure(em, std::move(v));
}

std::vector<std::shared_ptr<const ModuleView>> share(std::vector<ModuleView> views) {
    std::vector<std::shared_ptr<const ModuleView>> out;
    for (auto& v : views) out.push_back(std::make_shared<const ModuleView>(std::move(v)));
    return out;
}

// Timers and pump posts that run only when the test says so.
struct FakeRuntime {
    std::map<std::string, DocPublisher::Job> timers;
    std::vector<DocPublisher::Job> posted;
    int schedules = 0;
    bool accept = true;
    std::function<docs::DocContext()> inputs;

    DocPublisher::Io io() {
        DocPublisher::Io io;
        io.inputs = [this] { return inputs(); };
        io.schedule = [this](const std::string& key, std::chrono::milliseconds,
                             DocPublisher::Job job) {
            if (!accept) return false;
            ++schedules;
            timers[key] = std::move(job);
            return true;
        };
        io.post = [this](DocPublisher::Job job) {
            if (!accept) return false;
            posted.push_back(std::move(job));
            return true;
        };
        return io;
    }

    // Fires the timer, then runs what it posted.
    void run() {
        auto timers_ = std::move(timers);
        timers.clear();
        for (auto& kv : timers_) kv.second();
        auto jobs = std::move(posted);
        posted.clear();
        for (auto& j : jobs) j();
    }
};

} // namespace

// ── the adapter ─────────────────────────────────────────────────────────────

LOGOS_TEST(a_pending_view_maps_to_a_bare_pending_module) {
    const docs::ModuleDoc m = toModuleDoc(pendingView("m1"));
    LOGOS_ASSERT_EQ(m.name, std::string("m1"));
    LOGOS_ASSERT_EQ(m.status, std::string("pending"));
    LOGOS_ASSERT_TRUE(m.interface.is_null());
    LOGOS_ASSERT_TRUE(m.exposedMethods.empty() && m.liveMethods.empty());
    LOGOS_ASSERT_TRUE(m.interfaceSha256.empty() && m.contractSha256.empty());
}

LOGOS_TEST(an_ok_view_maps_to_its_interface_digests_and_exposure) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":[{"name":"m1","methods":{"deny":["reset"]}}]}})");
    const ModuleView v = typedView(cfg.modules[0]);
    const docs::ModuleDoc m = toModuleDoc(v);
    LOGOS_ASSERT_EQ(m.status, std::string("ok"));
    LOGOS_ASSERT_EQ(m.interface, v.contract->interface);
    LOGOS_ASSERT_EQ(m.interfaceSha256, v.interfaceSha256);
    LOGOS_ASSERT_EQ(m.interfaceSha256, sha256Hex(v.contract->interface.dump()));
    LOGOS_ASSERT_EQ(m.contractSha256, sha256Hex(kContract));
    const std::vector<std::string> methods = {"greet", "name", "version", "lidl"};
    LOGOS_ASSERT_TRUE(m.exposedMethods == methods);
    LOGOS_ASSERT_TRUE(m.exposedEvents == std::vector<std::string>{"greeted"});
    LOGOS_ASSERT_TRUE(m.liveMethods.empty());
    LOGOS_ASSERT_TRUE(docs::isTyped(m));
}

LOGOS_TEST(an_untyped_view_maps_to_live_names_with_their_params) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    const ModuleView v = withExposure(cfg.modules[0], untypedView(pendingView("m1"), live(kIface)));
    const docs::ModuleDoc m = toModuleDoc(v);
    LOGOS_ASSERT_EQ(m.status, std::string("untyped"));
    LOGOS_ASSERT_TRUE(m.interface.is_null());
    LOGOS_ASSERT_EQ(m.liveMethods.size(), static_cast<size_t>(3));
    LOGOS_ASSERT_EQ(m.liveMethods[0].name, std::string("greet"));
    LOGOS_ASSERT_TRUE(m.liveMethods[0].params == std::vector<std::string>{"who"});
    LOGOS_ASSERT_TRUE(m.exposedMethods == (std::vector<std::string>{"greet", "reset", "lidl"}));
    LOGOS_ASSERT_TRUE(m.exposedEvents == std::vector<std::string>{"greeted"});
    LOGOS_ASSERT_TRUE(docs::isNamesOnly(m));
}

LOGOS_TEST(an_invalid_view_maps_to_its_error_and_live_names) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    ModuleView v = invalidView(pendingView("m1"), live(kIface), "3:1: nope (lidl reader x)");
    v.contractSha256 = sha256Hex("garbage");
    const docs::ModuleDoc m = toModuleDoc(withExposure(cfg.modules[0], std::move(v)));
    LOGOS_ASSERT_EQ(m.status, std::string("invalid"));
    LOGOS_ASSERT_EQ(m.interfaceError, std::string("3:1: nope (lidl reader x)"));
    LOGOS_ASSERT_TRUE(m.interface.is_null());
    LOGOS_ASSERT_EQ(m.liveMethods.size(), static_cast<size_t>(3));
    docs::DocContext ctx;
    ctx.modules.push_back(m);
    const json modules = docs::xLogosModules(ctx);
    LOGOS_ASSERT_EQ(modules[0]["interface_error"], json("3:1: nope (lidl reader x)"));
    LOGOS_ASSERT_FALSE(modules[0].contains("contract_sha256"));   // documents give digests for ok only
}

// A stale view keeps serving what it knew; the documents do not change.
LOGOS_TEST(a_stale_view_maps_like_its_fresh_self) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    const ModuleView fresh = typedView(cfg.modules[0]);
    const ModuleView stale = staleView(fresh);
    LOGOS_ASSERT_TRUE(stale.stale);
    docs::DocContext a = docContext(cfg, "0.1.0", share({fresh}));
    docs::DocContext b = docContext(cfg, "0.1.0", share({stale}));
    LOGOS_ASSERT_EQ(docInputsKey(a), docInputsKey(b));
    LOGOS_ASSERT_EQ(docs::buildOpenRpc(a), docs::buildOpenRpc(b));
}

LOGOS_TEST(the_context_takes_endpoint_limits_version_and_config_order) {
    const BridgeConfig cfg = config(
        R"({"http":{"host":"::1","port":9001},"limits":{"max_frame_bytes":4096},)"
        R"("expose":{"modules":["zeta","alpha"]}})");
    const docs::DocContext ctx =
        docContext(cfg, "2.3.4", share({pendingView("zeta"), pendingView("alpha")}));
    LOGOS_ASSERT_EQ(ctx.bridgeVersion, std::string("2.3.4"));
    LOGOS_ASSERT_EQ(ctx.host, std::string("::1"));
    LOGOS_ASSERT_EQ(ctx.port, 9001);
    LOGOS_ASSERT_EQ(ctx.limits.maxFrameBytes, 4096);
    LOGOS_ASSERT_EQ(ctx.modules.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(ctx.modules[0].name, std::string("zeta"));
    const json rpc = docs::buildOpenRpc(ctx);
    LOGOS_ASSERT_EQ(rpc["servers"][0]["url"], json("http://[::1]:9001/rpc"));
    LOGOS_ASSERT_EQ(rpc["info"]["version"], json("2.3.4"));
}

LOGOS_TEST(the_inputs_key_follows_what_documents_show) {
    const BridgeConfig open = config(R"({"expose":{"modules":["m1"]}})");
    const BridgeConfig denied = config(R"({"expose":{"modules":[{"name":"m1","methods":{"deny":["greet"]}}]}})");
    const std::string base = docInputsKey(docContext(open, "0.1.0", share({typedView(open.modules[0])})));
    LOGOS_ASSERT_NE(base, docInputsKey(docContext(denied, "0.1.0", share({typedView(denied.modules[0])}))));
    LOGOS_ASSERT_NE(base, docInputsKey(docContext(open, "0.1.1", share({typedView(open.modules[0])}))));
    LOGOS_ASSERT_NE(base, docInputsKey(docContext(open, "0.1.0", share({pendingView("m1")}))));
    ModuleView otherBytes = typedView(open.modules[0]);
    otherBytes.contractSha256 = sha256Hex("different bytes");
    LOGOS_ASSERT_NE(base, docInputsKey(docContext(open, "0.1.0", share({otherBytes}))));
}

// ── snapshots ───────────────────────────────────────────────────────────────

LOGOS_TEST(a_snapshot_holds_the_three_documents_the_builders_make) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    const docs::DocContext ctx = docContext(cfg, "0.1.0", share({typedView(cfg.modules[0])}));
    const auto set = buildDocSet(ctx, docInputsKey(ctx), 7);
    LOGOS_ASSERT_EQ(set->generation, static_cast<std::uint64_t>(7));
    LOGOS_ASSERT_EQ(set->openrpc, docs::buildOpenRpc(ctx));
    LOGOS_ASSERT_EQ(set->openapi, docs::buildOpenApi(ctx).dump());
    LOGOS_ASSERT_EQ(set->asyncapi, docs::buildAsyncApi(ctx).dump());
    LOGOS_ASSERT_EQ(set->openrpcBytes, set->openrpc.dump().size());
    const json info = set->info();
    LOGOS_ASSERT_EQ(info["generation"], json(7));
    LOGOS_ASSERT_EQ(info["sizes"]["openapi"], json(set->openapi.size()));
    LOGOS_ASSERT_EQ(info["sizes"]["asyncapi"], json(set->asyncapi.size()));
    LOGOS_ASSERT_TRUE(info["built_at"].get<std::int64_t>() > 0);
    LOGOS_ASSERT_TRUE(info["build_ms"].is_number());
}

LOGOS_TEST(the_first_build_is_synchronous_and_generation_one) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeRuntime rt;
    rt.inputs = [&] { return docContext(cfg, "0.1.0", share({pendingView("m1")})); };
    DocPublisher pub(rt.io());
    LOGOS_ASSERT_TRUE(pub.current() == nullptr);
    LOGOS_ASSERT_TRUE(pub.buildNow());
    LOGOS_ASSERT_EQ(pub.current()->generation, static_cast<std::uint64_t>(1));
    LOGOS_ASSERT_EQ(rt.schedules, 0);
    const json rpc = pub.current()->openrpc;
    LOGOS_ASSERT_EQ(rpc["x-logos-modules"][0]["status"], json("pending"));
    for (const auto& m : rpc["methods"])
        LOGOS_ASSERT_TRUE(m.contains("x-logos-bridge-op"));   // no module operations yet
}

LOGOS_TEST(a_changed_view_is_rebuilt_on_the_pump) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    auto views = share({pendingView("m1")});
    FakeRuntime rt;
    rt.inputs = [&] { return docContext(cfg, "0.1.0", views); };
    DocPublisher pub(rt.io());
    pub.buildNow();
    views = share({typedView(cfg.modules[0])});
    pub.request();
    LOGOS_ASSERT_EQ(pub.current()->generation, static_cast<std::uint64_t>(1));   // nothing ran yet
    LOGOS_ASSERT_EQ(rt.timers.count(DocPublisher::kTimer), static_cast<size_t>(1));
    rt.run();
    const auto now = pub.current();
    LOGOS_ASSERT_EQ(now->generation, static_cast<std::uint64_t>(2));
    LOGOS_ASSERT_EQ(json::parse(now->openapi)["x-logos-modules"][0]["status"], json("ok"));
    LOGOS_ASSERT_TRUE(json::parse(now->openapi)["paths"].contains("/modules/m1/greet"));
}

LOGOS_TEST(requests_coalesce_into_one_build) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    auto views = share({pendingView("m1")});
    int reads = 0;
    FakeRuntime rt;
    rt.inputs = [&] { ++reads; return docContext(cfg, "0.1.0", views); };
    DocPublisher pub(rt.io());
    pub.buildNow();
    views = share({typedView(cfg.modules[0])});
    for (int i = 0; i < 10; ++i) pub.request();
    LOGOS_ASSERT_EQ(rt.schedules, 1);
    rt.run();
    LOGOS_ASSERT_EQ(reads, 2);
    LOGOS_ASSERT_EQ(pub.current()->generation, static_cast<std::uint64_t>(2));
    pub.request();   // after the build, a new change is a new build
    LOGOS_ASSERT_EQ(rt.schedules, 2);
}

// A request that arrives while a build runs is not lost.
LOGOS_TEST(a_change_during_a_build_gets_its_own_build) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    auto views = share({pendingView("m1")});
    FakeRuntime rt;
    DocPublisher* self = nullptr;
    bool changeMidBuild = false;
    rt.inputs = [&] {
        auto seen = views;
        if (changeMidBuild) {
            changeMidBuild = false;
            views = share({typedView(cfg.modules[0])});
            self->request();
        }
        return docContext(cfg, "0.1.0", seen);
    };
    DocPublisher pub(rt.io());
    self = &pub;
    pub.buildNow();
    views = share({untypedView(pendingView("m1"), live(kIface))});
    changeMidBuild = true;
    pub.request();
    rt.run();
    LOGOS_ASSERT_EQ(json::parse(pub.current()->asyncapi)["x-logos-modules"][0]["status"], json("untyped"));
    LOGOS_ASSERT_EQ(rt.timers.size(), static_cast<size_t>(1));
    rt.run();
    LOGOS_ASSERT_EQ(json::parse(pub.current()->asyncapi)["x-logos-modules"][0]["status"], json("ok"));
    LOGOS_ASSERT_EQ(pub.current()->generation, static_cast<std::uint64_t>(3));
}

LOGOS_TEST(an_unchanged_context_keeps_the_snapshot) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    auto views = share({typedView(cfg.modules[0])});
    FakeRuntime rt;
    rt.inputs = [&] { return docContext(cfg, "0.1.0", views); };
    DocPublisher pub(rt.io());
    pub.buildNow();
    const auto first = pub.current();
    views = share({staleView(*views[0])});
    pub.request();
    rt.run();
    LOGOS_ASSERT_TRUE(pub.current() == first);
}

// A reader keeps its snapshot, whole and unchanged, across any number of rebuilds.
LOGOS_TEST(a_held_snapshot_never_changes) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    auto views = share({pendingView("m1")});
    FakeRuntime rt;
    rt.inputs = [&] { return docContext(cfg, "0.1.0", views); };
    DocPublisher pub(rt.io());
    pub.buildNow();
    const auto held = pub.current();
    const std::string openapi = held->openapi;
    const json openrpc = held->openrpc;
    views = share({typedView(cfg.modules[0])});
    pub.request();
    rt.run();
    views = share({pendingView("m1")});
    pub.request();
    rt.run();
    LOGOS_ASSERT_EQ(pub.current()->generation, static_cast<std::uint64_t>(3));
    LOGOS_ASSERT_TRUE(pub.current() != held);
    LOGOS_ASSERT_EQ(held->generation, static_cast<std::uint64_t>(1));
    LOGOS_ASSERT_EQ(held->openapi, openapi);
    LOGOS_ASSERT_EQ(held->openrpc, openrpc);
}

// A stopping runtime refuses the job; the next request must still be able to schedule.
LOGOS_TEST(a_refused_schedule_or_post_does_not_wedge_the_publisher) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    FakeRuntime rt;
    rt.inputs = [&] { return docContext(cfg, "0.1.0", share({pendingView("m1")})); };
    DocPublisher pub(rt.io());
    pub.buildNow();
    rt.accept = false;
    pub.request();
    LOGOS_ASSERT_EQ(rt.schedules, 0);
    rt.accept = true;
    pub.request();
    LOGOS_ASSERT_EQ(rt.schedules, 1);
    rt.accept = false;   // the timer fires, but the pump refuses the build
    rt.run();
    rt.accept = true;
    pub.request();
    LOGOS_ASSERT_EQ(rt.schedules, 2);
}

// A build that throws keeps the previous snapshot served and is counted.
LOGOS_TEST(a_failed_build_keeps_the_previous_snapshot) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    bool fail = false;
    FakeRuntime rt;
    rt.inputs = [&]() -> docs::DocContext {
        if (fail) throw std::runtime_error("boom");
        return docContext(cfg, "0.1.0", share({pendingView("m1")}));
    };
    DocPublisher pub(rt.io());
    pub.buildNow();
    const auto first = pub.current();
    fail = true;
    pub.request();
    rt.run();
    LOGOS_ASSERT_TRUE(pub.current() == first);
    LOGOS_ASSERT_EQ(pub.failures(), static_cast<std::uint64_t>(1));
    LOGOS_ASSERT_EQ(pub.info()["build_failures"], json(1));
    LOGOS_ASSERT_EQ(pub.info()["generation"], json(1));
}

LOGOS_TEST(a_first_build_that_fails_reports_it) {
    FakeRuntime rt;
    rt.inputs = []() -> docs::DocContext { throw std::runtime_error("boom"); };
    DocPublisher pub(rt.io());
    LOGOS_ASSERT_FALSE(pub.buildNow());
    LOGOS_ASSERT_TRUE(pub.current() == nullptr);
}
