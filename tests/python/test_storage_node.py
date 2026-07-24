"""Acceptance tests for the Python StorageNode and SessionView bindings."""

from __future__ import annotations

import gc
import threading

import pytest

import sitos


def test_storage_node_exports_opaque_engine_and_read_only_surface() -> None:
    assert hasattr(sitos, "InMemoryEngine")
    assert hasattr(sitos, "StorageNode")
    assert hasattr(sitos, "SessionView")
    engine = sitos.InMemoryEngine()
    assert not any(hasattr(engine, name) for name in ("put", "get", "list", "delete"))


def test_storage_node_constructor_is_keyword_only_and_validates_arguments() -> None:
    with pytest.raises(TypeError):
        sitos.StorageNode(sitos.InMemoryEngine(), "sitos")
    with pytest.raises(ValueError):
        sitos.StorageNode(sitos.InMemoryEngine(), prefix="bad prefix")
    with pytest.raises(ValueError):
        sitos.StorageNode(sitos.InMemoryEngine(), zenoh_config_json="")


def test_storage_node_starts_and_session_view_reads_owned_values() -> None:
    with sitos.StorageNode(sitos.InMemoryEngine(), prefix="sitos") as node:
        assert node.active_sessions() == []
        node.create_session("session_a")
        assert node.active_sessions() == ["session_a"]
        view = node.session_view("session_a")
        assert view.contains("missing") is False
        assert view.get("missing", default=None) is None
        with pytest.raises(sitos.NotFoundError):
            view.get("missing")
        assert list(view.items()) == []


def test_session_view_typed_get_and_default_precedence() -> None:
    with sitos.StorageNode(sitos.InMemoryEngine(), prefix="sitos/python_node_typed") as node:
        node.create_session("session_a")
        view = node.session_view("session_a")
        assert view.get("missing", type=int, default=None) is None
        with pytest.raises(TypeError):
            view.get("missing", None)
        with pytest.raises(sitos.NotFoundError):
            view.get("missing", type=int)


def test_storage_node_lifecycle_precedes_argument_conversion() -> None:
    node = sitos.StorageNode(sitos.InMemoryEngine(), prefix="sitos/python_node_lifecycle")
    node.stop()
    node.stop()
    with pytest.raises(ValueError, match="StorageNode is stopped"):
        node.create_session(object())
    with pytest.raises(ValueError, match="StorageNode is stopped"):
        node.close_session(object())
    with pytest.raises(sitos.DisconnectedError):
        node.session_view(object())
    assert node.active_sessions() == []


def test_session_view_is_eager_and_survives_node_stop() -> None:
    with sitos.StorageNode(sitos.InMemoryEngine(), prefix="sitos/python_node_eager") as node:
        node.create_session("session_a")
        view = node.session_view("session_a")
        rows = view.items()
        node.stop()
        assert list(rows) == []
        with pytest.raises(sitos.DisconnectedError):
            view.contains("missing")


def test_session_view_close_and_recreate_generation_is_not_reused() -> None:
    with sitos.StorageNode(sitos.InMemoryEngine(), prefix="sitos/python_node_generation") as node:
        node.create_session("session_a")
        old_view = node.session_view("session_a")
        node.close_session("session_a")
        with pytest.raises(sitos.NotFoundError):
            old_view.contains("missing")
        node.create_session("session_a")
        with pytest.raises(sitos.NotFoundError):
            old_view.items()


def test_storage_node_stop_is_safe_for_concurrent_callers() -> None:
    node = sitos.StorageNode(sitos.InMemoryEngine(), prefix="sitos/python_node_stop")
    errors: list[BaseException] = []

    def stop() -> None:
        try:
            node.stop()
        except BaseException as error:  # pragma: no cover - diagnostic assertion
            errors.append(error)

    threads = [threading.Thread(target=stop) for _ in range(4)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=5)
    assert all(not thread.is_alive() for thread in threads)
    assert errors == []


def test_session_view_does_not_retain_node_resources() -> None:
    node = sitos.StorageNode(sitos.InMemoryEngine(), prefix="sitos/python_node_weak")
    node.create_session("session_a")
    view = node.session_view("session_a")
    del node
    gc.collect()
    with pytest.raises(sitos.DisconnectedError):
        view.contains("missing")


def test_private_gil_test_support_is_not_public() -> None:
    assert not hasattr(sitos, "_gil_test_arm")
    assert not hasattr(sitos, "GILTestControl")


def test_wheel_validator_rejects_private_gil_support_members() -> None:
    from importlib.util import module_from_spec, spec_from_file_location
    from pathlib import Path

    script = Path(__file__).resolve().parents[2] / "scripts" / "check_wheel.py"
    spec = spec_from_file_location("sitos_check_wheel", script)
    assert spec is not None and spec.loader is not None
    validator = module_from_spec(spec)
    spec.loader.exec_module(validator)
    with pytest.raises(RuntimeError, match="private Python test support"):
        validator.validate_private_test_support_absent(["sitos/_gil_test_control.pyd"])

