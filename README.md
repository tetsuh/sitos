# sitos

> A distributed parameter store for compute pipelines, powered by
> [Eclipse zenoh](https://zenoh.io/).

**sitos** (σῖτος — Greek for *grain*) delivers typed parameters and
look-up tables to distributed compute processes — without timing bugs,
and without copies on the hot path.

## Key ideas

- **Typed key-value store** on the zenoh key space (`bool` / `int64` /
  `double` / `string` / `bytes`), same API for scalars and large LUTs
- **Session snapshots**: computations see a consistent point-in-time view,
  isolated from concurrent external updates (O(1) via engine-native snapshots)
- **Overlay + pub/sub**: runtime changes propagate to all participants
- **Zero-copy reads**: subscriber-side in-process cache (`std::span` /
  read-only NumPy views)
- **Pluggable storage engines**: in-memory (zero deps) and RocksDB included;
  bring your own
- **Wire-compatible with plain zenoh**: any zenoh client in any language
  can read and write — no sitos library required
- **C++20 core + Python bindings** (nanobind)

## Status

🚧 **Pre-development.** The design is complete; implementation is starting.
See the [design documents](docs/) and the
[issue tracker](https://github.com/tetsuh/sitos/issues) for the roadmap
(v0.1 → v1.0 milestones).

## Building

```bash
# Linux
cmake --preset dev-linux
cmake --build --preset dev-linux
ctest --preset dev-linux

# Windows (Ninja)
cmake --preset dev-windows
cmake --build --preset dev-windows
ctest --preset dev-windows
```

See [docs/06_build_test_packaging.md](docs/06_build_test_packaging.md) for
full build options.

## License

Apache-2.0
