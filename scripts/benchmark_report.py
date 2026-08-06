#!/usr/bin/env python3
"""Validate and compare the deterministic Issue #33 benchmark artifacts."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import statistics
import sys
from decimal import Decimal, ROUND_HALF_EVEN, InvalidOperation
from pathlib import Path
from typing import Any, Iterable

SCHEMA_VERSION = "benchmark-v1"
ACTUAL_STATUSES = {"delta-only", "incomparable", "no-reference"}
REQUIRED_METRICS = (
    "N01/BM_ParamCacheGetScalar/10000",
    "N01/BM_ParamCacheGetSpan/10000",
    "N01/BM_DirectLookupScalar/10000",
    "N01/BM_DirectLookupSpan/10000",
    "N02/TakeSnapshot/1000",
    "N02/TakeSnapshot/100000",
    "N08_SESSION_START_V1/session_median_ns",
    "N09_VISIBILITY_S64_V1/aggregate_median_ns",
    "N09_CONTROL_RTT_V1/aggregate_median_ns",
    "N09_CALLBACK_THROUGHPUT_S64_1P_V1/median_updates_per_second",
    "N09_CALLBACK_THROUGHPUT_S64_4P_V1/median_updates_per_second",
)


def _decimal(value: Any) -> Decimal:
    if isinstance(value, bool):
        raise ValueError("boolean is not numeric")
    try:
        return Decimal(str(value))
    except (InvalidOperation, ValueError) as exc:
        raise ValueError(f"invalid decimal value: {value!r}") from exc


def _quantize(value: Decimal, places: int) -> str:
    quantum = Decimal(1).scaleb(-places)
    return format(value.quantize(quantum, rounding=ROUND_HALF_EVEN), "f")


def median(values: Iterable[Any]) -> Decimal:
    items = sorted(_decimal(x) for x in values)
    if not items:
        raise ValueError("empty sample set")
    middle = len(items) // 2
    if len(items) % 2:
        return items[middle]
    return (items[middle - 1] + items[middle]) / Decimal(2)


def mad(values: Iterable[Any]) -> Decimal:
    items = [_decimal(x) for x in values]
    centre = median(items)
    return median(abs(item - centre) for item in items)


def p95(values: Iterable[Any]) -> Decimal:
    items = sorted(_decimal(x) for x in values)
    if not items:
        raise ValueError("empty sample set")
    index = math.ceil(Decimal("0.95") * len(items)) - 1
    return items[index]


def compare_records(*args: Any, **kwargs: Any) -> dict[str, Any]:
    """Compare one metric using exact Decimal arithmetic.

    The compatibility keyword form is used by the deterministic synthetic fixture.
    Production records use ``value``, ``reference``, ``threshold_percent``, and
    ``direction`` with the same semantics.
    """
    result = _decimal(kwargs.get("result", args[0] if args else None))
    reference = _decimal(kwargs.get("reference", args[1] if len(args) > 1 else None))
    threshold = _decimal(kwargs.get("threshold_percent", args[2] if len(args) > 2 else None))
    direction = kwargs.get("direction", "lower-is-better")
    difference = result - reference
    percentage = None if reference == 0 else difference / abs(reference) * Decimal(100)
    if direction == "lower-is-better":
        regression = difference > reference.copy_abs() * threshold / Decimal(100)
    elif direction == "higher-is-better":
        regression = difference < -reference.copy_abs() * threshold / Decimal(100)
    else:
        raise ValueError(f"unsupported metric direction: {direction}")
    return {
        "status": "flagged-regression" if regression else "within-reference",
        "blocking": bool(regression),
        "absolute_delta": _quantize(difference, 6),
        "percentage_delta": None if percentage is None else _quantize(percentage, 6),
    }


def _partition(environment: dict[str, Any]) -> tuple[str, str]:
    fields = environment.get("partition_fields")
    if not isinstance(fields, dict):
        raise ValueError("environment.partition_fields is required")
    required = {
        "cpu_model", "runner_name", "runner_class", "image", "os", "kernel",
        "compiler", "compiler_version", "cmake", "ninja", "benchmark_version",
        "build_type", "build_flags", "zenoh_mode", "zenoh_version", "rocksdb_mode",
        "rocksdb_version", "power_frequency_controls",
    }
    if set(fields) != required or any(not isinstance(v, str) for v in fields.values()):
        raise ValueError("partition_fields must contain exactly the frozen string fields")
    encoded = json.dumps(fields, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest(), encoded.decode("utf-8")


def _load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read JSON {path}: {exc}") from exc


def validate_policy(policy: dict[str, Any]) -> None:
    if policy.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unsupported policy schema_version")
    if policy.get("runner_policy") != "github-hosted-ubuntu-informational":
        raise ValueError("runner policy must remain hosted informational")
    if policy.get("actual_thresholds") != "none—not established":
        raise ValueError("actual thresholds must remain none—not established")
    for key in ("statistics", "scenarios", "workflow"):
        if key not in policy:
            raise ValueError(f"policy missing {key}")


def _record_key(record: dict[str, Any]) -> str:
    return f"{record.get('scenario_id')}/{record.get('metric')}"


def validate_result(result: dict[str, Any], *, raw_path: Path) -> tuple[dict[str, Any], str]:
    if result.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unsupported result schema_version")
    environment = result.get("environment")
    if not isinstance(environment, dict):
        raise ValueError("result environment is required")
    partition, _ = _partition(environment)
    if result.get("environment_partition") != partition:
        raise ValueError("result environment partition mismatch")
    samples = result.get("records")
    if not isinstance(samples, list) or not samples:
        raise ValueError("result records must be non-empty")
    required = {"scenario_id", "metric", "unit", "sample_count", "statistic", "value", "classification", "threshold_rationale", "source_commit", "source_kind", "timestamp", "evidence_url", "run_url", "source_artifact_sha256"}
    raw_digest = hashlib.sha256(raw_path.read_bytes()).hexdigest()
    for record in samples:
        if not required.issubset(record):
            raise ValueError(f"record missing fields: {_record_key(record)}")
        if record["run_url"] in (None, "") or record["source_artifact_sha256"] in (None, ""):
            raise ValueError(f"new record requires run_url and source_artifact_sha256: {_record_key(record)}")
        if record["source_artifact_sha256"] != raw_digest:
            raise ValueError(f"raw artifact digest mismatch: {_record_key(record)}")
        if record["classification"] not in {"informational", "confirmation-required", "blocking"}:
            raise ValueError(f"invalid classification: {_record_key(record)}")
        if not isinstance(record["sample_count"], int) or record["sample_count"] <= 0:
            raise ValueError(f"invalid sample count: {_record_key(record)}")
        _decimal(record["value"])
    found = {_record_key(r) for r in samples}
    missing = set(REQUIRED_METRICS) - found
    if missing:
        raise ValueError(f"missing required metrics: {sorted(missing)}")
    return environment, partition


def _reference_index(reference: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    if reference.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unsupported baseline schema_version")
    records = reference.get("records")
    if not isinstance(records, list):
        raise ValueError("baseline records must be a list")
    state = reference.get("state")
    if state not in {"initialization-pending", "complete"}:
        raise ValueError("baseline state must be initialization-pending or complete")
    index: dict[str, list[dict[str, Any]]] = {}
    for record in records:
        key = _record_key(record)
        for field in ("scenario_id", "metric", "value", "environment_partition", "source_kind", "classification"):
            if field not in record:
                raise ValueError(f"baseline record missing {field}: {key}")
        if record["source_kind"] != "owner-local" and (not record.get("run_url") or not record.get("source_artifact_sha256")):
            raise ValueError(f"baseline provenance incomplete: {key}")
        index.setdefault(key, []).append(record)
    return index


def compare_result_records(result: dict[str, Any], reference: dict[str, Any], *, require_complete: bool) -> list[dict[str, Any]]:
    index = _reference_index(reference)
    current_partition = result["environment_partition"]
    output = []
    for record in result["records"]:
        key = _record_key(record)
        candidates = index.get(key, [])
        if not candidates:
            if require_complete:
                raise ValueError(f"missing required reference record: {key}")
            status = "no-reference"
            comparison = {"status": status, "absolute_delta": None, "percentage_delta": None}
        else:
            compatible = [item for item in candidates if item["environment_partition"] == current_partition]
            if not compatible:
                comparison = {"status": "incomparable", "absolute_delta": None, "percentage_delta": None}
            else:
                ref = compatible[0]
                value = _decimal(record["value"])
                reference_value = _decimal(ref["value"])
                delta = value - reference_value
                percentage = None if reference_value == 0 else delta / abs(reference_value) * Decimal(100)
                comparison = {
                    "status": "delta-only",
                    "absolute_delta": _quantize(delta, 6),
                    "percentage_delta": None if percentage is None else _quantize(percentage, 6),
                }
        output.append({**record, "comparison": comparison})
    return output


def _markdown(rows: list[dict[str, Any]], *, state: str, partition: str) -> str:
    lines = ["# Benchmark report", "", f"- Baseline state: `{state}`", f"- Environment partition: `{partition}`", "", "| Metric | Value | Target/reference status | Delta |", "|---|---:|---|---:|"]
    for row in rows:
        comp = row["comparison"]
        lines.append(f"| `{_record_key(row)}` | `{row['value']}` | `{comp['status']}` | `{comp['absolute_delta']}` |")
    lines.extend(["", "Historical hosted timing thresholds are `none—not established`; timing statuses are informational.", ""])
    return "\n".join(lines)


def build_report(result_path: Path, reference_path: Path, policy_path: Path, *, require_complete: bool, raw_artifact_path: Path | None = None) -> tuple[dict[str, Any], str]:
    policy = _load_json(policy_path)
    reference = _load_json(reference_path)
    result = _load_json(result_path)
    validate_policy(policy)
    validate_result(result, raw_path=raw_artifact_path or result_path)
    if require_complete and reference.get("state") != "complete":
        raise ValueError("final report requires complete baseline")
    rows = compare_result_records(result, reference, require_complete=require_complete)
    comparison = {"schema_version": SCHEMA_VERSION, "records": rows, "environment_partition": result["environment_partition"], "baseline_state": reference["state"]}
    return comparison, _markdown(rows, state=reference["state"], partition=result["environment_partition"])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-markdown", type=Path, required=True)
    parser.add_argument("--raw-artifact", type=Path)
    parser.add_argument("--require-complete-reference", action="store_true")
    args = parser.parse_args(argv)
    try:
        comparison, markdown = build_report(args.result, args.reference, args.policy, require_complete=args.require_complete_reference, raw_artifact_path=args.raw_artifact)
        args.output_json.write_text(json.dumps(comparison, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        args.output_markdown.write_text(markdown, encoding="utf-8")
    except (OSError, ValueError, KeyError) as exc:
        print(f"benchmark report error: {exc}", file=sys.stderr)
        return 2
    print(markdown, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
