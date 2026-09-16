{
  description = "json_rpc_bridge - exposes configured modules' methods and events to external clients over HTTP and WebSocket";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # The reader for lidl() contracts; SDKs that read the same contracts follow this pin.
    logos-lidl.url = "github:logos-co/logos-lidl";
    logos-lidl.inputs.logos-nix.follows = "logos-module-builder/logos-nix";
  };

  # A universal (Qt-free) module that owns a listening socket. It declares NO
  # module dependencies on purpose: every target it calls is an operator-supplied
  # runtime value, so there is nothing to name at build time.
  #
  # REQUIRES logos-protocol >= 0.9 for subscription continuity
  # (lp_client_set_subscription_status_cb). Below that the bridge still
  # builds and serves, but cannot detect a provider restart, so a subscription
  # silently resumes with an unrecoverable gap instead of terminating and
  # telling the client. The version is not pinned here because the protocol
  # arrives transitively through logos-module-builder; getInfo() reports the
  # version actually in use, and the README says what each one gives you.
  outputs = inputs@{ logos-module-builder, logos-lidl, ... }:
    let
      # logos-lidl's two static archives. mkLogosModuleTests passes a bare flake
      # input through unresolved, so the tests name the package.
      lidlModuleLibs = { logos_lidl_c = logos-lidl; logos_lidl = logos-lidl; };
      lidlPackage = { input = logos-lidl; packages.default = "logos-lidl"; };
      lidlTestLibs = { logos_lidl_c = lidlPackage; logos_lidl = lidlPackage; };

      # CMake reads include/lidl/ from the store. The backend's recursive ./lib staging
      # copies a read-only lib/cmake/ that nixpkgs' fixCmakeFiles cannot rewrite.
      lidlPreConfigure = { externalLibs }: ''
        if [ -d lib/cmake ]; then chmod -R u+w lib/cmake && rm -rf lib/cmake; fi
        export LOGOS_EXT_ROOT_LOGOS_LIDL="${externalLibs.logos_lidl}"
        export LOGOS_EXT_ROOT_LOGOS_LIDL_C="${externalLibs.logos_lidl_c}"
        export LOGOS_LIDL_REV="${logos-lidl.shortRev or logos-lidl.dirtyShortRev or "unknown"}"
        export LOGOS_LIDL_VERSION="${externalLibs.logos_lidl.version or ""}"
      '';

      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        externalLibInputs = lidlModuleLibs;
        preConfigure = lidlPreConfigure;
      };
    in
    module // {
      # Written as a literal `checks =` on purpose: `ws sync-graph` decides
      # hasTests by grepping the flake for exactly that, so a checks output
      # reached any other way records hasTests=false and `ws test` then reports
      # "no tests" WITHOUT failing.
      checks = logos-module-builder.lib.mkLogosModuleTests {
        src = ./.;
        testDir = ./tests;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        externalLibInputs = lidlTestLibs;
        preConfigure = lidlPreConfigure;
      };
    };
}
