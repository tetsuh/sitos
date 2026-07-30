"""Packaging boundary checks for the non-installed raw Zenoh fixture."""

import importlib.util
from pathlib import Path

import pytest


def _load_wheel_validator():
    script = Path(__file__).resolve().parents[2] / "scripts" / "check_wheel.py"
    spec = importlib.util.spec_from_file_location("sitos_check_wheel", script)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load the wheel validator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize(
    "member",
    ["sitos/sitos_raw_zenoh_fixture", "sitos/sitos_raw_zenoh_fixture.exe"],
)
def test_wheel_validator_rejects_raw_zenoh_fixture(member: str) -> None:
    with pytest.raises(RuntimeError, match="forbidden wheel entry"):
        _load_wheel_validator().validate_wheel_members([member])
