#!/usr/bin/env python3
"""Run end-to-end HAL cache usage scenarios via Redis/Valkey data plane + control-plane REST API.

Implemented now: USE-001..USE-006 (P0 propagation core).
Catalog includes all USE-* IDs from cache/cache-usage-test-matrix.md.
"""

from __future__ import annotations

import argparse
import json
import random
import shutil
import string
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Callable, Dict, Iterable, List, Optional, Sequence, Tuple
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlencode
from urllib.request import Request, urlopen

from scenario_catalog import SCENARIOS, SCENARIO_BY_ID, Scenario


class ScenarioFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class CacheNode:
    name: str
    instance_id: str
    host: str
    port: int


class RedisCli:
    def __init__(
        self,
        binary: str,
        password: Optional[str] = None,
        user: Optional[str] = None,
        db: Optional[int] = None,
        tls: bool = False,
    ):
        self.binary = binary
        self.password = password
        self.user = user
        self.db = db
        self.tls = tls

    @staticmethod
    def discover(preferred: Optional[str]) -> str:
        candidates = [preferred] if preferred else ["valkey-cli", "redis-cli"]
        for c in candidates:
            if c and shutil.which(c):
                return c
        raise ScenarioFailure("Neither valkey-cli nor redis-cli found in PATH")

    def _run(self, node: CacheNode, args: Sequence[str]) -> str:
        cmd: List[str] = [self.binary, "--raw", "-h", node.host, "-p", str(node.port)]
        if self.user:
            cmd.extend(["--user", self.user])
        if self.password:
            cmd.extend(["-a", self.password])
        if self.db is not None:
            cmd.extend(["-n", str(self.db)])
        if self.tls:
            cmd.append("--tls")
        cmd.extend(args)

        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise ScenarioFailure(
                f"Redis command failed on {node.name} ({node.host}:{node.port}): {' '.join(cmd)}\n"
                f"stdout: {proc.stdout}\nstderr: {proc.stderr}"
            )
        return proc.stdout.rstrip("\n")

    def set(self, node: CacheNode, key: str, value: str) -> None:
        out = self._run(node, ["SET", key, value])
        if out != "OK":
            raise ScenarioFailure(f"SET failed for {key} on {node.name}: {out}")

    def get(self, node: CacheNode, key: str) -> Optional[str]:
        out = self._run(node, ["GET", key])
        return out if out != "" else None

    def delete(self, node: CacheNode, key: str) -> int:
        out = self._run(node, ["DEL", key])
        try:
            return int(out)
        except ValueError as exc:
            raise ScenarioFailure(f"DEL returned non-integer for {key} on {node.name}: {out}") from exc

    def exists(self, node: CacheNode, key: str) -> bool:
        out = self._run(node, ["EXISTS", key])
        try:
            return int(out) > 0
        except ValueError as exc:
            raise ScenarioFailure(f"EXISTS returned non-integer for {key} on {node.name}: {out}") from exc


class ControlPlane:
    def __init__(self, base_url: str, insecure: bool = False):
        self.base_url = base_url.rstrip("/")
        self.insecure = insecure

    def _request(
        self,
        method: str,
        path: str,
        query: Optional[Dict[str, str]] = None,
        payload: Optional[Dict] = None,
    ) -> Tuple[int, Optional[Dict]]:
        url = f"{self.base_url}{path}"
        if query:
            url = f"{url}?{urlencode(query)}"

        body: Optional[bytes] = None
        headers = {"Accept": "application/json"}
        if payload is not None:
            body = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"

        req = Request(url=url, method=method, headers=headers, data=body)
        try:
            with urlopen(req, timeout=20) as resp:
                status = resp.status
                raw = resp.read().decode("utf-8")
        except HTTPError as err:
            status = err.code
            raw = err.read().decode("utf-8")
        except URLError as err:
            raise ScenarioFailure(f"Control plane request failed: {method} {url} -> {err}") from err

        raw = raw.strip()
        data = json.loads(raw) if raw else None
        return status, data

    def locate(self, key: str) -> Tuple[int, Optional[Dict]]:
        return self._request("GET", f"/v1/content/locate/{quote(key, safe='')}")

    def nearest(self, key: str, source_instance_id: str) -> Tuple[int, Optional[Dict]]:
        return self._request(
            "GET",
            f"/v1/content/nearest/{quote(key, safe='')}",
            query={"sourceInstanceId": source_instance_id},
        )

    def instance_keys(self, instance_id: str, limit: int = 10_000) -> Tuple[int, Optional[Dict]]:
        return self._request(
            "GET",
            f"/v1/content/instances/{quote(instance_id, safe='')}/keys",
            query={"limit": str(limit)},
        )


@dataclass
class ScenarioContext:
    redis: RedisCli
    cp: ControlPlane
    root: CacheNode
    branch: CacheNode
    leaf: CacheNode
    sibling_leaf: Optional[CacheNode]
    prefix: str
    settle_timeout_s: float
    settle_interval_s: float

    def key(self, suffix: str) -> str:
        return f"{self.prefix}:{suffix}"

    def value(self, suffix: str) -> str:
        return f"value:{suffix}:{int(time.time()*1000)}"

    def clear_key_all(self, key: str) -> None:
        nodes = [self.root, self.branch, self.leaf]
        if self.sibling_leaf is not None:
            nodes.append(self.sibling_leaf)
        for node in nodes:
            self.redis.delete(node, key)

    def wait_until(self, predicate: Callable[[], bool], message: str) -> None:
        deadline = time.time() + self.settle_timeout_s
        while time.time() < deadline:
            if predicate():
                return
            time.sleep(self.settle_interval_s)
        raise ScenarioFailure(message)

    def holders(self, key: str) -> List[str]:
        status, payload = self.cp.locate(key)
        if status != 200 or not isinstance(payload, dict):
            return []
        instances = payload.get("instances") or []
        out = []
        for item in instances:
            if isinstance(item, dict) and isinstance(item.get("instanceId"), str):
                out.append(item["instanceId"])
        return out

    def key_present_in_cp_inventory(self, instance_id: str, key: str) -> bool:
        status, payload = self.cp.instance_keys(instance_id)
        if status != 200 or not isinstance(payload, dict):
            return False
        keys = payload.get("keys") or []
        for item in keys:
            if isinstance(item, dict) and item.get("key") == key:
                return True
        return False


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise ScenarioFailure(message)


def use_001(ctx: ScenarioContext) -> None:
    key = ctx.key("use001")
    value = ctx.value("use001")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-001 expected leaf GET={value!r}, got {got!r}")
    assert_true(ctx.redis.exists(ctx.leaf, key), "USE-001 expected key cached on leaf")

    def propagated() -> bool:
        return ctx.leaf.instance_id in ctx.holders(key) and ctx.key_present_in_cp_inventory(ctx.leaf.instance_id, key)

    ctx.wait_until(propagated, "USE-001 control plane did not reflect leaf key placement")


def use_002(ctx: ScenarioContext) -> None:
    key = ctx.key("use002")
    value = ctx.value("use002")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)

    first = ctx.redis.get(ctx.leaf, key)
    second = ctx.redis.get(ctx.leaf, key)

    assert_true(first == value, f"USE-002 first read mismatch: {first!r}")
    assert_true(second == value, f"USE-002 second read mismatch: {second!r}")
    assert_true(ctx.redis.exists(ctx.leaf, key), "USE-002 expected local hit artifact on leaf")


def use_003(ctx: ScenarioContext) -> None:
    key = ctx.key("use003")
    value = ctx.value("use003")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.branch, key, value)

    def branch_visible() -> bool:
        status, payload = ctx.cp.nearest(key, ctx.leaf.instance_id)
        return status == 200 and isinstance(payload, dict) and payload.get("instanceId") == ctx.branch.instance_id

    ctx.wait_until(branch_visible, "USE-003 nearest source from leaf was not branch")

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-003 leaf value mismatch: {got!r}")


def use_004(ctx: ScenarioContext) -> None:
    key = ctx.key("use004")
    value = ctx.value("use004")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)
    ctx.redis.delete(ctx.branch, key)

    got = ctx.redis.get(ctx.branch, key)
    assert_true(got == value, f"USE-004 branch value mismatch: {got!r}")
    assert_true(ctx.redis.exists(ctx.branch, key), "USE-004 expected key cached on branch")


def use_005(ctx: ScenarioContext) -> None:
    key = ctx.key("use005")

    ctx.clear_key_all(key)
    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got is None, f"USE-005 expected miss (None), got {got!r}")

    status, payload = ctx.cp.nearest(key, ctx.leaf.instance_id)
    assert_true(status == 404, f"USE-005 expected CP nearest 404, got {status} payload={payload}")


def use_006(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-006 requires sibling leaf configuration")
    sibling = ctx.sibling_leaf
    assert sibling is not None

    key = ctx.key("use006")
    value = ctx.value("use006")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)

    with ThreadPoolExecutor(max_workers=2) as pool:
        fut_a = pool.submit(ctx.redis.get, ctx.leaf, key)
        fut_b = pool.submit(ctx.redis.get, sibling, key)
        a = fut_a.result()
        b = fut_b.result()

    assert_true(a == value, f"USE-006 leaf value mismatch: {a!r}")
    assert_true(b == value, f"USE-006 sibling leaf value mismatch: {b!r}")
    assert_true(ctx.redis.exists(ctx.leaf, key), "USE-006 expected key on leaf")
    assert_true(ctx.redis.exists(sibling, key), "USE-006 expected key on sibling leaf")

    def both_visible() -> bool:
        holders = set(ctx.holders(key))
        return ctx.leaf.instance_id in holders and sibling.instance_id in holders

    ctx.wait_until(both_visible, "USE-006 control plane did not include both leaves")


EXECUTORS: Dict[str, Callable[[ScenarioContext], None]] = {
    "use_001": use_001,
    "use_002": use_002,
    "use_003": use_003,
    "use_004": use_004,
    "use_005": use_005,
    "use_006": use_006,
}


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run HAL cache usage scenarios (data plane + control plane)")
    parser.add_argument("--control-plane-url", required=True, help="Base URL, e.g. http://localhost:8080")

    parser.add_argument("--root-id", required=True)
    parser.add_argument("--root-host", required=True)
    parser.add_argument("--root-port", type=int, required=True)

    parser.add_argument("--branch-id", required=True)
    parser.add_argument("--branch-host", required=True)
    parser.add_argument("--branch-port", type=int, required=True)

    parser.add_argument("--leaf-id", required=True)
    parser.add_argument("--leaf-host", required=True)
    parser.add_argument("--leaf-port", type=int, required=True)

    parser.add_argument("--sibling-leaf-id")
    parser.add_argument("--sibling-leaf-host")
    parser.add_argument("--sibling-leaf-port", type=int)

    parser.add_argument("--redis-cli", help="Path/name for valkey-cli or redis-cli")
    parser.add_argument("--redis-user")
    parser.add_argument("--redis-password")
    parser.add_argument("--redis-db", type=int)
    parser.add_argument("--redis-tls", action="store_true")

    parser.add_argument("--scenarios", help="Comma-separated scenario IDs, e.g. USE-001,USE-002")
    parser.add_argument("--priority", action="append", choices=["P0", "P1", "P2"], help="Filter by priority (repeatable)")
    parser.add_argument("--list", action="store_true", help="List scenarios and exit")
    parser.add_argument("--include-unimplemented", action="store_true", help="Include unimplemented scenarios in reporting")
    parser.add_argument("--fail-unimplemented", action="store_true", help="Fail if selected scenarios are not implemented")

    parser.add_argument("--prefix", help="Key prefix. Default is auto-generated")
    parser.add_argument("--settle-timeout", type=float, default=20.0)
    parser.add_argument("--settle-interval", type=float, default=1.0)

    return parser.parse_args(argv)


def validate_sibling_args(args: argparse.Namespace) -> Optional[CacheNode]:
    provided = [args.sibling_leaf_id, args.sibling_leaf_host, args.sibling_leaf_port]
    if all(v is None for v in provided):
        return None
    if any(v is None for v in provided):
        raise ScenarioFailure("Sibling leaf requires --sibling-leaf-id, --sibling-leaf-host, and --sibling-leaf-port")
    return CacheNode(
        name="sibling_leaf",
        instance_id=args.sibling_leaf_id,
        host=args.sibling_leaf_host,
        port=int(args.sibling_leaf_port),
    )


def scenario_selection(args: argparse.Namespace) -> List[Scenario]:
    if args.scenarios:
        selected: List[Scenario] = []
        for sid in [s.strip() for s in args.scenarios.split(",") if s.strip()]:
            scenario = SCENARIO_BY_ID.get(sid)
            if scenario is None:
                raise ScenarioFailure(f"Unknown scenario ID: {sid}")
            selected.append(scenario)
    else:
        selected = [s for s in SCENARIOS if s.executor is not None]

    if args.priority:
        allowed = set(args.priority)
        selected = [s for s in selected if s.priority in allowed]

    return selected


def list_scenarios() -> None:
    print("ID       Pri  Impl  Title")
    print("-------  ---  ----  -----")
    for s in SCENARIOS:
        impl = "yes" if s.executor in EXECUTORS else "no"
        print(f"{s.id:7}  {s.priority:3}  {impl:4}  {s.title}")


def run() -> int:
    args = parse_args(sys.argv[1:])

    if args.list:
        list_scenarios()
        return 0

    selected = scenario_selection(args)
    sibling = validate_sibling_args(args)

    if not selected:
        print("No scenarios selected")
        return 0

    redis_bin = RedisCli.discover(args.redis_cli)
    redis = RedisCli(
        binary=redis_bin,
        user=args.redis_user,
        password=args.redis_password,
        db=args.redis_db,
        tls=args.redis_tls,
    )
    cp = ControlPlane(args.control_plane_url)

    root = CacheNode("root", args.root_id, args.root_host, args.root_port)
    branch = CacheNode("branch", args.branch_id, args.branch_host, args.branch_port)
    leaf = CacheNode("leaf", args.leaf_id, args.leaf_host, args.leaf_port)

    prefix = args.prefix
    if not prefix:
        rand = "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
        prefix = f"hal:test:{int(time.time())}:{rand}"

    ctx = ScenarioContext(
        redis=redis,
        cp=cp,
        root=root,
        branch=branch,
        leaf=leaf,
        sibling_leaf=sibling,
        prefix=prefix,
        settle_timeout_s=args.settle_timeout,
        settle_interval_s=args.settle_interval,
    )

    executed = 0
    passed = 0
    skipped = 0
    failed = 0

    print(f"Using redis cli: {redis_bin}")
    print(f"Key prefix: {prefix}")

    for s in selected:
        exec_name = s.executor
        impl = exec_name in EXECUTORS

        if not impl:
            if args.fail_unimplemented:
                print(f"FAIL {s.id}: unimplemented")
                failed += 1
            elif args.include_unimplemented:
                print(f"SKIP {s.id}: unimplemented")
                skipped += 1
            continue

        if s.requires_sibling_leaf and sibling is None:
            print(f"SKIP {s.id}: requires sibling leaf args")
            skipped += 1
            continue

        executed += 1
        print(f"RUN  {s.id} ({s.priority}) {s.title}")

        try:
            EXECUTORS[exec_name](ctx)
            print(f"PASS {s.id}")
            passed += 1
        except ScenarioFailure as exc:
            print(f"FAIL {s.id}: {exc}")
            failed += 1
        except Exception as exc:  # defensive
            print(f"FAIL {s.id}: unexpected error: {exc}")
            failed += 1

    print("\nSummary")
    print(f"  selected: {len(selected)}")
    print(f"  executed: {executed}")
    print(f"  passed:   {passed}")
    print(f"  skipped:  {skipped}")
    print(f"  failed:   {failed}")

    return 1 if failed > 0 else 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except ScenarioFailure as exc:
        print(f"error: {exc}")
        raise SystemExit(2)
