#!/usr/bin/env python3
"""Contract and deterministic semantic tests for Issue #33."""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from benchmark_report import (  # noqa: E402
    REQUIRED_METRICS,
    _partition,
    build_report,
    compare_records,
    mad,
    median,
    p95,
)

EXPECTED = [
    ROOT / ".github/workflows/bench.yml",
    ROOT / "tests/bench/CMakeLists.txt",
    ROOT / "tests/bench/process_bench.cpp",
    ROOT / "tests/bench/benchmark_policy.json",
    ROOT / "tests/bench/reference_baseline.json",
    ROOT / "scripts/benchmark_report.py",
    ROOT / "tests/bench/test_benchmark_report.py",
    ROOT / "tests/bench/rocksdb_snapshot_bench.cpp",
    ROOT / "docs/06_build_test_packaging.md",
    ROOT / "docs/07_issue_breakdown.md",
]


def test_expected_artifacts_exist() -> None:
    missing = [str(path.relative_to(ROOT)) for path in EXPECTED if not path.is_file()]
    assert not missing, f"missing Issue #33 artifacts: {missing}"


def test_decimal_statistics_are_exact() -> None:
    assert median(["1.1", "2.2", "3.3", "4.4"]) == 2.75
    assert mad(["1", "2", "3", "4"]) == 1
    assert p95(range(1, 21)) == 19


def test_synthetic_regression_is_flagged() -> None:
    result = compare_records(
        result=111, reference=100, threshold_percent=10, direction="lower-is-better"
    )
    assert result["status"] == "flagged-regression"
    assert result["blocking"] is True


def _environment() -> dict[str, object]:
    fields = {
        "cpu_model": "test-cpu", "runner_name": "test-runner", "runner_class": "test",
        "image": "test-image", "os": "Linux", "kernel": "test", "compiler": "gcc",
        "compiler_version": "13", "cmake": "3.30", "ninja": "1.11",
        "benchmark_version": "1.8.3", "build_type": "Release", "build_flags": "none",
        "zenoh_mode": "ON", "zenoh_version": "1.6", "rocksdb_mode": "ON",
        "rocksdb_version": "11.1.2", "power_frequency_controls": "unavailable",
    }
    partition, _ = _partition({"partition_fields": fields})
    return {"partition_fields": fields, "partition": partition}


def _result(path: Path, partition: str | None = None) -> dict[str, object]:
    partition = _environment()["partition"]
    records = []
    for key in REQUIRED_METRICS:
        scenario, metric = key.split("/", 1)
        records.append({
            "scenario_id": scenario, "metric": metric, "unit": "ns", "sample_count": 5,
            "statistic": "median", "value": "100", "classification": "informational",
            "threshold_rationale": "none—not established", "source_commit": "abc",
            "source_kind": "pull-request", "timestamp": "2026-08-06T00:00:00Z",
            "evidence_url": "https://example.invalid/run", "run_url": "https://example.invalid/run",
            "source_artifact_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "environment_partition": partition,
        })
    return {"schema_version": "benchmark-v1", "environment": _environment(),
            "environment_partition": partition, "records": records}


def test_reference_modes_distinguish_missing_incompatible_and_compatible(tmp_path: Path) -> None:
    raw = tmp_path / "raw.json"
    raw.write_text('{"samples":[]}', encoding="utf-8")
    result = _result(raw)
    base = {"schema_version": "benchmark-v1", "state": "initialization-pending", "records": []}
    reference = tmp_path / "reference.json"
    reference.write_text(json.dumps(base), encoding="utf-8")
    policy = ROOT / "tests/bench/benchmark_policy.json"
    comparison, _ = build_report_from_objects_for_test(result, reference, raw, policy)
    assert {row["comparison"]["status"] for row in comparison["records"]} == {"no-reference"}

    base["state"] = "complete"
    base["records"] = [
        {"scenario_id": key.split("/")[0], "metric": key.split("/", 1)[1],
         "value": "90", "environment_partition": "other", "source_kind": "pull-request",
         "run_url": "https://example.invalid/run", "source_artifact_sha256": "a" * 64,
         "classification": "informational"}
        for key in REQUIRED_METRICS
    ]
    reference.write_text(json.dumps(base), encoding="utf-8")
    comparison, _ = build_report_from_objects_for_test(result, reference, raw, policy, require_complete=True)
    assert comparison["records"][0]["comparison"]["status"] == "incomparable"

    base["records"][0]["environment_partition"] = result["environment_partition"]
    reference.write_text(json.dumps(base), encoding="utf-8")
    comparison, _ = build_report_from_objects_for_test(result, reference, raw, policy, require_complete=True)
    assert comparison["records"][0]["comparison"]["status"] == "delta-only"


def build_report_from_objects_for_test(result: dict[str, object], reference: Path, raw: Path,
                                       policy: Path, require_complete: bool = False):
    result_path = raw.parent / "result.json"
    result_path.write_text(json.dumps(result), encoding="utf-8")
    return build_report(result_path, reference, policy, require_complete=require_complete, raw_artifact_path=raw)


def test_workflow_security_and_truth_table() -> None:
    workflow = (ROOT / ".github/workflows/bench.yml").read_text(encoding="utf-8")
    assert "pull_request_target" not in workflow
    assert "permissions:\n  contents: read" in workflow
    assert "contains(github.event.pull_request.labels.*.name, 'bench')" in workflow
    assert "actions/checkout@fbc6f3992d24b796d5a048ff273f7fcc4a7b6c09" in workflow
    assert "actions/cache/restore@55cc8345863c7cc4c66a329aec7e433d2d1c52a9" in workflow
    assert "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02" in workflow
    assert "timeout-minutes: 180" in workflow and "retention-days: 90" in workflow
    assert "--require-complete-reference" in workflow
    assert "sitos_cache_get_bench" in workflow and "sitos_rocksdb_snapshot_bench" in workflow
    assert "sitos_process_bench" in workflow


def test_policy_and_workload_fences() -> None:
    policy = json.loads((ROOT / "tests/bench/benchmark_policy.json").read_text(encoding="utf-8"))
    assert policy["actual_thresholds"] == "none—not established"
    assert policy["workload_identity"]["N08_lut_bytes"] == 100000000
    assert policy["workload_identity"]["N08_lut_sha256"] == "7975a2b50c79617f9a7d0e02702cb2c0fa533dd083fc999e6316c852fc06f2aa"
    assert policy["baseline"]["workflow_writes_baseline"] is False
    source = (ROOT / "tests/bench/process_bench.cpp").read_text(encoding="utf-8")
    assert "n08/v1/scalar/" in source and "n09/v1/value/" in source
    assert "0x13579bdf00000000ULL" in source
    rocks = (ROOT / "tests/bench/rocksdb_snapshot_bench.cpp").read_text(encoding="utf-8")
    assert "RocksDBEngine::Put failed" in rocks
    assert "TakeSnapshot returned null" in rocks


def test_report_cli_rejects_final_incomplete_reference(tmp_path: Path) -> None:
    raw = tmp_path / "raw.json"
    raw.write_text('{"samples":[]}', encoding="utf-8")
    env = _environment()
    result = _result(raw, env["partition"])
    result_path = tmp_path / "result.json"
    result_path.write_text(json.dumps(result), encoding="utf-8")
    reference = tmp_path / "reference.json"
    reference.write_text(json.dumps({"schema_version": "benchmark-v1", "state": "initialization-pending", "records": []}), encoding="utf-8")
    out_json, out_md = tmp_path / "out.json", tmp_path / "out.md"
    proc = subprocess.run([sys.executable, str(ROOT / "scripts/benchmark_report.py"), "--require-complete-reference", "--result", str(result_path), "--raw-artifact", str(raw), "--reference", str(reference), "--policy", str(ROOT / "tests/bench/benchmark_policy.json"), "--output-json", str(out_json), "--output-markdown", str(out_md)], capture_output=True, text=True)
    assert proc.returncode != 0
    assert "complete baseline" in proc.stderr


def main() -> None:
    for test in (
        test_expected_artifacts_exist, test_decimal_statistics_are_exact,
        test_synthetic_regression_is_flagged, test_workflow_security_and_truth_table,
        test_policy_and_workload_fences,
    ):
        test()
    print("benchmark contract tests passed")


if __name__ == "__main__":
    main()
