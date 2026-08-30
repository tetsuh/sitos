#!/usr/bin/env python3
"""Verify the exact Issue #158 Fence CTest registration contract."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess
import sys


ZENOH_OFF_TESTS = (
    "FenceLaneCodecTest.GoldenAndNegativeForms",
    "FenceAckResultTest.GoldenFailureMatrixRows",
    "FenceAdapterTest.ClassifiesAttachmentsByRouteWithoutDroppingParameters",
    "FencePublisherTest.LinearizesDataAndMarkerAndBoundsAdmission",
    "FenceReceiverTest.EvaluatesPrefixesFailuresBoundsAndCapacityPoison",
    "FenceParamCacheTest.CompletesOnlyTheMatchingAttachGeneration",
    "FenceStorageNodeTest.DispatchesFenceAndBindsTheSessionGeneration",
    "FenceCollisionTest.PinsDocumentedUuidAndTokenResidualBoundaries",
    "FenceLifecycleTest.QuiescesCallbacksAndPreventsPostReturnAccess",
    "FenceParamCacheTest.PublicWaitCoversPriorWritesAndExcludesLaterWrites",
    "FenceParamCacheTest.PublicWaitMapsValidationTimeoutAndReceiverFailure",
    "FenceParamCacheTest.PublicWaitRejectsSecondPendingWaitWithoutCorruptingFirst",
    "FenceLifecycleTest.PublicWaitDetachCancelsAndQuiesces",
)
ZENOH_ON_TESTS = ZENOH_OFF_TESTS + (
    "FenceZenohIntegrationTest.QualifiesTopologiesQosAndControlIsolation",
    "FenceRawZenohInteropTest.QualifiesPayloadTransparencyAndControlIsolation",
    "FenceZenohIntegrationTest.QualifiesPublicParamCacheLocalDelivery",
)
PROFILE_TESTS = {
    "zenoh-off": ZENOH_OFF_TESTS,
    "sanitizer": ZENOH_OFF_TESTS,
    "zenoh-on": ZENOH_ON_TESTS,
}


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--profile", required=True, choices=tuple(PROFILE_TESTS))
    return parser.parse_args()


def _fail(message: str) -> None:
    print(f"Fence CTest registration error: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    arguments = _arguments()
    completed = subprocess.run(
        (
            "ctest",
            "--test-dir",
            str(arguments.build),
            "--show-only=json-v1",
        ),
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        _fail(f"ctest failed with exit {completed.returncode}: {detail}")

    try:
        document = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        _fail(f"ctest returned invalid JSON: {error}")
    if not isinstance(document, dict) or not isinstance(document.get("tests"), list):
        _fail("ctest JSON does not contain a tests array")

    names: list[str] = []
    fence_entries: dict[str, dict[str, object]] = {}
    for entry in document["tests"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
            _fail("ctest JSON contains a test without a string name")
        if entry["name"].startswith("Fence"):
            names.append(entry["name"])
            fence_entries[entry["name"]] = entry

    expected = set(PROFILE_TESTS[arguments.profile])
    counts = Counter(names)
    missing = sorted(expected - set(counts))
    duplicates = sorted(name for name, count in counts.items() if count != 1)
    unexpected = sorted(set(counts) - expected)
    failures: list[str] = []
    if missing:
        failures.append("missing: " + ", ".join(missing))
    if duplicates:
        failures.append("duplicate: " + ", ".join(duplicates))
    if unexpected:
        failures.append("unexpected: " + ", ".join(unexpected))

    serial_tests = ZENOH_ON_TESTS[len(ZENOH_OFF_TESTS) :]
    not_serial: list[str] = []
    for name in serial_tests:
        entry = fence_entries.get(name)
        if entry is None:
            continue
        properties = entry.get("properties")
        run_serial = False
        if isinstance(properties, list):
            run_serial = any(
                isinstance(prop, dict)
                and prop.get("name") == "RUN_SERIAL"
                and prop.get("value") is True
                for prop in properties
            )
        if not run_serial:
            not_serial.append(name)
    if not_serial:
        failures.append("not RUN_SERIAL: " + ", ".join(not_serial))
    if failures:
        _fail("; ".join(failures))

    print(
        f"Fence CTest registration OK: profile={arguments.profile} "
        f"tests={len(expected)}"
    )


if __name__ == "__main__":
    main()
