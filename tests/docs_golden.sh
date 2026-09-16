#!/bin/sh
# docs-golden: json-rpc-bridge-docs reproduces what a running bridge serves, from
# --lidl, --lidl-dir and --lgx, and refuses what discovery refuses.
#
# Usage: sh tests/docs_golden.sh TESTS_DIR HOST_VARIANT
# Needs json-rpc-bridge-docs, lgx, python3, cmp and sha256sum on PATH.
set -eu

T=$1
VARIANT=$2
F="$T/fixtures"
G="$T/goldens"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/docs-golden.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM

passed=0
ok() { passed=$((passed + 1)); echo "ok   $*"; }
fail() { echo "FAIL $*" >&2; exit 1; }

# The goldens' endpoint and version, spelled out rather than defaulted.
PINNED="--server-host 127.0.0.1 --server-port 8645 --info-version 0.1.0"

module_of() {
    case "$1" in
        full_api) echo test_fullapi_cpp ;;
        full_api_ext_policy) echo test_fullapi_ext_cpp ;;
        storage) echo storage_module ;;
        *) fail "no module for context $1" ;;
    esac
}

# exits WANT CMD...: CMD exits with WANT; its output is left in $WORK/stdout and $WORK/stderr.
exits() {
    want=$1
    shift
    set +e
    "$@" > "$WORK/stdout" 2> "$WORK/stderr"
    got=$?
    set -e
    [ "$got" = "$want" ] || { cat "$WORK/stderr" >&2; fail "exit $got, not $want: $*"; }
}

same_json() {
    python3 -c 'import json, sys; sys.exit(json.load(open(sys.argv[1])) != json.load(open(sys.argv[2])))' "$1" "$2"
}

field() {
    python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$1" "$2"
}

sha256() { sha256sum "$1" | cut -d' ' -f1; }

json-rpc-bridge-docs --version
lgx --version 2>/dev/null || true

# 1. Every golden, from --lidl and from --lidl-dir. Compact output is the same document.
for ctx in full_api full_api_ext_policy storage; do
    m=$(module_of "$ctx")
    cfg="$F/configs/$ctx.json"
    for kind in openrpc openapi asyncapi; do
        json-rpc-bridge-docs --config "$cfg" --format "$kind" --lidl "$m=$F/lidl/$m.lidl" \
            $PINNED --pretty > "$WORK/lidl.json"
        cmp "$WORK/lidl.json" "$G/$ctx.$kind.json" || fail "$ctx.$kind: --lidl differs from the golden"
        json-rpc-bridge-docs --config "$cfg" --format "$kind" --lidl-dir "$F/lidl" \
            $PINNED --pretty > "$WORK/dir.json"
        cmp "$WORK/dir.json" "$G/$ctx.$kind.json" || fail "$ctx.$kind: --lidl-dir differs from the golden"
        json-rpc-bridge-docs --config "$cfg" --format "$kind" --lidl-dir "$F/lidl" \
            $PINNED > "$WORK/compact.json"
        same_json "$WORK/compact.json" "$G/$ctx.$kind.json" || fail "$ctx.$kind: compact output is another document"
    done
    ok "$ctx: --lidl and --lidl-dir reproduce its three goldens"
done

# 2. interface: the canonical bytes interface_sha256 is taken over, which are
#    also exactly what `lidl json --identity` wrote into ast/.
for ctx in full_api full_api_ext_policy storage; do
    m=$(module_of "$ctx")
    cfg="$F/configs/$ctx.json"
    json-rpc-bridge-docs --config "$cfg" --format interface --lidl "$m=$F/lidl/$m.lidl" > "$WORK/$m.interface"
    json-rpc-bridge-docs --config "$cfg" --format schema --module "$m" --lidl "$m=$F/lidl/$m.lidl" > "$WORK/$m.schema"
    [ "$(sha256 "$WORK/$m.interface")" = "$(field "$WORK/$m.schema" interface_sha256)" ] ||
        fail "$m: sha256 of the interface output is not interface_sha256"
    [ "$(sha256 "$F/lidl/$m.lidl")" = "$(field "$WORK/$m.schema" contract_sha256)" ] ||
        fail "$m: contract_sha256 is not the .lidl file's sha256"
    same_json "$WORK/$m.interface" "$F/ast/$m.json" || fail "$m: interface parses to another AST"
    { cat "$WORK/$m.interface"; echo; } | cmp - "$F/ast/$m.json" || fail "$m: interface bytes are not lidl json --identity's"
    ok "$m: sha256(interface) = interface_sha256, and it is the vendored AST"
done

# 3. --lgx, with packages built by the lgx CLI: one carrying its own contract and a
#    dependency's, one carrying only the dependency's.
mkdir -p "$WORK/files" "$WORK/own/lidl" "$WORK/deps/lidl"
printf 'not really a plugin\n' > "$WORK/files/storage_module_plugin.so"
cp "$F/lidl/storage_module.lidl" "$F/lidl/mini_module.lidl" "$WORK/own/lidl/"
cp "$F/lidl/mini_module.lidl" "$WORK/deps/lidl/"
(
    cd "$WORK"
    for pkg in storage_module:own deps_only:deps; do
        name=${pkg%%:*}
        assets=${pkg#*:}
        lgx create "$name" > /dev/null
        lgx add "$name.lgx" --variant "$VARIANT" --files files/storage_module_plugin.so \
            --main storage_module_plugin.so --assets "$assets" --yes > /dev/null
        lgx verify "$name.lgx" > /dev/null
    done
)
for kind in openrpc openapi asyncapi schema; do
    json-rpc-bridge-docs --config "$F/configs/storage.json" --format "$kind" \
        --lgx "$WORK/storage_module.lgx" $PINNED > "$WORK/from-lgx"
    json-rpc-bridge-docs --config "$F/configs/storage.json" --format "$kind" \
        --lidl "storage_module=$F/lidl/storage_module.lidl" $PINNED > "$WORK/from-lidl"
    cmp "$WORK/from-lgx" "$WORK/from-lidl" || fail "storage $kind: --lgx differs from --lidl"
done
ok "storage: --lgx equals --lidl for openrpc, openapi, asyncapi and schema"

printf '{"expose":{"modules":["mini_module"]}}\n' > "$WORK/mini.json"
json-rpc-bridge-docs --config "$WORK/mini.json" --format interface --lgx "$WORK/storage_module.lgx" > "$WORK/mini-lgx"
json-rpc-bridge-docs --config "$WORK/mini.json" --format interface \
    --lidl "mini_module=$F/lidl/mini_module.lidl" > "$WORK/mini-lidl"
cmp "$WORK/mini-lgx" "$WORK/mini-lidl" || fail "mini_module: the dependency contract in the package differs"
ok "--lgx selects a dependency's contract by module name"

exits 4 json-rpc-bridge-docs --config "$F/configs/storage.json" --format openrpc --lgx "$WORK/deps_only.lgx"
grep -q "carries no contract for storage_module" "$WORK/stderr" || fail "deps_only.lgx: $(cat "$WORK/stderr")"
ok "--lgx without the module's contract exits 4"

# 4. What discovery refuses, the renderer refuses, with discovery's messages.
exits 4 json-rpc-bridge-docs --config "$F/configs/storage.json" --format openapi \
    --lidl "storage_module=$F/invalid/malformed.lidl"
grep -q "4:17: .*(lidl reader " "$WORK/stderr" || fail "malformed: $(cat "$WORK/stderr")"
ok "a malformed contract exits 4, with its position"

exits 4 json-rpc-bridge-docs --config "$F/configs/storage.json" --format openapi \
    --lidl "storage_module=$F/lidl/mini_module.lidl"
grep -q "declares module 'mini_module', not 'storage_module'" "$WORK/stderr" || fail "mismatch: $(cat "$WORK/stderr")"
ok "a contract naming another module exits 4"

exits 4 json-rpc-bridge-docs --config "$F/configs/storage.json" --format openapi \
    --lidl "storage_module=$F/invalid/authored_lidl.lidl"
grep -q "declares 'lidl()'" "$WORK/stderr" || fail "authored lidl: $(cat "$WORK/stderr")"
ok "a contract that authors lidl() exits 4"

exits 5 json-rpc-bridge-docs --config "$F/configs/storage_with_untyped.json" --format openrpc \
    --lidl "storage_module=$F/lidl/storage_module.lidl" $PINNED --pretty
grep -q "legacy_module: no contract given" "$WORK/stderr" || fail "untyped: $(cat "$WORK/stderr")"
ok "an exposed module without a contract exits 5"

exits 0 json-rpc-bridge-docs --config "$F/configs/storage_with_untyped.json" --format openrpc \
    --lidl "storage_module=$F/lidl/storage_module.lidl" $PINNED --pretty --allow-untyped
cmp "$WORK/stdout" "$G/storage.openrpc.json" || fail "--allow-untyped: not the storage golden"
ok "--allow-untyped leaves it out, which is the storage golden"

exits 3 json-rpc-bridge-docs --config "$F/invalid/non_loopback.json" --format openrpc \
    --lidl "storage_module=$F/lidl/storage_module.lidl"
grep -q "config rejected: http.host must be a loopback address" "$WORK/stderr" || fail "config: $(cat "$WORK/stderr")"
ok "a rejected config exits 3"

echo "docs-golden: $passed checks passed"
