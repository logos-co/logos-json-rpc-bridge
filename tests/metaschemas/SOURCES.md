# Vendored meta-schemas

`tests/docs_metaschema.py` validates the golden documents against these files
with no network access. The JSON Schema draft-07 and 2020-12 meta-schemas they
refer to come from the `jsonschema-specifications` package that ships with
`jsonschema` in the pinned nixpkgs (jsonschema 4.25.0, referencing 0.36.2,
jsonschema-specifications 2025.4.1 at nixpkgs `e9f00bd`).

All files are unmodified copies, retrieved 2026-09-16. Every source project is
licensed Apache-2.0.

| File | `$id` | sha256 | Source |
|---|---|---|---|
| `openrpc/open-rpc-meta-schema-1.14.9.json` | `https://meta.open-rpc.org/` | `e5f534fb0dd9dd3598d26414e278a8c96e923ee7b54bef7009c499f4c71d0f82` | https://github.com/open-rpc/meta-schema/releases/download/1.14.9/open-rpc-meta-schema.json (open-rpc/meta-schema, Apache-2.0). Latest release; its `openrpc` enum accepts `1.3.2`. |
| `openrpc/json-schema-tools-meta-schema-1.7.5.json` | `https://meta.json-schema.tools/` | `ebaa0daa2efb3f8247ae62f6fea63a11a73fa7f6c9663c7cc3f38430d147a366` | https://github.com/json-schema-tools/meta-schema/releases/download/1.7.5/schema.json (json-schema-tools/meta-schema, Apache-2.0). The OpenRPC schema's `JSONSchema` definition is a `$ref` to it. 1.7.5 was current when OpenRPC 1.14.9 was released; 1.8.0 is byte-identical. |
| `openapi/schema-base-2022-10-07.json` | `https://spec.openapis.org/oas/3.1/schema-base/2022-10-07` | `14cd42cfaba6b4700b5995e60b6e0bf8ed9d800f996b00d22fa91fad21b4442c` | https://spec.openapis.org/oas/3.1/schema-base/2022-10-07 (OAI, Apache-2.0) |
| `openapi/schema-2022-10-07.json` | `https://spec.openapis.org/oas/3.1/schema/2022-10-07` | `da01ba28852cac0de53893797cb8d1942bc3b05084f526dcc216717dec314ed0` | https://spec.openapis.org/oas/3.1/schema/2022-10-07 |
| `openapi/schema-2024-11-14.json` | `https://spec.openapis.org/oas/3.1/schema/2024-11-14` | `94bf183558f36947cf1c7c17492105b484f8a479c447cd026d13b10d38eabf88` | https://spec.openapis.org/oas/3.1/schema/2024-11-14 |
| `openapi/dialect-base.json` | `https://spec.openapis.org/oas/3.1/dialect/base` | `8a0e89e365dadbebce2921ce6244340c1090e9d544c60d977e9ad6b97a61227b` | https://spec.openapis.org/oas/3.1/dialect/base |
| `openapi/meta-base.json` | `https://spec.openapis.org/oas/3.1/meta/base` | `267a88226e64e96dfc8c89dbd7e863160c84715e0fb893ca1d9fbf9f830f1f54` | https://spec.openapis.org/oas/3.1/meta/base |
| `asyncapi/asyncapi-3.0.0.json` | `http://asyncapi.com/definitions/3.0.0/asyncapi.json` | `d4571a420e6ffb7fcc7066c95a6db1202f299a3c51daa103d0706bf30f95e626` | https://raw.githubusercontent.com/asyncapi/spec-json-schemas/v6.11.1/schemas/3.0.0.json (asyncapi/spec-json-schemas, Apache-2.0; identical to `@asyncapi/specs@6.11.1`). The bundled schema, `$id`s kept. |

## Which OpenAPI 3.1 iteration

OpenAPI 3.1.1 names its dialect `https://spec.openapis.org/oas/3.1/dialect/base`,
and that is what the document's `jsonSchemaDialect` says. The published
iterations disagree with each other:

- `schema-base/2022-10-07` pins `jsonSchemaDialect` to `dialect/base`. It is the
  `schemas/v3.1/schema-base.yaml` at the OAS `3.1.1` git tag, and it validates every
  Schema Object against `dialect/base` + `meta/base`.
- `schema/2024-11-14` is structurally the `schemas/v3.1/schema.yaml` at that tag
  (only `$comment`, `$id` and the dialect default differ), so it is the 3.1.1
  document structure. Its companion `schema-base/2024-11-14` pins
  `jsonSchemaDialect` to `dialect/2024-10-25` instead, which contradicts the
  3.1.1 text, so it is not used.

The check validates each OpenAPI document against both `schema-base/2022-10-07`
and `schema/2024-11-14`, unmodified.
