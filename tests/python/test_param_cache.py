"""Fixture-free acceptance tests for the Python ParamCache binding."""

import importlib.util
from pathlib import Path

import pytest

import sitos


def _load_check_wheel():
    script = Path(__file__).resolve().parents[2] / "scripts" / "check_wheel.py"
    spec = importlib.util.spec_from_file_location("sitos_check_wheel", script)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load the wheel validator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize(
    "member",
    [
        "sitos/sitos_python_param_cache_fixture.exe",
        "sitos/sitos_python_param_cache_fixture",
    ],
)
def test_wheel_validator_rejects_param_cache_fixture_member(member: str) -> None:
    with pytest.raises(RuntimeError, match="forbidden wheel entry"):
        _load_check_wheel().validate_wheel_members([member])


def test_public_param_cache_is_exported_without_deferred_apis() -> None:
    assert hasattr(sitos, "ParamCache")
    assert not hasattr(sitos.ParamCache, "attach_base")
    assert not hasattr(sitos.ParamCache, "stale")
    assert not hasattr(sitos.ParamCache, "get_array")


def test_constructor_is_keyword_only_and_validates_timeout() -> None:
    with pytest.raises(TypeError):
        sitos.ParamCache("sitos", None, 5000)
    with pytest.raises(TypeError):
        sitos.ParamCache(query_timeout_ms=True)
    for timeout in (0, -1, 2**63):
        with pytest.raises(ValueError):
            sitos.ParamCache(query_timeout_ms=timeout)


def test_constructor_rejects_empty_json() -> None:
    with pytest.raises(ValueError):
        sitos.ParamCache(zenoh_config_json="")


def test_context_manager_performs_terminal_close() -> None:
    with sitos.ParamCache(query_timeout_ms=5000) as cache:
        assert isinstance(cache, sitos.ParamCache)
    with pytest.raises(ValueError, match="ParamCache is closed"):
        cache.contains("key")
    cache.close()
    cache.detach()


def test_close_precedes_conversion_and_batch_iteration() -> None:
    cache = sitos.ParamCache(query_timeout_ms=5000)
    cache.close()

    class ExplodingEntries:
        def __iter__(self):
            raise AssertionError("closed cache must not iterate entries")

    with pytest.raises(ValueError, match="ParamCache is closed"):
        cache.put("key", object())
    with pytest.raises(ValueError, match="ParamCache is closed"):
        cache.put_batch(ExplodingEntries())
    with pytest.raises(ValueError, match="ParamCache is closed"):
        cache.attach("s1")
    with pytest.raises(ValueError, match="ParamCache is closed"):
        cache.get("key")
    with pytest.raises(ValueError, match="ParamCache is closed"):
        cache.items()


def test_public_exception_types_are_shared_with_param_store() -> None:
    assert issubclass(sitos.NotFoundError, sitos.SitosError)
    assert issubclass(sitos.TypeMismatchError, sitos.SitosError)
    assert sitos.ParamCache.__module__ == "sitos._sitos"
