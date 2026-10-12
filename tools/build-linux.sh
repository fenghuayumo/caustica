#!/usr/bin/env bash
# Build Caustica for Linux (Vulkan). Run from a normal terminal (not the agent sandbox).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "==> [1/5] apt packages"
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git git-lfs python3-dev \
  pkg-config xorg-dev libxkbcommon-dev libwayland-dev wayland-protocols \
  libvulkan-dev vulkan-tools wget unzip curl

echo "==> [2/5] submodules + Assets (LFS)"
git lfs install || true
git submodule update --init --recursive
GIT_LFS_SKIP_SMUDGE=0 git -C Assets checkout -f HEAD || true
git -C Assets lfs pull || true

echo "==> [3/5] Vulkan SDK / SPIR-V dxc"
if [[ -z "${VULKAN_SDK:-}" || ! -x "${VULKAN_SDK}/bin/dxc" ]]; then
  if [[ -x /usr/bin/dxc ]]; then
    export DXC_SPIRV_PATH=/usr/bin/dxc
  elif command -v dxc >/dev/null 2>&1; then
    export DXC_SPIRV_PATH="$(command -v dxc)"
  else
    SDK_VER="${VULKAN_SDK_VERSION:-1.4.321.1}"
    SDK_DIR="${HOME}/VulkanSDK/${SDK_VER}"
    if [[ ! -x "${SDK_DIR}/x86_64/bin/dxc" ]]; then
      echo "Downloading LunarG Vulkan SDK ${SDK_VER}..."
      mkdir -p "${HOME}/VulkanSDK"
      TMP="$(mktemp -d)"
      ARCHIVE="vulkansdk-linux-x86_64-${SDK_VER}.tar.xz"
      URL="https://sdk.lunarg.com/sdk/download/${SDK_VER}/linux/${ARCHIVE}"
      wget -O "${TMP}/${ARCHIVE}" "${URL}"
      tar -C "${HOME}/VulkanSDK" -xf "${TMP}/${ARCHIVE}"
      rm -rf "${TMP}"
    fi
    # Layout varies: <ver>/x86_64 or <ver>
    if [[ -f "${SDK_DIR}/setup-env.sh" ]]; then
      # shellcheck disable=SC1090
      source "${SDK_DIR}/setup-env.sh"
    elif [[ -f "${SDK_DIR}/x86_64/setup-env.sh" ]]; then
      # shellcheck disable=SC1090
      source "${SDK_DIR}/x86_64/setup-env.sh"
    fi
    export DXC_SPIRV_PATH="${VULKAN_SDK}/bin/dxc"
  fi
fi

if [[ ! -x "${DXC_SPIRV_PATH:-}" ]]; then
  echo "ERROR: SPIR-V dxc not found. Install LunarG Vulkan SDK and set VULKAN_SDK."
  exit 1
fi
echo "Using DXC: ${DXC_SPIRV_PATH}"

echo "==> [4/5] cmake configure"
cmake -S . -B build-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAUSTICA_WITH_VULKAN=ON \
  -DCAUSTICA_WITH_NATIVE_DLSS=ON \
  -DCAUSTICA_WITH_PHYSX=OFF \
  -DDXC_SPIRV_PATH="${DXC_SPIRV_PATH}"

echo "==> [5/5] build caustica"
cmake --build build-linux --target caustica -j"$(nproc)"

echo
echo "Done. Run:"
echo "  ${ROOT}/bin/caustica --scene kitchen.scene.json"
