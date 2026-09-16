// exposure: what a client may call or subscribe to, from the contract (typed)
// or from live names (untyped, invalid). Policy restricts calls, not knowledge.

#include <logos_test.h>

#include <string>
#include <vector>

#include "discovery_model.h"
#include "exposure.h"

using namespace bridge;

namespace {

BridgeConfig config(const std::string& json) {
    ConfigParseResult r = parseBridgeConfig(json, "json_rpc_bridge");
    if (!r.ok) throw LogosTestFailure("bad test config: " + r.error);
    return r.config;
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : ",") + n;
    return out;
}

const std::vector<std::string> kDeclared = {"upload", "wipe", "status", "name", "version", "lidl"};
const std::vector<std::string> kEvents = {"progress", "done"};

} // namespace

LOGOS_TEST(an_open_module_exposes_every_declared_member_in_order) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    const Exposure x = exposeDeclared(*cfg.find("m1"), kDeclared, kEvents);
    LOGOS_ASSERT_EQ(joined(x.methods), std::string("upload,wipe,status,name,version,lidl"));
    LOGOS_ASSERT_EQ(joined(x.events), std::string("progress,done"));
}

LOGOS_TEST(deny_removes_declared_members) {
    const BridgeConfig cfg = config(
        R"({"expose":{"modules":[{"name":"m1","methods":{"deny":["wipe"]},)"
        R"("events":{"deny":["progress"]}}]}})");
    const Exposure x = exposeDeclared(*cfg.find("m1"), kDeclared, kEvents);
    LOGOS_ASSERT_EQ(joined(x.methods), std::string("upload,status,name,version,lidl"));
    LOGOS_ASSERT_EQ(joined(x.events), std::string("done"));
}

// An allow list keeps declaration order and never needs to name the built-ins.
LOGOS_TEST(allow_keeps_only_listed_members_plus_the_built_ins) {
    const BridgeConfig cfg = config(
        R"({"expose":{"modules":[{"name":"m1","methods":{"allow":["status","upload","ghost"]},)"
        R"("events":{"allow":["done"]}}]}})");
    const Exposure x = exposeDeclared(*cfg.find("m1"), kDeclared, kEvents);
    LOGOS_ASSERT_EQ(joined(x.methods), std::string("upload,status,name,version,lidl"));
    LOGOS_ASSERT_EQ(joined(x.events), std::string("done"));
}

LOGOS_TEST(an_empty_allow_list_still_exposes_the_built_ins) {
    const BridgeConfig cfg = config(
        R"({"expose":{"modules":[{"name":"m1","methods":{"allow":[]},"events":{"allow":[]}}]}})");
    const Exposure x = exposeDeclared(*cfg.find("m1"), kDeclared, kEvents);
    LOGOS_ASSERT_EQ(joined(x.methods), std::string("name,version,lidl"));
    LOGOS_ASSERT_TRUE(x.events.empty());
}

LOGOS_TEST(reserved_introspection_is_never_exposed) {
    const BridgeConfig cfg = config(R"({"expose":{"modules":["m1"]}})");
    const Exposure x = exposeLive(*cfg.find("m1"),
        {"getPluginInterface", "get", "getPluginMethods", "getPluginEvents"}, {}, true);
    LOGOS_ASSERT_EQ(joined(x.methods), std::string("get"));
}

// Untyped: live order, one entry per name even when Qt lists clones.
LOGOS_TEST(an_untyped_module_is_exposed_from_live_names) {
    const BridgeConfig cfg =
        config(R"({"expose":{"modules":[{"name":"m1","methods":{"allow":["get","set"]}}]}})");
    const Exposure x = exposeLive(*cfg.find("m1"),
        {"set", "get", "set", "debugDump", "version"}, {"changed"}, true);
    LOGOS_ASSERT_EQ(joined(x.methods), std::string("set,get,version"));
    LOGOS_ASSERT_EQ(joined(x.events), std::string("changed"));
}

// Untagged live events cannot be listed; an explicit allow list is what dispatch accepts.
LOGOS_TEST(untagged_events_are_exposed_through_an_explicit_allow_list_only) {
    const BridgeConfig cfg = config(
        R"({"expose":{"modules":["open",{"name":"listed","events":{"allow":["b","a"],"deny":["b"]}}]}})");
    LOGOS_ASSERT_TRUE(exposeLive(*cfg.find("open"), {"get"}, {}, false).events.empty());
    LOGOS_ASSERT_EQ(joined(exposeLive(*cfg.find("listed"), {"get"}, {}, false).events),
                    std::string("a"));
    LOGOS_ASSERT_TRUE(exposeLive(*cfg.find("listed"), {"get"}, {}, true).events.empty());
}

LOGOS_TEST(exposure_serialises_as_two_lists) {
    Exposure x;
    x.methods = {"a"};
    LOGOS_ASSERT_EQ(x.toJson().dump(), std::string(R"({"events":[],"methods":["a"]})"));
}

// ── exposure of a module view ───────────────────────────────────────────────

namespace {

LiveReport live(const std::string& json) {
    LiveReport r;
    parseLiveReport(nlohmann::json::parse(json), &r);
    return r;
}

std::shared_ptr<const TypedContract> contract() {
    ContractResult c;
    c.ok = true;
    c.methods = {{"upload", false, {}}, {"wipe", false, {}},
                 {"name", true, {}}, {"version", true, {}}, {"lidl", true, {}}};
    c.events = {{"progress", {}}};
    c.astJson = R"({"name":"m1"})";
    return typedContract(c);
}

const char* kLive = R"([{"name":"upload"},{"name":"wipe"},{"name":"extra"},{"name":"lidl"},)"
                    R"({"type":"event","name":"progress"},{"type":"event","name":"liveOnly"}])";

} // namespace

LOGOS_TEST(a_view_is_exposed_by_its_status) {
    const BridgeConfig cfg =
        config(R"({"expose":{"modules":[{"name":"m1","methods":{"deny":["wipe"]}}]}})");
    const ExposedModule& em = *cfg.find("m1");
    LOGOS_ASSERT_TRUE(exposureOf(em, pendingView("m1")).methods.empty());
    const ModuleView ok = okView(pendingView("m1"), live(kLive), contract());
    LOGOS_ASSERT_EQ(joined(exposureOf(em, ok).methods), std::string("upload,name,version,lidl"));
    LOGOS_ASSERT_EQ(joined(exposureOf(em, ok).events), std::string("progress"));
    const ModuleView bad = invalidView(pendingView("m1"), live(kLive), "why");
    LOGOS_ASSERT_EQ(joined(exposureOf(em, bad).methods), std::string("upload,extra,lidl"));
    LOGOS_ASSERT_EQ(joined(exposureOf(em, bad).events), std::string("progress,liveOnly"));
    LOGOS_ASSERT_EQ(joined(withExposure(em, ok).exposure.methods),
                    std::string("upload,name,version,lidl"));
}

// Exposure and gating are two readings of one rule; they must agree.
LOGOS_TEST(everything_exposed_is_callable_and_nothing_else_declared_is) {
    const BridgeConfig cfg = config(
        R"({"expose":{"modules":[{"name":"m1","methods":{"allow":["upload"]},)"
        R"("events":{"deny":["progress"]}}]}})");
    const ModuleView views[] = {
        withExposure(*cfg.find("m1"), okView(pendingView("m1"), live(kLive), contract())),
        withExposure(*cfg.find("m1"), invalidView(pendingView("m1"), live(kLive), "why")),
        withExposure(*cfg.find("m1"), untypedView(pendingView("m1"), live(kLive))),
    };
    for (const ModuleView& v : views) {
        for (const auto& m : v.exposure.methods) LOGOS_ASSERT_TRUE(methodPermitted(cfg, v, m));
        for (const auto& e : v.exposure.events) LOGOS_ASSERT_TRUE(eventPermitted(cfg, v, e));
        for (const char* m : {"upload", "wipe", "extra", "name", "version", "lidl"}) {
            const bool exposed = std::find(v.exposure.methods.begin(), v.exposure.methods.end(),
                                           m) != v.exposure.methods.end();
            LOGOS_ASSERT_EQ(exposed, methodPermitted(cfg, v, m));
        }
    }
}
