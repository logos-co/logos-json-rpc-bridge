# json_rpc_bridge

Exposes a configured set of Logos modules' **method calls** and **event
subscriptions** to external clients over **HTTP** and **WebSocket**, speaking
JSON-RPC 2.0.

It exists because there is otherwise no way into a Logos node from outside the
module system: every consumer of a module's methods or events has to be a loaded
module itself, which leaves scripts, dashboards, test harnesses and other
language runtimes with no entry point.

```bash
logoscore call json_rpc_bridge start \
  '{"http":{"port":8645},"expose":{"modules":["storage_module"]}}'

curl -s -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"rpc.call",
       "params":{"module":"storage_module","method":"get","params":{"key":"k-1"}}}' \
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
        "methods": { "deny": ["wipe"] },
        "events":  { "allow": ["storage.upload_progress_event"] } }
    ]
  },
  "limits": { "max_connections": 128, "call_timeout_ms": 30000 }
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

`expose.modules` entries are a bare string (expose everything) or an object with
`methods` / `events`, each taking `allow` and/or `deny`:

- an **absent** `allow` means *unconstrained*; an **empty** one permits *nothing*
- `deny` always subtracts, and wins over `allow`
- **event names are full schema identifiers** (`storage.upload_progress_event`),
  not bare names — the schema namespace is not derivable from the module name

`start()` refuses: a non-loopback host, an empty module list, and exposing
`json_rpc_bridge` itself.

## Wire protocol

JSON-RPC 2.0 on both transports. **Module and method are separate fields, never
dot-joined** — this mirrors the transport spec's `Request`, keeps calls
symmetric with subscriptions, and means there is no split rule to get wrong.

The JSON-RPC `method` always names a bridge operation in the reserved `rpc.`
namespace. Anything else is `METHOD_NOT_FOUND`.

| Operation | `params` | Result |
|---|---|---|
| `rpc.call` | `{module, method, params}` | the method's return value |
| `rpc.subscribe` | `{subscription, module, event}` | `{subscription, operation, state}` |
| `rpc.unsubscribe` | `{subscription}` | `{subscription, operation}` |
| `rpc.schema` | `{module}` | the module's bridge-derived view |
| `rpc.list_modules` | — | every exposed module's view |
| `rpc.ping` | — | `"pong"` |
| `rpc.cancel` | `{id}` | see *Cancellation* |

Server-initiated notifications (WebSocket only): `rpc.event` and
`rpc.subscription_terminated`.

### Params: by name or positional

The inner `params` may be an **object** (by name, the documented primary form,
matching the spec's map-valued params) or an **array** (positional). The bridge
translates by-name into positional internally, because the shipped ABI takes
only an array. A name not in the method's signature is `-32602` with
`data.invalid_params_detail`.

Positional stays supported as the fallback for a module whose introspection is
unavailable or stale.

### Argument encoding

Values pass through untouched. `bstr` travels as `{"_bytes":"<base64url,
unpadded>"}` — note **url** alphabet: the underlying decoder *skips* characters
outside it rather than erroring, so standard base64 would silently decode to a
different byte string. The bridge validates this at the edge and answers
`-32602`. Do not name a map key `_bytes`.

### HTTP

`POST /rpc` — one request object or a batch array. `Content-Type:
application/json` is **required** (415 otherwise), which forces a CORS preflight
for any cross-origin caller; no CORS headers are ever sent, so the preflight
fails.

`POST /modules/{module}/{method}` — the same two-field addressing in path form;
the body is the inner `params`.

`GET /healthz`, `/modules`, `/modules/{module}` — read-only, gated by the same
auth as `/rpc` (enumeration is a distinct authority action from calling).

There are **no GET-based calls**: nothing in a module's interface declares a
method idempotent, so a mutating GET would be both wrong and a CSRF surface.

JSON-RPC failures ride in the body at HTTP 200. 4xx/5xx are transport-level
only.

Notifications (an absent `id`) are **refused for `rpc.call`**: there is no
notification concept upstream — every call gets exactly one reply — so a
fire-and-forget module call would silently discard errors. Note that `id: null`
is a valid id that gets a response echoing `null`; only an *absent* `id` is a
notification.

### WebSocket

`ws://<host>:<port>/ws`, subprotocol `jsonrpc-bridge.v1`. Text frames only
(binary closes 1003); continuations are reassembled up to `max_frame_bytes`.

**Subscription ids are assigned by the client**, per the transport spec, so a
client can correlate without waiting for the ack. Re-subscribing the same id is
idempotent and never double-delivers.

```json
{"jsonrpc":"2.0","id":1,"method":"rpc.subscribe",
 "params":{"subscription":"s1","module":"storage_module",
           "event":"storage.upload_progress_event"}}
```

The ack reports `"state": "registered"` — **not** `"active"`. The upstream
subscription is held and arms when the provider appears, so claiming it is live
would be a promise this layer cannot keep.

Events arrive as:

```json
{"jsonrpc":"2.0","method":"rpc.event",
 "params":{"subscription":"s1","module":"storage_module",
           "event":"storage.upload_progress_event",
           "data":[42],"generation":1,"ts":1750000000123}}
```

`data` is the upstream payload verbatim — a positional array, not
self-describing. Join parameter names in from `rpc.schema`. `ts` is `uint64`
epoch milliseconds.

### Subscription loss

When a provider restarts, its subscription is **terminated, not silently
resumed**:

```json
{"jsonrpc":"2.0","method":"rpc.subscription_terminated",
 "params":{"subscription":"s1","module":"storage_module",
           "event":"storage.upload_progress_event",
           "reason":"provider_unavailable"}}
```

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
| `-32601` | `METHOD_NOT_FOUND` | not exposed, denied, unknown module, unknown method |
| `-32602` | `INVALID_PARAMS` | bad params |
| `-32001` | `NOT_READY` | module unavailable |
| `-32002` | `TIMEOUT` | deadline elapsed |
| `-32003` | `TRANSPORT_ERROR` | connection failed mid-call |
| `-32004` | `NOT_AUTHORISED` | provider rejected the call |
| `-32000` | `MODULE_ERROR` | could not be dispatched |

Two properties worth stating explicitly:

**An application-level failure is a success.** Only a transport/timeout/auth
failure becomes a JSON-RPC error. A module returning
`{success:false, error:"..."}` *answered the call*, and its payload arrives in
`result` untouched. The bridge never inspects a result and promotes it.

**Denial is non-revealing.** Not-loaded, not-exposed, denied-by-config and
does-not-exist all produce a byte-identical `-32601`, and upstream error text is
never forwarded (it routinely carries nix store and socket paths). Otherwise the
bridge would be an oracle for what a node runs and what it has been told to
hide.

### Cancellation

`rpc.cancel` stops the *response*, not the work: the shipped ABI has no cancel
primitive, so this is bridge-local bookkeeping. The bridge never auto-retries a
failed upstream call, and a cancelled call is not proof the operation had no
effect.

## Security

- **Loopback only.** A routable `http.host` is refused at `start()`, not warned
  about. Reach it from elsewhere with an SSH tunnel or a reverse proxy that
  terminates its own authentication.
- **Origin and Host are checked on every request**, including the WebSocket
  upgrade. This is not optional: a loopback bind is *not* a boundary against a
  browser, which does not apply same-origin policy to WebSockets and sends no
  preflight. Without it, any page the operator visits could drive the bridge.
  Host matching also closes DNS rebinding.
- **Bearer token** (`auth.mode: "bearer"`) is read from the module's instance
  persistence directory — never from config, which gets logged and pasted into
  issues. Compared in constant time against a stored digest.
- Bounded connections (total and per peer), in-flight calls, subscriptions,
  body and frame sizes. A slow WebSocket reader is **closed**, not silently
  starved of events: one upstream subscription feeds many clients, so the
  producer cannot be back-pressured and dropping events would be a silent gap.

## Discovery

`/modules` and `rpc.schema` are built from an ordinary by-name call to
`getPluginInterface`. That is the target module's **unvalidated self-report**,
not something the runtime checked, so the result is presented as a
bridge-derived *view* (`"authoritative": false`) and filtered to what is
exposed.

Legacy hand-written Qt plugins do not tag their events, so their event list
comes back empty and `events_declared` is `false`. The per-module
`events.allow` list is the manual override for that case.

## Limits

- Every bridged call authenticates as the bridge (see **Authority**).
- Under `--access-policy enforce` a `dependencies: []` module is denied every
  target, because the derived allow-list is a target's loaded dependents plus a
  hardcoded trusted set with no knob to extend it. Enforcement is off by default
  in both shipped frontends; when on, calls answer `NOT_AUTHORISED`.
- Subscription-loss notification needs logos-protocol ≥ 0.9.
- Module introspection is ungated upstream — the `getPlugin*` calls answer
  before the authorization gate by design. The bridge's auth on `/modules` is
  its own containment policy, not a privileged capability.
- Windows is unproven: libwebsockets evaluates for mingw but is not built here.
  CI is Linux and macOS.

## Building

```bash
nix build              # the plugin
nix build .#lgx        # an installable package
nix flake check        # unit tests
```
