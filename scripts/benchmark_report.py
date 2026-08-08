#!/usr/bin/env python3
"""Validate and compare the deterministic Issue #33 benchmark artifacts."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import re
import os
import stat
from collections import Counter
from decimal import Decimal, ROUND_HALF_EVEN, InvalidOperation
from pathlib import Path
from typing import Any, Iterable

SCHEMA_VERSION = "benchmark-v1"
UNAVAILABLE = 'unavailable—not retained'
NS_PER_OP = 'ns/op'
UPDATES_PER_SEC = 'updates/s'
MATRIX_MISMATCH = 'result record matrix mismatch (including multiplicity)'
CACHE_SCALAR = 'BM_ParamCacheGetScalar/10000'
CACHE_SPAN = 'BM_ParamCacheGetSpan/10000'
DIRECT_SCALAR = 'BM_DirectLookupScalar/10000'
DIRECT_SPAN = 'BM_DirectLookupSpan/10000'
BENCHMARK_N01 = (CACHE_SCALAR, CACHE_SPAN, DIRECT_SCALAR, DIRECT_SPAN)
BENCHMARK_N02 = ('TakeSnapshot/1000','TakeSnapshot/100000')
LEGACY_PARTITION_FIELDS = {
    'cpu_model': UNAVAILABLE, 'runner_name': 'owner-local-wsl2', 'runner_class': 'owner-local',
    'image': 'Ubuntu 24.04', 'os': 'Linux', 'kernel': UNAVAILABLE, 'compiler': 'gcc',
    'compiler_version': '13.3', 'cmake': UNAVAILABLE, 'ninja': UNAVAILABLE,
    'benchmark_version': '1.8.3', 'build_type': 'Release', 'build_flags': 'CPU-0-pinned',
    'zenoh_mode': 'OFF', 'zenoh_version': 'disabled', 'rocksdb_mode': 'OFF',
    'rocksdb_version': 'disabled', 'power_frequency_controls': 'CPU-0-pinned',
}
LEGACY_PARTITION = '342337392f3685845495bc954eef94a27c18679503392288529ca77c4820a18d'
LEGACY_VALUES = {CACHE_SCALAR: '135', DIRECT_SCALAR: '41.9', CACHE_SPAN: '122', DIRECT_SPAN: '43.0'}
REQUIRED_METRICS = (
    f"N01/{CACHE_SCALAR}",
    f"N01/{CACHE_SPAN}",
    f"N01/{DIRECT_SCALAR}",
    f"N01/{DIRECT_SPAN}",
    "N01/Scalar_ratio_cache_over_direct",
    "N01/Span_ratio_cache_over_direct",
    "N02/TakeSnapshot/1000",
    "N02/TakeSnapshot/100000",
    "N02/snapshot_100000_over_1000_ratio",
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
        return json.loads(path.read_text(encoding="utf-8"), parse_float=Decimal, parse_int=int)
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read JSON {path}: {exc}") from exc


def _safe_path(root: Path, candidate: Path, *, input_file: bool) -> Path:
    root = root.resolve(strict=True)
    candidate = candidate if candidate.is_absolute() else root / candidate
    if candidate.is_symlink():
        raise ValueError(f"symlink path is not allowed: {candidate}")
    resolved = candidate.resolve(strict=False)
    try:
        resolved.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"path escapes artifact root: {candidate}") from exc
    if input_file and (not resolved.is_file() or resolved.is_symlink()):
        raise ValueError(f"input is not a regular file: {candidate}")
    return resolved


def _validate_policy_matrix(matrix: Any) -> None:
    if not isinstance(matrix, list) or not matrix:
        raise ValueError('record_matrix must be non-empty')
    identities = {(x.get('scenario_id'), x.get('metric'), x.get('statistic'), x.get('unit')) for x in matrix}
    if len(identities) != len(matrix):
        raise ValueError('record_matrix must contain unique records')
    fields = {'scenario_id','metric','statistic','unit','sample_count','direction','requirement_id','target_applicability','source_artifact','artifact_kind','environment_class','classification','threshold_rationale','actual_threshold','tolerance','schema_version'}
    for entry in matrix:
        if set(entry) != fields or entry['schema_version'] != SCHEMA_VERSION:
            raise ValueError('record_matrix entry schema mismatch')
        if entry['actual_threshold'] is not None or entry['tolerance'] is not None or entry['classification'] != 'informational':
            raise ValueError('record_matrix threshold/classification mismatch')


def validate_policy(policy: dict[str, Any]) -> None:
    if policy.get('schema_version') != SCHEMA_VERSION or policy.get('runner_policy') != 'github-hosted-ubuntu-informational':
        raise ValueError('unsupported policy identity')
    if policy.get('actual_thresholds') != 'none—not established':
        raise ValueError('actual thresholds must remain none—not established')
    required = ('statistics','scenarios','workflow','numeric_rules','workload_identity','provenance','baseline')
    missing = [key for key in required if key not in policy]
    if missing: raise ValueError(f'policy missing {missing[0]}')
    if policy.get('actual_threshold') is not None or policy.get('tolerance') is not None:
        raise ValueError('policy timing thresholds must be null')
    if policy.get('baseline', {}).get('workflow_writes_baseline') is not False:
        raise ValueError('workflow must not write baseline')
    _validate_policy_matrix(policy.get('record_matrix'))
    if set(policy.get('provenance', {}).get('source_kinds', [])) != {'owner-local','reviewed-pr-provenance','scheduled','manual','pull-request'}:
        raise ValueError('policy source kinds mismatch')


def _record_key(record: dict[str, Any]) -> str:
    statistic = record.get('statistic', 'median')
    default_unit = NS_PER_OP if record.get('scenario_id') in {'N01', 'N02'} else 'ns'
    unit = record.get('unit', default_unit)
    return f"{record.get('scenario_id')}/{record.get('metric')}/{statistic}/{unit}"


def _required_key(record: dict[str, Any]) -> str:
    return f"{record.get('scenario_id')}/{record.get('metric')}"


def _matrix_identity(record: dict[str, Any]) -> tuple[Any, ...]:
    return tuple(record.get(field) for field in (
        'scenario_id','metric','statistic','unit','sample_count','direction','requirement_id',
        'target_applicability','source_artifact','artifact_kind','environment_class',
        'classification','threshold_rationale','actual_threshold','tolerance','schema_version'))


def _positive_integer(value: Any) -> bool:
    return not isinstance(value, bool) and isinstance(value, (int, Decimal)) and value == int(value) and int(value) > 0


def _validate_provenance_identity(record: dict[str, Any]) -> None:
    if record.get('source_kind') not in {'owner-local','reviewed-pr-provenance','scheduled','manual','pull-request'}: raise ValueError('invalid source_kind')
    if not re.fullmatch(r'[0-9a-f]{40}', str(record.get('source_commit',''))): raise ValueError('source_commit must be 40 lowercase hex characters')
    if not re.fullmatch(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?Z', str(record.get('timestamp',''))): raise ValueError('timestamp must be UTC RFC3339')


def _validate_provenance_urls(record: dict[str, Any], require_urls: bool) -> None:
    for field in ('evidence_url','run_url'):
        value = record.get(field)
        if value is None and require_urls: raise ValueError(f'{field} is required')
        if value is not None and not re.fullmatch(r'https?://[^\s]+', str(value)): raise ValueError(f'{field} must be http(s) URL')


def _validate_provenance_value(record: dict[str, Any]) -> None:
    digest = record.get('source_artifact_sha256')
    if digest is not None and not re.fullmatch(r'[0-9a-f]{64}', str(digest)): raise ValueError('source artifact digest must be 64 lowercase hex characters')
    if not _decimal(record.get('value')).is_finite(): raise ValueError('record value must be finite')


def _validate_provenance_environment(record: dict[str, Any]) -> None:
    fields = record.get('environment', {}).get('partition_fields', {})
    mode = record.get('environment_class')
    if mode == 'N01-OFF-OFF' and (fields.get('zenoh_mode') != 'OFF' or fields.get('rocksdb_mode') != 'OFF' or fields.get('zenoh_version') != 'disabled' or fields.get('rocksdb_version') != 'disabled'):
        raise ValueError('N01 environment class/version mismatch')
    if mode == 'LIVE-ON-ON' and (fields.get('zenoh_mode') != 'ON' or fields.get('rocksdb_mode') != 'ON' or not re.fullmatch(r'\d+\.\d+(?:\.\d+)?', str(fields.get('zenoh_version'))) or not re.fullmatch(r'\d+\.\d+(?:\.\d+)?', str(fields.get('rocksdb_version')))):
        raise ValueError('live environment class/version mismatch')


def _validate_provenance(record: dict[str, Any], *, require_urls: bool = False) -> None:
    _validate_provenance_identity(record)
    _validate_provenance_urls(record, require_urls)
    _validate_provenance_value(record)
    _validate_provenance_environment(record)

def _raw_stat(values: list[Decimal], statistic: str) -> Decimal:
    if statistic == 'median': return median(values)
    if statistic == 'p95': return p95(values)
    if statistic == 'min': return min(values)
    if statistic == 'max': return max(values)
    if statistic == 'MAD': return mad(values)
    raise ValueError(f'unsupported raw statistic: {statistic}')


def _add_raw(expected: dict, scenario: str, metric: str, stat: str, unit: str, values: list[Decimal]) -> None:
    if not values or any(isinstance(v, bool) or not isinstance(v, (int, Decimal)) or Decimal(v) <= 0 for v in values):
        raise ValueError(f'invalid raw values: {scenario}/{metric}')
    expected[(scenario, metric, stat, unit)] = (_raw_stat(values, stat), len(values))


def _google_artifact_expectations(artifact: str, names: tuple[str, ...], data: dict[str, Any]) -> dict[tuple[str, str, str, str], tuple[Decimal, int]]:
    if not isinstance(data.get('benchmarks'), list): raise ValueError(f'invalid Google Benchmark artifact: {artifact}')
    scenario = 'N01' if artifact.startswith('n01') else 'N02'
    expected = {}
    for name in names:
        _validate_google_rows(data['benchmarks'], name)
        row = next(r for r in data['benchmarks'] if r.get('name') == name + '_median')
        value = row.get('real_time', row.get('cpu_time'))
        if not _positive_integer(row.get('iterations')) or isinstance(value, bool) or not isinstance(value, (int, Decimal)) or Decimal(value) <= 0: raise ValueError(f'invalid Google aggregate value: {name}')
        expected[(scenario, name, 'median', NS_PER_OP)] = (Decimal(value), 5)
    return expected


def _google_ratio_expectations(expected: dict, scenario: str) -> None:
    a, b = ((CACHE_SCALAR, DIRECT_SCALAR) if scenario == 'N01' else BENCHMARK_N02)
    numerator, denominator = (a, b) if scenario == 'N01' else (b, a)
    metric = 'Scalar_ratio_cache_over_direct' if scenario == 'N01' else 'snapshot_100000_over_1000_ratio'
    expected[(scenario, metric, 'median', 'ratio')] = (expected[(scenario,numerator,'median',NS_PER_OP)][0] / expected[(scenario,denominator,'median',NS_PER_OP)][0], 5)
    if scenario == 'N01': expected[(scenario, 'Span_ratio_cache_over_direct', 'median', 'ratio')] = (expected[(scenario,CACHE_SPAN,'median',NS_PER_OP)][0] / expected[(scenario,DIRECT_SPAN,'median',NS_PER_OP)][0], 5)


def _google_expectations(raws: dict[str, Any]) -> dict[tuple[str, str, str, str], tuple[Decimal, int]]:
    expected = {}
    for artifact, names in (('n01_google_benchmark', BENCHMARK_N01), ('n02_google_benchmark', BENCHMARK_N02)):
        data = raws.get(artifact)
        if not isinstance(data, dict): raise ValueError(f'invalid Google Benchmark artifact: {artifact}')
        expected.update(_google_artifact_expectations(artifact, names, data))
        _google_ratio_expectations(expected, 'N01' if artifact.startswith('n01') else 'N02')
    return expected

def _reject_google_iterations(rows: list[dict[str, Any]], name: str) -> None:
    def identifies(row: dict[str, Any]) -> bool:
        return any(isinstance(row.get(field), str) and (row[field] == name or row[field].startswith(name + '/') or row[field].startswith(name + '_')) for field in ('name','run_name'))
    if any(identifies(row) and (row.get('run_type') == 'iteration' or row.get('repetition_index') is not None or row.get('name') == name) for row in rows): raise ValueError(f'Google aggregate-only artifact contains iteration rows: {name}')


def _google_aggregate_row(rows: list[dict[str, Any]], name: str) -> dict[str, Any]:
    matches = [row for row in rows if row.get('name') == name + '_median']
    if len(matches) != 1: raise ValueError(f'Google aggregate topology mismatch: {name}')
    row = matches[0]
    if row.get('run_type') != 'aggregate' or row.get('aggregate_name') != 'median' or row.get('repetitions') != 5 or row.get('time_unit') != 'ns': raise ValueError(f'Google aggregate metadata mismatch: {name}')
    return row


def _validate_google_rows(rows: list[dict[str, Any]], name: str) -> None:
    _reject_google_iterations(rows, name)
    _google_aggregate_row(rows, name)

def _process_header(raw: dict[str, Any]) -> None:
    if not isinstance(raw, dict) or raw.get('schema_version') != SCHEMA_VERSION: raise ValueError('invalid process artifact schema')
    required = {'N08','N09','N09_CONTROL_RTT_V1','N09_CALLBACK_THROUGHPUT_S64_1P_V1','N09_CALLBACK_THROUGHPUT_S64_4P_V1'}
    execution = raw.get('scenario_execution')
    if set(execution or {}) != required or any(execution[x] != 'completed' for x in required): raise ValueError('process scenario_execution mismatch')


def _process_latency_expectations(expected: dict, scenario: str, groups: list[list[Any]]) -> None:
    flat = [Decimal(v) for group in groups for v in group]
    for stat in ('median','p95','min','max','MAD'): _add_raw(expected, scenario, 'aggregate_median_ns', stat, 'ns', flat)
    for index, group in enumerate(groups, 1):
        for stat in ('median','p95','min','max','MAD'): _add_raw(expected, scenario, f'repetition_{index}_ns', stat, 'ns', [Decimal(v) for v in group])


def _process_expectations(raw: dict[str, Any]) -> dict[tuple[str, str, str, str], tuple[Decimal, int]]:
    _process_header(raw)
    expected = {}
    n08, vis, ctl = raw['n08_samples_ns'], raw['n09_visibility_samples_ns'], raw['n09_control_rtt_samples_ns']
    _validate_samples(n08, 5, 'N08 raw shape mismatch'); _validate_groups(vis, 5, 'N09 visibility raw shape mismatch'); _validate_groups(ctl, 5, 'N09 control raw shape mismatch')
    n08_values = [Decimal(v) for v in n08]
    for stat in ('median','min','max','MAD'): _add_raw(expected, 'N08_SESSION_START_V1', 'session_median_ns', stat, 'ns', n08_values)
    _process_latency_expectations(expected, 'N09_VISIBILITY_S64_V1', vis)
    _process_latency_expectations(expected, 'N09_CONTROL_RTT_V1', ctl)
    return expected

def _validate_samples(values: Any, length: int, message: str) -> None:
    if not isinstance(values, list) or len(values) != length or any(not _positive_integer(value) for value in values): raise ValueError(message)


def _validate_groups(groups: Any, length: int, message: str) -> None:
    if not isinstance(groups, list) or len(groups) != length or any(not isinstance(group, list) or len(group) != 200 or any(not _positive_integer(value) for value in group) for group in groups): raise ValueError(message)


def _throughput_expectations(raw: dict[str, Any]) -> dict[tuple[str, str, str, str], tuple[Decimal, int]]:
    expected: dict[tuple[str, str, str, str], tuple[Decimal, int]] = {}
    rates = {}
    for scenario in ('N09_CALLBACK_THROUGHPUT_S64_1P_V1','N09_CALLBACK_THROUGHPUT_S64_4P_V1'):
        counts, elapsed = raw.get(scenario + '_counts'), raw.get(scenario + '_elapsed_ns')
        _validate_samples(counts, 5, f'throughput raw shape mismatch: {scenario}'); _validate_samples(elapsed, 5, f'throughput raw shape mismatch: {scenario}')
        values = [Decimal(c) * Decimal(1000000000) / Decimal(e) for c, e in zip(counts, elapsed)]; rates[scenario] = values
        for index, (count, duration, rate) in enumerate(zip(counts, elapsed, values), 1):
            expected[(scenario,f'trial_{index}_count','value','count')] = (Decimal(count), 1); expected[(scenario,f'trial_{index}_elapsed_ns','value','ns')] = (Decimal(duration), 1); expected[(scenario,f'trial_{index}_updates_per_second','value',UPDATES_PER_SEC)] = (rate, 1)
        _add_raw(expected, scenario, 'median_updates_per_second', 'median', UPDATES_PER_SEC, values); expected[(scenario,'median_updates_per_second','MAD',UPDATES_PER_SEC)] = (mad(values), len(values))
    expected[('N09_CALLBACK_THROUGHPUT_S64_4P_V1','four_to_one_ratio','median','ratio')] = (median(rates['N09_CALLBACK_THROUGHPUT_S64_4P_V1']) / median(rates['N09_CALLBACK_THROUGHPUT_S64_1P_V1']), 5)
    return expected


def _raw_expectations(raws: dict[str, Any]) -> dict[tuple[str, str, str, str], tuple[Decimal, int]]:
    expected = _google_expectations(raws)
    expected.update(_process_expectations(raws.get('process_measurements')))
    expected.update(_throughput_expectations(raws.get('process_measurements')))
    return expected


def _validate_result_schema(record: dict[str, Any], required: set[str], key: str) -> None:
    if not required.issubset(record) or record['schema_version'] != SCHEMA_VERSION: raise ValueError(f'record schema mismatch: {key}')
    _validate_provenance(record, require_urls=True)


def _validate_result_artifact(record: dict[str, Any], raw_digests: dict[str, str], raw_expected: dict | None, key: str) -> None:
    digest = record['source_artifact_sha256']
    if record['source_kind'] == 'owner-local' or not digest or digest not in raw_digests: raise ValueError(f'record provenance/artifact mismatch: {key}')
    if raw_expected is not None:
        if raw_digests[digest] != record['source_artifact']: raise ValueError(f'artifact ownership mismatch: {key}')
        expected_value, expected_count = raw_expected[(record['scenario_id'],record['metric'],record['statistic'],record['unit'])]
        places = 3 if record['unit'] == UPDATES_PER_SEC else 6
        if _decimal(record['value']) != Decimal(_quantize(expected_value, places)) or record['sample_count'] != expected_count: raise ValueError(f'raw-derived value mismatch: {key}')


def _validate_result_metadata(record: dict[str, Any], matrix_by_key: dict[tuple, dict], key: str) -> None:
    matrix = matrix_by_key.get((record['scenario_id'],record['metric'],record['statistic'],record['unit']))
    if matrix is None: raise ValueError(f'policy metadata mismatch: {key}')
    fields = ('scenario_id','metric','statistic','unit','sample_count','direction','requirement_id','target_applicability','source_artifact','artifact_kind','environment_class','classification','threshold_rationale','actual_threshold','tolerance','schema_version')
    if any(record.get(field) != matrix[field] for field in fields): raise ValueError(f'policy metadata mismatch: {key}')


def _validate_result_semantics(record: dict[str, Any], key: str) -> None:
    target = 'informational'
    if record['target_applicability']:
        limit = Decimal('1000000000') if record['scenario_id'] == 'N08_SESSION_START_V1' else Decimal('10000000')
        target = 'meets-target' if _decimal(record['value']) < limit else 'exceeds-target'
    valid = (record['target_status'] == target and isinstance(record['sample_count'], int) and record['sample_count'] > 0 and record['classification'] in {'informational','confirmation-required','blocking'} and record['statistic'] in {'median','p95','min','max','MAD','value'} and record['direction'] in {'lower-is-better','higher-is-better'} and record['actual_threshold'] is None and record['tolerance'] is None)
    environment = record.get('environment')
    if not valid or not isinstance(environment, dict) or _partition(environment)[0] != record['environment_partition'] or not _decimal(record['value']).is_finite(): raise ValueError(f'record semantics mismatch: {key}')


def _validate_result_record(record: dict[str, Any], required: set[str], raw_digests: dict[str, str], matrix_by_key: dict[tuple, dict], raw_expected: dict | None) -> None:
    key = _record_key(record)
    _validate_result_schema(record, required, key)
    _validate_result_artifact(record, raw_digests, raw_expected, key)
    _validate_result_metadata(record, matrix_by_key, key)
    _validate_result_semantics(record, key)

def _matrix_counts(values: list[tuple[Any, ...]]) -> Counter:
    return Counter(json.dumps(value, ensure_ascii=False, default=str) for value in values)


def _validate_matrix_counts(actual: list[tuple[Any, ...]], expected: list[tuple[Any, ...]]) -> None:
    actual_counts = _matrix_counts(actual)
    expected_counts = _matrix_counts(expected)
    for identity, count in expected_counts.items():
        if actual_counts.get(identity, 0) != count: raise ValueError(MATRIX_MISMATCH)
    for identity in actual_counts:
        if identity not in expected_counts: raise ValueError(MATRIX_MISMATCH)


def _validate_result_matrix(samples: list[dict[str, Any]], policy: dict[str, Any]) -> None:
    expected = [_matrix_identity(x) for x in policy['record_matrix']]
    actual = [_matrix_identity(x) for x in samples]
    if len(actual) != len(set(actual)): raise ValueError(MATRIX_MISMATCH)
    _validate_matrix_counts(actual, expected)


def _load_result_raws(raw_paths: list[Path]) -> tuple[dict[str, Any], dict[str, str]]:
    raw_by_name = {path.stem: _load_json(path) for path in raw_paths}
    raw_digests = {hashlib.sha256(path.read_bytes()).hexdigest(): path.stem for path in raw_paths}
    return raw_by_name, raw_digests


def _validate_result_raws(raw_by_name: dict[str, Any], policy: dict[str, Any]) -> dict:
    designated = {'n01_google_benchmark','n02_google_benchmark','process_measurements'}
    if set(raw_by_name) != designated: raise ValueError('exactly the designated N01/N02/process raw artifacts are required')
    raw_expected = _raw_expectations(raw_by_name)
    expected = {(x['scenario_id'],x['metric'],x['statistic'],x['unit']) for x in policy['record_matrix']}
    if set(raw_expected) != expected: raise ValueError('raw-derived matrix mismatch')
    return raw_expected


def _validate_result_completeness(samples: list[dict[str, Any]], policy: dict[str, Any] | None) -> None:
    missing = set(REQUIRED_METRICS) - {_required_key(r) for r in samples}
    if missing: raise ValueError(f'missing required metrics: {sorted(missing)}')
    if policy is not None:
        actual = {(x['scenario_id'],x['metric'],x['statistic'],x['unit']) for x in samples}
        expected = {(x['scenario_id'],x['metric'],x['statistic'],x['unit']) for x in policy['record_matrix']}
        if actual != expected: raise ValueError(MATRIX_MISMATCH)


def validate_result(result: dict[str, Any], *, raw_paths: list[Path], policy: dict[str, Any] | None = None) -> tuple[dict[str, Any], str]:
    if result.get('schema_version') != SCHEMA_VERSION: raise ValueError('unsupported result schema_version')
    environment = result.get('environment')
    if not isinstance(environment, dict): raise ValueError('result environment is required')
    partition, _ = _partition(environment)
    if result.get('environment_partition') != partition: raise ValueError('result environment partition mismatch')
    samples = result.get('records')
    if not isinstance(samples, list) or not samples: raise ValueError('result records must be non-empty')
    required = {'schema_version','scenario_id','metric','unit','sample_count','statistic','value','classification','requirement_id','threshold_rationale','actual_threshold','tolerance','direction','target_applicability','target_status','source_commit','source_kind','timestamp','evidence_url','run_url','source_artifact_sha256','source_artifact','artifact_kind','environment_class','environment_partition'}
    raw_by_name, raw_digests = _load_result_raws(raw_paths)
    raw_expected = None
    if policy is not None:
        if len(raw_paths) != 3: raise ValueError('exactly the designated N01/N02/process raw artifacts are required')
        _validate_result_matrix(samples, policy)
        raw_expected = _validate_result_raws(raw_by_name, policy)
    matrix_by_key = {(x['scenario_id'],x['metric'],x['statistic'],x['unit']): x for x in (policy or {}).get('record_matrix', [])}
    for record in samples: _validate_result_record(record, required, raw_digests, matrix_by_key, raw_expected)
    _validate_result_completeness(samples, policy)
    return environment, partition

def _reference_owner_checks(record: dict[str, Any], key: str) -> None:
    exact = (record.get('legacy_provenance_reason') == UNAVAILABLE and record.get('timestamp') == '2026-07-19T14:46:04Z' and record.get('source_commit') == '9aca8250191f360b377b966ae09e743d4bd11437' and record.get('evidence_url') == 'https://github.com/tetsuh/sitos/pull/102#issuecomment-5016146492' and record.get('run_url') is None and record.get('source_artifact_sha256') is None and record['scenario_id'] == 'N01')
    if not exact: raise ValueError(f'owner-local baseline exception mismatch: {key}')


def _reference_source_identity(record: dict[str, Any], state: str, legacy: bool, key: str) -> None:
    if record['source_kind'] not in {'owner-local','reviewed-pr-provenance','scheduled','manual','pull-request'}: raise ValueError(f'baseline source kind invalid: {key}')
    if not re.fullmatch(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?Z', str(record['timestamp'])): raise ValueError(f'baseline timestamp format invalid: {key}')
    if legacy: _reference_owner_checks(record, key)
    elif state == 'complete' and record['source_kind'] != 'reviewed-pr-provenance': raise ValueError(f'complete baseline matrix must use reviewed-pr-provenance: {key}')


def _reference_artifact_checks(record: dict[str, Any], legacy: bool, key: str) -> None:
    if not legacy and (not record.get('run_url') or not record.get('source_artifact_sha256')): raise ValueError(f'baseline provenance incomplete: {key}')
    if not legacy and not re.fullmatch(r'[0-9a-f]{64}', str(record['source_artifact_sha256'])): raise ValueError(f'baseline artifact digest format invalid: {key}')
    if not legacy: _validate_provenance(record, require_urls=True)


def _reference_source_checks(record: dict[str, Any], state: str, legacy: bool, key: str) -> None:
    _reference_source_identity(record, state, legacy, key)
    _reference_artifact_checks(record, legacy, key)

def _reference_target_check(record: dict[str, Any], key: str) -> None:
    expected = 'informational'
    if record['target_applicability']:
        limit = Decimal('1000000000') if record['scenario_id'] == 'N08_SESSION_START_V1' else Decimal('10000000')
        expected = 'meets-target' if _decimal(record['value']) < limit else 'exceeds-target'
    if record['target_status'] != expected: raise ValueError(f'baseline target status mismatch: {key}')


def _reference_policy_checks(record: dict[str, Any], legacy: bool, matrix_by_key: dict[tuple, dict], state: str, key: str) -> None:
    matrix = matrix_by_key.get((record['scenario_id'],record['metric'],record['statistic'],record['unit']))
    if matrix is None:
        if state == 'complete': raise ValueError(f'baseline record is outside policy matrix: {key}')
        return
    fields = ('scenario_id','metric','unit','sample_count','statistic','classification','requirement_id','threshold_rationale','actual_threshold','tolerance','direction','target_applicability','schema_version')
    if legacy: fields = tuple(field for field in fields if field not in {'threshold_rationale','target_applicability'})
    if any(record.get(field) != matrix[field] for field in fields): raise ValueError(f'baseline policy metadata mismatch: {key}')
    if not legacy: _reference_target_check(record, key)

def _validate_reference_header(reference: dict[str, Any]) -> tuple[list[dict[str, Any]], str]:
    if reference.get('schema_version') != SCHEMA_VERSION: raise ValueError('unsupported baseline schema_version')
    records = reference.get('records')
    if not isinstance(records, list): raise ValueError('baseline records must be a list')
    state = reference.get('state')
    if state not in {'initialization-pending','complete'}: raise ValueError('baseline state must be initialization-pending or complete')
    return records, state


def _validate_reference_record(record: dict[str, Any], state: str, matrix_by_key: dict[tuple, dict], index: dict[str, list[dict[str, Any]]], common: set[str]) -> None:
    key = _record_key(record)
    legacy = record.get('source_kind') == 'owner-local'
    required = common - {'target_applicability'} if legacy else common
    if not required.issubset(record) or record['schema_version'] != SCHEMA_VERSION: raise ValueError(f'baseline record schema mismatch: {key}')
    _reference_source_checks(record, state, legacy, key)
    _reference_policy_checks(record, legacy, matrix_by_key, state, key)
    if not isinstance(record['sample_count'], int) or record['sample_count'] <= 0 or not _decimal(record['value']).is_finite(): raise ValueError(f'baseline sample/value mismatch: {key}')
    if record['environment_partition'] != _partition(record['environment'])[0]: raise ValueError(f'baseline partition mismatch: {key}')
    if any(item['environment_partition'] == record['environment_partition'] for item in index.get(key, [])): raise ValueError(f'duplicate baseline record within partition: {key}')
    index.setdefault(key, []).append(record)


def _validate_legacy_record(record: dict[str, Any]) -> None:
    if str(record['value']) != LEGACY_VALUES[record['metric']]: raise ValueError('owner-local baseline inherited value mismatch')
    if record['sample_count'] != 5 or record['direction'] != 'lower-is-better' or record['target_status'] != 'informational': raise ValueError('owner-local baseline inherited metadata mismatch')
    if record['environment_partition'] != LEGACY_PARTITION or record['environment']['partition_fields'] != LEGACY_PARTITION_FIELDS: raise ValueError('owner-local baseline inherited environment mismatch')


def _validate_legacy_records(records: list[dict[str, Any]], state: str) -> None:
    if state != 'complete': return
    owners = [r for r in records if r.get('source_kind') == 'owner-local']
    keys = {('N01', metric, 'median', NS_PER_OP) for metric in LEGACY_VALUES}
    if len(owners) != 4 or {(r['scenario_id'],r['metric'],r['statistic'],r['unit']) for r in owners} != keys: raise ValueError('owner-local baseline must contain exactly four inherited #19 records')
    for record in owners: _validate_legacy_record(record)

def _validate_reviewed_partitions(records: list[dict[str, Any]], state: str, matrix: list[dict[str, Any]]) -> None:
    if state != 'complete': return
    reviewed = [r for r in records if r.get('source_kind') == 'reviewed-pr-provenance']
    partitions = {r['environment_partition'] for r in reviewed}
    if not partitions: raise ValueError('complete baseline requires a reviewed full matrix per partition')
    for part in partitions:
        part_records = [r for r in reviewed if r['environment_partition'] == part]
        classes = {r['environment_class'] for r in part_records}
        expected = {(x['scenario_id'],x['metric'],x['statistic'],x['unit']) for x in matrix if x['environment_class'] in classes}
        actual = {(r['scenario_id'],r['metric'],r['statistic'],r['unit']) for r in part_records}
        if actual != expected: raise ValueError('complete baseline requires a reviewed full matrix per partition')


def _reference_index(reference: dict[str, Any], policy: dict[str, Any] | None = None) -> dict[str, list[dict[str, Any]]]:
    records, state = _validate_reference_header(reference)
    matrix = policy.get('record_matrix', []) if policy else []
    matrix_by_key = {(x['scenario_id'],x['metric'],x['statistic'],x['unit']): x for x in matrix}
    common = {'schema_version','scenario_id','metric','unit','sample_count','statistic','value','classification','requirement_id','threshold_rationale','actual_threshold','tolerance','direction','target_applicability','target_status','source_commit','source_kind','timestamp','evidence_url','environment_partition','environment'}
    index: dict[str, list[dict[str, Any]]] = {}
    for record in records: _validate_reference_record(record, state, matrix_by_key, index, common)
    _validate_legacy_records(records, state)
    if policy: _validate_reviewed_partitions(records, state, matrix)
    return index

def _compare_one_record(record: dict[str, Any], index: dict[str, list[dict[str, Any]]], state: str, require_complete: bool) -> dict[str, Any]:
    key = _record_key(record)
    candidates = index.get(key, [])
    if not candidates:
        if require_complete or state == 'complete': raise ValueError(f'missing required reference record: {key}')
        return {'status':'no-reference','absolute_delta':None,'percentage_delta':None}
    compatible = [item for item in candidates if item['environment_partition'] == record['environment_partition']]
    if not compatible: return {'status':'incomparable','absolute_delta':None,'percentage_delta':None}
    reference_value = _decimal(compatible[0]['value']); delta = _decimal(record['value']) - reference_value
    percentage = None if reference_value == 0 else delta / abs(reference_value) * Decimal(100)
    return {'status':'delta-only','absolute_delta':_quantize(delta, 6),'percentage_delta':None if percentage is None else _quantize(percentage, 6),'zero_reference_reason':'zero-reference' if reference_value == 0 else None}


def compare_result_records(result: dict[str, Any], reference: dict[str, Any], *, require_complete: bool, policy: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    index = _reference_index(reference, policy)
    state = reference.get('state')
    return [{**record, 'comparison': _compare_one_record(record, index, state, require_complete)} for record in result['records']]

def _markdown(rows: list[dict[str, Any]], *, state: str, partition: str) -> str:
    lines = ["# Benchmark report", "", f"- Baseline state: `{state}`", f"- Environment partition: `{partition}`", "", "| Metric | Value | Target status | Reference status | Delta |", "|---|---:|---|---|---:|"]
    for row in rows:
        comp = row["comparison"]
        lines.append(f"| `{_record_key(row)}` | `{row['value']}` | `{row.get('target_status')}` | `{comp['status']}` | `{comp['absolute_delta']}` |")
    lines.extend(["", "Historical hosted timing thresholds are `none—not established`; timing statuses are informational.", ""])
    return "\n".join(lines)


def build_report(result_path: Path, reference_path: Path, policy_path: Path, *, require_complete: bool, raw_artifact_paths: list[Path] | None = None) -> tuple[dict[str, Any], str]:
    policy = _load_json(policy_path)
    reference = _load_json(reference_path)
    result = _load_json(result_path)
    validate_policy(policy)
    validate_result(result, raw_paths=raw_artifact_paths or [result_path], policy=policy)
    if require_complete and reference.get("state") != "complete":
        raise ValueError("final report requires complete baseline")
    rows = compare_result_records(result, reference, require_complete=require_complete, policy=policy)
    synthetic_threshold = policy.get("synthetic_regression_threshold_percent")
    if synthetic_threshold is not None:
        for row in rows:
            candidates = _reference_index(reference, policy).get(_record_key(row), [])
            if candidates:
                verdict = compare_records(result=row["value"], reference=candidates[0]["value"], threshold_percent=synthetic_threshold, direction=row["direction"])
                row["comparison"]["synthetic_status"] = verdict["status"]
                if verdict["blocking"]: raise ValueError(f"synthetic regression is blocking: {_record_key(row)}")
    comparison = {"schema_version": SCHEMA_VERSION, "records": rows, "environment_partition": result["environment_partition"], "baseline_state": reference["state"]}
    return comparison, _markdown(rows, state=reference["state"], partition=result["environment_partition"])


def _absolute_relative_parts(root: Path, candidate: Path, tokens: list[str]) -> tuple[str, ...]:
    try:
        relative = candidate.relative_to(root)
    except ValueError as exc:
        raise ValueError(f'path escapes artifact root: {candidate}') from exc
    lexical = tuple(token for token in tokens if token)
    root_tokens = tuple(token for token in os.fspath(root).replace('\\', '/').split('/') if token)
    if lexical[:len(root_tokens)] != root_tokens: raise ValueError(f'path escapes artifact root: {candidate}')
    parts = lexical[len(root_tokens):]
    if tuple(relative.parts) != parts: raise ValueError(f'invalid relative output path: {candidate}')
    return parts


def _lexical_relative_parts(root: Path, candidate: Path) -> tuple[str, ...]:
    raw = os.fspath(candidate)
    tokens = raw.replace('\\', '/').split('/')
    parts = _absolute_relative_parts(root, candidate, tokens) if candidate.is_absolute() else tuple(tokens)
    if not parts or any(part in {'', '.', '..'} for part in parts): raise ValueError(f'invalid relative output path: {candidate}')
    if not candidate.is_absolute() and tuple(Path(*parts).parts) != parts: raise ValueError(f'invalid relative output path: {candidate}')
    return parts


def _required_open_flag(name: str) -> int:
    flag = getattr(os, name, 0)
    if not flag: raise ValueError(f'{name} is unavailable; refusing unsafe output open')
    return flag


def _open_output_parent(root: Path, parts: tuple[str, ...]) -> tuple[list[int], int]:
    directory_flag = _required_open_flag('O_DIRECTORY')
    nofollow_flag = _required_open_flag('O_NOFOLLOW')
    root_fd = os.open(os.fspath(root), os.O_RDONLY | directory_flag | nofollow_flag)
    descriptors = [root_fd]
    try:
        current_fd = root_fd
        for part in parts[:-1]:
            current_fd = os.open(part, os.O_RDONLY | directory_flag | nofollow_flag, dir_fd=current_fd)
            descriptors.append(current_fd)
        return descriptors, current_fd
    except BaseException:
        for descriptor in reversed(descriptors):
            try: os.close(descriptor)
            except OSError: pass
        raise


def _write_output_fd(parent_fd: int, basename: str, content: str) -> None:
    nofollow_flag = _required_open_flag('O_NOFOLLOW')
    file_fd = os.open(basename, os.O_WRONLY | os.O_CREAT | os.O_EXCL | nofollow_flag, 0o644, dir_fd=parent_fd)
    try:
        metadata = os.fstat(file_fd)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1 or (hasattr(os, 'getuid') and metadata.st_uid != os.getuid()): raise ValueError('output is not a regular single-owner file')
        stream = os.fdopen(file_fd, 'w', encoding='utf-8')
        file_fd = None
        try:
            stream.write(content); stream.flush(); os.fsync(stream.fileno())
        finally:
            stream.close()
    finally:
        if file_fd is not None:
            try: os.close(file_fd)
            except OSError: pass

def _write_safe_text(root: Path, candidate: Path, content: str) -> None:
    target = _safe_path(root, candidate, input_file=False)
    if not target.parent.is_dir(): raise ValueError('output parent must be an existing directory')
    parts = _lexical_relative_parts(root, candidate)
    descriptors, parent_fd = _open_output_parent(root, parts)
    try:
        _write_output_fd(parent_fd, parts[-1], content)
    finally:
        for descriptor in reversed(descriptors):
            try: os.close(descriptor)
            except OSError: pass

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-markdown", type=Path, required=True)
    parser.add_argument("--raw-artifact", type=Path, action="append", default=[])
    parser.add_argument("--require-complete-reference", action="store_true")
    args = parser.parse_args(argv)
    try:
        root = args.artifact_root.resolve(strict=True)
        result = _safe_path(root, args.result, input_file=True)
        reference = _safe_path(root, args.reference, input_file=True)
        policy = _safe_path(root, args.policy, input_file=True)
        raw = [_safe_path(root, path, input_file=True) for path in args.raw_artifact]
        comparison, markdown = build_report(
            result, reference, policy,
            require_complete=args.require_complete_reference,
            raw_artifact_paths=raw,
        )
        _write_safe_text(root, args.output_json, json.dumps(comparison, ensure_ascii=False, indent=2) + "\n")
        _write_safe_text(root, args.output_markdown, markdown)
    except (OSError, ValueError, KeyError) as exc:
        print(f"benchmark report error: {exc}", file=sys.stderr)
        return 2
    print(markdown, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
