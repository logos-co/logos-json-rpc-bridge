#!/usr/bin/env bash
#
# Execute the json-rpc-bridge doc-test end-to-end and regenerate its Markdown.
#
# Spec:
#   - json-rpc-bridge.test.yaml   builds the json_rpc_bridge module from this repo
#                             plus one inline provider module, runs both under a
#                             logoscore daemon, calls the provider over HTTP, and
#                             receives a pushed event on two concurrent
#                             WebSocket clients.
#
# The runner is the shared `doctest` CLI (https://github.com/logos-co/logos-doctest),
# invoked directly via its flake. `doctest run` executes every command in a temp
# directory and asserts on the output; `doctest generate` renders the spec to
# Markdown under outputs/; `doctest clean` strips build artifacts.
#
# To run against a local logos-doctest checkout instead of the published flake,
# set DOCTEST, e.g.:  DOCTEST="nix run path:../../logos-doctest --" ./run.sh
#
set -euo pipefail

cd "$(dirname "$0")"

# The doctest CLI. Override by exporting DOCTEST (space-separated command).
read -r -a DOCTEST <<< "${DOCTEST:-nix run github:logos-co/logos-doctest --}"
OUTPUT_DIR="./outputs"
SPECS=(
  "json-rpc-bridge.test.yaml"
)

# Build the doc-test against THIS repo's current commit rather than the latest
# published flake. The spec builds `github:logos-co/logos-json-rpc-bridge{release}#lgx`,
# and the pin below makes {release} expand to $COMMIT — so the json_rpc_bridge module
# under test is exactly what's checked out here. Override by exporting COMMIT, or
# set COMMIT="" to fall back to latest main.
#
# Note: nix fetches the commit from the GitHub remote, so $COMMIT must be pushed
# to logos-co/logos-json-rpc-bridge. A local-only / uncommitted HEAD won't resolve;
# export COMMIT="" (or push first) in that case.
COMMIT="${COMMIT-$(git rev-parse HEAD)}"
RELEASE_FOR=()
if [ -n "${COMMIT}" ]; then
  RELEASE_FOR=(--release-for "logos-json-rpc-bridge=${COMMIT}")
  echo "==> Pinning logos-json-rpc-bridge to ${COMMIT}"
else
  echo "==> COMMIT empty; building against latest logos-json-rpc-bridge main"
fi

echo "==> Clearing previous ${OUTPUT_DIR}/"
if [ -e "${OUTPUT_DIR}" ]; then
  chmod -R u+w "${OUTPUT_DIR}" 2>/dev/null || true
fi
rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

for SPEC in "${SPECS[@]}"; do
  name="$(basename "${SPEC%.test.yaml}")"
  spec_out="${OUTPUT_DIR}/${name}"
  mkdir -p "${spec_out}"

  echo "==> Running ${SPEC} into ${spec_out}/"
  "${DOCTEST[@]}" run "${SPEC}" \
    --verbose \
    --continue-on-fail \
    ${RELEASE_FOR[@]+"${RELEASE_FOR[@]}"} \
    --output-dir "${spec_out}/"

  echo "==> Generating ${OUTPUT_DIR}/${name}.md"
  "${DOCTEST[@]}" generate "${SPEC}" \
    ${RELEASE_FOR[@]+"${RELEASE_FOR[@]}"} \
    -o "${OUTPUT_DIR}/${name}.md"
done

echo "==> Cleaning build artifacts from ${OUTPUT_DIR}/"
"${DOCTEST[@]}" clean "${OUTPUT_DIR}" --verbose

echo "==> Done. Rendered docs are in ${OUTPUT_DIR}/"
