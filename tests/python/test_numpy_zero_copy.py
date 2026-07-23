"""Fixture-free NumPy API and conversion contract tests."""

import gc
import importlib.util
from pathlib import Path

import numpy as np
import pytest

import sitos


def _load_check_wheel():
    script = Path(__file__).resolve().parents[2] / "scripts" / "check_wheel.py"
    spec = importlib.util.spec_from_file_location("sitos_check_wheel_numpy", script)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_wheel_metadata_requires_numpy_2_without_mypy_runtime_dependency() -> None:
    validator = _load_check_wheel().validate_python_runtime_metadata
    validator("Requires-Dist: numpy>=2.0\n")

    with pytest.raises(RuntimeError, match="NumPy 2"):
        validator("Requires-Dist: numpy>=1.24\n")
    with pytest.raises(RuntimeError, match="mypy"):
        validator("Requires-Dist: numpy>=2.0\nRequires-Dist: mypy>=1.0\n")


def test_get_array_is_exported_only_on_param_cache() -> None:
    assert hasattr(sitos.ParamCache, "get_array")
    assert not hasattr(sitos.ParamStore, "get_array")


def test_public_typing_artifact_is_installed() -> None:
    assert (Path(sitos.__file__).parent / "py.typed").is_file()


def test_encode_value_accepts_exact_supported_ndarrays() -> None:
    for value in (
        np.array([1, 2, 3], dtype=np.int16),
        np.array(7, dtype=np.int64),
        np.array([], dtype=np.float32),
    ):
        encoded = sitos.encode_value(value)
        assert isinstance(encoded, bytes)
        assert sitos.decode_value(encoded) == value.tobytes()


def test_ndarray_input_rejects_subclasses_and_unsafe_layouts() -> None:
    class ArraySubclass(np.ndarray):
        pass

    subclass = np.arange(3, dtype=np.int16).view(ArraySubclass)
    with pytest.raises(TypeError):
        sitos.encode_value(subclass)

    with pytest.raises(ValueError):
        sitos.encode_value(np.arange(6, dtype=np.int16).reshape(2, 3)[:, ::2])
    with pytest.raises(TypeError):
        sitos.encode_value(np.array(["object"], dtype=object))
    with pytest.raises(TypeError):
        sitos.encode_value(bytearray(b"bytes"))


def test_get_array_requires_keyword_dtype() -> None:
    cache = sitos.ParamCache(query_timeout_ms=5000)
    cache.close()
    with pytest.raises(ValueError, match="ParamCache is closed"):
        cache.get_array(object(), dtype=object())
    with pytest.raises(TypeError):
        cache.get_array("key", np.int16)


def test_get_array_dtype_and_default_validation_contract() -> None:
    cache = sitos.ParamCache(query_timeout_ms=5000)
    cache.close()
    for dtype in (np.dtype("O"), np.dtype("V4"), np.dtype("datetime64[D]")):
        with pytest.raises(ValueError, match="ParamCache is closed"):
            cache.get_array("key", dtype=dtype)


def test_get_array_fixture_free_lifetime_dtype_and_alignment_contract() -> None:
    cache = sitos.ParamCache(query_timeout_ms=5000)
    cache.attach("numpy-zero-copy")
    source = np.array([0x0102, 0x0304], dtype=np.dtype(">u2"))
    cache.put("array", source)
    array = cache.get_array("array", dtype=np.dtype(">u2"))

    assert array.tolist() == [0x0102, 0x0304]
    assert array.dtype == np.dtype(">u2")
    assert array.flags.writeable is False
    assert array.flags.aligned is False
    for dtype in (np.dtype("V4"), np.dtype("datetime64[D]")):
        with pytest.raises(TypeError):
            cache.get_array("array", dtype=dtype)

    with pytest.raises(sitos.NotFoundError):
        cache.get_array("missing", dtype=object())
    cache.put("number", 7)
    with pytest.raises(sitos.TypeMismatchError):
        cache.get_array("number", dtype=object())
    with pytest.raises(ValueError):
        cache.get_array("bad key", dtype=object())
    cache.detach()
    with pytest.raises(ValueError, match="detached"):
        cache.get_array("array", dtype=object())

    cache.close()
    del cache
    gc.collect()
    assert array.tolist() == [0x0102, 0x0304]
