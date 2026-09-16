# docs-golden: json-rpc-bridge-docs reproduces the golden documents from --lidl,
# --lidl-dir and --lgx (packages made with the lgx CLI) and refuses what discovery refuses.
{ pkgs, docsCli, lgx }:

let
  # logos-package's lgx_host_variant() spellings.
  hostVariant = {
    aarch64-darwin = "darwin-arm64";
    x86_64-darwin = "darwin-x86_64";
    aarch64-linux = "linux-arm64";
    x86_64-linux = "linux-x86_64";
  }.${pkgs.stdenv.hostPlatform.system};
in
pkgs.runCommand "json-rpc-bridge-docs-golden" {
  nativeBuildInputs = [ docsCli lgx pkgs.python3 ];
} ''
  ${pkgs.runtimeShell} ${../tests/docs_golden.sh} ${../tests} ${hostVariant} | tee $out
''
