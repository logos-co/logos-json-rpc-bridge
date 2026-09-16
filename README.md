# json_rpc_bridge

Exposes a configured set of Logos modules' **method calls** and **event
subscriptions** to external clients over **HTTP** and **WebSocket**, speaking
JSON-RPC 2.0. It also **describes itself**: every module built with a current
module-builder publishes its LIDL contract through `lidl()`, and the bridge
serves typed views and OpenRPC, OpenAPI and AsyncAPI documents built from it.

It exists because there is otherwise no way into a Logos node from outside the
module system: every consumer of a module's methods or events has to be a loaded
module itself, which leaves scripts, dashboards, test harnesses and other
language runtimes with no entry point.

```bash
logoscore call json_rpc_bridge start \
  '{"http":{"port":8645},"expose":{"modules":["storage_module"]}}'

# the alias form...
curl -s -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"storage_module.exists","params":{"cid":"zDv..."}}' \
  http://127.0.0.1:8645/rpc

# ...is exactly this rpc.call
curl -s -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"rpc.call",
       "params":{"module":"storage_module","method":"exists","params":{"cid":"zDv..."}}}' \
  http://127.0.0.1:8645/rpc
```

## What it is, and what it is not

It is an **ordinary application module doing egress**. Every upstream call goes
through the normal by-name client; it never injects framed transport messages.

`LOGOS-MODULE-TRANSPORT` §9.3 anticipates something like this and says a
request-oriented binding "would need a separate interoperability specification
that maps the directional message semantics from section 1.4 onto the carrier,
including authorization and contract-binding metadata, request/response
correlation, cancellation, subscriptions, event delivery, and error handling".
This document is that mapping — but the module **makes no conformance claim**,
and is not a transport profile.

**Scope is only what a normal module can already do**: call methods, subscribe
to events. Module lifecycle — loading, installing, unloading — is deliberately
absent and will not be added; it is the one surface that would turn a read/write
bridge into a code-execution one.

**Framing: a development and integration tool.** Not advertised as a production
trust boundary.

## Authority — read this before deploying it

Every bridged call executes with **`json_rpc_bridge`'s own authority**. There is
no delegated identity in the model, so an external client inherits the bridge's
aggregate reach over every exposed module. The bridge is a confused deputy by
construction, and its upstream grant is the exact authority ceiling for every
client that reaches it.

`expose.modules` is a **containment filter, not an authorization system**.

The compensating controls are the loopback-only bind, the mandatory Origin/Host
checks, and the optional bearer token.

## Configuration

Configured at start time, like `openmetrics-module`. Keys are `snake_case`.

```json
{
  "http": { "host": "127.0.0.1", "port": 8645, "allowed_origins": [] },
  "auth": { "mode": "none" },
  "expose": {
    "modules": [
      "temperature_module",
      { "name": "storage_module",
        "methods": { "deny": ["destroy"] },
        "events":  { "allow": ["storageUploadProgress", "storageUploadDone"] } }
    ]
  },
  "limits": { "max_connections": 128, "call_timeout_ms": 30000 },
  "discovery": { "revalidate_ms": 10000 }
}
```

| Key | Default | Notes |
|---|---|---|
| `http.host` | `127.0.0.1` | **Loopback only.** A routable address is refused at `start()`. |
| `http.port` | `8645` | |
| `http.allowed_origins` | `[]` | Empty **refuses any request carrying `Origin`**. |
| `auth.mode` | `none` | or `bearer`. The secret lives in the instance persistence dir, never in config. |
| `expose.modules` | *required* | Explicit list. **No wildcard.** |
| `limits.*` | see above | All positive integers. |
| `discovery.revalidate_ms` | `10000` | How often each module's live report is re-read (see *Revalidation*). `0` turns it off; 1–999 are refused. |

`expose.modules` entries are a bare string (expose everything) or an object with
`methods` / `events`, each taking `allow` and/or `deny`:

- an **absent** `allow` means *unconstrained*; an **empty** one permits *nothing*
- `deny` always subtracts, and wins over `allow`
- names are the members the module declares: method names, and event names
  exactly as the module emits them (`storageUploadProgress`)
- `lidl`, `name` and `version` are always callable (see *Self-description*): an
  `allow` list need not name them, and a `deny` list may not

`start()` refuses:
- a non-loopback host, or a port outside 1–65535;
- an empty module list, a module listed twice, and exposing `json_rpc_bridge`
  itself;
- a module named `rpc` (its aliases would be bridge operations), and module
  names containing `.`, whitespace or a control character (`.` separates module
  from method in an alias, and names are also URL path segments);
- a `methods.deny` that names `lidl`, `name` or `version`;
- a name listed twice in one `allow` or `deny`;
- `auth.token` / `auth.token_file` (the secret never travels through config);
- a non-positive `limits.*` value, and a `discovery.revalidate_ms` that is
  negative, not an integer, or between 1 and 999.

## Wire protocol

JSON-RPC 2.0 on both transports. A JSON-RPC `method` is either a bridge
operation, in the reserved `rpc.` namespace, or a **module call alias**,
`<module>.<method>`.

| Operation | `params` | Result |
|---|---|---|
| `rpc.call` | `{module, method, params}` | the method's return value |
| `<module>.<method>` | the method's params | exactly `rpc.call`'s |
| `rpc.subscribe` | `{subscription, module, event}` | `{subscription, operation, module, event, state}` |
| `rpc.unsubscribe` | `{subscription}` | `{subscription, operation}` |
| `rpc.schema` | `{module}` | the module's view (see *Self-description*) |
| `rpc.list_modules` | — | every exposed module's view, without `interface` |
| `rpc.discover` | — | this bridge's OpenRPC 1.3.2 document |
| `rpc.ping` | — | `"pong"` |
| `rpc.cancel` | `{id}` | see *Cancellation* |

Server-initiated notifications (WebSocket only): `rpc.event` and
`rpc.subscription_terminated`.

### Aliases

A `method` that does not start with `rpc.` is a call when its **first** `.` is
neither its first nor its last character. It splits there, so
`storage_module.exists` is `{module: "storage_module", method: "exists"}` and
`m.a.b` is method `a.b` of module `m`. Any other name is `-32601`, like an
unknown `rpc.` operation.

- Absent `params` mean `[]`; an object or an array passes through; `null` or a
  scalar is `-32602`.
- An alias is exactly `rpc.call`, in every discovery state: the same gating, the
  same by-name translation, and byte-identical refusals.
- Sent as a notification, an alias is refused with `-32600`, like `rpc.call`.
- Inside `rpc.call`'s own params a dotted `method` is taken literally.

`rpc.call` still takes module and method as separate fields, mirroring the
transport spec's `Request`. The alias was added because generated OpenRPC and
OpenAPI clients name operations `<module>.<method>`, and the two-field envelope
is clumsy by hand. The split is unambiguous because module names cannot contain
`.`, and `rpc` is not a valid module name.

### Params: by name or positional

The inner `params` may be an **object** (by name, the documented primary form,
matching the spec's map-valued params) or an **array** (positional). The shipped
ABI takes only an array, so the bridge translates by-name params:

- **typed modules** (`interface_status: ok`) follow the contract's declarations.
  A name the method does not declare, or a **missing required parameter**, is
  `-32602` with `data.invalid_params_detail.path` naming it. A missing optional
  (`? T`) parameter is sent as `null`.
- **other resolved modules** follow the live report's parameter names. An
  unknown name is `-32602` with its `path`; a missing parameter is sent as
  `null`.
- a module that has not been discovered yet (`pending`) has no names to follow,
  so by-name params are `-32602`; positional params still go through.

Positional arrays are passed through as they are. The provider checks their
arity, and a provider that refuses the arguments answers with a
`ProviderRejection` result (see *Errors*).

### Argument encoding

Values pass through untouched; the bridge does not inspect or validate them.
`bstr` travels as `{"_bytes":"<base64url, unpadded>"}` — note the **url**
alphabet: the underlying decoder *skips* characters outside it rather than
erroring, so standard base64 silently decodes to a different byte string, and
the bridge does not catch that for you. Do not name a map key `_bytes`.

### HTTP

`POST /rpc` — one request object or a batch array. `Content-Type:
application/json` is **required** (415 otherwise), which forces a CORS preflight
for any cross-origin caller; no CORS headers are ever sent, so the preflight
fails. A `POST` must also state its `Content-Length` and carry no
`Transfer-Encoding` (the bridge reads no chunked body); otherwise the answer
is 411.

`POST /modules/{module}/{method}` — the same two-field addressing in path form;
the body is the inner `params`, and an empty body means `{}`.

`GET /healthz`, `/modules`, `/modules/{module}`, `/openapi.json`,
`/asyncapi.json` — read-only, gated by the same Host, Origin and auth checks as
`/rpc` (enumeration is a distinct authority action from calling).

There are **no GET-based calls**: nothing in a module's interface declares a
method idempotent, so a mutating GET would be both wrong and a CSRF surface.

JSON-RPC failures ride in the body at HTTP 200. 4xx/5xx are transport-level
only: 401, 403, 411 and 415 come before any routing, each as a small HTML page,
and the request's body is never read. So if the request declares one (a
`Content-Length` other than 0, any `Transfer-Encoding`, or a `POST` with
neither), the page says `Connection: close` and the connection closes after it;
otherwise, as for a refused `GET`, the connection stays usable for the next
request. A `GET` sent with a body is answered, then closed the same way.

A request waits for its answer as long as its calls take — up to
`limits.call_timeout_ms` each, and a batch answers when its slowest call does.
Nothing on the HTTP side cuts a slow answer short, whether or not the request
had a body.

Notifications (an absent `id`) are **refused for `rpc.call` and its aliases**
with `-32600`: there is no notification concept upstream — every call gets
exactly one reply — so a fire-and-forget module call would silently discard
errors. Other bridge operations sent without an `id` still run and are answered
with `id: null`. Note that `id: null` is itself a valid id that gets a response
echoing `null`; only an *absent* `id` is a notification.

### WebSocket

`ws://<host>:<port>/ws`, subprotocol `jsonrpc-bridge.v1`. Text frames only
(binary closes 1003); continuations are reassembled up to `max_frame_bytes`.

**Subscription ids are assigned by the client**, per the transport spec, so a
client can correlate without waiting for the ack. Re-subscribing the same id is
idempotent and never double-delivers. A subscribe that fails, or a subscription
that is terminated, frees its id, so retrying with the same id subscribes
afresh.

```json
{"jsonrpc":"2.0","id":1,"method":"rpc.subscribe",
 "params":{"subscription":"s1","module":"storage_module",
           "event":"storageUploadProgress"}}
```

The ack echoes the subscription:

```json
{"jsonrpc":"2.0","id":1,
 "result":{"subscription":"s1","operation":"subscribe","module":"storage_module",
           "event":"storageUploadProgress","state":"registered"}}
```

`state` is `"registered"` for a new subscription — **not** `"active"`: the
upstream subscription is held and arms when the provider appears, so claiming it
is live would be a promise this layer cannot keep. It is `"active"` only when
this connection already held the id, in which case nothing new was registered.

Events arrive as:

```json
{"jsonrpc":"2.0","method":"rpc.event",
 "params":{"subscription":"s1","module":"storage_module",
           "event":"storageUploadProgress",
           "data":["{\"sessionId\":\"...\",\"bytes\":1024}"],"generation":1,"ts":1750000000123}}
```

`data` is the upstream payload verbatim — a positional array, not
self-describing. For a typed module, the event's parameter names and types are
in `rpc.schema`'s `interface` (`events[].params`), and `/asyncapi.json` types
`data` as a tuple. For an untyped module the bridge knows only event names.
`ts` is `uint64` epoch milliseconds.

### Subscription loss

When a provider goes away or is replaced, its subscriptions are **terminated,
not silently resumed**:

```json
{"jsonrpc":"2.0","method":"rpc.subscription_terminated",
 "params":{"subscription":"s1","module":"storage_module",
           "event":"storageUploadProgress",
           "reason":"provider_unavailable"}}
```

- `provider_unavailable`: the protocol reported the provider lost.
- `provider_changed`: revalidation found a different build answering for the
  module (see *Revalidation*).

A re-established subscription is a *new* one and the events in between are
unrecoverable, so the client is told and decides whether to re-subscribe and
refetch state. **This requires logos-protocol ≥ 0.9**; below that the bridge
cannot detect a provider restart and the stream resumes with a silent gap.
`getInfo()` reports `subscription_continuity` so you can see which behaviour you
have.

### Errors

Standard JSON-RPC codes, with the Logos taxonomy carried losslessly in
`error.data`:

```json
{"code":-32002,"message":"upstream call timed out",
 "data":{"logos_error_code":6,"logos_error_name":"TIMEOUT"}}
```

| JSON-RPC | Logos | When |
|---|---|---|
| `-32700` | `INVALID_PARAMS` | the body is not JSON |
| `-32600` | `INVALID_PARAMS` | not a JSON-RPC request; a module call sent as a notification |
| `-32601` | `METHOD_NOT_FOUND` | not exposed, denied, unknown module, unknown method or operation |
| `-32602` | `INVALID_PARAMS` | bad params (with `invalid_params_detail` for by-name ones) |
| `-32603` | `MODULE_ERROR` | an upstream failure the bridge has no specific code for |
| `-32000` | `MODULE_ERROR` | could not be dispatched |
| `-32001` | `NOT_READY` | module unavailable |
| `-32002` | `TIMEOUT` | deadline elapsed |
| `-32003` | `TRANSPORT_ERROR` | connection failed mid-call |
| `-32004` | `NOT_AUTHORISED` | the call was not authorised upstream |
| `-32006` | `NOT_READY` | the bridge is shutting down |
| `-32029` | `NOT_READY` | too many requests in a batch, or subscriptions on a connection |

Two properties worth stating explicitly:

**An application-level failure is a success.** Only a transport/timeout/auth
failure becomes a JSON-RPC error. A module returning
`{success:false, error:"..."}` *answered the call*, and its payload arrives in
`result` untouched. The bridge never inspects a result and promotes it. The same
holds for a `ProviderRejection` (`{code, message, origin}`), which a provider
returns when it refuses the arguments.

**Denial blocks calls; the contract is public.** Not-loaded, not-exposed,
denied-by-config and does-not-exist calls all produce a byte-identical `-32601`,
and upstream error text is never forwarded (it routinely carries nix store and
socket paths). What a denial does not hide is the contract: `rpc.schema` serves
an exposed module's full interface, and its `exposure` lists what this bridge
lets you call. See *Self-description*.

### Cancellation

`rpc.cancel` stops the *response*, not the work: the shipped ABI has no cancel
primitive, so this is bridge-local bookkeeping. The bridge never auto-retries a
failed upstream call, and a cancelled call is not proof the operation had no
effect.

## Self-description

A module built with logos-module-builder ≥ `ca52a38` answers the derived
built-in **`lidl()`** with its canonical LIDL contract. That text is
byte-identical to the module's `#lidl` build output and to the
`assets/lidl/<module>.lidl` its package carries. The bridge reads it and serves
typed views and documents from it. Modules built earlier stay callable,
names-only.

### Discovery and statuses

For each exposed module the bridge reads the module's own `getPluginInterface`
answer (its *live report*, names only). If that lists a zero-argument `lidl`,
it calls `lidl()` (ignoring method policy), reads the contract with the
logos-lidl revision in `getInfo().lidl_reader`, calls `version()`, and
cross-checks the contract against the live report.

| `interface_status` | Meaning |
|---|---|
| `pending` | Not discovered yet: the module is unreachable, or discovery is still running. Retried at 0.5 s, doubling to 30 s. |
| `ok` | Typed. The contract parsed, validated, names this module, and passed the cross-check. Calls and subscriptions follow its declarations. |
| `untyped` | The module lists no `lidl` (built before `lidl()`). Names-only, as before. |
| `invalid` | `lidl()` exists but its answer cannot be served; `interface_error` says why. The module is served names-only. |

`invalid` covers: the `lidl()` call failing or timing out (retried), an answer
that is not a string, is over 4 MiB or is not UTF-8, a contract that does not
parse or validate (`interface_error` gives `line:col` and the reader revision),
a contract for another module, and a cross-check error.

### `rpc.schema` and `GET /modules/{module}`

```bash
curl -s -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"rpc.schema","params":{"module":"storage_module"}}' \
  http://127.0.0.1:8645/rpc
```

| Key | |
|---|---|
| `module`, `interface_status`, `stale`, `resolved` | the view's state; `stale` means the provider went away and a rediscovery is due |
| `source`, `authoritative` | `"lidl"` when ok, `"getPluginInterface"` otherwise; always `false` |
| `interface` | `ok` only: the **complete** contract, as `lidl json --identity` prints it (with `name`, `version` and `lidl` injected), never filtered by policy |
| `exposure` | `{methods, events}` this bridge lets clients call and subscribe to: the policy applied to the declarations (or to the live names when not ok), with the built-ins always included |
| `interface_sha256` | see *Digests*; null unless ok |
| `contract_sha256` | see *Digests*; null until `lidl()` bytes were read |
| `interface_error` | `invalid` only |
| `cross_check` | `{state, findings[]}` once a contract was checked, else null |
| `methods`, `events`, `events_declared` | the live report's names, filtered by policy |

`rpc.list_modules` and `GET /modules` carry the same views without `interface`.
Unexposed and unknown modules are `-32601` (a 404 over HTTP).

**Policy restricts calls, not knowledge.** Method and event policy decide only
what can be called or subscribed to; the served contract is always the whole
one, and `interface_sha256` does not depend on policy. A typed method is
callable iff it is in `exposure.methods`; a typed event is subscribable iff it
is in `exposure.events`. `getPluginInterface` and its siblings stay refused for
clients: `rpc.schema` and `lidl()` replace them.

**Built-ins.** `lidl`, `name` and `version` are callable on every exposed module,
by `rpc.call` or alias, whatever the method policy — for a typed module, and for
any other module whose live report lists them. `storage_module.lidl` returns the
exact contract text.

### Digests

- `contract_sha256`: SHA-256 of the exact `lidl()` bytes. It equals the
  SHA-256 of the module's `#lidl` output and of its package's
  `assets/lidl/<module>.lidl`, so the text can be checked without being fetched.
- `interface_sha256`: SHA-256 of `interface` as canonical JSON — sorted keys,
  `,` and `:` separators, raw UTF-8. In Python:
  `json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=False)`.
  `tests/fixtures/interface-digest-vectors.json` pins the encoding.

### Cross-check

The contract is compared with the live report by names and arity only (live
type spellings are host names such as `QString`).

| Severity | Codes | Effect |
|---|---|---|
| error | `declared_method_missing`, `method_arity_mismatch`, `declared_event_missing`, `event_arity_mismatch` | `invalid`: the contract does not describe the running binary |
| warning | `undeclared_method`, `undeclared_event`, `param_names_differ`, `version_mismatch`, `non_canonical`, `contract_lint` | stays `ok` |
| info | `derived_method_unlisted`, `events_untagged`, `runtime_version_unavailable` | stays `ok` |

Events are checked only when the live report tags them; legacy Qt plugins do not
(`events_declared: false`), and their `events.allow` list is the manual
override for subscribing.

### Revalidation

Every `discovery.revalidate_ms` the bridge re-reads each settled module's live
report. The comparison covers the whole answer, type spellings included.

- **Changed:** a different build answers for the module. The view goes stale,
  a full rediscovery runs, and every client subscription to the module is
  terminated with `provider_changed`.
- **Unreachable:** the view goes stale and rediscovery backs off as above.
- **Unchanged:** nothing happens.

Rediscovery also starts when the protocol reports a provider lost (subscriptions
end with `provider_unavailable`) and when a bridged call finds the module
unavailable. A reload can therefore move a module between typed and untyped.

**Limitation: a fast reload of the same build is invisible.** A reload onto an
identical live report, finished inside the protocol's 1 s liveness poll, changes
nothing the bridge can see. Its events keep flowing on the old subscription,
with the same generation and no termination. The fix belongs in logos-protocol,
which is to report a replica leaving its valid state when that happens instead
of sampling it once a second; a bridge built on that protocol will terminate
these subscriptions too.

### Documents

| Route | Document | Schemas |
|---|---|---|
| `rpc.discover` | OpenRPC 1.3.2 | JSON Schema draft-07 |
| `GET /openapi.json` | OpenAPI 3.1.1 | JSON Schema 2020-12 |
| `GET /asyncapi.json` | AsyncAPI 3.0.0 (the WebSocket endpoint) | draft-07-compatible |

```bash
curl -s http://127.0.0.1:8645/openapi.json
curl -s http://127.0.0.1:8645/asyncapi.json
curl -s -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"rpc.discover"}' http://127.0.0.1:8645/rpc
```

There are **no CORS headers**, so a browser page on another origin cannot read
these; fetch them from a script or a tool, or add the page's origin to
`http.allowed_origins` and serve it through your own proxy.

- **Operations cover the exposed surface only**: OpenRPC methods
  `<module>.<method>`, OpenAPI paths `/modules/<module>/<method>`, AsyncAPI
  request/result and subscribe/event messages. A generated client never contains
  a call this bridge refuses.
- **Component schemas cover every record** in every typed contract, used or
  not.
- Types: `tstr` → string, `bstr` → the `Bytes` tag object, `int`/`uint` → integer
  with int64/uint64 bounds, `float64` → number, `bool` → boolean, `any` → `{}`,
  `result` → `LogosResult`, `[T]` → array, `{tstr: V}` → object, `? T` → T or
  null (an optional record field is not `required`), a record →
  `#/components/schemas/<module>.<Type>`.
- A method's result is `anyOf [T, ProviderRejection]`; a method with no return
  answers `true` (`{"const": true}`).
- Untyped and invalid modules get names-only operations with `{}` schemas and
  `x-logos-untyped`. Pending modules appear only in `x-logos-modules`.
- Every `$ref` resolves inside its own document.

Extensions:

| Key | Where | |
|---|---|---|
| `x-logos-modules` | all three | every exposed module: `status`, `exposure`, the digests (ok) and `interface_error` (invalid) |
| `x-logos-module`, `x-logos-method`, `x-logos-event` | operations and messages | the member an operation targets |
| `x-logos-derived` | operations | the injected built-ins `name`, `version`, `lidl` |
| `x-logos-untyped` | operations, messages, events | names-only |
| `x-logos-bridge-op` | operations | the bridge's own `rpc.*` operations and routes |
| `x-logos-events` | OpenRPC | subscribable events, with a typed `dataSchema` tuple |
| `x-logos-params` | AsyncAPI event messages | the event's parameter names, in `data` order |
| `x-logos-ws-close-codes` | AsyncAPI | 1003, 1006, 1008, 1009 and what each means |
| `x-logos-unknown-type`, `x-logos-map-key-type` | schemas | a type spelling the mapping does not know; a map key that is not `tstr` |

The documents are rebuilt off the socket thread whenever a view changes, and
served as immutable snapshots; `getInfo().docs` says which.

### Offline: `json-rpc-bridge-docs`

The same documents, rendered from contracts without a running node:

```bash
nix build .#json-rpc-bridge-docs
json-rpc-bridge-docs --config bridge.json --format openapi \
  --lgx storage_module.lgx > openapi.json
json-rpc-bridge-docs --config bridge.json --format interface --module storage_module \
  --lidl storage_module=storage_module.lidl | sha256sum     # = interface_sha256
```

```
json-rpc-bridge-docs --config PATH --format openrpc|openapi|asyncapi|schema|interface
                     [--module NAME] (--lidl NAME=FILE | --lidl-dir DIR | --lgx FILE)...
                     [--server-host H] [--server-port P] [--info-version V]
                     [--allow-untyped] [--out PATH] [--pretty] [--version]
```

- For typed modules the output is what a bridge started with that config
  serves: every module is taken as resolved and consistent with its contract
  (`ok`, `cross_check: null`).
- `schema` prints the `rpc.schema` view of `--module`, or the
  `rpc.list_modules` views without it. `interface` prints the canonical JSON
  that `interface_sha256` is taken over.
- `--lgx` reads `assets/lidl/<name>.lidl` through logos-package's C API
  (`lgx_verify`, `lgx_load`, extraction into a private temporary directory);
  a package also carries its dependencies' contracts, so files are picked by
  module name.
- An exposed module without a contract exits 5 unless `--allow-untyped`
  leaves it out (offline, its names are unknown).
- Exit codes: 0 ok, 2 usage, 3 config rejected, 4 contract unreadable or
  invalid (with discovery's messages), 5 untyped, 6 write error.

## `getInfo()`

| Key | |
|---|---|
| `running`, `version` | whether a server is up; this bridge's version (also `info.version` in the documents) |
| `http`, `ws`, `auth` | the endpoints and auth mode (while running) |
| `modules` | the configured policy per module |
| `connections`, `subscriptions`, `upstream_subscriptions` | live counts |
| `protocol_version`, `subscription_continuity` | the logos-protocol in use, and whether it can report a provider loss |
| `lidl_reader` | `"lidl <version> (<rev>)"`, the logos-lidl that reads contracts |
| `discovery` | `{revalidate_ms}` |
| `docs` | `{generation, built_at (ms), build_ms, sizes {openrpc, openapi, asyncapi}, build_failures}` of the served snapshot |

## Security

- **Loopback only.** A routable `http.host` is refused at `start()`, not warned
  about. Reach it from elsewhere with an SSH tunnel or a reverse proxy that
  terminates its own authentication.
- **Origin and Host are checked on every request**, including the WebSocket
  upgrade and the document routes. This is not optional: a loopback bind is
  *not* a boundary against a browser, which does not apply same-origin policy
  to WebSockets and sends no preflight. Without it, any page the operator
  visits could drive the bridge. Host matching also closes DNS rebinding. No
  route sends CORS headers.
- **Bearer token** (`auth.mode: "bearer"`) is read from the module's instance
  persistence directory — never from config, which gets logged and pasted into
  issues. Compared in constant time against a stored digest.
- Bounded connections (total and per peer), in-flight calls, subscriptions,
  body and frame sizes. A connection counts once against the per-peer bound,
  however many HTTP requests it carries, and every loopback client is the same
  peer. A slow WebSocket reader is **closed**, not silently starved of events:
  one upstream subscription feeds many clients, so the producer cannot be
  back-pressured and dropping events would be a silent gap. An HTTP response
  goes out in 16 KiB slices, and the connection is dropped if the reader takes
  none for 15 s.

## Limits

- Every bridged call authenticates as the bridge (see **Authority**).
- Under `--access-policy enforce` a `dependencies: []` module is denied every
  target, because the derived allow-list is a target's loaded dependents plus a
  hardcoded trusted set with no knob to extend it. Enforcement is off by default
  in both shipped frontends; when on, calls answer `NOT_AUTHORISED`.
- Subscription-loss notification needs logos-protocol ≥ 0.9, and a fast reload
  of the same build is not detected (see *Revalidation*).
- Module introspection is ungated upstream — `getPluginInterface` and `lidl()`
  answer before the authorization gate by design, and discovery uses them
  whatever the method policy. The bridge's auth on `/modules` and the documents
  is its own containment policy, not a privileged capability.
- Windows is unproven: libwebsockets evaluates for mingw but is not built here.
  CI is Linux and macOS.

## Building

```bash
nix build                            # the plugin
nix build .#lgx                      # an installable package
nix build .#json-rpc-bridge-docs     # the offline renderer
nix flake check                      # unit-tests, docs-metaschema, docs-golden
```

- `unit-tests`: the pure layers, the document builders against their goldens,
  and the renderer in-process.
- `docs-metaschema`: the goldens against vendored OpenRPC, OpenAPI and AsyncAPI
  meta-schemas.
- `docs-golden`: the renderer reproduces the goldens from `.lidl` files and from
  packages built with the `lgx` CLI.

`tests/fixtures/SOURCES.md` records where every fixture came from and how to
regenerate the goldens.
