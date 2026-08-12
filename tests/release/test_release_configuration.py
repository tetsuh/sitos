#!/usr/bin/env python3
"""Offline contract tests for release and publication configuration."""

from __future__ import annotations

import json
import re
import subprocess
import tempfile
import tomllib
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NOTICE = ROOT / "NOTICE"
CMAKE = ROOT / "CMakeLists.txt"
PYPROJECT = ROOT / "python" / "pyproject.toml"
WHEELS = ROOT / ".github" / "workflows" / "wheels.yml"
RELEASE_WORKFLOW = ROOT / ".github" / "workflows" / "release-please.yml"
RELEASE_CONFIG = ROOT / ".github" / "release-please-config.json"
RELEASE_MANIFEST = ROOT / ".release-please-manifest.json"
INTEROP_REQUIREMENTS = ROOT / "tests" / "interop" / "requirements.in"

RELEASE_PLEASE_SHA = "45996ed1f6d02564a971a2fa1b5860e934307cf7"
PYPI_PUBLISH_SHA = "dc37677b2e1c63e2034f94d8a5b11f265b73ba33"
DOWNLOAD_ARTIFACT_SHA = "018cc2cf5baa6db3ef3c5f8a56943fffe632ef53"
UPLOAD_ARTIFACT_SHA = "b7c566a772e6b6bfb58ed0dc250532a479d7789f"
CHECKOUT_SHA = "08c6903cd8c0fde910a37f88322edcfb5dd907a8"
RELEASE_TOKEN_SECRET = "RELEASE_PLEASE_TOKEN"
ROOT_COMMIT = "e8230fa407e4b5f82b63d7c0593aff57c0a2e0d1"


def read(path: Path) -> str:
    """Read a required UTF-8 repository file."""
    return path.read_text(encoding="utf-8")


def yaml_code(text: str) -> str:
    """Remove blank and comment-only lines from a workflow for policy checks."""
    return "\n".join(
        line for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")
    )


def yaml_block(text: str, key: str, indent: int) -> str:
    """Return one indentation-delimited YAML mapping block without parsing YAML 1.1."""
    lines = yaml_code(text).splitlines()
    header = " " * indent + key + ":"
    starts = [index for index, line in enumerate(lines) if line == header]
    if len(starts) != 1:
        raise AssertionError(f"expected one {header!r} block, found {len(starts)}")
    start = starts[0]
    end = len(lines)
    for index in range(start + 1, len(lines)):
        line = lines[index]
        leading = len(line) - len(line.lstrip(" "))
        if leading <= indent:
            end = index
            break
    return "\n".join(lines[start:end])


def yaml_action_step(text: str, owner_repo: str, sha: str) -> str:
    """Return the unique sequence item that invokes one pinned action."""
    lines = yaml_code(text).splitlines()
    expected = f"- uses: {owner_repo}@{sha}"
    starts = [index for index, line in enumerate(lines) if line.strip() == expected]
    if len(starts) != 1:
        raise AssertionError(f"expected one {expected!r} step, found {len(starts)}")
    start = starts[0]
    indent = len(lines[start]) - len(lines[start].lstrip(" "))
    end = len(lines)
    for index in range(start + 1, len(lines)):
        line = lines[index]
        leading = len(line) - len(line.lstrip(" "))
        if leading == indent and line.lstrip().startswith("- "):
            end = index
            break
        if leading < indent:
            end = index
            break
    return "\n".join(lines[start:end])


def yaml_named_run(text: str, name: str) -> str:
    """Return the shell body for one uniquely named workflow step."""
    lines = yaml_code(text).splitlines()
    expected = f"- name: {name}"
    starts = [index for index, line in enumerate(lines) if line.strip() == expected]
    if len(starts) != 1:
        raise AssertionError(f"expected one {expected!r} step, found {len(starts)}")
    start = starts[0]
    step_indent = len(lines[start]) - len(lines[start].lstrip(" "))
    run = next(
        index
        for index in range(start + 1, len(lines))
        if lines[index].strip() == "run: |"
    )
    body_indent = len(lines[run]) - len(lines[run].lstrip(" ")) + 2
    body: list[str] = []
    for line in lines[run + 1 :]:
        leading = len(line) - len(line.lstrip(" "))
        if leading <= step_indent:
            break
        body.append(line[body_indent:])
    return "\n".join(body) + "\n"


def assert_full_sha_action(test: unittest.TestCase, text: str, owner_repo: str, sha: str) -> None:
    """Require an action reference to use one reviewed immutable commit."""
    text = yaml_code(text)
    expected = f"uses: {owner_repo}@{sha}"
    test.assertIn(expected, text)
    for reference in re.findall(rf"uses:\s*{re.escape(owner_repo)}@([^\s#]+)", text):
        test.assertRegex(reference, r"^[0-9a-f]{40}$")
        test.assertEqual(reference, sha)


def assert_all_actions_pinned(test: unittest.TestCase, text: str) -> None:
    """Require every executable workflow action reference to use a full commit SHA."""
    references = re.findall(r"(?m)^\s*(?:-\s+)?uses:\s*([^\s#]+)", yaml_code(text))
    test.assertTrue(references, "workflow contains no action references")
    for reference in references:
        test.assertRegex(reference, r"^[^@\s]+@[0-9a-f]{40}$")


class ReleaseConfigurationContractTest(unittest.TestCase):
    def test_notice_inventory(self) -> None:
        self.assertTrue(NOTICE.exists(), "NOTICE is missing")
        notice = read(NOTICE)
        expected = {
            "Eclipse zenoh-c": ("1.9.0", "EPL-2.0 OR Apache-2.0"),
            "RocksDB": ("11.1.2", "GPL-2.0 OR Apache-2.0"),
            "nanobind": ("2.9.2", "BSD-3-Clause"),
            "scikit-build-core": ("0.10.7", "Apache-2.0"),
            "NumPy": ("2.0.0", "BSD-3-Clause"),
            "Eclipse zenoh Python API": ("1.9.0", "EPL-2.0 OR Apache-2.0"),
            "pytest": ("8.3.3", "MIT"),
            "GoogleTest": ("1.14.0", "BSD-3-Clause"),
            "Google Benchmark": ("1.8.3", "Apache-2.0"),
            "github/gitignore": ("6fb8f99e", "CC0-1.0"),
            "gitattributes/gitattributes": ("6fb8f99e", "MIT"),
        }
        for component, anchors in expected.items():
            with self.subTest(component=component):
                self.assertIn(component, notice)
                for anchor in anchors:
                    self.assertIn(anchor, notice)
        self.assertIn("upstream revision was not recorded", notice)

        pyproject = tomllib.loads(read(PYPROJECT))
        direct_requirements = [
            *pyproject["build-system"]["requires"],
            *pyproject["project"]["dependencies"],
        ]
        direct_requirements.extend(
            line.strip()
            for line in read(INTEROP_REQUIREMENTS).splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        )
        for requirement in direct_requirements:
            name = re.match(r"[A-Za-z0-9_.-]+", requirement)
            self.assertIsNotNone(name, requirement)
            self.assertIn(name.group(0).lower(), notice.lower(), requirement)
        license_files = pyproject["tool"]["scikit-build"]["wheel"]["license-files"]
        self.assertIn("../NOTICE", license_files)

    def test_shared_version_and_release_please_config(self) -> None:
        for path in (RELEASE_CONFIG, RELEASE_MANIFEST):
            self.assertTrue(path.exists(), f"{path.relative_to(ROOT)} is missing")

        cmake = read(CMAKE)
        version_lines = [line for line in cmake.splitlines() if "project(sitos VERSION" in line]
        self.assertEqual(len(version_lines), 1)
        version_match = re.search(r"VERSION ([0-9]+\.[0-9]+\.[0-9]+)\b", version_lines[0])
        self.assertIsNotNone(version_match)
        cmake_version = version_match.group(1)
        self.assertIn("x-release-please-version", version_lines[0])
        self.assertEqual(cmake.count("x-release-please-version"), 1)

        pyproject = tomllib.loads(read(PYPROJECT))
        provider = pyproject["tool"]["scikit-build"]["metadata"]["version"]
        self.assertEqual(provider["input"], "../CMakeLists.txt")
        self.assertIn("project\\(sitos VERSION", provider["regex"])

        config = json.loads(read(RELEASE_CONFIG))
        self.assertEqual(config["bootstrap-sha"], ROOT_COMMIT)
        package = config["packages"]["."]
        self.assertEqual(package["release-type"], "simple")
        self.assertEqual(package["package-name"], "sitos")
        self.assertEqual(package["changelog-path"], "CHANGELOG.md")
        self.assertIs(package["include-v-in-tag"], True)
        self.assertIs(package["bump-minor-pre-major"], True)
        self.assertIs(package["bump-patch-for-minor-pre-major"], False)
        self.assertIn(
            {"type": "generic", "path": "CMakeLists.txt"}, package["extra-files"]
        )
        manifest = json.loads(read(RELEASE_MANIFEST))
        self.assertEqual(set(manifest), {"."})
        manifest_version = manifest["."]
        self.assertRegex(manifest_version, r"^[0-9]+\.[0-9]+\.[0-9]+$")
        bootstrap = manifest_version == "0.0.0" and cmake_version == "0.1.0"
        released = manifest_version == cmake_version
        self.assertTrue(bootstrap or released, (manifest_version, cmake_version))

    def test_release_please_workflow_is_pinned_and_owner_gated(self) -> None:
        self.assertTrue(RELEASE_WORKFLOW.exists(), "release-please workflow is missing")
        workflow = read(RELEASE_WORKFLOW)
        code = yaml_code(workflow)
        trigger = yaml_block(workflow, "on", 0)
        permissions = yaml_block(workflow, "permissions", 0)
        job = yaml_block(workflow, "release-please", 2)
        assert_all_actions_pinned(self, workflow)
        assert_full_sha_action(
            self, job, "googleapis/release-please-action", RELEASE_PLEASE_SHA
        )
        self.assertIn("branches: [main]", trigger)
        self.assertNotIn("workflow_dispatch", trigger)
        self.assertIn("contents: write", permissions)
        self.assertIn("pull-requests: write", permissions)
        self.assertIn("issues: write", permissions)
        self.assertNotIn("actions: write", permissions)
        self.assertNotRegex(code, r"(?i)\b(?:gh\s+pr\s+(?:merge|review)|auto-merge|force-push)\b")
        self.assertIn(f"token: ${{{{ secrets.{RELEASE_TOKEN_SECRET} }}}}", job)
        self.assertNotIn("secrets.GITHUB_TOKEN", job)
        self.assertNotIn("uses: ./.github/workflows/wheels.yml", job)
        self.assertNotIn("id-token: write", job)
        self.assertNotIn("gh-action-pypi-publish", job)

    def test_wheel_publication_policy(self) -> None:
        workflow = read(WHEELS)
        code = yaml_code(workflow)
        trigger = yaml_block(workflow, "on", 0)
        linux = yaml_block(workflow, "linux", 2)
        windows = yaml_block(workflow, "windows", 2)
        latest = yaml_block(workflow, "latest-compatible", 2)
        testpypi = yaml_block(workflow, "publish-testpypi", 2)
        pypi = yaml_block(workflow, "publish-pypi", 2)
        assert_all_actions_pinned(self, workflow)
        for event in ("push:", "pull_request:", "workflow_dispatch:", "schedule:"):
            self.assertIn(event, trigger)
        self.assertNotIn("workflow_call:", trigger)
        self.assertRegex(trigger, r'(?m)^\s+tags:\s*\["v\*"\]$')
        self.assertIn("publish_target:", trigger)
        self.assertIn("- none", trigger)
        self.assertIn("- testpypi", trigger)
        self.assertNotIn("- pypi", trigger)
        self.assertNotIn("release_ref:", trigger)
        self.assertNotRegex(code, r"(?m)^permissions:\n  contents: read$")
        for validation_job in (linux, windows, latest):
            permissions_block = yaml_block(validation_job, "permissions", 4)
            self.assertIn("contents: read", permissions_block)
            self.assertNotIn("contents: write", permissions_block)

        linux_upload = yaml_action_step(
            linux, "actions/upload-artifact", UPLOAD_ARTIFACT_SHA
        )
        windows_upload = yaml_action_step(
            windows, "actions/upload-artifact", UPLOAD_ARTIFACT_SHA
        )
        self.assertIn("name: sitos-wheel-linux-cp312", linux_upload)
        self.assertNotIn("name: sitos-wheel-windows-cp312", linux)
        self.assertIn("name: sitos-wheel-windows-cp312", windows_upload)
        self.assertNotIn("name: sitos-wheel-linux-cp312", windows)

        for job, environment in ((testpypi, "testpypi"), (pypi, "pypi")):
            with self.subTest(environment=environment):
                environment_block = yaml_block(job, "environment", 4)
                permissions_block = yaml_block(job, "permissions", 4)
                self.assertRegex(job, r"(?m)^    needs: \[linux, windows\]$")
                self.assertIn(f"name: {environment}", environment_block)
                self.assertEqual(permissions_block.count("id-token: write"), 1)
                self.assertIn("actions: read", permissions_block)
                self.assertIn("contents: read", permissions_block)
                self.assertNotIn("contents: write", permissions_block)
                download_step = yaml_action_step(
                    job, "actions/download-artifact", DOWNLOAD_ARTIFACT_SHA
                )
                self.assertIn("name: sitos-wheel-linux-cp312", download_step)
                self.assertNotIn("sitos-wheel-windows", job)
                if environment == "testpypi":
                    self.assertNotIn("actions/checkout", job)
                else:
                    checkout_step = yaml_action_step(job, "actions/checkout", CHECKOUT_SHA)
                    self.assertIn("persist-credentials: false", checkout_step)
                    assert_full_sha_action(self, job, "actions/checkout", CHECKOUT_SHA)
                self.assertEqual(
                    job.count(f"uses: actions/download-artifact@{DOWNLOAD_ARTIFACT_SHA}"), 1
                )
                self.assertEqual(
                    job.count(f"uses: pypa/gh-action-pypi-publish@{PYPI_PUBLISH_SHA}"), 1
                )
                assert_full_sha_action(
                    self, job, "actions/download-artifact", DOWNLOAD_ARTIFACT_SHA
                )
                assert_full_sha_action(
                    self, job, "pypa/gh-action-pypi-publish", PYPI_PUBLISH_SHA
                )

        self.assertRegex(
            testpypi,
            r"(?m)^    if: >-$\n"
            r"      github\.event_name == 'workflow_dispatch' &&$\n"
            r"      inputs\.publish_target == 'testpypi'$",
        )
        self.assertNotIn("refs/tags/", testpypi)
        self.assertIn("repository-url: https://test.pypi.org/legacy/", testpypi)
        self.assertRegex(
            pypi,
            r"(?m)^    if: >-$\n"
            r"      github\.event_name == 'push' &&$\n"
            r"      startsWith\(github\.ref, 'refs/tags/v'\)$",
        )
        self.assertNotIn("workflow_dispatch", pypi)
        self.assertNotIn("repository-url: https://test.pypi.org/legacy/", pypi)
        self.assertIn("Verify release version provenance", pypi)
        self.assertIn("RELEASE_TAG: ${{ github.ref_name }}", pypi)
        self.assertIn('re.fullmatch(r"v[0-9]+\\.[0-9]+\\.[0-9]+", tag)', pypi)
        self.assertIn('"CMake": cmake_versions[0]', pypi)
        self.assertIn('"release-please manifest": manifest_version', pypi)
        self.assertIn('"wheel metadata": wheel_versions[0]', pypi)
        self.assertEqual(code.count("id-token: write"), 2)
        self.assertEqual(code.count("name: sitos-wheel-linux-cp312"), 3)
        self.assertIn('SITOS_WITH_ROCKSDB = "OFF"', read(PYPROJECT))

    def test_release_version_provenance_verifier(self) -> None:
        pypi = yaml_block(read(WHEELS), "publish-pypi", 2)
        script = yaml_named_run(pypi, "Verify release version provenance")

        cases = (
            ("equal", "v0.1.0", "0.1.0", "0.1.0", "0.1.0", True),
            ("stale manifest", "v0.1.0", "0.1.0", "0.0.0", "0.1.0", False),
            ("CMake mismatch", "v0.1.0", "0.2.0", "0.1.0", "0.1.0", False),
            ("wheel mismatch", "v0.1.0", "0.1.0", "0.1.0", "0.2.0", False),
            ("noncanonical tag", "release-0.1.0", "0.1.0", "0.1.0", "0.1.0", False),
        )
        for name, tag, cmake, manifest, wheel, succeeds in cases:
            with self.subTest(case=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "dist").mkdir()
                (root / "CMakeLists.txt").write_text(
                    f"project(sitos VERSION {cmake} LANGUAGES CXX)\n", encoding="utf-8"
                )
                (root / ".release-please-manifest.json").write_text(
                    json.dumps({".": manifest}), encoding="utf-8"
                )
                with zipfile.ZipFile(root / "dist" / f"sitos-{wheel}.whl", "w") as archive:
                    archive.writestr(
                        f"sitos-{wheel}.dist-info/METADATA",
                        f"Metadata-Version: 2.2\nName: sitos\nVersion: {wheel}\n",
                    )
                result = subprocess.run(
                    ["bash", "-eu", "-o", "pipefail", "-c", script],
                    cwd=root,
                    env={"PATH": "/usr/bin:/bin", "RELEASE_TAG": tag},
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    check=False,
                )
                if succeeds:
                    self.assertEqual(result.returncode, 0, result.stdout)
                else:
                    self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_release_documentation_policy(self) -> None:
        contributing = read(ROOT / "CONTRIBUTING.md")
        build_doc = read(ROOT / "docs" / "06_build_test_packaging.md")
        roadmap = read(ROOT / "docs" / "07_issue_breakdown.md")
        dependency = read(ROOT / "docs" / "09_dependency_policy.md")
        combined = "\n".join((contributing, build_doc, roadmap, dependency))

        for anchor in (
            "manual TestPyPI",
            "Linux CPython 3.12",
            "Windows",
            "RocksDB wheel",
            "crates.io",
            "owner",
        ):
            with self.subTest(anchor=anchor):
                self.assertIn(anchor, combined)
        normalized = re.sub(r"\s+", " ", combined)
        self.assertIn("Nightly TestPyPI publication is prohibited", normalized)
        self.assertNotRegex(
            normalized,
            r"(?i)(?:enable|schedule|require)[^.]{0,80}nightly TestPyPI|"
            r"nightly TestPyPI[^.]{0,80}(?:enabled|scheduled|required)",
        )
        self.assertNotRegex(build_doc, r"prebuilt static/shared")


if __name__ == "__main__":
    unittest.main()
