# Copyright 2026 sitos contributors
# SPDX-License-Identifier: Apache-2.0

import json
import subprocess
import sys
from pathlib import Path


def test_buffer_publisher_report_contains_all_workloads(tmp_path: Path) -> None:
    raw = {
        "benchmarks": [
            {"name": name, "real_time": value}
            for name in (
                "BufferPublisherPush/3/262144",
                "BufferPublisherPush/3/1048576",
                "BufferPublisherPush/300/262144",
                "BufferPublisherPush/300/1048576",
            )
            for value in (1, 2, 3, 4, 5)
        ]
    }
    raw_path = tmp_path / "n107.json"
    output_path = tmp_path / "report.json"
    raw_path.write_text(json.dumps(raw))
    subprocess.run(
        [
            sys.executable,
            "scripts/buffer_publisher_benchmark_report.py",
            "--input",
            str(raw_path),
            "--output",
            str(output_path),
        ],
        check=True,
    )
    report = json.loads(output_path.read_text())
    assert [record["scenario_id"] for record in report["records"]] == [
        "N107_PUSH_3PS_256KIB_V1",
        "N107_PUSH_3PS_1MIB_V1",
        "N107_PUSH_300PS_256KIB_V1",
        "N107_PUSH_300PS_1MIB_V1",
    ]
    assert all(record["metric"] == "per_push_overhead_ns" for record in report["records"])
    assert [record["workload"]["target_pushes_per_second"] for record in report["records"]] == [3, 3, 300, 300]
    assert all(record["workload"]["paced"] is False for record in report["records"])
    assert [record["workload"]["payload_bytes"] for record in report["records"]] == [262144, 1048576, 262144, 1048576]
