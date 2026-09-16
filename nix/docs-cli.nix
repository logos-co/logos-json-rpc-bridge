# json-rpc-bridge-docs: plain CMake over tools/, src/lidl_contract.cpp and the pure headers.
{ pkgs, lidl, lidlRev, lgx }:

let
  inherit (pkgs) lib;
  root = ../.;
in
pkgs.stdenv.mkDerivation {
  pname = "json-rpc-bridge-docs";
  version = (builtins.fromJSON (builtins.readFile ../metadata.json)).version;

  # Only what the renderer compiles, so README or test edits do not rebuild it.
  src = lib.fileset.toSource {
    inherit root;
    fileset = lib.fileset.unions [ ../metadata.json ../src ../tools ];
  };
  cmakeDir = "../tools";

  nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
  buildInputs = [ pkgs.nlohmann_json ];
  cmakeFlags = [
    "-DLIDL_ROOT=${lidl}"
    "-DLGX_ROOT=${lgx}"
    "-DLOGOS_LIDL_REV=${lidlRev}"
    "-DLOGOS_LIDL_VERSION=${lidl.version or ""}"
  ];

  doInstallCheck = true;
  installCheckPhase = ''
    $out/bin/json-rpc-bridge-docs --version
  '';

  meta = {
    description = "Renders json_rpc_bridge's OpenRPC/OpenAPI/AsyncAPI documents from LIDL contracts";
    mainProgram = "json-rpc-bridge-docs";
  };
}
