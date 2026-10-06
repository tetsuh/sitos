# sitos

![CI](https://github.com/tetsuh/sitos/actions/workflows/ci.yml/badge.svg)

> A distributed parameter store for compute pipelines, powered by
> [Eclipse zenoh](https://zenoh.io/).

**sitos** (σῖτος — Greek for *grain*) delivers typed parameters and look-up tables to
distributed compute processes without timing bugs and without copies on the read hot path.

## When to use sitos

sitos fits a pipeline where several processes on one host or a local network share configuration
and look-up tables while a job runs:

- Values are typed: `bool`, `int64`, `double`, `string`, and `bytes`. NumPy arrays are stored as
  bytes.
- Keys are hierarchical (`recon/fov`) and can be listed by prefix.
- A **Session** is one compute run. It starts from a consistent snapshot of the shared base, and
  later base updates do not leak into it. Changes made during the run go to the Session's overlay
  and reach every process attached to it.
- Readers keep an in-process cache, so a read is a local lookup with no network round trip and no
  copy. Byte values can be read as zero-copy NumPy arrays.
- No external daemon is required. Processes find each other through Zenoh peer discovery.
- Plain Zenoh clients can read and write the same keys through the documented wire format.

sitos is not a replicated or consensus store such as etcd, and it is not a general-purpose
database. Data lives in the storage engine of the StorageNode that serves the key space. The engine
can be in-memory, RocksDB, or your own.

## Status

sitos 0.1.0 is the first public release. Until 1.0, fixes ship in patch releases. New features and
breaking changes to the C++ API, the Python API, or the wire format ship in minor releases. The
design is described in the [architecture document](docs/02_architecture.md) and the
[ADRs](docs/adr/README.md).

## Install

```bash
pip install sitos
```

The PyPI wheel is published for Linux x86_64 and CPython 3.12. It bundles the Zenoh runtime and the
in-memory engine. For the C++ library, RocksDB, Windows, or other Python versions, build from
source as described [below](#build-from-source).

## Quickstart (Python)

The StorageNode, the writer, and the reader run as separate processes. Save the three scripts below
and run them in three terminals, in this order. They find each other on the local host without any
configuration.

`node.py` serves the key space and opens the Session `run1`:

```python
import sitos

with sitos.StorageNode(sitos.InMemoryEngine(), prefix="demo") as node:
    node.create_session("run1")
    input("Serving session 'run1'. Press Enter to stop.\n")
```

`writer.py` writes two values into the Session with ParamStore:

```python
import sitos

with sitos.ParamStore(prefix="demo") as store:
    store.put("session/run1", "recon/fov", 240.0)  # returns once the node acknowledges it
    store.put("session/run1", "recon/kernel", "sharp")
    print(store.get("session/run1", "recon/fov"))
```

`reader.py` attaches a ParamCache to the Session and reads locally:

```python
import sitos

with sitos.ParamCache(prefix="demo") as cache:
    cache.attach("run1")  # fetches the Session once
    print(cache.get("recon/fov"), cache.get("recon/kernel"))  # local reads, no round trip
```

`writer.py` prints `240.0`, and `reader.py` prints `240.0 sharp`. Use a different `prefix` to keep
independent deployments apart on the same network.

## Core components

The [architecture document](docs/02_architecture.md) describes the complete component model and
lifecycle:

- **StorageNode** connects Zenoh queryables and subscribers to a storage engine and owns session
  snapshots and overlays.
- **ParamStore** is the client for remote put, get, list, delete, and batch operations.
- **ParamCache** attaches to a session and provides synchronized local reads, including zero-copy
  byte and NumPy views.

The [Python API](docs/05_api_python.md) and [C++ API](docs/04_api_cpp.md) documents cover the full
surface, including acknowledged writes, subscriptions, fences, custom engines, and error types.

## Build from source

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

## Examples

- [C++ quickstart](examples/cpp/quickstart.cpp) opens one transport, starts an in-memory
  StorageNode, submits values with ParamStore, creates a session, and reads them through ParamCache.
- [Python quickstart](examples/python/quickstart.py) runs the public Python APIs in isolated
  processes with bounded startup, observation, and cleanup.

These examples are executable acceptance tutorials rather than installed library artifacts. Follow
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
