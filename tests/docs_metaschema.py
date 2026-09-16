"""Validate the bridge's self-description documents against vendored meta-schemas, offline.

Also: the dialect lint, $refs that must resolve inside their own document, and operations only
for exposed (and not denied) members.
"""

from __future__ import annotations

import argparse
import copy
import json
import pathlib
import re
import sys
import urllib.parse

import jsonschema
import jsonschema_specifications
from jsonschema.exceptions import best_match
from referencing.jsonschema import DRAFT7, DRAFT202012

KINDS = ("openrpc", "openapi", "asyncapi")
HERE = pathlib.Path(__file__).resolve().parent
REQUIRED_CONTEXTS = ("full_api", "full_api_ext_policy", "storage", "multi")
TYPED_STATUSES = ("ok", "untyped", "invalid")
NAME_MAPS = {"properties", "patternProperties", "$defs", "definitions", "dependentSchemas"}
COMPONENT_KEY = re.compile(r"[a-zA-Z0-9.\-_]+")  # all three specs, for every components map

# Golden contexts built with a policy: denied members, and records that must stay documented.
DENIED = {
    "full_api_ext_policy": {
        "methods": {("test_fullapi_ext_cpp", "echoWrapper")},
        "events": {("test_fullapi_ext_cpp", "blobEvent")},
        "components": {"test_fullapi_ext_cpp.Wrapper", "test_fullapi_ext_cpp.Blob"},
    },
    "storage": {
        "methods": {("storage_module", "destroy")},
        "events": set(),
        "components": set(),
    },
}


def load(path: pathlib.Path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def build_validators(ms: pathlib.Path):
    orpc = load(ms / "openrpc" / "open-rpc-meta-schema-1.14.9.json")
    jst = load(ms / "openrpc" / "json-schema-tools-meta-schema-1.7.5.json")
    oas_base = load(ms / "openapi" / "schema-base-2022-10-07.json")
    oas_2022 = load(ms / "openapi" / "schema-2022-10-07.json")
    oas_2024 = load(ms / "openapi" / "schema-2024-11-14.json")
    dialect = load(ms / "openapi" / "dialect-base.json")
    meta = load(ms / "openapi" / "meta-base.json")
    aapi = load(ms / "asyncapi" / "asyncapi-3.0.0.json")

    jst_resource = DRAFT7.create_resource(jst)
    resources = [
        (orpc["$id"], DRAFT7.create_resource(orpc)),
        # The OpenRPC schema refers to it without the trailing slash of its $id.
        ("https://meta.json-schema.tools", jst_resource),
        (jst["$id"], jst_resource),
        (aapi["$id"], DRAFT7.create_resource(aapi)),
    ]
    resources += [(d["$id"], DRAFT202012.create_resource(d))
                  for d in (oas_base, oas_2022, oas_2024, dialect, meta)]
    registry = jsonschema_specifications.REGISTRY.with_resources(resources).crawl()

    def v7(schema):
        return jsonschema.Draft7Validator(schema, registry=registry)

    def v2020(schema):
        return jsonschema.Draft202012Validator(schema, registry=registry)

    return {
        "openrpc": [("OpenRPC meta-schema 1.14.9", v7(orpc))],
        "openapi": [
            # Validates Schema Objects against the dialect the document names (dialect/base).
            ("OAS 3.1 schema-base 2022-10-07", v2020(oas_base)),
            # The structure published with OAS 3.1.1 (same as schemas/v3.1 at the 3.1.1 tag).
            ("OAS 3.1 schema 2024-11-14", v2020(oas_2024)),
        ],
        "asyncapi": [("AsyncAPI 3.0.0", v7(aapi))],
    }


def clip(text: str, limit: int = 300) -> str:
    return text if len(text) <= limit else text[:limit] + "..."


def describe(err) -> str:
    line = f"{err.json_path}: {clip(err.message)}"
    if err.context:
        best = best_match(err.context)
        line += f" [best: {best.json_path}: {clip(best.message)}]"
    return line


def escape(token) -> str:
    return str(token).replace("~", "~0").replace("/", "~1")


def lint(kind: str, doc) -> list[str]:
    """Dialect lint: draft-07 documents have no prefixItems, 2020-12 ones no
    additionalItems or array-valued items; nothing sits next to $ref."""
    problems = []

    def visit(node, path: str, names_only: bool):
        if isinstance(node, dict):
            if not names_only:
                if "$ref" in node and len(node) != 1:
                    problems.append(f"{path or '/'}: $ref has siblings {sorted(node)}")
                if kind in ("openrpc", "asyncapi") and "prefixItems" in node:
                    problems.append(f"{path}: prefixItems in a draft-07 document")
                if kind == "openapi" and "additionalItems" in node:
                    problems.append(f"{path}: additionalItems in a 2020-12 document")
                if kind == "openapi" and isinstance(node.get("items"), list):
                    problems.append(f"{path}: array-valued items in a 2020-12 document")
            for key, value in node.items():
                visit(value, f"{path}/{escape(key)}", not names_only and key in NAME_MAPS)
        elif isinstance(node, list):
            for i, value in enumerate(node):
                visit(value, f"{path}/{i}", False)

    visit(doc, "", False)
    for section, items in doc.get("components", {}).items():
        if isinstance(items, dict) and not section.startswith("x-"):
            problems += [f"/components/{section}: key {key!r} breaks ^[a-zA-Z0-9\\.\\-_]+$"
                         for key in items if not COMPONENT_KEY.fullmatch(key)]
    return problems


def resolve(doc, pointer: str):
    node = doc
    if pointer in ("#", "#/"):
        return node
    for raw in pointer[2:].split("/"):
        token = urllib.parse.unquote(raw).replace("~1", "/").replace("~0", "~")
        node = node[int(token)] if isinstance(node, list) else node[token]
    return node


def dangling_refs(doc) -> list[str]:
    problems = []

    def visit(node, path: str):
        if isinstance(node, dict):
            target = node.get("$ref")
            if isinstance(target, str):
                if target.startswith("#"):
                    try:
                        resolve(doc, target)
                    except (KeyError, IndexError, ValueError, TypeError):
                        problems.append(f"{path}: $ref {target} does not resolve")
                else:
                    problems.append(f"{path}: $ref {target} points outside the document")
            for key, value in node.items():
                visit(value, f"{path}/{escape(key)}")
        elif isinstance(node, list):
            for i, value in enumerate(node):
                visit(value, f"{path}/{i}")

    visit(doc, "")
    return problems


def operations(kind: str, doc) -> list[tuple[str, str, str, str]]:
    """(where, module, member, axis) for everything that targets a module member."""
    found = []

    def take(where, obj, member_key, axis):
        if isinstance(obj, dict) and "x-logos-module" in obj and member_key in obj:
            found.append((where, obj["x-logos-module"], obj[member_key], axis))

    if kind == "openrpc":
        for i, method in enumerate(doc.get("methods", [])):
            take(f"/methods/{i}", method, "x-logos-method", "methods")
        for i, event in enumerate(doc.get("x-logos-events", [])):
            found.append((f"/x-logos-events/{i}", event["module"], event["event"], "events"))
    elif kind == "openapi":
        for path, item in doc.get("paths", {}).items():
            for verb, op in item.items():
                take(f"/paths/{escape(path)}/{verb}", op, "x-logos-method", "methods")
    else:
        sections = [("operations", doc.get("operations", {})),
                    ("components/messages", doc.get("components", {}).get("messages", {}))]
        for section, items in sections:
            for key, obj in items.items():
                take(f"/{section}/{key}", obj, "x-logos-method", "methods")
                take(f"/{section}/{key}", obj, "x-logos-event", "events")
    return found


def check_exposure(kind: str, doc, context: str | None) -> list[str]:
    problems = []
    modules = {m["name"]: m for m in doc.get("x-logos-modules", [])}
    ops = operations(kind, doc)
    for where, module, member, axis in ops:
        view = modules.get(module)
        if view is None:
            problems.append(f"{where}: {module} is not in x-logos-modules")
        elif view["status"] not in TYPED_STATUSES:
            problems.append(f"{where}: {module} is {view['status']} and has an operation")
        elif member not in view["exposure"][axis]:
            problems.append(f"{where}: {module}.{member} is not in exposure.{axis}")

    denied = DENIED.get(context or "")
    if not denied:
        return problems
    for axis in ("methods", "events"):
        for module, member in sorted(denied[axis]):
            if any(o[1:] == (module, member, axis) for o in ops):
                problems.append(f"denied {module}.{member} has an operation")
            if member in modules.get(module, {}).get("exposure", {}).get(axis, []):
                problems.append(f"denied {module}.{member} is listed in exposure.{axis}")
    schemas = doc.get("components", {}).get("schemas", {})
    for name in sorted(denied["components"]):
        if name not in schemas:
            problems.append(f"record component {name} is missing")
    for module in sorted({m for m, _ in denied["methods"]}):
        if not any(o[1:] == (module, "lidl", "methods") for o in ops):
            problems.append(f"{module}.lidl has no operation: built-ins stay callable")
    return problems


def check(validators, kind: str, doc, context: str | None) -> list[str]:
    problems = []
    for label, validator in validators[kind]:
        errors = sorted(validator.iter_errors(doc), key=lambda e: list(map(str, e.absolute_path)))
        problems += [f"{label}: {describe(e)}" for e in errors]
    problems += lint(kind, doc)
    problems += dangling_refs(doc)
    problems += check_exposure(kind, doc, context)
    return problems


def _unexpose_first_method(doc):
    doc["x-logos-modules"][0]["exposure"]["methods"].pop(0)


MUTATIONS = [
    ("an unknown spec version", lambda k, d: d.__setitem__(k, "0.0.0")),
    ("an invalid schema type", lambda k, d: d["components"]["schemas"]["Bytes"].__setitem__("type", 5)),
    ("a dangling $ref", lambda k, d: d["components"]["schemas"].__setitem__(
        "Dangling", {"$ref": "#/components/schemas/Nope"})),
    ("an external $ref", lambda k, d: d["components"]["schemas"].__setitem__(
        "External", {"$ref": "https://example.com/schema.json"})),
    ("a $ref with a sibling", lambda k, d: d["components"]["schemas"].__setitem__(
        "Sibling", {"$ref": "#/components/schemas/Bytes", "description": "x"})),
    ("the other dialect's tuple keyword", lambda k, d: d["components"]["schemas"]["Bytes"].__setitem__(
        "additionalItems" if k == "openapi" else "prefixItems", [{}] if k != "openapi" else False)),
    ("a component key outside the pattern", lambda k, d: d["components"]["schemas"].__setitem__(
        "bad key", {})),
    ("an operation for an unexposed method", lambda k, d: _unexpose_first_method(d)),
]


def self_test(validators, goldens: pathlib.Path) -> list[str]:
    missed = []
    for kind in KINDS:
        base = load(goldens / f"full_api.{kind}.json")
        if check(validators, kind, base, "full_api"):
            missed.append(f"{kind}: the unmodified full_api golden already fails")
            continue
        for label, mutate in MUTATIONS:
            doc = copy.deepcopy(base)
            mutate(kind, doc)
            if not check(validators, kind, doc, "full_api"):
                missed.append(f"{kind}: {label} was not caught")
    return missed


def classify(path: pathlib.Path):
    for kind in KINDS:
        suffix = f".{kind}.json"
        if path.name.endswith(suffix):
            return path.name[: -len(suffix)], kind
    return None, None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--metaschemas", type=pathlib.Path, default=HERE / "metaschemas")
    ap.add_argument("--goldens", type=pathlib.Path, help="check every <context>.<kind>.json here")
    ap.add_argument("--doc", action="append", default=[], metavar="KIND=PATH",
                    help=f"check one document; KIND is one of {', '.join(KINDS)}")
    ap.add_argument("--self-test", action="store_true",
                    help="first prove that broken copies of the full_api goldens fail")
    args = ap.parse_args(argv)
    if args.self_test and not args.goldens:
        ap.error("--self-test needs --goldens")

    jobs = []
    if args.goldens:
        for path in sorted(args.goldens.glob("*.json")):
            context, kind = classify(path)
            if kind is None:
                ap.error(f"{path}: not named <context>.<{'|'.join(KINDS)}>.json")
            jobs.append((context, kind, path))
        present = {(c, k) for c, k, _ in jobs}
        missing = [f"{c}.{k}.json" for c in REQUIRED_CONTEXTS for k in KINDS
                   if (c, k) not in present]
        if missing:
            ap.error(f"missing goldens in {args.goldens}: {', '.join(missing)}")
    for spec in args.doc:
        kind, sep, path = spec.partition("=")
        if not sep or kind not in KINDS or not path:
            ap.error(f"--doc {spec}: expected KIND=PATH with KIND in {', '.join(KINDS)}")
        jobs.append((None, kind, pathlib.Path(path)))
    if not jobs:
        ap.error("nothing to check: pass --goldens DIR or --doc KIND=PATH")

    validators = build_validators(args.metaschemas)
    if args.self_test:
        missed = self_test(validators, args.goldens)
        for m in missed:
            print(f"self-test: {m}")
        if missed:
            return 1
        print(f"self-test: {len(MUTATIONS) * len(KINDS)} broken documents rejected")
    failures = 0
    for context, kind, path in jobs:
        problems = check(validators, kind, load(path), context)
        print(f"{'ok' if not problems else 'FAIL':4} {kind:8} {path}")
        for p in problems[:40]:
            print(f"     {p}")
        if len(problems) > 40:
            print(f"     ... and {len(problems) - 40} more")
        failures += bool(problems)
    print(f"{len(jobs) - failures}/{len(jobs)} documents passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
