#!/usr/bin/env python3
"""Run HAL cache usage scenarios via Redis/Valkey API + control-plane REST API.

This runner maps directly to `cache/cache-usage-test-matrix.md` and implements
USE-001 through USE-040.
"""

from __future__ import annotations

import argparse
import json
import random
import shutil
import ssl
import string
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Callable, Dict, List, Optional, Sequence, Tuple
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
    ) -> None:
        self.binary = binary
        self.password = password
        self.user = user
        self.db = db
        self.tls = tls

    @staticmethod
    def discover(preferred: Optional[str]) -> str:
        candidates = [preferred] if preferred else ["valkey-cli", "redis-cli"]
        for candidate in candidates:
            if candidate and shutil.which(candidate):
                return candidate
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

    def expire(self, node: CacheNode, key: str, seconds: int) -> bool:
        out = self._run(node, ["EXPIRE", key, str(seconds)])
        try:
            return int(out) == 1
        except ValueError as exc:
            raise ScenarioFailure(f"EXPIRE returned non-integer for {key} on {node.name}: {out}") from exc

    def ttl(self, node: CacheNode, key: str) -> int:
        out = self._run(node, ["TTL", key])
        try:
            return int(out)
        except ValueError as exc:
            raise ScenarioFailure(f"TTL returned non-integer for {key} on {node.name}: {out}") from exc

    def info(self, node: CacheNode, section: str = "stats") -> Dict[str, str]:
        raw = self._run(node, ["INFO", section])
        data: Dict[str, str] = {}
        for line in raw.splitlines():
            if not line or line.startswith("#") or ":" not in line:
                continue
            key, value = line.split(":", 1)
            data[key] = value
        return data


class ControlPlane:
    def __init__(self, base_url: str, insecure: bool = False) -> None:
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

        headers = {"Accept": "application/json"}
        data: Optional[bytes] = None
        if payload is not None:
            headers["Content-Type"] = "application/json"
            data = json.dumps(payload).encode("utf-8")

        req = Request(url=url, method=method, headers=headers, data=data)
        context = ssl._create_unverified_context() if self.insecure else None

        try:
            with urlopen(req, timeout=20, context=context) as resp:
                status = resp.status
                body = resp.read().decode("utf-8")
        except HTTPError as err:
            status = err.code
            body = err.read().decode("utf-8")
        except URLError as err:
            raise ScenarioFailure(f"Control-plane request failed: {method} {url}: {err}") from err

        body = body.strip()
        if not body:
            return status, None

        try:
            return status, json.loads(body)
        except json.JSONDecodeError:
            return status, None

    def instances(self) -> Tuple[int, Optional[Dict]]:
        return self._request("GET", "/v1/instances")

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

    def update_instance_keys(self, instance_id: str, mode: str, keys: List[Dict]) -> Tuple[int, Optional[Dict]]:
        payload = {"mode": mode, "keys": keys}
        return self._request("POST", f"/v1/content/instances/{quote(instance_id, safe='')}/keys", payload=payload)

    def topology(self) -> Tuple[int, Optional[Dict]]:
        return self._request("GET", "/v1/topology")

    def route(self, from_instance: str, to_instance: str) -> Tuple[int, Optional[Dict]]:
        return self._request(
            "GET",
            f"/v1/topology/routes/{quote(from_instance, safe='')}/{quote(to_instance, safe='')}",
        )

    def submit_event(self, payload: Dict) -> Tuple[int, Optional[Dict]]:
        return self._request("POST", "/v1/events", payload=payload)

    def submit_batch(self, instance_id: str, events: List[Dict]) -> Tuple[int, Optional[Dict]]:
        return self._request("POST", "/v1/events/batch", payload={"instanceId": instance_id, "events": events})


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
    event_delay_s: float
    high_rate_key_count: int
    read_concurrency: int
    allow_peer_sourcing: bool
    delete_policy: str
    strict_hooks: bool
    source_down_cmd: Optional[str]
    source_up_cmd: Optional[str]
    intermediate_down_cmd: Optional[str]
    intermediate_up_cmd: Optional[str]
    control_plane_down_cmd: Optional[str]
    control_plane_up_cmd: Optional[str]
    control_plane_restart_cmd: Optional[str]
    requester_restart_cmd: Optional[str]

    def nodes(self) -> List[CacheNode]:
        result = [self.root, self.branch, self.leaf]
        if self.sibling_leaf is not None:
            result.append(self.sibling_leaf)
        return result

    def key(self, suffix: str) -> str:
        return f"{self.prefix}:{suffix}"

    def value(self, suffix: str) -> str:
        return f"value:{suffix}:{int(time.time() * 1000)}"

    def clear_key_all(self, key: str) -> None:
        for node in self.nodes():
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
        out: List[str] = []
        for item in instances:
            if isinstance(item, dict) and isinstance(item.get("instanceId"), str):
                out.append(item["instanceId"])
        return out

    def key_present_in_cp_inventory(self, instance_id: str, key: str) -> bool:
        status, payload = self.cp.instance_keys(instance_id)
        if status != 200 or not isinstance(payload, dict):
            return False
        for item in payload.get("keys") or []:
            if isinstance(item, dict) and item.get("key") == key:
                return True
        return False

    def key_occurrences_in_cp_inventory(self, instance_id: str, key: str) -> int:
        status, payload = self.cp.instance_keys(instance_id)
        if status != 200 or not isinstance(payload, dict):
            return 0
        count = 0
        for item in payload.get("keys") or []:
            if isinstance(item, dict) and item.get("key") == key:
                count += 1
        return count

    def wait_for_holder(self, key: str, instance_id: str) -> None:
        self.wait_until(
            lambda: instance_id in self.holders(key),
            f"Expected holder {instance_id} for key {key} in control-plane map",
        )

    def wait_for_not_holder(self, key: str, instance_id: str) -> None:
        self.wait_until(
            lambda: instance_id not in self.holders(key),
            f"Expected holder {instance_id} to be removed for key {key}",
        )

    def cp_keyinfo(self, key: str, size: int) -> Dict:
        return {"key": key, "size": size, "lastAccessed": iso_now()}

    def run_hook(self, name: str, command: Optional[str]) -> bool:
        if not command:
            if self.strict_hooks:
                raise ScenarioFailure(f"Scenario requires hook command: {name}")
            print(f"WARN hook not set: {name}; running soft-mode fallback")
            return False

        expanded = command.format(
            root_id=self.root.instance_id,
            branch_id=self.branch.instance_id,
            leaf_id=self.leaf.instance_id,
            sibling_leaf_id=(self.sibling_leaf.instance_id if self.sibling_leaf else ""),
        )
        proc = subprocess.run(expanded, shell=True)
        if proc.returncode != 0:
            raise ScenarioFailure(f"Hook command failed ({name}): {expanded}")
        return True

    def wait_for_cp_healthy(self) -> None:
        self.wait_until(
            lambda: self.cp.instances()[0] == 200,
            "Control plane did not become healthy within timeout",
        )


def iso_now(offset_seconds: float = 0.0) -> str:
    ts = datetime.now(tz=timezone.utc) + timedelta(seconds=offset_seconds)
    return ts.isoformat().replace("+00:00", "Z")


def assert_true(condition: bool, message: str) -> None:
    if not condition:
        raise ScenarioFailure(message)


def assert_status(status: int, expected: int, message: str) -> None:
    if status != expected:
        raise ScenarioFailure(f"{message}: expected HTTP {expected}, got {status}")


def assert_status_in(status: int, expected: Sequence[int], message: str) -> None:
    if status not in expected:
        raise ScenarioFailure(f"{message}: expected one of {list(expected)}, got {status}")


def prime_leaf_from_root(ctx: ScenarioContext, key: str, value: str) -> None:
    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)
    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"Expected leaf read to return {value!r}, got {got!r}")


# USE-001 .. USE-040

def use_001(ctx: ScenarioContext) -> None:
    key = ctx.key("use001")
    value = ctx.value("use001")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)

    ctx.wait_until(
        lambda: ctx.cp.nearest(key, ctx.leaf.instance_id)[0] == 200,
        "USE-001 nearest lookup from leaf did not become available",
    )
    nearest_status, nearest_payload = ctx.cp.nearest(key, ctx.leaf.instance_id)
    assert_status(nearest_status, 200, "USE-001 nearest")
    assert_true(isinstance(nearest_payload, dict), "USE-001 nearest payload must be object")
    nearest_source = nearest_payload.get("instanceId")
    assert_true(
        nearest_source in {ctx.root.instance_id, ctx.branch.instance_id},
        f"USE-001 expected upstream source, got {nearest_source!r}",
    )

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-001 expected leaf read to return {value!r}, got {got!r}")
    assert_true(ctx.redis.exists(ctx.leaf, key), "USE-001 expected key cached on leaf")
    ctx.wait_for_holder(key, ctx.leaf.instance_id)
    ctx.wait_until(
        lambda: ctx.key_present_in_cp_inventory(ctx.leaf.instance_id, key),
        "USE-001 expected key in leaf CP inventory",
    )


def use_002(ctx: ScenarioContext) -> None:
    key = ctx.key("use002")
    value = ctx.value("use002")

    prime_leaf_from_root(ctx, key, value)
    before_count = ctx.key_occurrences_in_cp_inventory(ctx.leaf.instance_id, key)
    first = ctx.redis.get(ctx.leaf, key)
    second = ctx.redis.get(ctx.leaf, key)
    assert_true(first == value, f"USE-002 first read mismatch: {first!r}")
    assert_true(second == value, f"USE-002 second read mismatch: {second!r}")
    after_count = ctx.key_occurrences_in_cp_inventory(ctx.leaf.instance_id, key)
    assert_true(before_count <= 1 and after_count <= 1, "USE-002 duplicate key entries in CP inventory")


def use_003(ctx: ScenarioContext) -> None:
    key = ctx.key("use003")
    value = ctx.value("use003")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.branch, key, value)

    ctx.wait_until(
        lambda: ctx.cp.nearest(key, ctx.leaf.instance_id)[0] == 200,
        "USE-003 nearest lookup from leaf did not become available",
    )
    status, payload = ctx.cp.nearest(key, ctx.leaf.instance_id)
    assert_status(status, 200, "USE-003 nearest")
    assert_true(isinstance(payload, dict), "USE-003 nearest payload must be object")
    assert_true(
        payload.get("instanceId") == ctx.branch.instance_id,
        f"USE-003 expected branch source, got {payload}",
    )

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-003 leaf read mismatch: {got!r}")


def use_004(ctx: ScenarioContext) -> None:
    key = ctx.key("use004")
    value = ctx.value("use004")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)
    ctx.redis.delete(ctx.branch, key)

    got = ctx.redis.get(ctx.branch, key)
    assert_true(got == value, f"USE-004 branch read mismatch: {got!r}")
    assert_true(ctx.redis.exists(ctx.branch, key), "USE-004 expected key cached on branch")
    ctx.wait_for_holder(key, ctx.branch.instance_id)


def use_005(ctx: ScenarioContext) -> None:
    key = ctx.key("use005")
    ctx.clear_key_all(key)

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got is None, f"USE-005 expected leaf miss, got {got!r}")

    status, _ = ctx.cp.nearest(key, ctx.leaf.instance_id)
    assert_status(status, 404, "USE-005 nearest")


def use_006(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-006 requires sibling leaf")
    sibling = ctx.sibling_leaf
    assert sibling is not None

    key = ctx.key("use006")
    value = ctx.value("use006")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)

    with ThreadPoolExecutor(max_workers=2) as pool:
        leaf_future = pool.submit(ctx.redis.get, ctx.leaf, key)
        sibling_future = pool.submit(ctx.redis.get, sibling, key)
        leaf_value = leaf_future.result()
        sibling_value = sibling_future.result()

    assert_true(leaf_value == value, f"USE-006 leaf mismatch: {leaf_value!r}")
    assert_true(sibling_value == value, f"USE-006 sibling leaf mismatch: {sibling_value!r}")

    ctx.wait_until(
        lambda: ctx.leaf.instance_id in ctx.holders(key) and sibling.instance_id in ctx.holders(key),
        "USE-006 expected both leaves in CP map",
    )


def use_007(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-007 requires sibling leaf")
    sibling = ctx.sibling_leaf
    assert sibling is not None

    key = ctx.key("use007")
    branch_value = ctx.value("use007_branch")
    sibling_value = ctx.value("use007_sibling")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.branch, key, branch_value)
    ctx.redis.set(sibling, key, sibling_value)

    status, payload = ctx.cp.nearest(key, ctx.leaf.instance_id)
    assert_status(status, 200, "USE-007 nearest")
    assert_true(isinstance(payload, dict), "USE-007 nearest payload must be object")
    source = payload.get("instanceId")
    assert_true(source in {ctx.branch.instance_id, sibling.instance_id}, f"USE-007 unexpected source {source!r}")

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got in {branch_value, sibling_value}, f"USE-007 leaf value not from expected source: {got!r}")


def use_008(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-008 requires sibling leaf")
    sibling = ctx.sibling_leaf
    assert sibling is not None

    key = ctx.key("use008")
    value = ctx.value("use008")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)
    ctx.redis.set(sibling, key, value)

    status, payload = ctx.cp.nearest(key, ctx.leaf.instance_id)
    assert_status(status, 200, "USE-008 nearest")
    assert_true(isinstance(payload, dict), "USE-008 nearest payload must be object")
    source = payload.get("instanceId")
    assert_true(source in {ctx.root.instance_id, sibling.instance_id}, f"USE-008 source {source!r} not in holder set")

    route_status, route_payload = ctx.cp.route(source, ctx.leaf.instance_id)
    assert_status(route_status, 200, "USE-008 route")
    assert_true(isinstance(route_payload, dict), "USE-008 route payload must be object")
    assert_true(isinstance(route_payload.get("hops"), list) and len(route_payload["hops"]) >= 2, "USE-008 route hops invalid")


def use_009(ctx: ScenarioContext) -> None:
    key = ctx.key("use009")
    value = ctx.value("use009")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)
    ctx.redis.delete(ctx.branch, key)

    status, _ = ctx.cp.update_instance_keys(
        ctx.branch.instance_id,
        "partial",
        [ctx.cp_keyinfo(key, len(value))],
    )
    assert_status_in(status, [200, 404], "USE-009 stale-map inject")

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-009 expected fallback to root value, got {got!r}")
    ctx.wait_for_holder(key, ctx.leaf.instance_id)


def use_010(ctx: ScenarioContext) -> None:
    key = ctx.key("use010")
    value = ctx.value("use010")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)

    clear_status, _ = ctx.cp.update_instance_keys(ctx.root.instance_id, "full", [])
    assert_status_in(clear_status, [200, 404, 400], "USE-010 clear root map")

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-010 expected leaf read success, got {got!r}")
    ctx.wait_until(
        lambda: ctx.root.instance_id in ctx.holders(key) or ctx.leaf.instance_id in ctx.holders(key),
        "USE-010 expected CP reconciliation after read",
    )


def use_011(ctx: ScenarioContext) -> None:
    key = ctx.key("use011")
    old_value = ctx.value("use011_old")
    new_value = ctx.value("use011_new")

    prime_leaf_from_root(ctx, key, old_value)
    ctx.redis.set(ctx.root, key, new_value)
    ctx.redis.delete(ctx.leaf, key)

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == new_value, f"USE-011 expected refreshed value {new_value!r}, got {got!r}")
    ctx.wait_until(
        lambda: ctx.key_present_in_cp_inventory(ctx.leaf.instance_id, key),
        "USE-011 expected key in leaf CP inventory after refresh",
    )


def use_012(ctx: ScenarioContext) -> None:
    key = ctx.key("use012")
    value = ctx.value("use012")

    prime_leaf_from_root(ctx, key, value)

    clear_status, _ = ctx.cp.update_instance_keys(ctx.leaf.instance_id, "full", [])
    assert_status_in(clear_status, [200, 404, 400], "USE-012 clear leaf CP inventory")

    time.sleep(ctx.event_delay_s)
    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-012 expected value after delayed convergence, got {got!r}")
    ctx.wait_for_holder(key, ctx.leaf.instance_id)


def use_013(ctx: ScenarioContext) -> None:
    key = ctx.key("use013")
    value = ctx.value("use013")

    prime_leaf_from_root(ctx, key, value)

    evt = {
        "instanceId": ctx.leaf.instance_id,
        "eventType": "key_updated",
        "timestamp": iso_now(),
        "key": key,
        "size": len(value),
        "sourceInstanceId": ctx.root.instance_id,
        "retrievalTimeMs": 1.0,
    }

    status_1, _ = ctx.cp.submit_event(evt)
    status_2, _ = ctx.cp.submit_event(evt)
    assert_status(status_1, 202, "USE-013 first event")
    assert_status(status_2, 202, "USE-013 duplicate event")

    holders = ctx.holders(key)
    assert_true(len(holders) == len(set(holders)), "USE-013 holders must remain unique")
    assert_true(ctx.key_occurrences_in_cp_inventory(ctx.leaf.instance_id, key) <= 1, "USE-013 duplicate key entries in CP inventory")


def use_014(ctx: ScenarioContext) -> None:
    key = ctx.key("use014")
    value = ctx.value("use014")

    prime_leaf_from_root(ctx, key, value)

    newer = {
        "instanceId": ctx.leaf.instance_id,
        "eventType": "key_updated",
        "timestamp": iso_now(5),
        "key": key,
        "size": len(value) + 1,
    }
    older = {
        "instanceId": ctx.leaf.instance_id,
        "eventType": "key_added",
        "timestamp": iso_now(-5),
        "key": key,
        "size": len(value),
        "sourceInstanceId": ctx.root.instance_id,
    }

    s1, _ = ctx.cp.submit_event(newer)
    s2, _ = ctx.cp.submit_event(older)
    assert_status(s1, 202, "USE-014 newer event")
    assert_status(s2, 202, "USE-014 older event")

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-014 value mismatch after out-of-order events: {got!r}")


def use_015(ctx: ScenarioContext) -> None:
    key = ctx.key("use015")
    value = ctx.value("use015")

    prime_leaf_from_root(ctx, key, value)
    deleted = ctx.redis.delete(ctx.leaf, key)
    assert_true(deleted in (0, 1), "USE-015 invalid delete result")

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-015 expected refetch value, got {got!r}")
    ctx.wait_for_holder(key, ctx.leaf.instance_id)


def use_016(ctx: ScenarioContext) -> None:
    key = ctx.key("use016")
    value = ctx.value("use016")

    prime_leaf_from_root(ctx, key, value)
    ctx.redis.expire(ctx.leaf, key, 1)
    time.sleep(2)

    ttl = ctx.redis.ttl(ctx.leaf, key)
    assert_true(ttl in (-2, -1) or ttl <= 0, f"USE-016 unexpected TTL state: {ttl}")

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-016 expected refill value, got {got!r}")
    ctx.wait_for_holder(key, ctx.leaf.instance_id)


def use_017(ctx: ScenarioContext) -> None:
    key = ctx.key("use017")
    value = ctx.value("use017")

    prime_leaf_from_root(ctx, key, value)
    ctx.redis.delete(ctx.root, key)
    ctx.redis.delete(ctx.branch, key)
    if ctx.sibling_leaf is not None:
        ctx.redis.delete(ctx.sibling_leaf, key)

    if ctx.delete_policy == "strict_invalidate":
        ctx.redis.delete(ctx.leaf, key)
        got = ctx.redis.get(ctx.leaf, key)
        assert_true(got is None, f"USE-017 strict policy expected miss, got {got!r}")
        ctx.wait_for_not_holder(key, ctx.leaf.instance_id)
    else:
        got = ctx.redis.get(ctx.leaf, key)
        assert_true(got in (None, value), f"USE-017 stale policy expected None or old value, got {got!r}")


def use_018(ctx: ScenarioContext) -> None:
    key = ctx.key("use018")
    root_value = ctx.value("use018_root")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, root_value)

    sibling_value: Optional[str] = None
    if ctx.sibling_leaf is not None:
        sibling_value = ctx.value("use018_sibling")
        ctx.redis.set(ctx.sibling_leaf, key, sibling_value)

    down_applied = False
    try:
        down_applied = ctx.run_hook("source_down", ctx.source_down_cmd)
        if not down_applied:
            ctx.redis.delete(ctx.root, key)

        got = ctx.redis.get(ctx.leaf, key)
        expected = {root_value}
        if sibling_value is not None:
            expected.add(sibling_value)
            expected.add(None)
        else:
            expected.add(None)
        assert_true(got in expected, f"USE-018 unexpected read result {got!r}")
    finally:
        if down_applied:
            ctx.run_hook("source_up", ctx.source_up_cmd)


def use_019(ctx: ScenarioContext) -> None:
    key = ctx.key("use019")
    value = ctx.value("use019")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)

    down_applied = False
    try:
        down_applied = ctx.run_hook("intermediate_down", ctx.intermediate_down_cmd)
        if not down_applied:
            ctx.redis.delete(ctx.branch, key)

        got = ctx.redis.get(ctx.leaf, key)
        assert_true(got in (value, None), f"USE-019 expected value or miss during intermediate failure, got {got!r}")
    finally:
        if down_applied:
            ctx.run_hook("intermediate_up", ctx.intermediate_up_cmd)


def use_020(ctx: ScenarioContext) -> None:
    key = ctx.key("use020")
    value = ctx.value("use020")

    prime_leaf_from_root(ctx, key, value)

    cp_down = False
    try:
        cp_down = ctx.run_hook("control_plane_down", ctx.control_plane_down_cmd)
        got = ctx.redis.get(ctx.leaf, key)
        assert_true(got in (value, None), f"USE-020 expected value or miss while CP down, got {got!r}")
    finally:
        if cp_down:
            ctx.run_hook("control_plane_up", ctx.control_plane_up_cmd)

    ctx.wait_for_cp_healthy()
    ctx.redis.get(ctx.leaf, key)
    ctx.wait_for_holder(key, ctx.leaf.instance_id)


def use_021(ctx: ScenarioContext) -> None:
    key = ctx.key("use021")
    value = ctx.value("use021")

    prime_leaf_from_root(ctx, key, value)

    restarted = ctx.run_hook("requester_restart", ctx.requester_restart_cmd)
    if not restarted:
        ctx.redis.delete(ctx.leaf, key)

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-021 expected post-restart read to return value, got {got!r}")


def use_022(ctx: ScenarioContext) -> None:
    key = ctx.key("use022")
    value = ctx.value("use022")

    prime_leaf_from_root(ctx, key, value)

    restarted = ctx.run_hook("control_plane_restart", ctx.control_plane_restart_cmd)
    if restarted:
        ctx.wait_for_cp_healthy()

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-022 value mismatch after CP restart: {got!r}")

    status, payload = ctx.cp.nearest(key, ctx.leaf.instance_id)
    assert_status(status, 200, "USE-022 nearest after restart")
    assert_true(isinstance(payload, dict) and "instanceId" in payload, "USE-022 nearest payload invalid")
    inst_status, _ = ctx.cp.instances()
    assert_status(inst_status, 200, "USE-022 instances after restart")
    topo_status, topo_payload = ctx.cp.topology()
    assert_status(topo_status, 200, "USE-022 topology after restart")
    assert_true(isinstance(topo_payload, dict), "USE-022 topology payload invalid")
    ctx.wait_for_holder(key, ctx.leaf.instance_id)


def use_023(ctx: ScenarioContext) -> None:
    key = ctx.key("use023")
    value = ctx.value("use023")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)
    ctx.redis.delete(ctx.branch, key)

    inject_status, _ = ctx.cp.update_instance_keys(ctx.branch.instance_id, "partial", [ctx.cp_keyinfo(key, len(value))])
    assert_status_in(inject_status, [200, 404], "USE-023 stale map inject")

    restarted = ctx.run_hook("control_plane_restart", ctx.control_plane_restart_cmd)
    if restarted:
        ctx.wait_for_cp_healthy()

    time.sleep(ctx.event_delay_s)
    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-023 expected fallback value, got {got!r}")
    ctx.wait_for_holder(key, ctx.leaf.instance_id)


def use_024(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-024 requires sibling leaf")
    sibling = ctx.sibling_leaf
    assert sibling is not None

    keys: List[str] = []
    for i in range(ctx.high_rate_key_count):
        key = ctx.key(f"use024:{i}")
        value = ctx.value(f"use024:{i}")
        ctx.clear_key_all(key)
        ctx.redis.set(ctx.root, key, value)
        with ThreadPoolExecutor(max_workers=2) as pool:
            v1 = pool.submit(ctx.redis.get, ctx.leaf, key).result()
            v2 = pool.submit(ctx.redis.get, sibling, key).result()
        assert_true(v1 == value and v2 == value, f"USE-024 propagation mismatch for key {key}")
        keys.append(key)

    events = [{"eventType": "key_added", "timestamp": iso_now(), "key": k, "size": 1} for k in keys[: min(100, len(keys))]]
    status, _ = ctx.cp.submit_batch(ctx.leaf.instance_id, events)
    assert_status(status, 202, "USE-024 submit batch")

    for key in keys:
        holders = set(ctx.holders(key))
        assert_true(ctx.leaf.instance_id in holders or sibling.instance_id in holders, f"USE-024 key {key} not visible on leaves in CP")


def use_025(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-025 requires sibling leaf")

    key = ctx.key("use025")
    value = ctx.value("use025")
    prime_leaf_from_root(ctx, key, value)

    batch = [{"eventType": "key_added", "timestamp": iso_now(), "key": key, "size": len(value)}]
    s1, _ = ctx.cp.submit_batch(ctx.leaf.instance_id, batch)
    s2, _ = ctx.cp.submit_batch(ctx.leaf.instance_id, batch)
    assert_status(s1, 202, "USE-025 first batch")
    assert_status(s2, 202, "USE-025 duplicate batch")

    holders = ctx.holders(key)
    assert_true(len(holders) == len(set(holders)), "USE-025 duplicate holders after duplicate batch")


def use_026(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-026 requires sibling leaf")

    key = ctx.key("use026")
    value = ctx.value("use026")
    prime_leaf_from_root(ctx, key, value)

    newer = [{"eventType": "key_updated", "timestamp": iso_now(5), "key": key, "size": len(value) + 1}]
    older = [{"eventType": "key_added", "timestamp": iso_now(-5), "key": key, "size": len(value)}]

    s1, _ = ctx.cp.submit_batch(ctx.leaf.instance_id, newer)
    s2, _ = ctx.cp.submit_batch(ctx.leaf.instance_id, older)
    assert_status(s1, 202, "USE-026 newer batch")
    assert_status(s2, 202, "USE-026 older batch")

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-026 value mismatch after out-of-order batch: {got!r}")


def use_027(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-027 requires sibling leaf")

    key = ctx.key("use027")
    old_value = ctx.value("use027_old")
    new_value = ctx.value("use027_new")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, old_value)
    ctx.redis.get(ctx.leaf, key)

    observed: List[Optional[str]] = []

    def reader() -> None:
        for _ in range(max(10, ctx.read_concurrency)):
            observed.append(ctx.redis.get(ctx.leaf, key))
            time.sleep(0.02)

    with ThreadPoolExecutor(max_workers=2) as pool:
        future = pool.submit(reader)
        time.sleep(0.1)
        ctx.redis.set(ctx.root, key, new_value)
        ctx.redis.delete(ctx.leaf, key)
        future.result()

    final = ctx.redis.get(ctx.leaf, key)
    assert_true(final == new_value, f"USE-027 final value expected new value, got {final!r}")
    allowed = {old_value, new_value, None}
    assert_true(all(v in allowed for v in observed), f"USE-027 observed invalid values: {observed}")


def use_028(ctx: ScenarioContext) -> None:
    key = ctx.key("use028")
    value = ctx.value("use028")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)
    ctx.redis.delete(ctx.branch, key)

    leaves = [ctx.leaf] + ([ctx.sibling_leaf] if ctx.sibling_leaf is not None else [])
    reads_per_leaf = max(8, ctx.read_concurrency)

    def read_many(node: CacheNode) -> None:
        for _ in range(reads_per_leaf):
            got = ctx.redis.get(node, key)
            assert_true(got == value, f"USE-028 leaf read mismatch on {node.name}: {got!r}")

    with ThreadPoolExecutor(max_workers=len(leaves)) as pool:
        futures = [pool.submit(read_many, node) for node in leaves]
        for future in futures:
            future.result()

    assert_true(ctx.redis.exists(ctx.branch, key), "USE-028 expected branch to cache key after fan-in reads")


def use_029(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-029 requires sibling leaf")
    sibling = ctx.sibling_leaf
    assert sibling is not None

    key = ctx.key("use029")
    value = ctx.value("use029")

    prime_leaf_from_root(ctx, key, value)
    ctx.redis.get(sibling, key)

    def churn(node: CacheNode) -> None:
        for _ in range(max(8, ctx.read_concurrency)):
            ctx.redis.delete(node, key)
            got = ctx.redis.get(node, key)
            assert_true(got in (value, None), f"USE-029 unexpected value during churn on {node.name}: {got!r}")

    with ThreadPoolExecutor(max_workers=2) as pool:
        f1 = pool.submit(churn, ctx.leaf)
        f2 = pool.submit(churn, sibling)
        f1.result()
        f2.result()

    assert_true(ctx.redis.get(ctx.leaf, key) == value, "USE-029 final leaf value mismatch")
    assert_true(ctx.redis.get(sibling, key) == value, "USE-029 final sibling value mismatch")


def use_030(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-030 requires sibling leaf")
    sibling = ctx.sibling_leaf
    assert sibling is not None

    key = ctx.key("use030")
    value = ctx.value("use030")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)
    ctx.redis.set(sibling, key, value)

    status, payload = ctx.cp.nearest(key, ctx.leaf.instance_id)
    assert_status(status, 200, "USE-030 nearest")
    assert_true(isinstance(payload, dict), "USE-030 nearest payload invalid")
    source = payload.get("instanceId")
    assert_true(source in {ctx.root.instance_id, sibling.instance_id}, f"USE-030 unexpected source {source!r}")

    route_status, route_payload = ctx.cp.route(source, ctx.leaf.instance_id)
    assert_status(route_status, 200, "USE-030 route")
    assert_true(isinstance(route_payload, dict) and isinstance(route_payload.get("hops"), list), "USE-030 route payload invalid")


def use_031(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-031 requires sibling leaf")

    key = ctx.key("use031")
    value = ctx.value("use031")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)
    if ctx.sibling_leaf is not None:
        ctx.redis.set(ctx.sibling_leaf, key, value)

    source_ids: List[str] = []
    for _ in range(3):
        status, payload = ctx.cp.nearest(key, ctx.leaf.instance_id)
        assert_status(status, 200, "USE-031 nearest")
        assert_true(isinstance(payload, dict), "USE-031 nearest payload invalid")
        source = payload.get("instanceId")
        assert_true(isinstance(source, str), "USE-031 nearest missing source")
        source_ids.append(source)
        route_status, route_payload = ctx.cp.route(source, ctx.leaf.instance_id)
        assert_status(route_status, 200, "USE-031 route")
        assert_true(isinstance(route_payload, dict) and isinstance(route_payload.get("hops"), list), "USE-031 route payload invalid")

    assert_true(len(source_ids) >= 1, "USE-031 expected at least one source observation")


def use_032(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-032 requires sibling leaf")
    sibling = ctx.sibling_leaf
    assert sibling is not None

    key = ctx.key("use032")
    sibling_value = ctx.value("use032")

    ctx.clear_key_all(key)
    ctx.redis.set(sibling, key, sibling_value)
    ctx.redis.delete(ctx.root, key)
    ctx.redis.delete(ctx.branch, key)
    ctx.redis.delete(ctx.leaf, key)

    status, payload = ctx.cp.nearest(key, ctx.leaf.instance_id)

    if ctx.allow_peer_sourcing:
        assert_status(status, 200, "USE-032 nearest with peer sourcing")
        assert_true(isinstance(payload, dict) and payload.get("instanceId") == sibling.instance_id, "USE-032 expected sibling as source")
        got = ctx.redis.get(ctx.leaf, key)
        assert_true(got == sibling_value, f"USE-032 expected peer-fetched value, got {got!r}")
    else:
        assert_status_in(status, [200, 404], "USE-032 nearest without peer sourcing")
        if status == 200:
            assert_true(isinstance(payload, dict), "USE-032 payload invalid")
            assert_true(payload.get("instanceId") != sibling.instance_id, "USE-032 sibling sourcing not allowed")


def use_033(ctx: ScenarioContext) -> None:
    key = ctx.key("use033")
    value = ctx.value("use033")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.leaf, key, value)
    ctx.redis.delete(ctx.root, key)
    ctx.redis.delete(ctx.branch, key)

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-033 local leaf hit mismatch: {got!r}")

    status, payload = ctx.cp.nearest(key, ctx.leaf.instance_id)
    assert_status_in(status, [200, 404], "USE-033 nearest")
    if status == 200:
        assert_true(isinstance(payload, dict), "USE-033 nearest payload invalid")

    update_status, _ = ctx.cp.update_instance_keys(ctx.leaf.instance_id, "partial", [ctx.cp_keyinfo(key, len(value))])
    assert_status_in(update_status, [200, 404], "USE-033 leaf inventory update")
    ctx.wait_for_holder(key, ctx.leaf.instance_id)


def use_034(ctx: ScenarioContext) -> None:
    key = ctx.key("use034")
    value = ctx.value("use034")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)
    branch_val = ctx.redis.get(ctx.branch, key)
    leaf_val = ctx.redis.get(ctx.leaf, key)
    assert_true(branch_val == value and leaf_val == value, "USE-034 setup propagation failed")

    ctx.redis.delete(ctx.branch, key)
    assert_true(ctx.redis.exists(ctx.leaf, key), "USE-034 expected leaf to retain key")

    ctx.wait_for_holder(key, ctx.leaf.instance_id)


def use_035(ctx: ScenarioContext) -> None:
    key = ctx.key("use035")
    value = ctx.value("use035")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-035 expected root->leaf pull-through value, got {got!r}")
    route_status, route_payload = ctx.cp.route(ctx.root.instance_id, ctx.leaf.instance_id)
    assert_status(route_status, 200, "USE-035 route")
    assert_true(isinstance(route_payload, dict), "USE-035 route payload invalid")
    hops = route_payload.get("hops")
    assert_true(isinstance(hops, list) and len(hops) >= 2, "USE-035 route hops invalid")
    assert_true(hops[0] == ctx.root.instance_id, "USE-035 route must start at root")
    assert_true(hops[-1] == ctx.leaf.instance_id, "USE-035 route must end at leaf")


def use_036(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-036 requires sibling leaf")

    topo_status, topo_payload = ctx.cp.topology()
    assert_status(topo_status, 200, "USE-036 topology")
    assert_true(isinstance(topo_payload, dict), "USE-036 topology payload invalid")

    route_status, route_payload = ctx.cp.route(ctx.root.instance_id, ctx.leaf.instance_id)
    assert_status(route_status, 200, "USE-036 route")
    assert_true(isinstance(route_payload, dict), "USE-036 route payload invalid")
    hops = route_payload.get("hops")
    assert_true(isinstance(hops, list) and len(hops) >= 2, "USE-036 route hops invalid")
    assert_true(hops[0] == ctx.root.instance_id, "USE-036 route must start at root")
    assert_true(hops[-1] == ctx.leaf.instance_id, "USE-036 route must end at leaf")


def use_037(ctx: ScenarioContext) -> None:
    key = ctx.key("use037")
    value = ctx.value("use037")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)
    before = set(ctx.holders(key))
    assert_true(ctx.root.instance_id in before or len(before) == 0, "USE-037 unexpected holders before leaf read")
    assert_true(ctx.leaf.instance_id not in before, "USE-037 leaf should not be holder before read")
    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-037 immediate read-after-write mismatch: {got!r}")
    ctx.wait_for_holder(key, ctx.leaf.instance_id)


def use_038(ctx: ScenarioContext) -> None:
    key = ctx.key("use038")
    value = ctx.value("use038")

    ctx.clear_key_all(key)
    ctx.redis.set(ctx.root, key, value)

    clear_status, _ = ctx.cp.update_instance_keys(ctx.root.instance_id, "full", [])
    assert_status_in(clear_status, [200, 404, 400], "USE-038 clear root inventory")

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == value, f"USE-038 expected data-plane hit despite lagging CP map, got {got!r}")
    ctx.wait_for_holder(key, ctx.leaf.instance_id)


def use_039(ctx: ScenarioContext) -> None:
    key = ctx.key("use039")

    ctx.clear_key_all(key)
    before = set(ctx.holders(key))
    deleted = ctx.redis.delete(ctx.root, key)
    assert_true(deleted in (0, 1), "USE-039 invalid delete result")

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got is None, f"USE-039 expected miss after deleting absent key, got {got!r}")

    status, _ = ctx.cp.nearest(key, ctx.leaf.instance_id)
    assert_status(status, 404, "USE-039 nearest")
    after = set(ctx.holders(key))
    assert_true(after == before, "USE-039 expected control-plane holder set to remain unchanged")


def use_040(ctx: ScenarioContext) -> None:
    assert_true(ctx.sibling_leaf is not None, "USE-040 requires sibling leaf")

    key = ctx.key("use040")
    old_value = ctx.value("use040_old")
    new_value = ctx.value("use040_new")

    prime_leaf_from_root(ctx, key, old_value)

    mixed_events = [
        {"eventType": "key_added", "timestamp": iso_now(-5), "key": key, "size": len(old_value)},
        {"eventType": "key_updated", "timestamp": iso_now(0), "key": key, "size": len(new_value)},
        {"eventType": "key_evicted", "timestamp": iso_now(5), "key": key},
    ]
    batch_status, _ = ctx.cp.submit_batch(ctx.leaf.instance_id, mixed_events)
    assert_status(batch_status, 202, "USE-040 mixed batch")

    ctx.redis.set(ctx.root, key, new_value)
    ctx.redis.delete(ctx.leaf, key)

    got = ctx.redis.get(ctx.leaf, key)
    assert_true(got == new_value, f"USE-040 expected final new value, got {got!r}")
    ctx.wait_for_holder(key, ctx.leaf.instance_id)


EXECUTORS: Dict[str, Callable[[ScenarioContext], None]] = {
    "use_001": use_001,
    "use_002": use_002,
    "use_003": use_003,
    "use_004": use_004,
    "use_005": use_005,
    "use_006": use_006,
    "use_007": use_007,
    "use_008": use_008,
    "use_009": use_009,
    "use_010": use_010,
    "use_011": use_011,
    "use_012": use_012,
    "use_013": use_013,
    "use_014": use_014,
    "use_015": use_015,
    "use_016": use_016,
    "use_017": use_017,
    "use_018": use_018,
    "use_019": use_019,
    "use_020": use_020,
    "use_021": use_021,
    "use_022": use_022,
    "use_023": use_023,
    "use_024": use_024,
    "use_025": use_025,
    "use_026": use_026,
    "use_027": use_027,
    "use_028": use_028,
    "use_029": use_029,
    "use_030": use_030,
    "use_031": use_031,
    "use_032": use_032,
    "use_033": use_033,
    "use_034": use_034,
    "use_035": use_035,
    "use_036": use_036,
    "use_037": use_037,
    "use_038": use_038,
    "use_039": use_039,
    "use_040": use_040,
}


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run HAL cache usage scenarios (data plane + control plane)")
    parser.add_argument("--control-plane-url", required=True, help="Base URL, e.g. http://localhost:8080")
    parser.add_argument("--control-plane-insecure", action="store_true", help="Disable TLS verification for HTTPS control plane")

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
    parser.add_argument("--priority", action="append", choices=["P0", "P1", "P2"], help="Filter by priority")
    parser.add_argument("--list", action="store_true", help="List scenarios and exit")
    parser.add_argument("--fail-unimplemented", action="store_true", help="Fail if selected scenarios are not implemented")

    parser.add_argument("--prefix", help="Key prefix. Default is auto-generated")
    parser.add_argument("--settle-timeout", type=float, default=30.0)
    parser.add_argument("--settle-interval", type=float, default=1.0)
    parser.add_argument("--event-delay-seconds", type=float, default=2.0)
    parser.add_argument("--high-rate-key-count", type=int, default=20)
    parser.add_argument("--read-concurrency", type=int, default=16)

    parser.add_argument("--allow-peer-sourcing", action="store_true")
    parser.add_argument(
        "--delete-policy",
        choices=["strict_invalidate", "stale_until_expire"],
        default="stale_until_expire",
    )

    parser.add_argument("--strict-hooks", action="store_true", help="Fail if a hook-required scenario has no hook command")
    parser.add_argument("--source-down-cmd")
    parser.add_argument("--source-up-cmd")
    parser.add_argument("--intermediate-down-cmd")
    parser.add_argument("--intermediate-up-cmd")
    parser.add_argument("--control-plane-down-cmd")
    parser.add_argument("--control-plane-up-cmd")
    parser.add_argument("--control-plane-restart-cmd")
    parser.add_argument("--requester-restart-cmd")

    return parser.parse_args(argv)


def validate_sibling_args(args: argparse.Namespace) -> Optional[CacheNode]:
    values = [args.sibling_leaf_id, args.sibling_leaf_host, args.sibling_leaf_port]
    if all(v is None for v in values):
        return None
    if any(v is None for v in values):
        raise ScenarioFailure("Sibling leaf requires --sibling-leaf-id, --sibling-leaf-host, --sibling-leaf-port")
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
        selected = list(SCENARIOS)

    if args.priority:
        allowed = set(args.priority)
        selected = [s for s in selected if s.priority in allowed]

    return selected


def list_scenarios() -> None:
    print("ID       Pri  Impl  Title")
    print("-------  ---  ----  -----")
    for scenario in SCENARIOS:
        impl = "yes" if scenario.executor in EXECUTORS else "no"
        print(f"{scenario.id:7}  {scenario.priority:3}  {impl:4}  {scenario.title}")


def build_context(args: argparse.Namespace, sibling: Optional[CacheNode]) -> ScenarioContext:
    redis_bin = RedisCli.discover(args.redis_cli)
    redis = RedisCli(
        binary=redis_bin,
        user=args.redis_user,
        password=args.redis_password,
        db=args.redis_db,
        tls=args.redis_tls,
    )
    cp = ControlPlane(base_url=args.control_plane_url, insecure=args.control_plane_insecure)

    prefix = args.prefix
    if not prefix:
        rand = "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
        prefix = f"hal:test:{int(time.time())}:{rand}"

    ctx = ScenarioContext(
        redis=redis,
        cp=cp,
        root=CacheNode("root", args.root_id, args.root_host, args.root_port),
        branch=CacheNode("branch", args.branch_id, args.branch_host, args.branch_port),
        leaf=CacheNode("leaf", args.leaf_id, args.leaf_host, args.leaf_port),
        sibling_leaf=sibling,
        prefix=prefix,
        settle_timeout_s=args.settle_timeout,
        settle_interval_s=args.settle_interval,
        event_delay_s=args.event_delay_seconds,
        high_rate_key_count=max(1, args.high_rate_key_count),
        read_concurrency=max(2, args.read_concurrency),
        allow_peer_sourcing=args.allow_peer_sourcing,
        delete_policy=args.delete_policy,
        strict_hooks=args.strict_hooks,
        source_down_cmd=args.source_down_cmd,
        source_up_cmd=args.source_up_cmd,
        intermediate_down_cmd=args.intermediate_down_cmd,
        intermediate_up_cmd=args.intermediate_up_cmd,
        control_plane_down_cmd=args.control_plane_down_cmd,
        control_plane_up_cmd=args.control_plane_up_cmd,
        control_plane_restart_cmd=args.control_plane_restart_cmd,
        requester_restart_cmd=args.requester_restart_cmd,
    )

    print(f"Using redis cli: {redis_bin}")
    print(f"Key prefix: {ctx.prefix}")
    return ctx


def run() -> int:
    args = parse_args(sys.argv[1:])

    if args.list:
        list_scenarios()
        return 0

    sibling = validate_sibling_args(args)
    selected = scenario_selection(args)
    if not selected:
        print("No scenarios selected")
        return 0

    ctx = build_context(args, sibling)

    executed = 0
    skipped = 0
    passed = 0
    failed = 0

    for scenario in selected:
        executor_name = scenario.executor
        executor = EXECUTORS.get(executor_name or "")

        if executor is None:
            if args.fail_unimplemented:
                print(f"FAIL {scenario.id}: unimplemented")
                failed += 1
            else:
                print(f"SKIP {scenario.id}: unimplemented")
                skipped += 1
            continue

        if scenario.requires_sibling_leaf and ctx.sibling_leaf is None:
            print(f"SKIP {scenario.id}: requires sibling leaf arguments")
            skipped += 1
            continue

        executed += 1
        print(f"RUN  {scenario.id} ({scenario.priority}) {scenario.title}")
        try:
            executor(ctx)
            print(f"PASS {scenario.id}")
            passed += 1
        except ScenarioFailure as exc:
            print(f"FAIL {scenario.id}: {exc}")
            failed += 1
        except Exception as exc:
            print(f"FAIL {scenario.id}: unexpected error: {exc}")
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
