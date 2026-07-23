"""Fixture-free NumPy API and conversion contract tests."""

from pathlib import Path

import numpy as np
import pytest

import sitos


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
