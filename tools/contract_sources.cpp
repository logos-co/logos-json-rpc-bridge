#include "contract_sources.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include <unistd.h>   // mkdtemp

#include <lgx.h>

// Set by CMake when the pinned lgx.h declares lgx_extract_assets (logos-package P0p).
#ifndef BRIDGE_HAVE_LGX_EXTRACT_ASSETS
#define BRIDGE_HAVE_LGX_EXTRACT_ASSETS 0
#endif

namespace bridge_docs {
namespace {

namespace fs = std::filesystem;

std::string joined(const char** list) {
    std::string out;
    for (const char** p = list; p && *p; ++p) {
        if (!out.empty()) out += "; ";
        out += *p;
    }
    return out;
}

std::string lastError(const char* fallback) {
    const char* e = lgx_get_last_error();
    return e && *e ? std::string(e) : std::string(fallback);
}

// A private scratch directory, removed with its contents on every exit path.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        fs::path base = fs::temp_directory_path(ec);
        if (ec) base = "/tmp";
        std::string pattern = (base / "json-rpc-bridge-docs.XXXXXX").string();
        std::vector<char> buf(pattern.begin(), pattern.end());
        buf.push_back('\0');
        if (::mkdtemp(buf.data())) m_path = buf.data();
    }
    ~TempDir() {
        std::error_code ec;
        if (!m_path.empty()) fs::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::string& path() const { return m_path; }

private:
    std::string m_path;
};

struct PackageHandle {
    lgx_package_t pkg = nullptr;
    ~PackageHandle() {
        if (pkg) lgx_free_package(pkg);
    }
};

struct StringArray {
    const char** items = nullptr;
    ~StringArray() {
        if (items) lgx_free_string_array(items);
    }
};

// The host's variant under any spelling, then one of those with a suffix (-dev), then the first.
[[maybe_unused]] std::string pickVariant(lgx_package_t pkg) {
    StringArray variants;
    variants.items = lgx_get_variants(pkg);
    if (!variants.items || !variants.items[0]) return {};
    StringArray spellings;
    spellings.items = lgx_variant_spellings(lgx_host_variant());
    for (const char** s = spellings.items; s && *s; ++s)
        for (const char** v = variants.items; *v; ++v)
            if (std::string(*v) == *s) return *v;
    for (const char** s = spellings.items; s && *s; ++s)
        for (const char** v = variants.items; *v; ++v)
            if (std::string(*v).rfind(std::string(*s) + "-", 0) == 0) return *v;
    return variants.items[0];
}

} // namespace

ReadStatus readContractFile(const std::string& path, std::size_t maxBytes, std::string* out,
                            std::string* why) {
    std::error_code ec;
    const fs::file_status st = fs::status(path, ec);
    if (ec || !fs::exists(st)) {
        *why = "no such file";
        return ReadStatus::Missing;
    }
    if (!fs::is_regular_file(st)) {
        *why = "not a regular file";
        return ReadStatus::Failed;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *why = "cannot be opened";
        return ReadStatus::Failed;
    }
    std::string data;
    char chunk[65536];
    while (in) {
        in.read(chunk, sizeof(chunk));
        data.append(chunk, static_cast<std::size_t>(in.gcount()));
        if (data.size() > maxBytes) {
            *why = "larger than " + std::to_string(maxBytes / (1024 * 1024)) + " MiB";
            return ReadStatus::TooLarge;
        }
    }
    if (in.bad()) {
        *why = "cannot be read";
        return ReadStatus::Failed;
    }
    *out = std::move(data);
    return ReadStatus::Ok;
}

LgxContracts readLgxContracts(const std::string& path, std::size_t maxBytes) {
    LgxContracts r;
    lgx_verify_result_t verdict = lgx_verify(path.c_str());
    const bool valid = verdict.valid;
    const std::string problems = joined(verdict.errors);
    lgx_free_verify_result(verdict);
    if (!valid) {
        r.error = "not a valid package: " + (problems.empty() ? lastError("lgx_verify failed") : problems);
        return r;
    }

    PackageHandle handle;
    handle.pkg = lgx_load(path.c_str());
    if (!handle.pkg) {
        r.error = "cannot be loaded: " + lastError("lgx_load failed");
        return r;
    }
    TempDir scratch;
    if (scratch.path().empty()) {
        r.error = "no scratch directory for extraction";
        return r;
    }

    fs::path assets;
#if BRIDGE_HAVE_LGX_EXTRACT_ASSETS
    const lgx_result_t extracted = lgx_extract_assets(handle.pkg, scratch.path().c_str());
    r.extractedFrom = "assets";
    assets = fs::path(scratch.path()) / "assets";
#else
    const std::string variant = pickVariant(handle.pkg);
    if (variant.empty()) {
        r.error = "the package has no variant to extract";
        return r;
    }
    const lgx_result_t extracted = lgx_extract(handle.pkg, variant.c_str(), scratch.path().c_str());
    r.extractedFrom = variant;
    assets = fs::path(scratch.path()) / variant / "assets";
#endif
    if (!extracted.success) {
        r.error = "cannot be extracted: " +
                  (extracted.error ? std::string(extracted.error) : lastError("extraction failed"));
        return r;
    }

    std::error_code ec;
    const fs::path dir = assets / "lidl";
    if (!fs::is_directory(dir, ec)) {   // a package that carries no contract at all
        r.ok = true;
        return r;
    }
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path file = it->path();
        if (file.extension() != ".lidl") continue;
        const std::string name = file.stem().string();
        std::string text, why;
        if (readContractFile(file.string(), maxBytes, &text, &why) == ReadStatus::Ok)
            r.lidl[name] = std::move(text);
        else
            r.refused[name] = why;
    }
    if (ec) {
        r.error = "assets/lidl cannot be listed: " + ec.message();
        return r;
    }
    r.ok = true;
    return r;
}

std::string lgxLibraryVersion() {
    const char* v = lgx_version();
    return std::string("lgx ") + (v && *v ? v : "unknown");
}

} // namespace bridge_docs
