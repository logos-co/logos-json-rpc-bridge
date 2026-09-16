#pragma once

// What the bridge knows about each exposed module, and every decision that
// follows from it. Pure: no IO, no threads, no locks. discovery.h does the IO.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge_config.h"
#include "exposure.h"
#include "lidl_contract.h"
#include "sha256.h"

namespace bridge {

enum class InterfaceStatus { Pending, Ok, Untyped, Invalid };

inline const char* interfaceStatusName(InterfaceStatus s) {
    switch (s) {
        case InterfaceStatus::Pending: return "pending";
        case InterfaceStatus::Ok:      return "ok";
        case InterfaceStatus::Untyped: return "untyped";
        case InterfaceStatus::Invalid: return "invalid";
    }
    return "pending";
}

// ---------------------------------------------------------------------------
// Live report: the module's own getPluginInterface answer, names only
// ---------------------------------------------------------------------------

struct LiveMember {
    std::string name;
    std::vector<std::string> params;   // "" where the host gave no name
};

struct LiveReport {
    // False when no entry was tagged "type":"event". Legacy Qt plugins never
    // tag events, so this means "cannot know", not "has none".
    bool eventsDeclared = false;
    std::vector<LiveMember> methods;   // host order; Qt default-argument clones repeat a name
    std::vector<LiveMember> events;

    const LiveMember* method(const std::string& name) const {
        for (const auto& m : methods)
            if (m.name == name) return &m;
        return nullptr;
    }
    bool hasMethod(const std::string& name) const { return method(name) != nullptr; }
    bool hasEvent(const std::string& name) const {
        for (const auto& e : events)
            if (e.name == name) return true;
        return false;
    }
};

// False when `iface` is not an array. Entries without a string name are skipped.
inline bool parseLiveReport(const nlohmann::json& iface, LiveReport* out) {
    if (!iface.is_array()) return false;
    LiveReport r;
    for (const auto& entry : iface) {
        if (!entry.is_object()) continue;
        const auto name = entry.find("name");
        if (name == entry.end() || !name->is_string()) continue;
        LiveMember m;
        m.name = name->get<std::string>();
        const auto params = entry.find("parameters");
        if (params != entry.end() && params->is_array()) {
            for (const auto& p : *params) {
                const bool named = p.is_object() && p.contains("name") && p["name"].is_string();
                m.params.push_back(named ? p["name"].get<std::string>() : std::string());
            }
        }
        const auto type = entry.find("type");
        if (type != entry.end() && type->is_string() && type->get<std::string>() == "event") {
            r.eventsDeclared = true;
            r.events.push_back(std::move(m));
        } else {
            r.methods.push_back(std::move(m));
        }
    }
    *out = std::move(r);
    return true;
}

// A zero-parameter live method named "lidl". The Qt glue lists it as QString
// and cpp-sdk as tstr, so the return spelling is ignored.
inline bool lidlPresent(const LiveReport& live) {
    for (const auto& m : live.methods)
        if (m.name == "lidl" && m.params.empty()) return true;
    return false;
}

// ---------------------------------------------------------------------------
// The lidl() answer
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxContractBytes = 4 * 1024 * 1024;

// Well-formed UTF-8: no overlongs, surrogates, or code points past U+10FFFF.
inline bool isValidUtf8(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* end = p + s.size();
    while (p < end) {
        const unsigned char c = *p;
        if (c < 0x80) { ++p; continue; }
        std::size_t n = 0;
        unsigned char lo = 0x80, hi = 0xBF;   // bounds for the first continuation byte
        if (c >= 0xC2 && c <= 0xDF) { n = 1; }
        else if (c == 0xE0) { n = 2; lo = 0xA0; }
        else if (c == 0xED) { n = 2; hi = 0x9F; }
        else if (c >= 0xE1 && c <= 0xEF) { n = 2; }
        else if (c == 0xF0) { n = 3; lo = 0x90; }
        else if (c == 0xF4) { n = 3; hi = 0x8F; }
        else if (c >= 0xF1 && c <= 0xF3) { n = 3; }
        else return false;
        if (static_cast<std::size_t>(end - p) <= n) return false;
        if (p[1] < lo || p[1] > hi) return false;
        for (std::size_t i = 2; i <= n; ++i)
            if (p[i] < 0x80 || p[i] > 0xBF) return false;
        p += n + 1;
    }
    return true;
}

// A lidl() answer before parsing: the text, or a constant reason. Upstream
// error detail never reaches a view.
struct LidlText {
    bool ok = false;
    bool retryable = false;   // the call itself failed; the next refresh may do better
    std::string text;
    std::string error;
};

inline LidlText lidlText(bool callOk, const nlohmann::json& value) {
    LidlText t;
    if (!callOk) {
        t.retryable = true;
        t.error = "lidl() call failed";
    } else if (!value.is_string()) {
        t.error = "lidl() did not return a string";
    } else if (value.get_ref<const std::string&>().size() > kMaxContractBytes) {
        t.error = "lidl() returned more than 4 MiB";
    } else if (!isValidUtf8(value.get_ref<const std::string&>())) {
        t.error = "lidl() returned invalid UTF-8";
    } else {
        t.ok = true;
        t.text = value.get<std::string>();
    }
    return t;
}

// "line:col: message (lidl reader <rev>)". Parser messages quote only the
// contract, which is public.
inline std::string contractErrorText(const ContractResult& c, const std::string& readerRev) {
    std::string where;
    if (c.errorLine > 0)
        where = std::to_string(c.errorLine) + ":" + std::to_string(c.errorCol) + ": ";
    return where + c.error + " (lidl reader " + readerRev + ")";
}

// ---------------------------------------------------------------------------
// Digests
// ---------------------------------------------------------------------------

// nlohmann's default dump: sorted keys, no spaces, raw UTF-8. Equals Python's
// json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=False).
inline std::string canonicalJson(const nlohmann::json& doc) {
    return doc.dump();
}

// interface_sha256: lowercase hex SHA-256 of the canonical JSON.
inline std::string interfaceDigest(const nlohmann::json& doc) {
    return sha256Hex(canonicalJson(doc));
}

// A contract the bridge serves and dispatches by. Immutable once built.
struct TypedContract {
    std::string version;
    std::vector<ContractMethod> methods;
    std::vector<ContractEvent> events;
    std::vector<std::string> warnings;   // reader codes, e.g. "non_canonical"
    std::vector<std::string> lint;       // validator warnings
    nlohmann::json interface;            // the full AST, never filtered by policy
    std::string interfaceSha256;         // independent of policy

    const ContractMethod* method(const std::string& name) const {
        for (const auto& m : methods)
            if (m.name == name) return &m;
        return nullptr;
    }
    bool declaresEvent(const std::string& name) const {
        for (const auto& e : events)
            if (e.name == name) return true;
        return false;
    }
};

// Null when the reader's JSON does not parse back or cannot be digested.
inline std::shared_ptr<const TypedContract> typedContract(const ContractResult& c) {
    auto t = std::make_shared<TypedContract>();
    t->interface = nlohmann::json::parse(c.astJson, nullptr, /*allow_exceptions=*/false);
    if (t->interface.is_discarded() || !t->interface.is_object()) return nullptr;
    try {
        t->interfaceSha256 = interfaceDigest(t->interface);
    } catch (const std::exception&) {
        return nullptr;
    }
    t->version = c.contractVersion;
    t->methods = c.methods;
    t->events = c.events;
    t->warnings = c.warnings;
    t->lint = c.lint;
    return t;
}

// ---------------------------------------------------------------------------
// Cross-check: the contract against the live report, names and arity only
// ---------------------------------------------------------------------------

// Live type spellings are host names (QString, ...), so types are not compared.
struct Finding {
    std::string severity;   // error | warning | info
    std::string code;       // stable, see crossCheck()
    std::string member;     // empty when not about one member
    std::string detail;

    nlohmann::json toJson() const {
        nlohmann::json j{{"severity", severity}, {"code", code}, {"detail", detail}};
        if (!member.empty()) j["member"] = member;
        return j;
    }
};

struct CrossCheck {
    std::vector<Finding> findings;

    bool has(const char* severity) const {
        for (const auto& f : findings)
            if (f.severity == severity) return true;
        return false;
    }
    bool hasErrors() const { return has("error"); }
    const char* state() const {
        return hasErrors() ? "inconsistent" : has("warning") ? "warnings" : "consistent";
    }
    nlohmann::json toJson() const {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& f : findings) list.push_back(f.toJson());
        return nlohmann::json{{"state", state()}, {"findings", std::move(list)}};
    }
};

// What `version()` answered; unknown when the call failed or was not a string.
struct RuntimeVersion {
    bool known = false;
    std::string value;
};

inline RuntimeVersion runtimeVersion(bool callOk, const nlohmann::json& value) {
    RuntimeVersion v;
    if (callOk && value.is_string()) {
        v.known = true;
        v.value = value.get<std::string>();
    }
    return v;
}

namespace detail {

inline std::string namesOf(const std::vector<std::string>& names) {
    std::string out = "(";
    for (std::size_t i = 0; i < names.size(); ++i) out += (i ? ", " : "") + names[i];
    return out + ")";
}

inline std::vector<std::string> paramNames(const std::vector<ContractParam>& params) {
    std::vector<std::string> out;
    for (const auto& p : params) out.push_back(p.name);
    return out;
}

// Declared member vs every live entry of that name (Qt clones repeat a name).
// Arity must match one entry; names are compared where the host gave them.
inline void checkMember(const char* kind, const std::string& name,
                        const std::vector<ContractParam>& declared,
                        const std::vector<const LiveMember*>& live, CrossCheck* out) {
    const LiveMember* sameArity = nullptr;
    std::string arities;
    for (const LiveMember* m : live) {
        if (m->params.size() == declared.size() && !sameArity) sameArity = m;
        arities += (arities.empty() ? "" : "/") + std::to_string(m->params.size());
    }
    const std::vector<std::string> names = paramNames(declared);
    if (!sameArity) {
        out->findings.push_back({"error", std::string(kind) + "_arity_mismatch", name,
            "declared " + std::to_string(declared.size()) + " parameter(s), live " + arities});
        return;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (sameArity->params[i].empty() || sameArity->params[i] == names[i]) continue;
        out->findings.push_back({"warning", "param_names_differ", name,
            "declared " + namesOf(names) + ", live " + namesOf(sameArity->params)});
        return;
    }
}

inline std::vector<const LiveMember*> liveEntries(const std::vector<LiveMember>& live,
                                                  const std::string& name) {
    std::vector<const LiveMember*> out;
    for (const auto& m : live)
        if (m.name == name) out.push_back(&m);
    return out;
}

} // namespace detail

// Errors (*_missing, *_arity_mismatch) make the module invalid; warnings and
// info keep it ok. The codes are part of the served view: keep them stable.
inline CrossCheck crossCheck(const TypedContract& c, const LiveReport& live,
                             const RuntimeVersion& runtime) {
    CrossCheck out;
    for (const auto& m : c.methods) {
        const auto entries = detail::liveEntries(live.methods, m.name);
        if (entries.empty()) {
            // ModuleProxy and the Qt glue answer name/version without always listing them;
            // lidl is listed, or discovery would not have asked for it.
            if (m.derived)
                out.findings.push_back({"info", "derived_method_unlisted", m.name,
                                        "answered by the host, not listed live"});
            else
                out.findings.push_back({"error", "declared_method_missing", m.name,
                                        "declared, but not in the live interface"});
            continue;
        }
        detail::checkMember("method", m.name, m.params, entries, &out);
    }
    std::vector<std::string> undeclared;
    for (const auto& m : live.methods) {
        if (c.method(m.name) || isReservedIntrospection(m.name)) continue;
        if (std::find(undeclared.begin(), undeclared.end(), m.name) != undeclared.end()) continue;
        undeclared.push_back(m.name);
        out.findings.push_back({"warning", "undeclared_method", m.name,
                                "live, but not declared"});
    }

    if (live.eventsDeclared) {
        for (const auto& e : c.events) {
            const auto entries = detail::liveEntries(live.events, e.name);
            if (entries.empty())
                out.findings.push_back({"error", "declared_event_missing", e.name,
                                        "declared, but not in the live interface"});
            else
                detail::checkMember("event", e.name, e.params, entries, &out);
        }
        for (const auto& e : live.events) {
            if (c.declaresEvent(e.name)) continue;
            out.findings.push_back({"warning", "undeclared_event", e.name,
                                    "live, but not declared"});
        }
    } else if (!c.events.empty()) {
        out.findings.push_back({"info", "events_untagged", "",
                                "the live interface tags no events, so they were not checked"});
    }

    for (const auto& w : c.warnings) {
        if (w == "non_canonical")
            out.findings.push_back({"warning", "non_canonical", "",
                                    "lidl() is not in the canonical form this reader writes"});
        else
            out.findings.push_back({"warning", w, "", ""});
    }
    for (const auto& l : c.lint)
        out.findings.push_back({"warning", "contract_lint", "", l});

    if (!runtime.known)
        out.findings.push_back({"info", "runtime_version_unavailable", "version",
                                "version() did not answer with a string"});
    else if (!c.version.empty() && runtime.value != c.version)
        out.findings.push_back({"warning", "version_mismatch", "version",
                                "contract " + c.version + ", runtime " + runtime.value});
    return out;
}

// ---------------------------------------------------------------------------
// Module view and its status machine
// ---------------------------------------------------------------------------

// One immutable snapshot per module; discovery swaps whole snapshots.
struct ModuleView {
    std::string module;
    InterfaceStatus status = InterfaceStatus::Pending;
    LiveReport live;                 // empty while pending
    std::uint64_t generation = 0;    // completed discoveries of this module
    bool stale = false;              // the provider went away since; a refresh is due
    std::string interfaceError;      // invalid only
    std::shared_ptr<const TypedContract> contract;   // ok only
    std::string contractSha256;      // SHA-256 of the exact lidl() bytes, when they were read
    std::string interfaceSha256;     // of the served interface; ok only
    bool crossChecked = false;
    CrossCheck crossCheck;           // kept on an inconsistent (invalid) view too
    Exposure exposure;               // what this bridge lets clients use

    bool resolved() const { return status != InterfaceStatus::Pending; }
    bool typed() const { return status == InterfaceStatus::Ok && contract != nullptr; }
};

inline ModuleView pendingView(const std::string& module) {
    ModuleView v;
    v.module = module;
    return v;
}

// Every completed discovery goes through here: a fresh snapshot, never stale.
inline ModuleView nextView(const ModuleView& prev, InterfaceStatus status, LiveReport live) {
    ModuleView v;
    v.module = prev.module;
    v.status = status;
    v.live = std::move(live);
    v.generation = prev.generation + 1;
    return v;
}

// pending|ok|untyped|invalid -> untyped: a live report and nothing more.
inline ModuleView untypedView(const ModuleView& prev, LiveReport live) {
    return nextView(prev, InterfaceStatus::Untyped, std::move(live));
}

// -> invalid: a contract that cannot be served. Gating falls back to live names.
inline ModuleView invalidView(const ModuleView& prev, LiveReport live, std::string error) {
    ModuleView v = nextView(prev, InterfaceStatus::Invalid, std::move(live));
    v.interfaceError = std::move(error);
    return v;
}

// -> ok: the contract is served and dispatch follows its declarations.
inline ModuleView okView(const ModuleView& prev, LiveReport live,
                         std::shared_ptr<const TypedContract> contract) {
    ModuleView v = nextView(prev, InterfaceStatus::Ok, std::move(live));
    v.interfaceSha256 = contract->interfaceSha256;
    v.contract = std::move(contract);
    return v;
}

constexpr const char* kInconsistentContract =
    "the contract does not match the running module (see cross_check)";

// A read contract after the cross-check: ok, or invalid when it found errors.
inline ModuleView checkedView(const ModuleView& prev, LiveReport live,
                              std::shared_ptr<const TypedContract> contract,
                              const RuntimeVersion& runtime, const std::string& contractSha256) {
    CrossCheck cc = crossCheck(*contract, live, runtime);
    ModuleView v = cc.hasErrors()
        ? invalidView(prev, std::move(live), kInconsistentContract)
        : okView(prev, std::move(live), std::move(contract));
    v.crossChecked = true;
    v.crossCheck = std::move(cc);
    v.contractSha256 = contractSha256;
    return v;
}

// A lost provider keeps its last view for gating (the upstream call answers
// authoritatively anyway) until a refresh replaces it. Pending stays pending.
inline ModuleView staleView(const ModuleView& prev) {
    ModuleView v = prev;
    v.stale = prev.resolved();
    return v;
}

// Exposure follows the declarations when typed and the live names otherwise.
inline Exposure exposureOf(const ExposedModule& em, const ModuleView& view) {
    if (view.typed()) {
        std::vector<std::string> methods, events;
        for (const auto& m : view.contract->methods) methods.push_back(m.name);
        for (const auto& e : view.contract->events) events.push_back(e.name);
        return exposeDeclared(em, methods, events);
    }
    if (!view.resolved()) return Exposure{};
    std::vector<std::string> methods, events;
    for (const auto& m : view.live.methods) methods.push_back(m.name);
    for (const auto& e : view.live.events) events.push_back(e.name);
    return exposeLive(em, methods, events, view.live.eventsDeclared);
}

inline ModuleView withExposure(const ExposedModule& em, ModuleView view) {
    view.exposure = exposureOf(em, view);
    return view;
}

// ---------------------------------------------------------------------------
// Retry backoff
// ---------------------------------------------------------------------------

constexpr std::chrono::milliseconds kRetryBase{500};
constexpr std::chrono::milliseconds kRetryCap{30000};

// Delay before the next attempt after `failures` consecutive misses: 0.5 s doubling to 30 s.
inline std::chrono::milliseconds retryDelay(int failures) {
    std::chrono::milliseconds d = kRetryBase;
    for (int i = 1; i < failures && d < kRetryCap; ++i) d *= 2;
    return std::min(d, kRetryCap);
}

// ---------------------------------------------------------------------------
// Gating
// ---------------------------------------------------------------------------

// Is (module, method) callable by an external client? One boolean on purpose:
// not exposed, denied and unknown must be indistinguishable from outside.
inline bool methodPermitted(const BridgeConfig& cfg, const ModuleView& view,
                            const std::string& method) {
    const ExposedModule* em = cfg.find(view.module);
    if (!em || isReservedIntrospection(method)) return false;
    if (!em->methodAllowed(method)) return false;   // built-ins bypass policy, not existence
    if (view.typed()) return view.contract->method(method) != nullptr;
    // Unresolved is not a refusal: "starting" must not look like "forbidden",
    // and the upstream call answers authoritatively.
    if (!view.resolved() || view.live.methods.empty()) return true;
    return view.live.hasMethod(method);
}

// Stricter than methods: subscribing to an undeclared event is a SILENT no-op
// upstream, so this is the only place a client's typo becomes an error.
inline bool eventPermitted(const BridgeConfig& cfg, const ModuleView& view,
                           const std::string& event) {
    const ExposedModule* em = cfg.find(view.module);
    if (!em || !em->events.permits(event)) return false;
    if (view.typed()) return view.contract->declaresEvent(event);
    // Undeclared (a legacy Qt plugin) or unresolved: the operator's allow list
    // is the override that case exists for.
    if (!view.resolved() || !view.live.eventsDeclared) return true;
    return view.live.hasEvent(event);
}

// Typed: declaration order; an unknown name or a missing required param fails,
// a missing optional one is null.
inline bool typedPositional(const ContractMethod& m, const nlohmann::json& byName,
                            nlohmann::json* out, std::string* badPath) {
    for (auto kv = byName.begin(); kv != byName.end(); ++kv) {
        const bool known = std::any_of(m.params.begin(), m.params.end(),
            [&](const ContractParam& p) { return p.name == kv.key(); });
        if (!known) {
            *badPath = kv.key();
            return false;
        }
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : m.params) {
        const auto it = byName.find(p.name);
        if (it != byName.end()) {
            arr.push_back(*it);
        } else if (p.optional) {
            arr.push_back(nullptr);
        } else {
            *badPath = p.name;
            return false;
        }
    }
    *out = std::move(arr);
    return true;
}

// By-name params -> the positional array the ABI takes. False when a name is
// not in the signature, or the signature is unknown; *badPath names which.
inline bool toPositional(const ModuleView& view, const std::string& method,
                         const nlohmann::json& byName, nlohmann::json* out,
                         std::string* badPath) {
    if (view.typed()) {
        const ContractMethod* cm = view.contract->method(method);
        if (!cm) {
            *badPath = method;
            return false;
        }
        return typedPositional(*cm, byName, out, badPath);
    }
    const LiveMember* m = view.resolved() ? view.live.method(method) : nullptr;
    if (!m) {
        *badPath = method;
        return false;
    }
    for (auto kv = byName.begin(); kv != byName.end(); ++kv) {
        if (std::find(m->params.begin(), m->params.end(), kv.key()) == m->params.end()) {
            *badPath = kv.key();
            return false;
        }
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& n : m->params)
        arr.push_back(byName.contains(n) ? byName.at(n) : nlohmann::json());
    *out = std::move(arr);
    return true;
}

// ---------------------------------------------------------------------------
// Views as JSON
// ---------------------------------------------------------------------------

// rpc.schema / GET /modules/{m}. A bridge-derived view, never the module's word.
inline nlohmann::json describeView(const ExposedModule& em, const ModuleView& view,
                                   bool withInterface = true) {
    nlohmann::json methods = nlohmann::json::array();
    for (const auto& m : view.live.methods)
        if (em.methodAllowed(m.name)) methods.push_back(m.name);
    nlohmann::json events = nlohmann::json::array();
    for (const auto& e : view.live.events)
        if (em.events.permits(e.name)) events.push_back(e.name);
    nlohmann::json d{
        {"module", view.module},
        {"resolved", view.resolved()},
        {"events_declared", view.live.eventsDeclared},
        {"methods", std::move(methods)},
        {"events", std::move(events)},
        {"source", view.typed() ? "lidl" : "getPluginInterface"},
        {"authoritative", false},
        {"interface_status", interfaceStatusName(view.status)},
        {"stale", view.stale},
        {"exposure", view.exposure.toJson()},
        {"interface_sha256", nullptr},
        {"contract_sha256", nullptr},
        {"cross_check", view.crossChecked ? view.crossCheck.toJson() : nlohmann::json()},
    };
    if (!view.interfaceSha256.empty()) d["interface_sha256"] = view.interfaceSha256;
    if (!view.contractSha256.empty()) d["contract_sha256"] = view.contractSha256;
    if (withInterface && view.typed()) d["interface"] = view.contract->interface;
    if (view.status == InterfaceStatus::Invalid) d["interface_error"] = view.interfaceError;
    return d;
}

// rpc.list_modules / GET /modules entries: the same view, minus the contract.
inline nlohmann::json listView(const ExposedModule& em, const ModuleView& view) {
    return describeView(em, view, /*withInterface=*/false);
}

} // namespace bridge
