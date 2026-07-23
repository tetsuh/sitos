from pathlib import Path
import math
import struct

import pytest

import sitos


FIXTURE_DIR = Path(__file__).parents[1] / "fixtures" / "payload_v1"


def fixture(name: str) -> bytes:
    text = (FIXTURE_DIR / f"{name}.hex").read_text(encoding="ascii")
    return bytes.fromhex(text)


@pytest.mark.parametrize(
    ("name", "value"),
    [
        ("bool_false", False),
        ("bool_true", True),
        ("s64_minus1", -1),
        ("s64_zero", 0),
        ("s64_i32max", 2**31 - 1),
        ("dp_zero", 0.0),
        ("dp_240", 240.0),
        ("str_empty", ""),
        ("str_ascii", "abc"),
        ("str_utf8", "穀"),
        ("bytes_empty", b""),
        ("bytes_0102ff", b"\x01\x02\xff"),
    ],
)
def test_encode_matches_golden_fixture(name: str, value: object) -> None:
    assert sitos.encode_value(value) == fixture(name)


def test_nan_encoding_matches_golden_fixture() -> None:
    assert sitos.encode_value(float("nan")) == fixture("dp_nan")


def test_nan_decode_and_round_trip_are_canonical() -> None:
    decoded = sitos.decode_value(fixture("dp_nan"))
    assert math.isnan(decoded)
    assert sitos.encode_value(decoded) == fixture("dp_nan")


@pytest.mark.parametrize("value", [-(2**63), 2**63 - 1])
def test_int64_boundaries_are_accepted(value: int) -> None:
    assert sitos.decode_value(sitos.encode_value(value)) == value


@pytest.mark.parametrize("value", [-(2**63) - 1, 2**63])
def test_int64_overflow_is_rejected(value: int) -> None:
    with pytest.raises(OverflowError):
        sitos.encode_value(value)


def test_decode_returns_python_values() -> None:
    assert sitos.decode_value(fixture("dp_240")) == 240.0
    assert sitos.decode_value(fixture("str_utf8")) == "穀"
    assert sitos.decode_value(fixture("bytes_0102ff")) == b"\x01\x02\xff"


def test_special_floating_values_round_trip() -> None:
    for value in (-0.0, float("inf"), float("-inf")):
        decoded = sitos.decode_value(sitos.encode_value(value))
        assert struct.pack("<d", decoded) == struct.pack("<d", value)


@pytest.mark.parametrize("value", [bytearray(b"bytes"), memoryview(b"bytes"), [1], object()])
def test_encode_unsupported_input_is_rejected(value: object) -> None:
    with pytest.raises(TypeError):
        sitos.encode_value(value)


@pytest.mark.parametrize("payload", [bytearray(b"\x00"), memoryview(b"\x00"), [0], object()])
def test_decode_non_bytes_input_is_rejected(payload: object) -> None:
    with pytest.raises(TypeError):
        sitos.decode_value(payload)  # type: ignore[arg-type]


def test_encode_numpy_array_copies_c_order_bytes() -> None:
    numpy = pytest.importorskip("numpy")
    value = numpy.array([[1, 2], [3, 4]], dtype=numpy.uint16)
    assert sitos.encode_value(value) == b"\x04\x01\x00\x02\x00\x03\x00\x04\x00"


def test_encode_numpy_rejects_non_contiguous_and_object_arrays() -> None:
    numpy = pytest.importorskip("numpy")
    with pytest.raises(ValueError):
        sitos.encode_value(numpy.arange(8, dtype=numpy.uint8)[::2])
    with pytest.raises(TypeError):
        sitos.encode_value(numpy.array([object()], dtype=object))


def test_decode_numpy_input_is_rejected() -> None:
    numpy = pytest.importorskip("numpy")
    with pytest.raises(TypeError):
        sitos.decode_value(numpy.array([0], dtype=numpy.uint8))


def test_malformed_payloads_are_rejected() -> None:
    for payload in (
        b"",
        b"\xff",
        b"\x00",
        b"\x00\x00\x00",
        b"\x01\x00",
        b"\x01" + b"\x00" * 9,
        b"\x02\x00",
        b"\x02" + b"\x00" * 9,
    ):
        with pytest.raises(ValueError):
            sitos.decode_value(payload)


def test_invalid_utf8_is_rejected() -> None:
    with pytest.raises(ValueError):
        sitos.decode_value(b"\x03\xff")


def test_unpaired_surrogate_is_rejected() -> None:
    with pytest.raises(ValueError):
        sitos.encode_value("\ud800")


def test_variable_length_values_consume_the_remaining_body() -> None:
    assert sitos.decode_value(b"\x03abc") == "abc"
    assert sitos.decode_value(b"\x04\x00\xff") == b"\x00\xff"
