#!/usr/bin/env bash
# Continue / finish Linux Vulkan build. Run in a normal terminal (has network + sudo).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export PATH="${HOME}/.local/photara-prefix/usr/bin:${HOME}/.local/bin:${PATH}"

DXC="${DXC_SPIRV_PATH:-${HOME}/.local/bin/dxc}"
if [[ ! -x "$DXC" ]]; then
  echo "ERROR: SPIR-V dxc not found at $DXC"
  exit 1
fi

if ! command -v ninja >/dev/null 2>&1; then
  echo "Installing ninja-build..."
  sudo apt-get install -y ninja-build
fi

echo "==> MathLib (NRD dependency, FetchContent)"
if [[ ! -f External/MathLib/CMakeLists.txt ]]; then
  rm -rf External/MathLib
  git clone --depth 1 --branch v7 https://github.com/NVIDIA-RTX/MathLib.git External/MathLib
fi

echo "==> DLSS SDK submodule (native NGX DLSS)"
if [[ ! -f External/DLSS/include/nvsdk_ngx.h ]]; then
  git submodule update --init External/DLSS
fi

echo "==> cmake configure"
# NATIVE_DLSS follows the CMake default (ON with Vulkan); the NGX SDK comes from
# the External/DLSS submodule.
cmake -S . -B build-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAUSTICA_WITH_VULKAN=ON \
  -DCAUSTICA_WITH_OIDN=OFF \
  -DCAUSTICA_WITH_PYTHON=OFF \
  -DCAUSTICA_WITH_PHYSX=OFF \
  -DDXC_SPIRV_PATH="$DXC" \
  -DFETCHCONTENT_SOURCE_DIR_MATHLIB="${ROOT}/External/MathLib"

echo "==> build caustica"
cmake --build build-linux --target caustica -j"$(nproc)"

echo
echo "Done: ${ROOT}/bin/caustica"
echo "Run:  ./bin/caustica --scene kitchen.scene.json"
