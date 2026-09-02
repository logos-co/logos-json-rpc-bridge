{
  description = "json_rpc_bridge - exposes configured modules' methods and events to external clients over HTTP and WebSocket";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
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
  outputs = inputs@{ logos-module-builder, ... }:
    let
      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
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
      };
    };
}
