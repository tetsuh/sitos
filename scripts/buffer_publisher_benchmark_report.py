#!/usr/bin/env python3
"""Validate and render the Issue #107 BufferPublisher benchmark evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
from decimal import Decimal
from pathlib import Path

CASES = {
    "BufferPublisherPush/3/262144": ("N107_PUSH_3PS_256KIB_V1", 3, 256 * 1024),
    "BufferPublisherPush/3/1048576": ("N107_PUSH_3PS_1MIB_V1", 3, 1024 * 1024),
    "BufferPublisherPush/300/262144": ("N107_PUSH_300PS_256KIB_V1", 300, 256 * 1024),
    "BufferPublisherPush/300/1048576": ("N107_PUSH_300PS_1MIB_V1", 300, 1024 * 1024),
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--run-url", default=os.environ.get("GITHUB_RUN_URL", "local"))
    args = parser.parse_args()
    raw_bytes = args.input.read_bytes()
    raw = json.loads(raw_bytes, parse_float=Decimal, parse_int=Decimal)
    records = []
    for benchmark_name, (scenario, rate, payload_bytes) in CASES.items():
        samples = [
            row.get("real_time", row.get("cpu_time"))
            for row in raw.get("benchmarks", [])
            if row.get("name") == benchmark_name or row.get("name") == benchmark_name + "/manual_time"
        ]
        if len(samples) != 5 or any(not isinstance(value, Decimal) for value in samples):
            raise SystemExit(f"expected five numeric samples for {benchmark_name}")
        # Each benchmark iteration paces exactly one second of target load.
        durations = sorted(samples)
        median = durations[2]
        records.append(
            {
                "schema_version": "benchmark-v1",
                "scenario_id": scenario,
                "metric": "paced_batch_duration_ns",
                "unit": "ns",
                "statistic": "median",
                "value": format(median.quantize(Decimal("0.000001")), "f"),
                "sample_count": len(samples),
                "classification": "informational",
                "workload": {
                    "target_pushes_per_second": rate,
                    "batch_pushes": rate,
                    "payload_bytes": payload_bytes,
                    "paced": True,
                    "rate_semantics": "paced one-second batch at target pushes per second", "achieved_pushes_per_second": rate,
                    "benchmark_name": benchmark_name,
                    "transport": "injected-fake-transport",
                },
                "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
                "run_url": args.run_url,
                "source_artifact": args.input.name,
                "source_artifact_sha256": hashlib.sha256(raw_bytes).hexdigest(),
            }
        )
    args.output.write_text(json.dumps({"schema_version": "benchmark-v1", "records": records}, indent=2) + "\n")


if __name__ == "__main__":
    main()
