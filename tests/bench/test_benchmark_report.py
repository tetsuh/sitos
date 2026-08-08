#!/usr/bin/env python3
"""Contract and deterministic semantic tests for Issue #33."""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import shutil
import yaml
import pytest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import benchmark_report as report  # noqa: E402
from benchmark_report import (  # noqa: E402
    REQUIRED_METRICS,
    _partition,
    build_report,
    compare_records,
    mad,
    median,
    p95,
    _raw_expectations,
    _write_safe_text,
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


def test_synthetic_within_reference_is_nonblocking() -> None:
    result = compare_records(result=110, reference=100, threshold_percent=10, direction='lower-is-better')
    assert result['status'] == 'within-reference'
    assert result['blocking'] is False


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
        "zenoh_mode": "ON", "zenoh_version": "1.9.0", "rocksdb_mode": "ON",
        "rocksdb_version": "11.1.2", "power_frequency_controls": "unavailable",
    }
    partition, _ = _partition({"partition_fields": fields})
    return {"partition_fields": fields, "partition": partition}


def _result(path: Path, partition: str | None = None) -> dict[str, object]:
    partition = _environment()["partition"]
    records = []
    n01_environment = _environment(); n01_environment['partition_fields'] = {**n01_environment['partition_fields'], 'zenoh_mode':'OFF', 'zenoh_version':'disabled', 'rocksdb_mode':'OFF', 'rocksdb_version':'disabled'}; n01_environment['partition'], _ = _partition(n01_environment)
    matrix = json.loads((ROOT / "tests/bench/benchmark_policy.json").read_text(encoding="utf-8"))["record_matrix"]
    for entry in matrix:
        scenario, metric = entry["scenario_id"], entry["metric"]
        records.append({
            "schema_version": "benchmark-v1", "scenario_id": scenario, "metric": metric, "unit": entry["unit"], "sample_count": entry["sample_count"],
            "statistic": entry["statistic"], "value": "100", "classification": "informational",
            "requirement_id": entry["requirement_id"], "actual_threshold": None, "tolerance": None,
            "direction": entry["direction"], "target_applicability": entry["target_applicability"], "target_status": "meets-target" if entry["target_applicability"] else "informational",
            "threshold_rationale": entry["threshold_rationale"], "source_commit": "a" * 40,
            "source_kind": "reviewed-pr-provenance", "timestamp": "2026-08-06T00:00:00Z",
            "evidence_url": "https://example.invalid/run", "run_url": "https://example.invalid/run",
            "source_artifact_sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "source_artifact": entry["source_artifact"], "artifact_kind": entry["artifact_kind"], "environment_class": entry["environment_class"],
            "environment_partition": partition, "environment": _environment(),
        })
    for record in records:
        if 'ratio' in record['metric']:
            record['value'] = '1'
        if record['metric'].endswith('_elapsed_ns'):
            record['value'] = '1000000000'
        if record['statistic'] == 'MAD':
            record['value'] = '0'
        if record['environment_class'] == 'N01-OFF-OFF':
            record['environment'] = n01_environment; record['environment_partition'] = n01_environment['partition']
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
    base["records"] = []
    for x in json.loads((ROOT / "tests/bench/benchmark_policy.json").read_text())["record_matrix"]:
        other = _environment(); other["partition_fields"] = {**other["partition_fields"], "cpu_model": "other-cpu"};
        if x['environment_class'] == 'N01-OFF-OFF': other['partition_fields'] = {**other['partition_fields'], 'zenoh_mode':'OFF', 'zenoh_version':'disabled', 'rocksdb_mode':'OFF', 'rocksdb_version':'disabled'}
        other["partition"], _ = _partition(other)
        base["records"].append({**x, "value": "90", "target_status": "meets-target" if x["target_applicability"] else "informational", "source_commit": "a" * 40, "source_kind": "reviewed-pr-provenance", "timestamp": "2026-08-06T00:00:00Z", "evidence_url": "https://example.invalid/run", "run_url": "https://example.invalid/run", "source_artifact_sha256": "a" * 64, "environment_partition": other["partition"], "environment": other})
    base["records"].extend(json.loads((ROOT / 'tests/bench/reference_baseline.json').read_text())['records'])
    reference.write_text(json.dumps(base), encoding="utf-8")
    comparison, _ = build_report_from_objects_for_test(result, reference, raw, policy, require_complete=True)
    assert comparison["records"][0]["comparison"]["status"] == "incomparable"

    for record, current in zip(base["records"], result["records"]):
        record["environment_partition"] = current["environment_partition"]
        record["environment"] = current["environment"]
    reference.write_text(json.dumps(base), encoding="utf-8")
    comparison, _ = build_report_from_objects_for_test(result, reference, raw, policy, require_complete=True)
    assert comparison["records"][0]["comparison"]["status"] == "delta-only"


def test_complete_reference_rejects_same_partition_duplicate(tmp_path: Path) -> None:
    raw = tmp_path / 'raw.json'; raw.write_text('{"samples":[]}', encoding='utf-8')
    result = _result(raw); reference = tmp_path / 'reference.json'
    reference.write_text(json.dumps({'schema_version':'benchmark-v1','state':'complete','records':[dict(record) for record in result['records']] + json.loads((ROOT / 'tests/bench/reference_baseline.json').read_text())['records']}), encoding='utf-8')
    reference_data = json.loads(reference.read_text()); reference_data['records'].append(dict(reference_data['records'][0])); reference.write_text(json.dumps(reference_data), encoding='utf-8')
    with pytest.raises(ValueError): build_report_from_objects_for_test(result, reference, raw, ROOT / 'tests/bench/benchmark_policy.json')
    missing_legacy = {'schema_version':'benchmark-v1','state':'complete','records':[dict(record) for record in result['records']]}
    reference.write_text(json.dumps(missing_legacy), encoding='utf-8')
    with pytest.raises(ValueError): build_report_from_objects_for_test(result, reference, raw, ROOT / 'tests/bench/benchmark_policy.json')
    for field, value in [('source_commit','abc'), ('evidence_url','not-a-url'), ('evidence_url',None), ('run_url','x'), ('target_status','informational')]:
        valid = {'schema_version':'benchmark-v1','state':'complete','records':[dict(record) for record in result['records']] + json.loads((ROOT / 'tests/bench/reference_baseline.json').read_text())['records']}
        target_record = next(record for record in valid['records'] if record['target_applicability'])
        (target_record if field == 'target_status' else valid['records'][0])[field] = value
        reference.write_text(json.dumps(valid), encoding='utf-8')
        with pytest.raises(ValueError): build_report_from_objects_for_test(result, reference, raw, ROOT / 'tests/bench/benchmark_policy.json')


def build_report_from_objects_for_test(result: dict[str, object], reference: Path, raw: Path,
                                       policy: Path, require_complete: bool = False):
    from decimal import Decimal
    parent = raw.parent
    names = ['BM_ParamCacheGetScalar/10000','BM_ParamCacheGetSpan/10000','BM_DirectLookupScalar/10000','BM_DirectLookupSpan/10000']
    n01 = {'benchmarks':[{'name':name+'_median','run_type':'aggregate','aggregate_name':'median','repetitions':5,'iterations':1,'time_unit':'ns','real_time':100} for name in names]}
    n02_names = ['TakeSnapshot/1000','TakeSnapshot/100000']
    n02 = {'benchmarks':[{'name':name+'_median','run_type':'aggregate','aggregate_name':'median','repetitions':5,'iterations':1,'time_unit':'ns','real_time':100} for name in n02_names]}
    process = {'schema_version':'benchmark-v1','scenario_execution':{x:'completed' for x in ('N08','N09','N09_CONTROL_RTT_V1','N09_CALLBACK_THROUGHPUT_S64_1P_V1','N09_CALLBACK_THROUGHPUT_S64_4P_V1')},'n08_samples_ns':[100]*5,'n09_visibility_samples_ns':[[100]*200 for _ in range(5)],'n09_control_rtt_samples_ns':[[100]*200 for _ in range(5)]}
    for scenario in ('N09_CALLBACK_THROUGHPUT_S64_1P_V1','N09_CALLBACK_THROUGHPUT_S64_4P_V1'):
        process[scenario+'_counts']=[100]*5; process[scenario+'_elapsed_ns']=[1000000000]*5
    artifacts = {'n01_google_benchmark': n01, 'n02_google_benchmark': n02, 'process_measurements': process}
    paths = {}
    for name, data in artifacts.items():
        path = parent / (name + '.json'); path.write_text(json.dumps(data), encoding='utf-8'); paths[name] = path
    for record in result['records']:
        if record['source_artifact'] in paths:
            record['source_artifact_sha256'] = hashlib.sha256(paths[record['source_artifact']].read_bytes()).hexdigest()
    result_path = parent / 'result.json'; result_path.write_text(json.dumps(result), encoding='utf-8')
    return build_report(result_path, reference, policy, require_complete=require_complete, raw_artifact_paths=list(paths.values()))


def _validate_workflow_contract(text: str) -> None:
    document = yaml.safe_load(text)
    events = document.get(True, document.get("on"))
    assert set(events) == {"schedule", "workflow_dispatch", "pull_request"}
    assert events["pull_request"]["branches"] == ["main"]
    assert events["pull_request"]["types"] == ["opened", "reopened", "synchronize", "labeled"]
    assert events["schedule"] == [{"cron": "17 3 * * *"}]
    assert document["permissions"] == {"contents": "read"}
    assert set(document["jobs"]) == {"benchmark"}
    job = document["jobs"]["benchmark"]
    assert "permissions" not in job
    assert job["runs-on"] == "ubuntu-24.04" and job["timeout-minutes"] == 180
    expected_steps = ["Verify benchmark source ref", "Restore dependency archives", "Install build tools", "Provision exact vcpkg dependencies", "Run report contract tests", "Configure N01 Release tree", "Build N01 Release tree", "Run N01 Google Benchmark", "Configure N02/N08/N09 Release tree", "Build N02/N08/N09 Release tree", "Run N02/N08/N09 process scenarios", "Build complete measurement record", "Compare and render report", "Require complete reference on final baseline head", "Upload benchmark evidence"]
    assert job["steps"][0].get("uses", "").startswith("actions/checkout@") and job["steps"][1].get("name") == expected_steps[0]
    assert [step.get("name") for step in job["steps"] if "name" in step] == expected_steps
    assert "with" in job["steps"][-1] and job["steps"][-1]["with"].get("retention-days") == 90
    run_text = "\n".join(step.get("run", "") for step in job["steps"])
    assert "-DSITOS_WITH_ZENOH=OFF" in run_text and "-DSITOS_WITH_ZENOH=ON" in run_text
    assert "--target sitos_cache_get_bench" in run_text and "--target sitos_rocksdb_snapshot_bench sitos_process_bench" in run_text
    assert "--benchmark_repetitions=5" in run_text and "--benchmark_min_time=1s" in run_text
    by_name = {step.get('name'): step for step in job['steps'] if step.get('name')}
    assert 'python3-pytest' in by_name['Install build tools']['run']
    assert by_name['Run N01 Google Benchmark']['run'].count('--benchmark_report_aggregates_only=true') == 1
    assert by_name['Run N02/N08/N09 process scenarios']['run'].count('--benchmark_report_aggregates_only=true') == 1
    assert '-DSITOS_WITH_ZENOH=OFF' in by_name['Configure N01 Release tree']['run'] and '-DSITOS_WITH_ROCKSDB=OFF' in by_name['Configure N01 Release tree']['run']
    measurement = by_name['Build complete measurement record']['run']
    assert "Path('/proc/cpuinfo')" in measurement and 'cpu_models' in measurement and 'cpu_model = cpu_models[0]' in measurement
    assert 'platform.processor()' not in measurement and 'platform.machine()' not in measurement and 'platform.architecture()' not in measurement
    assert "if not cpu_models or not cpu_models[0]" in measurement
    assert "n01_fields = dict(fields, zenoh_mode='OFF', zenoh_version='disabled', rocksdb_mode='OFF', rocksdb_version='disabled')" in measurement
    assert "'zenoh_mode': 'ON', 'zenoh_version': '1.9.0'" in measurement
    assert '-DSITOS_WITH_ZENOH=ON' in by_name['Configure N02/N08/N09 Release tree']['run'] and '-DSITOS_WITH_ROCKSDB=ON' in by_name['Configure N02/N08/N09 Release tree']['run']
    assert 'artifacts/n01_google_benchmark.json' in by_name['Run N01 Google Benchmark']['run']
    assert '--artifact-root "${{ github.workspace }}"' in by_name['Require complete reference on final baseline head']['run']
    assert 'artifacts/n02_google_benchmark.json' in by_name['Run N02/N08/N09 process scenarios']['run'] and 'artifacts/process_measurements.json' in by_name['Run N02/N08/N09 process scenarios']['run']
    compare = by_name['Compare and render report']['run']
    assert compare.count('--artifact-root "${{ github.workspace }}"') == 1
    assert compare.count('--raw-artifact') == 3 and all(path in compare for path in ('artifacts/process_measurements.json','artifacts/n01_google_benchmark.json','artifacts/n02_google_benchmark.json'))
    assert job['steps'][-1].get('if') == '${{ always() }}'
    serialized_steps = yaml.safe_dump(job['steps']).replace('persist-credentials', '')
    lowered = serialized_steps.lower()
    assert not any(token in lowered for token in ('secrets.', 'github.token', "github['token']", 'github["token"]', 'github[', 'github_token', 'authorization', 'credentials', 'credential'))
    assert job["if"] == "${{ github.event_name != 'pull_request' || contains(github.event.pull_request.labels.*.name, 'bench') }}"
    uses = [step["uses"] for step in job["steps"] if "uses" in step]
    assert uses == [
        "actions/checkout@fbc6f3992d24b796d5a048ff273f7fcc4a7b6c09",
        "actions/cache/restore@55cc8345863c7cc4c66a329aec7e433d2d1c52a9",
        "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
    ]
    checkout = next(step for step in job["steps"] if step.get("uses", "").startswith("actions/checkout"))
    assert checkout["with"]["persist-credentials"] is False
    assert not any("actions/cache/save" in step.get("uses", "") for step in job["steps"])
    assert all("pull_request_target" not in str(step) for step in job["steps"])


def test_workflow_security_and_truth_table() -> None:
    workflow = (ROOT / ".github/workflows/bench.yml").read_text(encoding="utf-8")
    _validate_workflow_contract(workflow)
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


def test_workflow_mutations_are_rejected() -> None:
    workflow = (ROOT / ".github/workflows/bench.yml").read_text(encoding="utf-8")
    mutations = []
    document = yaml.safe_load(workflow); document[True]["push"] = {}; mutations.append(document)
    document = yaml.safe_load(workflow); document[True]["pull_request"]["branches"] = ["dev"]; mutations.append(document)
    document = yaml.safe_load(workflow); document["permissions"]["contents"] = "write"; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["if"] = "true"; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["runs-on"] = "ubuntu-latest"; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["timeout-minutes"] = 60; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["extra"] = document["jobs"]["benchmark"]; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][-1]["with"]["retention-days"] = 1; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][0]["with"]["persist-credentials"] = True; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][0]["uses"] = "actions/checkout@bad"; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][0], document["jobs"]["benchmark"]["steps"][1] = document["jobs"]["benchmark"]["steps"][1], document["jobs"]["benchmark"]["steps"][0]; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][6]["run"] = "cmake --build build --target wrong"; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][6]["run"] = document["jobs"]["benchmark"]["steps"][6]["run"].replace('SITOS_WITH_ZENOH=OFF','SITOS_WITH_ZENOH=ON'); mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][13]["run"] = document["jobs"]["benchmark"]["steps"][13]["run"].replace('--raw-artifact artifacts/process_measurements.json','--raw-artifact artifacts/wrong.json'); mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][-1]["if"] = "true"; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][3]["env"] = {"TOKEN":"${{ secrets.X }}"}; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][8]["run"] = document["jobs"]["benchmark"]["steps"][8]["run"].replace('--benchmark_report_aggregates_only=true',''); mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][7]["run"] = document["jobs"]["benchmark"]["steps"][7]["run"].replace('sitos_cache_get_bench','wrong_target'); mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][4]["run"] += '\\n echo "${{ github.token }}"'; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][4]["run"] += '\\n echo "${{ github[\'token\'] }}"'; mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"][12]["run"] = document["jobs"]["benchmark"]["steps"][12]["run"].replace("zenoh_version='disabled'", "zenoh_version='1.9.0'"); mutations.append(document)
    document = yaml.safe_load(workflow); document["jobs"]["benchmark"]["steps"].append({"uses":"actions/cache/save@bad"}); mutations.append(document)
    for forbidden in ('platform.processor()', 'platform.machine()', 'platform.architecture()'):
        document = yaml.safe_load(workflow)
        measurement_step = next(step for step in document["jobs"]["benchmark"]["steps"] if step.get("name") == "Build complete measurement record")
        measurement_step["run"] = measurement_step["run"].replace("'cpu_model': cpu_model", f"'cpu_model': {forbidden}")
        mutations.append(document)
    document = yaml.safe_load(workflow)
    measurement_step = next(step for step in document["jobs"]["benchmark"]["steps"] if step.get("name") == "Build complete measurement record")
    measurement_step["run"] = measurement_step["run"].replace("if not cpu_models or not cpu_models[0]", "if False")
    mutations.append(document)
    for mutation in mutations:
        with pytest.raises(AssertionError): _validate_workflow_contract(yaml.safe_dump(mutation))


def test_result_metadata_and_duplicates_are_rejected(tmp_path: Path) -> None:
    raw = tmp_path / 'raw.json'; raw.write_text('{"samples":[]}', encoding='utf-8')
    policy = ROOT / 'tests/bench/benchmark_policy.json'; reference = tmp_path / 'reference.json'
    reference.write_text(json.dumps({'schema_version':'benchmark-v1','state':'initialization-pending','records':[]}), encoding='utf-8')
    for field, value in [('requirement_id','I33-WRONG'),('direction','higher-is-better'),('target_applicability',True),('source_artifact','wrong')]:
        result = _result(raw); result['records'][0][field] = value
        with pytest.raises(ValueError): build_report_from_objects_for_test(result, reference, raw, policy)
    result = _result(raw); result['records'].append(dict(result['records'][0]))
    with pytest.raises(ValueError): build_report_from_objects_for_test(result, reference, raw, policy)


def test_raw_fixtures_reject_fractional_negative_and_truncated_evidence() -> None:
    policy = json.loads((ROOT / 'tests/bench/benchmark_policy.json').read_text())
    def google(names):
        rows=[]
        for index, name in enumerate(names, 1):
            rows.append({'name':name+'_median','run_type':'aggregate','aggregate_name':'median','repetitions':5,'iterations':index,'time_unit':'ns','real_time':Decimal(str(index + 1))})
        return {'benchmarks':rows}
    from decimal import Decimal
    raw = {'n01_google_benchmark': google(['BM_ParamCacheGetScalar/10000','BM_ParamCacheGetSpan/10000','BM_DirectLookupScalar/10000','BM_DirectLookupSpan/10000']), 'n02_google_benchmark': google(['TakeSnapshot/1000','TakeSnapshot/100000']), 'process_measurements': {'schema_version':'benchmark-v1','scenario_execution': {x:'completed' for x in ('N08','N09','N09_CONTROL_RTT_V1','N09_CALLBACK_THROUGHPUT_S64_1P_V1','N09_CALLBACK_THROUGHPUT_S64_4P_V1')}, 'n08_samples_ns':[1]*5, 'n09_visibility_samples_ns':[[1]*200 for _ in range(5)], 'n09_control_rtt_samples_ns':[[1]*200 for _ in range(5)]}}
    for scenario in ('N09_CALLBACK_THROUGHPUT_S64_1P_V1','N09_CALLBACK_THROUGHPUT_S64_4P_V1'):
        raw['process_measurements'][scenario+'_counts']=[1]*5; raw['process_measurements'][scenario+'_elapsed_ns']=[1000000000]*5
    expected = _raw_expectations(raw)
    assert expected[('N01','Scalar_ratio_cache_over_direct','median','ratio')][0] == Decimal('0.5')
    raw['n01_google_benchmark']['benchmarks'].append({'name':'BM_ParamCacheGetScalar/10000','real_time':1})
    with pytest.raises(ValueError): _raw_expectations(raw)
    raw['n01_google_benchmark']['benchmarks'].pop()
    raw['n01_google_benchmark']['benchmarks'].append({'name':'BM_ParamCacheGetScalar/10000/0','run_name':'BM_ParamCacheGetScalar/10000','run_type':'iteration','repetition_index':0,'time_unit':'ns','real_time':1})
    with pytest.raises(ValueError): _raw_expectations(raw)
    raw['n01_google_benchmark']['benchmarks'].pop()
    assert expected[('N01','Span_ratio_cache_over_direct','median','ratio')][0] == Decimal('0.6')
    raw['process_measurements']['n08_samples_ns'][0] = 1.5
    with pytest.raises(ValueError): _raw_expectations(raw)
    raw['process_measurements']['n08_samples_ns'][0] = -1
    with pytest.raises(ValueError): _raw_expectations(raw)
    raw['process_measurements']['n08_samples_ns'][0] = 1
    raw['process_measurements']['N09_CALLBACK_THROUGHPUT_S64_1P_V1_elapsed_ns'][0] = 0
    with pytest.raises(ValueError): _raw_expectations(raw)


def test_policy_and_workload_fences() -> None:
    policy = json.loads((ROOT / "tests/bench/benchmark_policy.json").read_text(encoding="utf-8"))
    assert policy["actual_thresholds"] == "none—not established"
    assert policy["workload_identity"]["N08_lut_bytes"] == 100000000
    assert policy["workload_identity"]["N08_lut_sha256"] == "7975a2b50c79617f9a7d0e02702cb2c0fa533dd083fc999e6316c852fc06f2aa"
    assert policy["baseline"]["workflow_writes_baseline"] is False
    assert policy["actual_threshold"] is None and policy["tolerance"] is None
    matrix = policy["record_matrix"]
    assert len(matrix) >= 50 and len({(x["scenario_id"], x["metric"], x["statistic"], x["unit"]) for x in matrix}) == len(matrix)
    reference = json.loads((ROOT / "tests/bench/reference_baseline.json").read_text(encoding="utf-8"))
    assert all(record["environment_partition"] == "342337392f3685845495bc954eef94a27c18679503392288529ca77c4820a18d" for record in reference["records"])
    assert all(record["timestamp"] == "2026-07-19T14:46:04Z" for record in reference["records"])
    assert all(record["legacy_provenance_reason"] == "unavailable—not retained" for record in reference["records"])
    source = (ROOT / "tests/bench/process_bench.cpp").read_text(encoding="utf-8")
    assert "n08/v1/scalar/" in source and "n09/v1/value/" in source
    assert 'store.Put("session/" + sid' in source
    assert 'N09Key' in source and 'std::setw(6)' in source
    assert 'sitos/bench/n09/v1/' in source
    assert 'ThroughputSequence' in source and 'std::setw(10)' in source
    assert '(trial << 48) | (producer << 40) | sequence' in source
    assert 'store.Put("base"' in source
    assert 'store.Put("",' not in source
    assert 'VERIFY_BASE' in source and 'BASE_READY' in source
    assert 'BASE_STATUS ' in source and 'base readiness resubmission failed' in source
    assert 'ParamValue::Decode(value)' in source and 'AsSpan<std::byte>()' in source
    assert 'std::equal(value.begin(), value.end()' not in source
    assert 'scalar_count == kScalarCount' in source
    assert 'readiness_deadline' in source
    assert 'std::chrono::seconds(60)' in source
    assert 'std::optional<std::size_t> cache_ready_trial;' in source
    assert 'direct.cache_ready_trial.reset();' in source
    assert 'direct.cache_ready_trial.has_value()' in source and '*direct.cache_ready_trial == trial' in source
    assert 'repetition_deadline' in source and 'n09_close' in source
    assert 'query_timeout = std::chrono::seconds(55)' in source
    assert 'ATTACH_ERROR ' in source and 'result.Message()' in source
    assert 'ReadLineUntil' in source and 'sample_deadline' in source
    assert 'CREATE_ENTRY ' in source and 'session call-entry failed: ' in source
    assert 'N08_VERIFY_ACK ' in source and 'N08_COMPLETE ' in source
    assert 'constexpr std::string_view verify_n08_prefix = "VERIFY_N08 ";' in source
    assert 'command.substr(verify_n08_prefix.size())' in source and 'command.substr(12)' not in source
    assert 'struct AckCleanup' in source and 'node.Stop();' in source and '::shutdown(control_fd, SHUT_RDWR)' in source
    assert 'socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC' in source
    assert 'execv(self, argv.data())' in source and 'sh -c' not in source
    assert 'THROUGHPUT_OBSERVER_READY' in source and 'THROUGHPUT_CACHE_READY' in source and 'THROUGHPUT_RELEASE' in source
    assert 'THROUGHPUT_PROGRESS' in source and 'THROUGHPUT_DRAIN_ERROR' in source
    assert 'kThroughputInflightWindow = 128' in source
    assert 'ready_count' in source and 'start_barrier(static_cast<std::ptrdiff_t>(producers + 1))' in source
    assert 'inflight.compare_exchange_weak' in source and 'progress_observed' in source
    assert 'in >> sid >> trial >> producers;' in source and 'std::vector<std::size_t> counts(producers);' in source
    for name, literal in [('verify_n08_prefix','VERIFY_N08 '),('verify_n09_prefix','VERIFY_N09 '),('observer_ready_prefix','THROUGHPUT_OBSERVER_READY '),('throughput_ready_prefix','THROUGHPUT_READY '),('verify_throughput_prefix','VERIFY_THROUGHPUT ')]:
        assert f'constexpr std::string_view {name} = "{literal}";' in source
        assert f'command.substr({name}.size())' in source


def test_trial_zero_cache_ready_requires_unambiguous_sentinel() -> None:
    source = (ROOT / 'tests/bench/process_bench.cpp').read_text(encoding='utf-8')
    def validate(text: str) -> None:
        assert 'std::optional<std::size_t> cache_ready_trial;' in text
        assert 'direct.cache_ready_trial.reset();' in text
        assert 'direct.cache_ready_trial.has_value()' in text
        assert '*direct.cache_ready_trial == trial' in text
    validate(source)
    with pytest.raises(AssertionError):
        validate(source.replace('std::optional<std::size_t> cache_ready_trial;', 'std::size_t cache_ready_trial = 0;'))
    with pytest.raises(AssertionError):
        validate(source.replace('direct.cache_ready_trial.has_value()', 'direct.cache_ready_trial == trial'))


def test_command_prefix_model_preserves_nonzero_fields() -> None:
    source = (ROOT / 'tests/bench/process_bench.cpp').read_text(encoding='utf-8')
    messages = {
        'THROUGHPUT_OBSERVER_READY ': 'THROUGHPUT_OBSERVER_READY 7',
        'THROUGHPUT_READY ': 'THROUGHPUT_READY 7 4 123 31 29 30 33',
        'VERIFY_THROUGHPUT ': 'VERIFY_THROUGHPUT n09-v1-x 7 4 31 29 30 33',
        'VERIFY_N09 ': 'VERIFY_N09 37',
        'VERIFY_N08 ': 'VERIFY_N08 n08-v1-x'
    }
    for prefix, message in messages.items():
        fields = message[len(prefix):].split()
        assert fields
    trial, producers, total, *counts = messages['THROUGHPUT_READY '][len('THROUGHPUT_READY '):].split()
    assert (int(trial), int(producers), int(total), [int(x) for x in counts]) == (7, 4, 123, [31, 29, 30, 33])
    assert messages['VERIFY_N09 '][len('VERIFY_N09 '):] == '37'
    assert messages['VERIFY_N08 '][len('VERIFY_N08 '):] == 'n08-v1-x'
    assert 'THROUGHPUT_PUT_ERROR' in source and 'THROUGHPUT_SAMPLE' in source
    assert 'total / 2' not in source and 'elapsed_ns' in source
    assert 'N09_VERIFY_ACK' not in source
    assert 'cache attach failed: ' in source and 'EOF/timeout' in source
    assert 'const auto elapsed =' in source and 'n08.push_back(elapsed)' in source
    assert 'WriteLine(cache.input, "DETACH")' in source
    assert 'detach_response != "DETACHED"' in source
    assert 'WriteLine(node.input, "CLOSE " + sid)' in source
    assert 'close_response != "CLOSED " + sid' in source
    assert 'n08.push_back(elapsed)' in source
    assert 'n09_control_rtt_samples_ns' in source
    assert 'for (std::size_t sample = 0; sample < 20; ++sample)' in source
    assert 'for (std::size_t sample = 0; sample < 200; ++sample)' in source
    assert "process benchmark failure:" in source
    assert "N08 scalar verification failed index=" in source
    assert "N08 LUT verification failed status=" in source
    assert "std::filesystem::absolute(self)" in source
    assert "0x13579bdf00000000ULL" in source
    assert 'socketpair(AF_UNIX, SOCK_STREAM' in source
    assert 'pipe2(parent_to_child, O_CLOEXEC)' in source and 'F_SETFD, 0' in source
    assert 'SIGKILL' in source and 'WNOHANG' in source and 'graceful_deadline' in source
    assert '--control-fd' in source and 'int control_fd' in source
    assert 'WriteLine(control_fd, "PUT_READY "' in source
    assert 'WriteLine(control_fd, "PING "' in source
    assert 'WriteLine(control_fd, ready.str())' in source
    assert 'WriteLine(cache.input, "VERIFY_N09 "' not in source
    assert 'WriteLine(cache.input, "PING "' not in source
    assert 'WriteLine(cache.input, "VERIFY_THROUGHPUT' not in source
    assert 'THROUGHPUT_READY' in source and 'THROUGHPUT_DRAINED' in source
    assert 'observed_for_trial != expected_total' in source and 'release_ticks' in source
    assert 'std::chrono::seconds(2)' in source and 'std::chrono::seconds(60)' in source
    assert 'ThroughputKey(trial, producer, sequence)' in source
    assert 'ThroughputValue(trial, producer, sequence)' in source
    assert 'N09_CALLBACK_THROUGHPUT_S64_1P_V1_counts' in source
    assert 'N09_CALLBACK_THROUGHPUT_S64_1P_V1_elapsed_ns' in source
    assert 'N09_CALLBACK_THROUGHPUT_S64_4P_V1_counts' in source
    assert 'N09_CALLBACK_THROUGHPUT_S64_4P_V1_elapsed_ns' in source
    assert 'throughput_1p[0]' not in source and 'throughput_4p[0]' not in source
    workflow = (ROOT / ".github/workflows/bench.yml").read_text(encoding="utf-8")
    assert "missing required callback throughput evidence" in workflow
    assert workflow.count("--raw-artifact") >= 6
    assert "parse_float=Decimal" in workflow and "see-build-log" not in workflow
    assert "value,0)" not in workflow and "value':0" not in workflow
    assert 'github.event.pull_request.head.sha' in workflow
    assert 'environment.json' in workflow
    rocks = (ROOT / "tests/bench/rocksdb_snapshot_bench.cpp").read_text(encoding="utf-8")
    assert "RocksDBEngine::Put failed" in rocks
    assert "TakeSnapshot returned null" in rocks


def test_synthetic_cli_within_reference_and_regression_exit(tmp_path: Path) -> None:
    raw = tmp_path / "raw.json"; raw.write_text('{"samples":[]}', encoding="utf-8")
    env = _environment(); result = _result(raw); reference = tmp_path / "reference.json"
    reference.write_text(json.dumps({"schema_version":"benchmark-v1","state":"complete","records":[{**record} for record in result["records"]] + json.loads((ROOT / 'tests/bench/reference_baseline.json').read_text())['records']}), encoding="utf-8")
    policy = tmp_path / "policy.json"; policy_data = json.loads((ROOT / "tests/bench/benchmark_policy.json").read_text()); policy_data["synthetic_regression_threshold_percent"] = "10"; policy.write_text(json.dumps(policy_data), encoding="utf-8")
    build_report_from_objects_for_test(result, reference, raw, ROOT / 'tests/bench/benchmark_policy.json')
    n01_path = tmp_path / 'n01_google_benchmark.json'
    n01_data = json.loads(n01_path.read_text()); n01_data['benchmarks'][0]['real_time'] = 110; n01_path.write_text(json.dumps(n01_data), encoding='utf-8')
    n01_digest = hashlib.sha256(n01_path.read_bytes()).hexdigest()
    result["records"][0]["value"] = "110"
    result["records"][4]["value"] = "1.10"
    for record in result['records']:
        if record['source_artifact'] == 'n01_google_benchmark': record['source_artifact_sha256'] = n01_digest
    result_path = tmp_path / "result.json"
    result_path.write_text(json.dumps(result), encoding='utf-8')
    output_json, output_md = tmp_path / "out.json", tmp_path / "out.md"
    raw_args = sum((["--raw-artifact", str(tmp_path / (name + '.json'))] for name in ('n01_google_benchmark','n02_google_benchmark','process_measurements')), [])
    ok = subprocess.run([sys.executable, str(ROOT / "scripts/benchmark_report.py"), "--artifact-root", str(tmp_path), "--result", str(result_path), *raw_args, "--reference", str(reference), "--policy", str(policy), "--output-json", str(output_json), "--output-markdown", str(output_md)], capture_output=True, text=True)
    assert ok.returncode == 0
    n01_data = json.loads(n01_path.read_text()); n01_data['benchmarks'][0]['real_time'] = 111; n01_path.write_text(json.dumps(n01_data), encoding='utf-8')
    n01_digest = hashlib.sha256(n01_path.read_bytes()).hexdigest()
    result['records'][0]['value'] = '111'; result['records'][4]['value'] = '1.11'
    for record in result['records']:
        if record['source_artifact'] == 'n01_google_benchmark': record['source_artifact_sha256'] = n01_digest
    result_path.write_text(json.dumps(result), encoding="utf-8")
    bad = subprocess.run([sys.executable, str(ROOT / "scripts/benchmark_report.py"), "--artifact-root", str(tmp_path), "--result", str(result_path), *raw_args, "--reference", str(reference), "--policy", str(policy), "--output-json", str(output_json), "--output-markdown", str(output_md)], capture_output=True, text=True)
    assert bad.returncode != 0 and "synthetic regression" in bad.stderr


def test_safe_output_rejects_root_swap_race(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    root = tmp_path / 'root'; root.mkdir()
    outside = tmp_path / 'outside-root'; outside.mkdir()
    sentinel = outside / 'sentinel.txt'; sentinel.write_text('sentinel', encoding='utf-8')
    original_open = report.os.open
    swapped = {'value': False}
    backup = tmp_path / 'root-backup'
    def racing_open(path, flags, *args, **kwargs):
        if path == os.fspath(root) and kwargs.get('dir_fd') is None and not swapped['value']:
            root.rename(backup); os.symlink(outside, root); swapped['value'] = True
        return original_open(path, flags, *args, **kwargs)
    monkeypatch.setattr(report.os, 'open', racing_open)
    with pytest.raises(OSError):
        _write_safe_text(root, Path('out.txt'), 'unsafe')
    if root.is_symlink(): root.unlink()
    if backup.is_dir(): backup.rename(root)
    assert sentinel.read_text(encoding='utf-8') == 'sentinel'


def test_safe_output_rejects_intermediate_symlink_and_race(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    nested = tmp_path / 'nested'; nested.mkdir()
    outside = tmp_path.parent / 'outside-directory'; outside.mkdir(exist_ok=True)
    sentinel = outside / 'sentinel.txt'; sentinel.write_text('sentinel', encoding='utf-8')
    link = nested / 'link'
    try:
        os.symlink(outside, link)
    except (OSError, NotImplementedError):
        pytest.skip('symlink unsupported')
    with pytest.raises(ValueError):
        _write_safe_text(tmp_path, Path('nested/link/out.txt'), 'unsafe')
    link.unlink()
    original_open = report.os.open
    swapped = {'value': False}
    backup = tmp_path / 'nested-backup'
    def racing_open(path, flags, *args, **kwargs):
        if path == 'nested' and kwargs.get('dir_fd') is not None and not swapped['value']:
            nested.rename(backup); os.symlink(outside, nested); swapped['value'] = True
        return original_open(path, flags, *args, **kwargs)
    monkeypatch.setattr(report.os, 'open', racing_open)
    with pytest.raises(OSError):
        _write_safe_text(tmp_path, Path('nested/out.txt'), 'unsafe')
    if nested.is_symlink(): nested.unlink()
    if backup.is_dir(): backup.rename(nested)
    assert sentinel.read_text(encoding='utf-8') == 'sentinel'


def test_safe_output_rejects_symlink_and_hardlink_and_missing_parent(tmp_path: Path) -> None:
    link = tmp_path / 'link'
    outside = tmp_path.parent / 'outside-output.txt'
    outside.write_text('sentinel', encoding='utf-8')
    try:
        os.symlink(outside, link)
    except (OSError, NotImplementedError):
        pytest.skip('symlink unsupported')
    with pytest.raises(ValueError):
        _write_safe_text(tmp_path, link, 'unsafe')
    hardlink_target = tmp_path / 'hardlink-output.txt'
    os.link(outside, hardlink_target)
    with pytest.raises((FileExistsError, OSError, ValueError)):
        _write_safe_text(tmp_path, hardlink_target, 'unsafe')
    assert outside.read_text(encoding='utf-8') == 'sentinel'
    with pytest.raises(ValueError):
        _write_safe_text(tmp_path, Path('missing/out.txt'), 'unsafe')


def test_report_cli_rejects_final_incomplete_reference(tmp_path: Path) -> None:
    raw = tmp_path / "raw.json"
    raw.write_text('{"samples":[]}', encoding="utf-8")
    env = _environment()
    result = _result(raw, env["partition"])
    reference = tmp_path / "reference.json"
    reference.write_text(json.dumps({"schema_version": "benchmark-v1", "state": "initialization-pending", "records": []}), encoding="utf-8")
    build_report_from_objects_for_test(result, reference, raw, ROOT / 'tests/bench/benchmark_policy.json')
    result_path = tmp_path / "result.json"
    out_json, out_md = tmp_path / "out.json", tmp_path / "out.md"
    raw_args = sum((["--raw-artifact", str(tmp_path / (name + '.json'))] for name in ('n01_google_benchmark','n02_google_benchmark','process_measurements')), [])
    policy = tmp_path / 'policy.json'; shutil.copy(ROOT / 'tests/bench/benchmark_policy.json', policy)
    proc = subprocess.run([sys.executable, str(ROOT / "scripts/benchmark_report.py"), "--require-complete-reference", "--artifact-root", str(tmp_path), "--result", str(result_path), *raw_args, "--reference", str(reference), "--policy", str(policy), "--output-json", str(out_json), "--output-markdown", str(out_md)], capture_output=True, text=True)
    assert proc.returncode != 0
    assert "complete baseline" in proc.stderr
    outside = tmp_path.parent / 'outside-result.json'
    escaped = subprocess.run([sys.executable, str(ROOT / 'scripts/benchmark_report.py'), '--artifact-root', str(tmp_path), '--result', str(outside), '--reference', str(reference), '--policy', str(policy), '--output-json', str(out_json), '--output-markdown', str(out_md)], capture_output=True, text=True)
    assert escaped.returncode == 2 and 'escapes artifact root' in escaped.stderr


def main() -> None:
    for test in (
        test_expected_artifacts_exist, test_decimal_statistics_are_exact,
        test_synthetic_regression_is_flagged, test_workflow_security_and_truth_table,
        test_trial_zero_cache_ready_requires_unambiguous_sentinel,
        test_policy_and_workload_fences,
    ):
        test()
    print("benchmark contract tests passed")


if __name__ == "__main__":
    main()
