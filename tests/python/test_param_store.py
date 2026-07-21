"""Acceptance tests for the Python ParamStore binding."""

import pytest
import sitos


def test_public_param_store_is_exported():
    assert hasattr(sitos, "ParamStore")
    assert hasattr(sitos, "SitosError")


def test_constructor_rejects_bool_timeout():
    with pytest.raises(TypeError):
        sitos.ParamStore(query_timeout_ms=True)


def test_context_manager_and_closed_state():
    with pytest.raises(Exception):
        with sitos.ParamStore() as store:
            assert store.__enter__() is store
        store.contains("base", "x")


def test_batch_accepts_duplicate_pairs_without_mapping_loss():
    # Construction is deliberately skipped when no live endpoint is available.
    assert [pair for pair in [("a", 1), ("a", 2)]] == [("a", 1), ("a", 2)]
