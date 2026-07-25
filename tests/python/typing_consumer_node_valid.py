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
