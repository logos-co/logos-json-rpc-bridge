#include "json_rpc_bridge_docs.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge_config.h"
#include "contract_sources.h"
#include "discovery_model.h"
#include "doc_publisher.h"
#include "lidl_contract.h"

// metadata.json's version, as the plugin reports it; tools/CMakeLists.txt reads it.
#ifndef JSON_RPC_BRIDGE_VERSION
#error "JSON_RPC_BRIDGE_VERSION is not defined; tools/CMakeLists.txt reads it from metadata.json"
#endif

namespace bridge_docs {
namespace {

using nlohmann::json;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitConfig = 3;
constexpr int kExitInput = 4;
constexpr int kExitUntyped = 5;
constexpr int kExitWrite = 6;

constexpr const char* kProgram = "json-rpc-bridge-docs";
constexpr std::size_t kMaxConfigBytes = 16 * 1024 * 1024;

constexpr const char* kUsageText =
    R"(usage: json-rpc-bridge-docs --config PATH --format openrpc|openapi|asyncapi|schema|interface
                            [--module NAME]
                            (--lidl NAME=FILE | --lidl-dir DIR | --lgx FILE)...
                            [--server-host H] [--server-port P] [--info-version V]
                            [--allow-untyped] [--out PATH] [--pretty]
       json-rpc-bridge-docs --version

Prints what a json_rpc_bridge started with this config serves for these contracts,
treating every module as resolved and consistent with them:
  openrpc     the rpc.discover result
  openapi     GET /openapi.json
  asyncapi    GET /asyncapi.json
  schema      the rpc.schema view of --module, or the rpc.list_modules views
  interface   --module's interface as canonical JSON: its sha256 is interface_sha256

  --module NAME       only this exposed module; interface needs it when the config
                      exposes more than one
  --lidl NAME=FILE    NAME's canonical contract, as its #lidl build writes it
  --lidl-dir DIR      DIR/<name>.lidl for each exposed module found there
  --lgx FILE          a package's assets/lidl/<name>.lidl, read through logos-package;
                      the package must carry the contract of a module being rendered
  --server-host H     the host the documents name (default: the config's http.host)
  --server-port P     the port the documents name (default: the config's http.port)
  --info-version V    info.version (default: this bridge's version)
  --allow-untyped     leave out exposed modules that have no contract
  --out PATH          write there instead of to stdout
  --pretty            indent, and end with a newline

Exit codes: 0 ok, 2 usage, 3 config rejected, 4 contract unreadable or invalid,
5 an exposed module has no contract, 6 output not written.
)";

enum class Format { None, OpenRpc, OpenApi, AsyncApi, Schema, Interface };

struct Options {
    std::string configPath;
    Format format = Format::None;
    std::string module;
    std::vector<std::pair<std::string, std::string>> lidl;   // name, file
    std::vector<std::string> lidlDirs;
    std::vector<std::string> packages;
    std::string serverHost;
    int serverPort = 0;
    std::string infoVersion;
    bool allowUntyped = false;
    std::string outPath;
    bool pretty = false;
    bool version = false;
    bool help = false;
};

Format parseFormat(const std::string& s) {
    if (s == "openrpc") return Format::OpenRpc;
    if (s == "openapi") return Format::OpenApi;
    if (s == "asyncapi") return Format::AsyncApi;
    if (s == "schema") return Format::Schema;
    if (s == "interface") return Format::Interface;
    return Format::None;
}

bool isDocument(Format f) {
    return f == Format::OpenRpc || f == Format::OpenApi || f == Format::AsyncApi;
}

// Printable ASCII with no space or '/': it lands verbatim in the documents' URLs.
bool plainHost(const std::string& h) {
    for (unsigned char c : h)
        if (c <= 0x20 || c >= 0x7f || c == '/') return false;
    return !h.empty();
}

bool parsePort(const std::string& s, int* port) {
    if (s.empty() || s.size() > 5) return false;
    int v = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        v = v * 10 + (c - '0');
    }
    if (v < 1 || v > 65535) return false;
    *port = v;
    return true;
}

bool parseArgs(int argc, const char* const* argv, Options* o, std::string* error) {
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i] ? argv[i] : "";
        std::string value;
        bool inlineValue = false;
        if (arg.rfind("--", 0) == 0) {
            const std::size_t eq = arg.find('=');
            if (eq != std::string::npos) {
                value = arg.substr(eq + 1);
                arg.resize(eq);
                inlineValue = true;
            }
        }
        const auto take = [&](std::string* dst) {
            if (!inlineValue) {
                if (i + 1 >= argc || !argv[i + 1]) {
                    *error = arg + " needs a value";
                    return false;
                }
                value = argv[++i];
            }
            if (value.empty()) {
                *error = arg + " needs a non-empty value";
                return false;
            }
            *dst = value;
            return true;
        };
        const auto once = [&]() {
            if (seen.insert(arg).second) return true;
            *error = arg + " is given twice";
            return false;
        };
        const auto flag = [&](bool* dst) {
            if (inlineValue) {
                *error = arg + " takes no value";
                return false;
            }
            *dst = true;
            return true;
        };

        std::string v;
        if (arg == "--config") {
            if (!once() || !take(&o->configPath)) return false;
        } else if (arg == "--format") {
            if (!once() || !take(&v)) return false;
            o->format = parseFormat(v);
            if (o->format == Format::None) {
                *error = "--format must be openrpc, openapi, asyncapi, schema or interface (got '" + v + "')";
                return false;
            }
        } else if (arg == "--module") {
            if (!once() || !take(&o->module)) return false;
        } else if (arg == "--lidl") {
            if (!take(&v)) return false;
            const std::size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 == v.size()) {
                *error = "--lidl takes NAME=FILE (got '" + v + "')";
                return false;
            }
            o->lidl.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        } else if (arg == "--lidl-dir") {
            if (!take(&v)) return false;
            o->lidlDirs.push_back(v);
        } else if (arg == "--lgx") {
            if (!take(&v)) return false;
            o->packages.push_back(v);
        } else if (arg == "--server-host") {
            if (!once() || !take(&o->serverHost)) return false;
            if (!plainHost(o->serverHost)) {
                *error = "--server-host must be a host name or address (got '" + o->serverHost + "')";
                return false;
            }
        } else if (arg == "--server-port") {
            if (!once() || !take(&v)) return false;
            if (!parsePort(v, &o->serverPort)) {
                *error = "--server-port must be in 1..65535 (got '" + v + "')";
                return false;
            }
        } else if (arg == "--info-version") {
            if (!once() || !take(&o->infoVersion)) return false;
        } else if (arg == "--out") {
            if (!once() || !take(&o->outPath)) return false;
        } else if (arg == "--allow-untyped") {
            if (!flag(&o->allowUntyped)) return false;
        } else if (arg == "--pretty") {
            if (!flag(&o->pretty)) return false;
        } else if (arg == "--version") {
            if (!flag(&o->version)) return false;
        } else if (arg == "--help" || arg == "-h") {
            if (!flag(&o->help)) return false;
        } else {
            *error = arg.rfind("-", 0) == 0 ? "unknown option " + arg : "unexpected argument '" + arg + "'";
            return false;
        }
    }
    if (o->help || o->version) return true;
    if (o->configPath.empty()) {
        *error = "--config is required";
        return false;
    }
    if (o->format == Format::None) {
        *error = "--format is required";
        return false;
    }
    if (o->lidl.empty() && o->lidlDirs.empty() && o->packages.empty()) {
        *error = "no contracts: pass --lidl NAME=FILE, --lidl-dir DIR or --lgx FILE";
        return false;
    }
    return true;
}

// One place a module's contract came from.
struct Candidate {
    std::string source;
    std::string text;
};

class Cli {
public:
    Cli(std::ostream& out, std::ostream& err) : m_out(out), m_err(err) {}

    int run(int argc, const char* const* argv) {
        std::string error;
        if (!parseArgs(argc, argv, &m_opt, &error)) return usage(error);
        if (m_opt.help) {
            m_out << kUsageText;
            return kExitOk;
        }
        if (m_opt.version) {
            m_out << kProgram << ' ' << JSON_RPC_BRIDGE_VERSION << " (" << bridge::lidlReaderVersion()
                  << ", " << lgxLibraryVersion() << ")\n";
            return kExitOk;
        }
        int rc = loadConfig();
        if (rc == kExitOk) rc = select();
        if (rc == kExitOk) rc = readPackages();
        if (rc == kExitOk) rc = resolveModules();
        if (rc != kExitOk) return rc;
        return write(render());
    }

private:
    int usage(const std::string& message) {
        m_err << kProgram << ": " << message << "\n" << "Try '" << kProgram << " --help'.\n";
        return kExitUsage;
    }

    int fail(int code, const std::string& message) {
        m_err << kProgram << ": " << message << "\n";
        return code;
    }

    void warn(const std::string& message) { m_err << kProgram << ": warning: " << message << "\n"; }

    int loadConfig() {
        std::string text, why;
        if (readContractFile(m_opt.configPath, kMaxConfigBytes, &text, &why) != ReadStatus::Ok)
            return fail(kExitConfig, "config rejected: " + m_opt.configPath + ": " + why);
        bridge::ConfigParseResult parsed = bridge::parseBridgeConfig(text, "json_rpc_bridge");
        if (!parsed.ok) return fail(kExitConfig, "config rejected: " + parsed.error);
        m_config = std::move(parsed.config);
        return kExitOk;
    }

    int select() {
        if (!m_opt.module.empty()) {
            const bridge::ExposedModule* em = m_config.find(m_opt.module);
            if (!em) return usage("--module " + m_opt.module + " is not exposed by the config");
            m_selected.push_back(em);
        } else {
            for (const auto& em : m_config.modules) m_selected.push_back(&em);
        }
        if (m_opt.format == Format::Interface && m_selected.size() != 1)
            return usage("--format interface renders one module: pass --module");
        std::set<std::string> named;
        for (const auto& l : m_opt.lidl) {
            if (!m_config.find(l.first)) return usage("--lidl names '" + l.first + "', which the config does not expose");
            if (!named.insert(l.first).second) return usage("--lidl names '" + l.first + "' twice");
        }
        return kExitOk;
    }

    int readPackages() {
        for (const std::string& path : m_opt.packages) {
            LgxContracts c = readLgxContracts(path, bridge::kMaxContractBytes);
            if (!c.ok) return fail(kExitInput, path + ": " + c.error);
            bool useful = false;
            std::string carried;
            for (const auto& kv : c.lidl) carried += (carried.empty() ? "" : ", ") + kv.first;
            for (const auto* em : m_selected)
                useful = useful || c.lidl.count(em->name) || c.refused.count(em->name);
            if (!useful)
                return fail(kExitInput, path + ": carries no contract for " + selectedNames() +
                                            " (assets/lidl has " + (carried.empty() ? "none" : carried) + ")");
            m_packages.emplace_back(path, std::move(c));
        }
        for (const std::string& dir : m_opt.lidlDirs) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) return fail(kExitInput, dir + ": not a directory");
        }
        return kExitOk;
    }

    std::string selectedNames() const {
        std::string out;
        for (std::size_t i = 0; i < m_selected.size(); ++i)
            out += (i ? (i + 1 == m_selected.size() ? " or " : ", ") : "") + m_selected[i]->name;
        return out;
    }

    // Every source that has this module's contract; *code is set when one cannot be read.
    std::vector<Candidate> candidates(const std::string& module, int* code) {
        std::vector<Candidate> found;
        const auto read = [&](const std::string& path, bool optional) {
            std::string text, why;
            const ReadStatus st = readContractFile(path, bridge::kMaxContractBytes, &text, &why);
            if (st == ReadStatus::Ok) found.push_back({path, std::move(text)});
            else if (!(optional && st == ReadStatus::Missing))
                *code = fail(kExitInput, module + ": " + path + ": " + why);
        };
        for (const auto& l : m_opt.lidl)
            if (l.first == module && *code == kExitOk) read(l.second, false);
        for (const std::string& dir : m_opt.lidlDirs)
            if (*code == kExitOk) read((std::filesystem::path(dir) / (module + ".lidl")).string(), true);
        for (const auto& p : m_packages) {
            if (*code != kExitOk) break;
            const std::string source = p.first + ": assets/lidl/" + module + ".lidl";
            const auto refused = p.second.refused.find(module);
            if (refused != p.second.refused.end()) *code = fail(kExitInput, module + ": " + source + ": " + refused->second);
            const auto hit = p.second.lidl.find(module);
            if (hit != p.second.lidl.end()) found.push_back({source, hit->second});
        }
        return found;
    }

    int resolveModules() {
        for (const bridge::ExposedModule* em : m_selected) {
            int code = kExitOk;
            const std::vector<Candidate> found = candidates(em->name, &code);
            if (code != kExitOk) return code;
            if (found.empty()) {
                const bool single = m_opt.format == Format::Interface ||
                                    (m_opt.format == Format::Schema && !m_opt.module.empty());
                if (m_opt.allowUntyped && !single) {
                    warn(em->name + ": no contract; left out (--allow-untyped)");
                    continue;
                }
                return fail(kExitUntyped, em->name + ": no contract given; offline, an untyped module "
                                          "cannot be described (--allow-untyped leaves it out)");
            }
            for (std::size_t i = 1; i < found.size(); ++i)
                if (found[i].text != found[0].text)
                    return fail(kExitInput, em->name + ": " + found[0].source + " and " + found[i].source +
                                                " differ");
            const Candidate& c = found[0];
            // What discovery checks of a lidl() answer before reading it.
            if (!bridge::isValidUtf8(c.text))
                return fail(kExitInput, em->name + ": " + c.source + ": not valid UTF-8");
            bridge::OfflineView v = bridge::offlineView(*em, c.text);
            if (!v.ok) return fail(kExitInput, em->name + ": " + c.source + ": " + v.error);
            for (const std::string& w : v.warnings)
                warn(em->name + ": " + c.source + ": " +
                     (w == "non_canonical" ? "not in canonical form (lidl fmt)" : w));
            m_resolved.push_back(em);
            m_views.push_back(std::make_shared<const bridge::ModuleView>(std::move(v.view)));
        }
        return kExitOk;
    }

    std::string dumped(const json& doc) const {
        return m_opt.pretty ? doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n"
                            : bridge::compactJson(doc);
    }

    std::string render() const {
        if (isDocument(m_opt.format)) {
            bridge::BridgeConfig shown = m_config;
            if (!m_opt.serverHost.empty()) shown.host = m_opt.serverHost;
            if (m_opt.serverPort) shown.port = m_opt.serverPort;
            const std::string version = m_opt.infoVersion.empty() ? JSON_RPC_BRIDGE_VERSION : m_opt.infoVersion;
            const bridge::docs::DocContext ctx = bridge::docContext(shown, version, m_views);
            if (m_opt.format == Format::OpenRpc) return dumped(bridge::docs::buildOpenRpc(ctx));
            if (m_opt.format == Format::OpenApi) return dumped(bridge::docs::buildOpenApi(ctx));
            return dumped(bridge::docs::buildAsyncApi(ctx));
        }
        if (m_opt.format == Format::Interface) {
            const json& interface = m_views.front()->contract->interface;
            return m_opt.pretty ? dumped(interface) : bridge::canonicalJson(interface);
        }
        if (!m_opt.module.empty()) return dumped(bridge::describeView(*m_resolved.front(), *m_views.front()));
        json list = json::array();
        for (std::size_t i = 0; i < m_views.size(); ++i) list.push_back(bridge::listView(*m_resolved[i], *m_views[i]));
        return dumped(list);
    }

    int write(const std::string& text) {
        if (m_opt.outPath.empty()) {
            m_out << text;
            m_out.flush();
            if (!m_out) return fail(kExitWrite, "stdout: write failed");
            return kExitOk;
        }
        std::ofstream file(m_opt.outPath, std::ios::binary | std::ios::trunc);
        if (file) {
            file << text;
            file.close();
        }
        if (!file) return fail(kExitWrite, m_opt.outPath + ": cannot be written");
        return kExitOk;
    }

    std::ostream& m_out;
    std::ostream& m_err;
    Options m_opt;
    bridge::BridgeConfig m_config;
    std::vector<const bridge::ExposedModule*> m_selected;
    std::vector<std::pair<std::string, LgxContracts>> m_packages;
    std::vector<const bridge::ExposedModule*> m_resolved;   // parallel to m_views
    std::vector<std::shared_ptr<const bridge::ModuleView>> m_views;
};

} // namespace

int runDocsCli(int argc, const char* const* argv, std::ostream& out, std::ostream& err) {
    try {
        return Cli(out, err).run(argc, argv);
    } catch (const std::exception& e) {
        err << kProgram << ": " << e.what() << "\n";
        return kExitInput;
    }
}

} // namespace bridge_docs

#ifndef BRIDGE_DOCS_NO_MAIN
int main(int argc, char** argv) {
    return bridge_docs::runDocsCli(argc, argv, std::cout, std::cerr);
}
#endif
