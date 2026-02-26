#!/usr/bin/env python3
"""HAL cache test framework CLI.

This tool drives Redis-compatible cache nodes directly and verifies control plane
state through the HAL cache control plane REST API.

It supports one-off write/read checks and multi-step scenario execution from a
JSON file so it can be used for repeated scenario testing.
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple
from urllib import error as urlerror
from urllib import parse as urlparse
from urllib import request as urlrequest


class FrameworkError(RuntimeError):
    """Raised when the framework cannot complete a test step."""


@dataclass(frozen=True)
class NodeSpec:
    name: str
    role: str
    address: str
    control_plane_id: Optional[str] = None

    @property
    def redis_url(self) -> str:
        if "://" in self.address:
            return self.address
        return f"redis://{self.address}"

    @property
    def host_port(self) -> str:
        parsed = urlparse.urlparse(self.redis_url)
        if not parsed.hostname or not parsed.port:
            raise FrameworkError(
                f"Invalid node address for {self.name!r}: {self.address!r}. "
                "Expected host:port or redis://host:port[/db]."
            )
        return f"{parsed.hostname}:{parsed.port}"


def parse_node_spec(spec: str, role: str) -> NodeSpec:
    """Parse node specs in the form 'name=address[;id=cp-instance-id]'."""

    if "=" not in spec:
        raise FrameworkError(
            f"Invalid {role} node spec {spec!r}. "
            "Use 'name=host:port' or 'name=redis://host:port/0;id=instance-id'."
        )

    name, remainder = spec.split("=", 1)
    name = name.strip()
    if not name:
        raise FrameworkError(f"Invalid {role} node spec {spec!r}: missing name.")

    address = remainder.strip()
    cp_id = None
    if ";" in remainder:
        parts = [p.strip() for p in remainder.split(";") if p.strip()]
        address = parts[0]
        for part in parts[1:]:
            if part.startswith("id="):
                cp_id = part[3:].strip()
            else:
                raise FrameworkError(
                    f"Unknown node option {part!r} in spec {spec!r}. Supported: id=<cp-instance-id>."
                )

    if not address:
        raise FrameworkError(f"Invalid {role} node spec {spec!r}: missing address.")

    return NodeSpec(name=name, role=role, address=address, control_plane_id=cp_id or None)


def parse_header_spec(spec: str) -> Tuple[str, str]:
    if ":" not in spec:
        raise FrameworkError(f"Invalid header {spec!r}. Use 'Header-Name: value'.")
    key, value = spec.split(":", 1)
    return key.strip(), value.strip()


class ControlPlaneClient:
    def __init__(
        self,
        base_url: str,
        headers: Optional[Dict[str, str]] = None,
        timeout_seconds: float = 5.0,
    ) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout_seconds = timeout_seconds
        self.headers = {"Accept": "application/json"}
        if headers:
            self.headers.update(headers)

    def _request(
        self,
        method: str,
        path: str,
        *,
        query: Optional[Dict[str, Any]] = None,
        body: Optional[Dict[str, Any]] = None,
    ) -> Tuple[int, Any]:
        url = f"{self.base_url}{path}"
        if query:
            encoded = urlparse.urlencode(
                {k: v for k, v in query.items() if v is not None}, doseq=True
            )
            if encoded:
                url = f"{url}?{encoded}"

        data = None
        headers = dict(self.headers)
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"

        req = urlrequest.Request(url, data=data, method=method.upper(), headers=headers)
        try:
            with urlrequest.urlopen(req, timeout=self.timeout_seconds) as resp:
                status = resp.getcode()
                raw = resp.read()
                if not raw:
                    return status, None
                try:
                    return status, json.loads(raw.decode("utf-8"))
                except json.JSONDecodeError:
                    return status, raw.decode("utf-8", errors="replace")
        except urlerror.HTTPError as exc:
            raw = exc.read()
            payload: Any = None
            if raw:
                try:
                    payload = json.loads(raw.decode("utf-8"))
                except json.JSONDecodeError:
                    payload = raw.decode("utf-8", errors="replace")
            return exc.code, payload
        except urlerror.URLError as exc:
            raise FrameworkError(f"Control plane request failed for {url}: {exc}") from exc

    def list_instances(self) -> List[Dict[str, Any]]:
        status, payload = self._request("GET", "/v1/instances")
        if status != 200:
            raise FrameworkError(
                f"Control plane /v1/instances returned {status}: {payload!r}"
            )
        if not isinstance(payload, dict) or not isinstance(payload.get("instances"), list):
            raise FrameworkError(f"Unexpected /v1/instances response: {payload!r}")
        return payload["instances"]

    def locate_key(self, key: str, source_instance_id: Optional[str] = None) -> Dict[str, Any]:
        status, payload = self._request(
            "GET",
            f"/v1/content/locate/{urlparse.quote(key, safe='')}",
            query={"sourceInstanceId": source_instance_id},
        )
        if status != 200:
            raise FrameworkError(
                f"Control plane locate for key {key!r} returned {status}: {payload!r}"
            )
        if not isinstance(payload, dict):
            raise FrameworkError(f"Unexpected locate response: {payload!r}")
        return payload

    def find_nearest_source(self, key: str, source_instance_id: str) -> Tuple[int, Any]:
        return self._request(
            "GET",
            f"/v1/content/nearest/{urlparse.quote(key, safe='')}",
            query={"sourceInstanceId": source_instance_id},
        )

    def get_instance(self, instance_id: str) -> Dict[str, Any]:
        status, payload = self._request("GET", f"/v1/instances/{urlparse.quote(instance_id, safe='')}")
        if status != 200:
            raise FrameworkError(
                f"Control plane get instance {instance_id!r} returned {status}: {payload!r}"
            )
        if not isinstance(payload, dict):
            raise FrameworkError(f"Unexpected instance response: {payload!r}")
        return payload

    def resolve_instance_id(self, node: NodeSpec) -> str:
        if node.control_plane_id:
            return node.control_plane_id

        instances = self.list_instances()
        target = node.host_port.lower()
        matches: List[str] = []
        for inst in instances:
            address = str(inst.get("address", "")).strip().lower()
            if not address:
                continue
            if address == target:
                matches.append(str(inst.get("id")))
                continue
            if "://" in address:
                try:
                    parsed = urlparse.urlparse(address)
                    if parsed.hostname and parsed.port and f"{parsed.hostname}:{parsed.port}".lower() == target:
                        matches.append(str(inst.get("id")))
                except Exception:
                    pass

        matches = [m for m in matches if m and m != "None"]
        if not matches:
            raise FrameworkError(
                f"Could not resolve control plane instance id for node {node.name!r} "
                f"({node.host_port}). Provide ';id=<instance-id>' in the node spec."
            )
        if len(set(matches)) > 1:
            raise FrameworkError(
                f"Multiple control plane instances matched {node.name!r} ({node.host_port}): {matches}. "
                "Provide an explicit id=... in the node spec."
            )
        return matches[0]


class RedisNodeClient:
    def __init__(self, node: NodeSpec) -> None:
        self.node = node
        self._client = None

    def _ensure_client(self) -> Any:
        if self._client is not None:
            return self._client
        try:
            import redis  # type: ignore
        except ImportError as exc:
            raise FrameworkError(
                "Missing dependency 'redis'. Install it with: pip install redis"
            ) from exc
        self._client = redis.Redis.from_url(self.node.redis_url, decode_responses=False)
        return self._client

    def set_value(self, key: str, value: bytes) -> None:
        ok = self._ensure_client().set(key, value)
        if not ok:
            raise FrameworkError(f"SET failed for key {key!r} on node {self.node.name!r}")

    def get_value(self, key: str) -> Optional[bytes]:
        value = self._ensure_client().get(key)
        if value is None:
            return None
        if isinstance(value, bytes):
            return value
        if isinstance(value, str):
            return value.encode("utf-8")
        raise FrameworkError(
            f"Unexpected GET return type {type(value)} for key {key!r} on {self.node.name!r}"
        )

    def delete_key(self, key: str) -> int:
        return int(self._ensure_client().delete(key))


@dataclass
class PollObservation:
    holders: List[str]
    locate_response: Dict[str, Any]
    attempts: int
    elapsed_seconds: float


class CacheTestFramework:
    def __init__(
        self,
        control_plane: ControlPlaneClient,
        nodes: Sequence[NodeSpec],
        *,
        poll_interval_seconds: float = 0.5,
        timeout_seconds: float = 10.0,
    ) -> None:
        self.control_plane = control_plane
        self.poll_interval_seconds = poll_interval_seconds
        self.timeout_seconds = timeout_seconds
        self.nodes: Dict[str, NodeSpec] = {}
        self.node_clients: Dict[str, RedisNodeClient] = {}
        self.node_ids: Dict[str, str] = {}

        for node in nodes:
            if node.name in self.nodes:
                raise FrameworkError(f"Duplicate node name {node.name!r}")
            self.nodes[node.name] = node
            self.node_clients[node.name] = RedisNodeClient(node)

    def initialize(self) -> None:
        for node in self.nodes.values():
            self.node_ids[node.name] = self.control_plane.resolve_instance_id(node)

    def node(self, name: str) -> NodeSpec:
        if name not in self.nodes:
            raise FrameworkError(
                f"Unknown node {name!r}. Available nodes: {', '.join(sorted(self.nodes))}"
            )
        return self.nodes[name]

    def node_id(self, name: str) -> str:
        if name not in self.node_ids:
            raise FrameworkError(
                f"Control plane IDs not resolved for node {name!r}. Did initialize() run?"
            )
        return self.node_ids[name]

    def _holders_from_locate(self, locate_response: Dict[str, Any]) -> List[str]:
        instances = locate_response.get("instances", [])
        if not isinstance(instances, list):
            raise FrameworkError(f"Invalid locate response format: {locate_response!r}")
        holders: List[str] = []
        for item in instances:
            if isinstance(item, dict) and item.get("instanceId") is not None:
                holders.append(str(item["instanceId"]))
        return holders

    def poll_until_holder(
        self,
        *,
        key: str,
        holder_instance_id: str,
        source_instance_id: Optional[str] = None,
        timeout_seconds: Optional[float] = None,
    ) -> PollObservation:
        timeout = self.timeout_seconds if timeout_seconds is None else timeout_seconds
        start = time.monotonic()
        attempts = 0
        last_locate: Dict[str, Any] = {"key": key, "instances": []}

        while True:
            attempts += 1
            last_locate = self.control_plane.locate_key(key, source_instance_id=source_instance_id)
            holders = self._holders_from_locate(last_locate)
            if holder_instance_id in holders:
                return PollObservation(
                    holders=holders,
                    locate_response=last_locate,
                    attempts=attempts,
                    elapsed_seconds=time.monotonic() - start,
                )

            elapsed = time.monotonic() - start
            if elapsed >= timeout:
                raise FrameworkError(
                    f"Timed out waiting for control plane to show key {key!r} on instance "
                    f"{holder_instance_id!r}. Last holders={holders}"
                )
            time.sleep(self.poll_interval_seconds)

    def write_and_verify(
        self,
        *,
        node_name: str,
        key: str,
        value: bytes,
        timeout_seconds: Optional[float] = None,
    ) -> Dict[str, Any]:
        node_id = self.node_id(node_name)
        self.node_clients[node_name].set_value(key, value)
        observation = self.poll_until_holder(
            key=key,
            holder_instance_id=node_id,
            source_instance_id=node_id,
            timeout_seconds=timeout_seconds,
        )
        return {
            "operation": "write_and_verify",
            "node": node_name,
            "nodeId": node_id,
            "key": key,
            "valueBytes": len(value),
            "holders": observation.holders,
            "pollAttempts": observation.attempts,
            "elapsedSeconds": round(observation.elapsed_seconds, 3),
            "locate": observation.locate_response,
        }

    def read_and_verify(
        self,
        *,
        node_name: str,
        key: str,
        expected_value: Optional[bytes] = None,
        require_state_change: bool = False,
        timeout_seconds: Optional[float] = None,
    ) -> Dict[str, Any]:
        node_id = self.node_id(node_name)

        pre_locate = self.control_plane.locate_key(key, source_instance_id=node_id)
        pre_holders = self._holders_from_locate(pre_locate)

        value = self.node_clients[node_name].get_value(key)
        if value is None:
            raise FrameworkError(
                f"GET returned nil for key {key!r} on node {node_name!r}. "
                "If this was expected, model it as a different scenario."
            )
        if expected_value is not None and value != expected_value:
            raise FrameworkError(
                f"GET returned unexpected value for key {key!r} on {node_name!r}: "
                f"{value!r} != {expected_value!r}"
            )

        observation = self.poll_until_holder(
            key=key,
            holder_instance_id=node_id,
            source_instance_id=node_id,
            timeout_seconds=timeout_seconds,
        )
        if require_state_change and node_id in pre_holders:
            raise FrameworkError(
                f"Expected control plane state change for read on {node_name!r}, but instance "
                f"{node_id!r} already held key {key!r} before the read."
            )

        status = self.control_plane.get_instance(node_id)
        return {
            "operation": "read_and_verify",
            "node": node_name,
            "nodeId": node_id,
            "key": key,
            "value": self._decode_bytes_safe(value),
            "valueBytes": len(value),
            "preReadHolders": pre_holders,
            "postReadHolders": observation.holders,
            "stateChanged": node_id not in pre_holders,
            "pollAttempts": observation.attempts,
            "elapsedSeconds": round(observation.elapsed_seconds, 3),
            "locate": observation.locate_response,
            "instanceStatus": status,
        }

    def delete(self, *, node_name: str, key: str) -> Dict[str, Any]:
        node_id = self.node_id(node_name)
        deleted = self.node_clients[node_name].delete_key(key)
        return {
            "operation": "delete",
            "node": node_name,
            "nodeId": node_id,
            "key": key,
            "deleted": deleted,
        }

    def run_scenario_file(self, path: Path, scenario_name: Optional[str] = None) -> Dict[str, Any]:
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError as exc:
            raise FrameworkError(f"Scenario file not found: {path}") from exc
        except json.JSONDecodeError as exc:
            raise FrameworkError(f"Invalid JSON in scenario file {path}: {exc}") from exc

        if isinstance(payload, dict) and "scenarios" in payload:
            scenarios = payload["scenarios"]
        elif isinstance(payload, dict) and "steps" in payload:
            scenarios = [payload]
        elif isinstance(payload, list):
            scenarios = payload
        else:
            raise FrameworkError(
                "Scenario file must be a scenario object with 'steps', an object with "
                "'scenarios', or a list of scenario objects."
            )

        if not isinstance(scenarios, list) or not scenarios:
            raise FrameworkError("Scenario file contains no scenarios.")

        selected: List[Dict[str, Any]] = []
        for scenario in scenarios:
            if not isinstance(scenario, dict):
                raise FrameworkError(f"Invalid scenario entry: {scenario!r}")
            name = str(scenario.get("name", "unnamed"))
            if scenario_name is None or name == scenario_name:
                selected.append(scenario)

        if scenario_name is not None and not selected:
            raise FrameworkError(f"Scenario {scenario_name!r} not found in {path}")

        results: List[Dict[str, Any]] = []
        for scenario in selected:
            results.append(self._run_scenario(scenario))

        return {
            "operation": "run_scenario",
            "file": str(path),
            "scenarioCount": len(results),
            "results": results,
        }

    def _run_scenario(self, scenario: Dict[str, Any]) -> Dict[str, Any]:
        name = str(scenario.get("name", "unnamed"))
        steps = scenario.get("steps")
        if not isinstance(steps, list) or not steps:
            raise FrameworkError(f"Scenario {name!r} has no valid 'steps' list.")

        outputs: List[Dict[str, Any]] = []
        started = time.monotonic()
        for index, step in enumerate(steps, start=1):
            if not isinstance(step, dict):
                raise FrameworkError(f"Scenario {name!r} step {index} is not an object.")
            outputs.append(self._run_step(name, index, step))

        return {
            "scenario": name,
            "stepCount": len(outputs),
            "elapsedSeconds": round(time.monotonic() - started, 3),
            "steps": outputs,
        }

    def _run_step(self, scenario_name: str, index: int, step: Dict[str, Any]) -> Dict[str, Any]:
        op = str(step.get("op", "")).strip()
        if not op:
            raise FrameworkError(f"Scenario {scenario_name!r} step {index} is missing 'op'.")

        if op == "sleep":
            seconds = float(step.get("seconds", 1))
            time.sleep(seconds)
            return {"op": "sleep", "seconds": seconds}

        if op == "write":
            node = str(step["node"])
            key = str(step["key"])
            value = self._value_from_step(step)
            result = self.write_and_verify(
                node_name=node,
                key=key,
                value=value,
                timeout_seconds=self._opt_float(step.get("timeout_seconds")),
            )
            return {"op": "write", "step": index, "result": result}

        if op == "read":
            node = str(step["node"])
            key = str(step["key"])
            expected = step.get("expected_value")
            expected_bytes = None if expected is None else str(expected).encode("utf-8")
            result = self.read_and_verify(
                node_name=node,
                key=key,
                expected_value=expected_bytes,
                require_state_change=bool(step.get("require_state_change", False)),
                timeout_seconds=self._opt_float(step.get("timeout_seconds")),
            )
            return {"op": "read", "step": index, "result": result}

        if op == "delete":
            node = str(step["node"])
            key = str(step["key"])
            return {"op": "delete", "step": index, "result": self.delete(node_name=node, key=key)}

        if op == "assert_locate_contains":
            key = str(step["key"])
            node_name = str(step["node"])
            source_name = step.get("source_node")
            source_id = self.node_id(str(source_name)) if source_name else None
            locate = self.control_plane.locate_key(key, source_instance_id=source_id)
            holders = self._holders_from_locate(locate)
            node_id = self.node_id(node_name)
            if node_id not in holders:
                raise FrameworkError(
                    f"Scenario {scenario_name!r} step {index}: locate for {key!r} does not contain "
                    f"{node_name!r}/{node_id!r}. Holders={holders}"
                )
            return {
                "op": "assert_locate_contains",
                "step": index,
                "key": key,
                "node": node_name,
                "nodeId": node_id,
                "holders": holders,
                "locate": locate,
            }

        raise FrameworkError(
            f"Scenario {scenario_name!r} step {index} uses unsupported op {op!r}. "
            "Supported ops: write, read, delete, assert_locate_contains, sleep"
        )

    @staticmethod
    def _value_from_step(step: Dict[str, Any]) -> bytes:
        if "value_base64" in step:
            return base64.b64decode(str(step["value_base64"]))
        if "value" not in step:
            raise FrameworkError("Write step requires 'value' or 'value_base64'.")
        return str(step["value"]).encode("utf-8")

    @staticmethod
    def _decode_bytes_safe(value: bytes) -> str:
        try:
            return value.decode("utf-8")
        except UnicodeDecodeError:
            return base64.b64encode(value).decode("ascii")

    @staticmethod
    def _opt_float(value: Any) -> Optional[float]:
        if value is None:
            return None
        return float(value)


def build_parser() -> argparse.ArgumentParser:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "--control-plane",
        required=True,
        help="Control plane base URL, e.g. http://localhost:8080",
    )
    common.add_argument(
        "--root",
        required=True,
        help="Root node spec: name=host:port or name=redis://host:port/0;id=<cp-instance-id>",
    )
    common.add_argument(
        "--branch",
        action="append",
        default=[],
        help="Branch node spec (repeatable): name=host:port[;id=<cp-instance-id>]",
    )
    common.add_argument(
        "--leaf",
        action="append",
        default=[],
        help="Leaf node spec (repeatable): name=host:port[;id=<cp-instance-id>]",
    )
    common.add_argument(
        "--cp-header",
        action="append",
        default=[],
        help="Extra control plane HTTP header (repeatable), format: 'Header-Name: value'",
    )
    common.add_argument(
        "--timeout-seconds",
        type=float,
        default=10.0,
        help="Default timeout for control plane convergence checks (default: 10)",
    )
    common.add_argument(
        "--poll-interval-seconds",
        type=float,
        default=0.5,
        help="Polling interval for control plane checks (default: 0.5)",
    )

    parser = argparse.ArgumentParser(
        description="HAL cache test framework for node I/O + control plane verification"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    p_write = subparsers.add_parser(
        "write-verify", parents=[common], help="Write a key to a node and verify control plane mapping"
    )
    p_write.add_argument("--node", required=True, help="Node name (from --root/--branch/--leaf specs)")
    p_write.add_argument("--key", required=True, help="Cache key")
    p_write_group = p_write.add_mutually_exclusive_group(required=True)
    p_write_group.add_argument("--value", help="UTF-8 value to write")
    p_write_group.add_argument("--value-base64", help="Base64-encoded bytes to write")

    p_read = subparsers.add_parser(
        "read-verify", parents=[common], help="Read a key from a node and verify control plane mapping"
    )
    p_read.add_argument("--node", required=True, help="Node name (from --root/--branch/--leaf specs)")
    p_read.add_argument("--key", required=True, help="Cache key")
    p_read.add_argument("--expected-value", help="Optional expected UTF-8 value")
    p_read.add_argument(
        "--require-state-change",
        action="store_true",
        help="Fail if the node already appeared as a holder in control plane before the read",
    )

    p_scenario = subparsers.add_parser(
        "run-scenario", parents=[common], help="Run one or more scenarios from a JSON file"
    )
    p_scenario.add_argument("--file", required=True, help="Scenario JSON file")
    p_scenario.add_argument("--scenario", help="Run only the named scenario from the file")

    return parser


def build_framework_from_args(args: argparse.Namespace) -> CacheTestFramework:
    headers = dict(parse_header_spec(item) for item in (args.cp_header or []))
    control_plane = ControlPlaneClient(
        args.control_plane,
        headers=headers,
        timeout_seconds=max(args.timeout_seconds, 1.0),
    )

    nodes = [parse_node_spec(args.root, "root")]
    nodes.extend(parse_node_spec(spec, "branch") for spec in (args.branch or []))
    nodes.extend(parse_node_spec(spec, "leaf") for spec in (args.leaf or []))

    framework = CacheTestFramework(
        control_plane,
        nodes,
        poll_interval_seconds=args.poll_interval_seconds,
        timeout_seconds=args.timeout_seconds,
    )
    framework.initialize()
    return framework


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        framework = build_framework_from_args(args)

        if args.command == "write-verify":
            if args.value_base64 is not None:
                value = base64.b64decode(args.value_base64)
            else:
                value = args.value.encode("utf-8")
            result = framework.write_and_verify(node_name=args.node, key=args.key, value=value)
        elif args.command == "read-verify":
            expected = None if args.expected_value is None else args.expected_value.encode("utf-8")
            result = framework.read_and_verify(
                node_name=args.node,
                key=args.key,
                expected_value=expected,
                require_state_change=args.require_state_change,
            )
        elif args.command == "run-scenario":
            result = framework.run_scenario_file(Path(args.file), scenario_name=args.scenario)
        else:
            raise FrameworkError(f"Unsupported command {args.command!r}")

        print(json.dumps({"ok": True, "result": result}, indent=2, sort_keys=True))
        return 0
    except FrameworkError as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, indent=2))
        return 2
    except Exception as exc:  # pragma: no cover - safety net
        print(json.dumps({"ok": False, "error": f"Unexpected error: {exc}"}, indent=2))
        return 3


if __name__ == "__main__":
    sys.exit(main())
