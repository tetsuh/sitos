from importlib import metadata

import sitos


def test_import_and_version_match() -> None:
    assert sitos.__version__ == metadata.version("sitos")
    assert hasattr(sitos, "_sitos")
    assert callable(sitos.encode_value)
    assert callable(sitos.decode_value)
