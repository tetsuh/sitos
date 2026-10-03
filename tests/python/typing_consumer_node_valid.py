import sitos
from sitos._sitos import ParamValue

engine = sitos.InMemoryEngine()
node = sitos.StorageNode(engine, prefix="sitos")
node.create_session("session_a")
view = node.session_view("session_a")
value: ParamValue | None = view.get("missing", default=None)
flag: bool = view.contains("missing")
rows: list[tuple[str, ParamValue]] = list(view.items())
assert value is None or value
assert isinstance(flag, bool)
assert rows == []
node.stop()


class _DictEngine(sitos.StorageEngine):
    def __init__(self) -> None:
        self._data: dict[str, bytes] = {}

    def get(self, key: str) -> bytes | None:
        return self._data.get(key)

    def list(self, prefix: str) -> list[tuple[str, bytes]]:
        return [(key, value) for key, value in self._data.items() if key.startswith(prefix)]

    def put(self, key: str, value: bytes) -> None:
        self._data[key] = value

    def delete(self, key: str) -> None:
        self._data.pop(key, None)

    def take_snapshot(self) -> sitos.StorageReader:
        return self


def _python_engine_node() -> None:
    with sitos.StorageNode(_DictEngine(), prefix="sitos") as python_node:
        python_node.create_session("session_b")
