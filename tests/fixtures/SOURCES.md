# Fixture provenance

One set of fixtures serves the unit tests, the `docs-golden` check and the
`docs-metaschema` check.

- `lidl/<module>.lidl` are canonical contracts: each file equals its own `lidl fmt`
  output.
- `ast/<module>.json` is `lidl json --identity lidl/<module>.lidl`, byte for byte, so
  `interface_sha256` is the SHA-256 of that file without its final newline. The bridge's
  reader produces the same JSON (`every_fixture_reads_to_its_vendored_ast`).
- `legacy/` holds a contract as an older writer published it (not canonical).
- `configs/<context>.json` are the bridge configs of the golden contexts.
- `invalid/` and `configs/storage_with_untyped.json` are what `docs-golden` expects
  the renderer to refuse (see below).
- `interface-digest-vectors.json` is the digest contract with the Python SDK.

Both kinds of file were written with the `lidl` CLI from logos-lidl branch `feat/lidl-cli` at
`1f54a2a` (`lidl --version`: `lidl 0.1.0 (1f54a2a)`), whose parser, serializer
and identity injection are logos-lidl master `9df8e00`.

The files are named after the module they declare, which is how a contract
directory is looked up.

| File | sha256 | Source |
|---|---|---|
| `lidl/test_fullapi_cpp.lidl` | `6e92054db23a3b3bfb78179647a0d67315c48a2efd6e0a515b5f0b9bf6b7687c` | `github:logos-co/logos-test-modules/a8b1d82#modules.aarch64-darwin.test_fullapi_cpp.lidl` (`/nix/store/025vq9w1acylyn4np4y3q87kmvcnc6m5-logos-test_fullapi_cpp-lidl`), put through `lidl fmt`. Its only change: `method doVoid() -> void` became `method doVoid()`. |
| `lidl/test_fullapi_ext_cpp.lidl` | `7308b6432d35a7e367dc33219e9c059c59faac89a3aabd05b2b6984f934bd1fe` | `github:logos-co/logos-test-modules/a8b1d82#modules.aarch64-darwin.test_fullapi_ext_cpp.lidl` (`/nix/store/m3gbgwxh2zkikvd2d60xpw05z3fnj6zb-logos-test_fullapi_ext_cpp-lidl`). Already canonical. |
| `lidl/mini_module.lidl` | `68bde1680866ea0e450ed711e334e8c70013110eddb8889887cba029232e1f7c` | Written by hand for the `multi` context, then put through `lidl fmt` (which only dropped a comment and reindented). One of each shape the builders treat specially: the record `Note` has a `bstr` field and both optional spellings; `put` takes and returns it, `find` has a `? tstr` parameter and returns `result`, `clear` returns nothing, and the event `added` carries a `Note`. `put` has a Doxygen `@code{.json}` description. |
| `lidl/storage_module.lidl` | `9f6bd141a1401b14ec151b579fd1e1076ba7916929f54101fd6843501dee92ac` | The `#lidl` output of logos-storage-module `9e8890f` (branch `chore/relock-module-builder-lidl`, relocked onto logos-module-builder `5a962b8`), which is byte-identical to what `storage_module.lidl()` returns and to the package's `assets/lidl/storage_module.lidl`. 31 declared methods. Already canonical; `lidl fmt` of storage master `5eee8f7` (the old builder, which still wrote `-> void`) gives the same bytes. |
| `legacy/test_fullapi_cpp.lidl` | `28c70aaa24299bc68668a7d757ff7e86ba17348c96263f433e836cf3a22ee414` | That same `#lidl` output of test-modules `a8b1d82`, unchanged: its writer still spelled `method doVoid() -> void`, so the reader accepts it with a `non_canonical` warning. `lidl fmt` of it is `lidl/test_fullapi_cpp.lidl`. |
| `ast/test_fullapi_cpp.json` | `813ac52598a51f992a391c199c6ca650970547021aabc06e828e526cf1a26251` | `lidl json --identity lidl/test_fullapi_cpp.lidl` |
| `ast/test_fullapi_ext_cpp.json` | `7e14b495d578976405ac7c1f014489a360d7b438c286f2951401fe29fcde868a` | `lidl json --identity lidl/test_fullapi_ext_cpp.lidl` |
| `ast/storage_module.json` | `d071bc92e1bf23f2bf8d189f96bc370678d96dce2fae3ca00819bd0d605a5611` | `lidl json --identity lidl/storage_module.lidl` |
| `ast/mini_module.json` | `659e5e15188aab4968cbfb3979ab4698f3469017a990dcdcd6080cac54fdd94c` | `lidl json --identity lidl/mini_module.lidl` |

## Refusals (`docs-golden`, `tests/docs_golden.sh`)

Written by hand for the refusals `json-rpc-bridge-docs` shares with discovery:

| File | Expected |
|---|---|
| `invalid/malformed.lidl` | exit 4, `4:17: Expected parameter name (lidl reader <rev>)` |
| `invalid/authored_lidl.lidl` | exit 4: it declares `lidl()`, which identity injection refuses (`lidl check` alone accepts it) |
| `lidl/mini_module.lidl` given as `storage_module` | exit 4: it declares another module |
| `invalid/non_loopback.json` | exit 3: `http.host` is not loopback |
| `configs/storage_with_untyped.json` | exit 5 with only `storage_module`'s contract; with `--allow-untyped`, exactly `goldens/storage.openrpc.json` |

## `interface-digest-vectors.json`

`[{name, value, canonical, sha256}]` (sha256
`ed57c702ad9ea55196143f0cde781e1f651b4afdf6b2f1348044f1fcda86bf06`). `canonical` is
Python 3.13's `json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)`
and `sha256` its `hashlib` digest; the file itself was written with
`json.dump(..., ensure_ascii=False, indent=2)`. The bridge owns it and
logos-json-rpc-bridge-py consumes it. `storage_module_identity_ast` is `ast/storage_module.json`.

## Regenerating

```sh
lidl fmt <source.lidl> > lidl/<module>.lidl
lidl json --identity lidl/<module>.lidl > ast/<module>.json
```

The golden test rewrites `tests/goldens/` when `BRIDGE_UPDATE_GOLDENS=1` is set.
The unit tests run inside a nix build, so set it there and copy the result out
(stage new files first; the flake sees only tracked ones):

```sh
nix build --impure -o /tmp/bridge-goldens --expr '
  let flake = builtins.getFlake "git+file://'"$PWD"'";
  in flake.checks.${builtins.currentSystem}.unit-tests.overrideAttrs (_: {
    BRIDGE_UPDATE_GOLDENS = "1";
    postInstall = "cp -r $NIX_BUILD_TOP/$sourceRoot/tests/goldens $out/goldens";
  })'
cp /tmp/bridge-goldens/goldens/*.json tests/goldens/
```

Review the diff under `tests/goldens/`, then run the `docs-metaschema` and
`docs-golden` checks.

## Golden contexts (`tests/goldens/<context>.<kind>.json`)

| Context | Config (`configs/`) | Modules |
|---|---|---|
| `full_api` | `full_api.json` | `test_fullapi_cpp`, everything exposed |
| `full_api_ext_policy` | `full_api_ext_policy.json` | `test_fullapi_ext_cpp` with method `echoWrapper` and event `blobEvent` denied; `Wrapper` stays in the components |
| `storage` | `storage.json` | `storage_module` with method `destroy` denied |
| `multi` | `multi.json` | `mini_module` (ok), `legacy_module` (untyped, two live methods and one event), `broken_module` (invalid, with an `interface_error`), `late_module` (pending) |

`tests/test_doc_goldens.cpp` (`goldenContext`) builds each context the way the bridge
does: the config through `parseBridgeConfig`, each typed module through `offlineView`,
then `docContext`, all with `info.version` pinned to `0.1.0`. `json-rpc-bridge-docs`
takes the same path, so the `docs-golden` check reproduces the first three contexts
from their configs and `lidl/` files. `multi` uses the small `mini_module` rather than
`test_fullapi_cpp`, which `full_api` already covers. Its other three modules are live
views (untyped, invalid, pending) that only a running bridge has, so the renderer
cannot reproduce it.
