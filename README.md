# sitos

![CI](https://github.com/tetsuh/sitos/actions/workflows/ci.yml/badge.svg)

> A distributed parameter store for compute pipelines, powered by
> [Eclipse zenoh](https://zenoh.io/).

**sitos** (σῖτος — Greek for *grain*) delivers typed parameters and look-up tables to
distributed compute processes without timing bugs and without copies on the read hot path.

## Status

sitos is under active development. The repository contains the C++20 core, Python bindings,
in-memory and RocksDB storage engines, session snapshots and caches, raw-Zenoh interoperability,
executable examples, package validation, and cross-platform CI. The remaining work toward the
v1.0 quality boundary is tracked in the
[issue roadmap](docs/07_issue_breakdown.md).

## Core components

The [architecture document](docs/02_architecture.md) describes the complete component model and
lifecycle:

- **StorageNode** connects Zenoh queryables and subscribers to a storage engine and owns session
  snapshots and overlays.
- **ParamStore** is the client for remote put, get, list, delete, and batch operations.
- **ParamCache** attaches to a session and provides synchronized local reads, including zero-copy
  byte and NumPy views.

The typed key space supports `bool`, `int64`, `double`, `string`, and `bytes`. Plain Zenoh clients
can use the documented wire format without linking sitos.

## Build and install

CMake 3.20 or newer and a C++20 compiler are required. The development presets configure tests and
the supported optional components for each platform.

### Linux

```bash
cmake --preset dev-linux
cmake --build --preset dev-linux
ctest --preset dev-linux
```

### Windows

Run these commands from a Visual Studio developer shell with Ninja available:

```powershell
cmake --preset dev-windows
cmake --build --preset dev-windows
ctest --preset dev-windows
```

For an installable C++ package, configure the `release` preset, build it, and choose an install
prefix:

```bash
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix /opt/sitos
```

A local Python wheel can be built from the same source tree:

```bash
python -m build --wheel python --outdir dist
```

See [build, test, and packaging](docs/06_build_test_packaging.md) for optional Zenoh and RocksDB
configuration, installed CMake consumers, and repaired-wheel validation.

## Quickstarts

- [C++ quickstart](examples/cpp/quickstart.cpp) opens one transport, starts an in-memory
  StorageNode, submits values with ParamStore, creates a session, and reads them through ParamCache.
- [Python quickstart](examples/python/quickstart.py) runs the public Python APIs in isolated
  processes with bounded startup, observation, and cleanup.

The examples are executable acceptance tutorials rather than installed library artifacts. Follow
the build document's example configuration before running them.

## Documentation

- [Overview and document map](docs/00_overview.md)
- [Requirements](docs/01_requirements.md)
- [Architecture](docs/02_architecture.md)
- [Wire protocol](docs/03_wire_protocol.md)
- [C++ API](docs/04_api_cpp.md)
- [Python API](docs/05_api_python.md)
- [Build, test, and packaging](docs/06_build_test_packaging.md)
- [Issue roadmap](docs/07_issue_breakdown.md)
- [Public contract registry](docs/08_contract_registry.md)
- [Dependency policy](docs/09_dependency_policy.md)
- [ADR process](docs/10_adr_process.md)
- [Contributing](CONTRIBUTING.md)

## License

Apache-2.0. See [LICENSE](LICENSE).
