"""Acceptance tests for the Python ParamCache binding."""

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


def test_detach_is_idempotent_and_cache_can_reattach() -> None:
    cache = sitos.ParamCache(query_timeout_ms=5000)
    try:
        cache.detach()
        cache.detach()
        cache.attach("unknown_but_valid")
        assert cache.contains("missing") is False
        cache.detach()
        cache.attach("unknown_but_valid")
    finally:
        cache.close()


def test_malformed_sid_and_already_attached_are_rejected() -> None:
    cache = sitos.ParamCache(query_timeout_ms=5000)
    try:
        with pytest.raises(ValueError):
            cache.attach("bad/session")
        cache.attach("s1")
        with pytest.raises(ValueError):
            cache.attach("s2")
    finally:
        cache.close()


def test_detached_operations_preserve_invalid_argument_mapping() -> None:
    cache = sitos.ParamCache(query_timeout_ms=5000)
    try:
        with pytest.raises(ValueError):
            cache.get("key")
        with pytest.raises(ValueError):
            cache.contains("key")
        with pytest.raises(ValueError):
            cache.items()
        with pytest.raises(ValueError):
            cache.put("key", 1)
        with pytest.raises(ValueError):
            cache.put_batch([])
    finally:
        cache.close()


def test_public_exception_types_are_shared_with_param_store() -> None:
    assert issubclass(sitos.NotFoundError, sitos.SitosError)
    assert issubclass(sitos.TypeMismatchError, sitos.SitosError)
    assert sitos.ParamCache.__module__ == "sitos._sitos"


def test_get_type_is_keyword_only_and_exact_builtin() -> None:
    cache = sitos.ParamCache(query_timeout_ms=5000)
    try:
        cache.attach("s1")
        cache.put("typed", 3.5)
        assert cache.get("typed", type=int) == 3
        with pytest.raises(TypeError):
            cache.get("typed", None, int)

        class SpoofType:
            def __eq__(self, other: object) -> bool:
                return other in (bool, int, float, str, bytes)

        with pytest.raises(TypeError, match="type must be None"):
            cache.get("typed", type=SpoofType())
    finally:
        cache.close()


def test_get_default_only_substitutes_not_found() -> None:
    cache = sitos.ParamCache(query_timeout_ms=5000)
    try:
        cache.attach("s1")
        assert cache.get("missing", default=None) is None
        assert cache.get("missing", default=object()) is not None
        with pytest.raises(ValueError):
            cache.get("bad key", default="must not mask validation")
    finally:
        cache.close()


def test_local_values_batch_order_raw_prefix_and_owned_items() -> None:
    cache = sitos.ParamCache(query_timeout_ms=5000)
    try:
        cache.attach("s1")
        values = {
            "bool": True,
            "s64": -7,
            "dp": 3.5,
            "str": "text",
            "bytes": b"bytes",
        }
        cache.put_batch(values)
        cache.put_batch([("dup", 1), ("dup", 2)])
        cache.put("foo/bar", 7)
        cache.put("foobar", "outside")
        assert cache.get("dup") == 2
        assert cache.contains("foo/bar")
        assert list(cache.items("foo")) == [("foo/bar", 7), ("foobar", "outside")]
        rows = cache.items("foo/")
        cache.detach()
        assert list(rows) == [("foo/bar", 7)]
    finally:
        cache.close()


def test_batch_validation_and_supported_value_boundary() -> None:
    cache = sitos.ParamCache(query_timeout_ms=5000)
    try:
        cache.attach("s1")
        with pytest.raises(ValueError, match="two items"):
            cache.put_batch([("a",)])
        with pytest.raises(TypeError, match="two-item pairs"):
            cache.put_batch(["not-a-pair"])
        with pytest.raises(OverflowError):
            cache.put("overflow", 2**63)
        with pytest.raises(TypeError):
            cache.put("unsupported", bytearray(b"bytes"))
        cache.put_batch([])
    finally:
        cache.close()
