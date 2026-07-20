#!/usr/bin/env bash
set -euo pipefail

stage_root="${1:?usage: build_manylinux_zenohc.sh <stage-root>}"
version="1.9.0"
rust_toolchain="1.88.0"
archive="/tmp/zenoh-c-${version}.tar.gz"
url="https://github.com/eclipse-zenoh/zenoh-c/archive/refs/tags/${version}.tar.gz"
expected_sha256="6d66b1d1c725700148a6ea90faf93aa99c72db71a348bf30f5838b5a1be192d9"
source_root="/tmp/zenoh-c-${version}"

rm -rf "${source_root}" "${stage_root}"
mkdir -p "${stage_root}"
curl --fail --silent --show-error --location "${url}" --output "${archive}"
printf '%s  %s\n' "${expected_sha256}" "${archive}" | sha256sum --check --strict
mkdir -p "${source_root}"
tar -xzf "${archive}" --strip-components=1 -C "${source_root}"

if ! command -v rustup >/dev/null 2>&1; then
  curl --fail --silent --show-error https://sh.rustup.rs | sh -s -- -y --profile minimal
  # shellcheck disable=SC1091
  source "${HOME}/.cargo/env"
fi
rustup toolchain install --profile minimal "${rust_toolchain}"
cargo "+${rust_toolchain}" build --manifest-path "${source_root}/Cargo.toml" --release

mkdir -p "${stage_root}/include" "${stage_root}/lib"
cp -a "${source_root}/include/." "${stage_root}/include/"
cp "${source_root}/target/release/libzenohc.so" "${stage_root}/lib/libzenohc.so"
cp "${source_root}/LICENSE" "${stage_root}/LICENSE"
