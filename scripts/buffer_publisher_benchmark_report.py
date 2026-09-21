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
            if row.get("name") == benchmark_name
        ]
        if len(samples) != 5 or any(not isinstance(value, Decimal) for value in samples):
            raise SystemExit(f"expected five numeric samples for {benchmark_name}")
        # The benchmark iteration performs an unpaced batch, not a wall-clock rate.
        # Report the amortized per-push overhead for a one-second target-load batch.
        per_push = sorted(value / Decimal(rate) for value in samples)
        median = per_push[2]
        records.append(
            {
                "schema_version": "benchmark-v1",
                "scenario_id": scenario,
                "metric": "per_push_overhead_ns",
                "unit": "ns/op",
                "statistic": "median",
                "value": format(median.quantize(Decimal("0.000001")), "f"),
                "sample_count": len(samples),
                "classification": "informational",
                "workload": {
                    "target_pushes_per_second": rate,
                    "batch_pushes": rate,
                    "payload_bytes": payload_bytes,
                    "paced": False,
                    "rate_semantics": "unpaced batch representing one second of target load",
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
