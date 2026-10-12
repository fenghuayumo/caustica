#!/usr/bin/env bash
# Prefetch OIDN / DLSS / nanobind, then rebuild Linux caustica with them ON.
# Run in a normal terminal (needs GitHub / release download access).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export PATH="${HOME}/.local/photara-prefix/usr/bin:${HOME}/.local/bin:${PATH}"
DXC="${DXC_SPIRV_PATH:-${HOME}/.local/bin/dxc}"
OIDN_VER=2.4.1
DLSS_TAG=a8ed84e2d1cc1efa7bef63fb9394be6cb06c0d74
NANOBIND_TAG=v2.4.0

if [[ ! -x "$DXC" ]]; then
  echo "ERROR: dxc not found at $DXC"
  exit 1
fi

echo "==> [1/4] DLSS SDK -> External/DLSS"
if [[ ! -f External/DLSS/include/nvsdk_ngx.h ]]; then
  rm -rf External/DLSS
  git clone https://github.com/NVIDIA/DLSS.git External/DLSS
  git -C External/DLSS checkout "$DLSS_TAG"
else
  echo "    already present"
fi

echo "==> [2/4] OIDN ${OIDN_VER} -> External/OIDN"
OIDN_DIR="External/OIDN"
OIDN_PKG="${OIDN_DIR}/oidn-${OIDN_VER}.x86_64.linux"
if [[ ! -f "${OIDN_DIR}/v${OIDN_VER}.x86_64.linux.ver" ]]; then
  rm -rf "${OIDN_DIR}"
  mkdir -p "${OIDN_DIR}"
  ARCHIVE="/tmp/oidn-${OIDN_VER}.x86_64.linux.tar.gz"
  URL="https://github.com/RenderKit/oidn/releases/download/v${OIDN_VER}/oidn-${OIDN_VER}.x86_64.linux.tar.gz"
  wget -O "${ARCHIVE}" "${URL}"
  tar -C "${OIDN_DIR}" -xzf "${ARCHIVE}"
  # tarball usually extracts to oidn-2.4.1.x86_64.linux/
  if [[ ! -d "${OIDN_PKG}" ]]; then
    echo "ERROR: expected ${OIDN_PKG} after extract; got:"
    ls "${OIDN_DIR}"
    exit 1
  fi
  touch "${OIDN_DIR}/v${OIDN_VER}.x86_64.linux.ver"
  rm -f "${ARCHIVE}"
else
  echo "    already present"
fi

echo "==> [3/4] nanobind ${NANOBIND_TAG} -> External/nanobind"
if [[ ! -f External/nanobind/CMakeLists.txt ]]; then
  rm -rf External/nanobind
  git clone --depth 1 --branch "${NANOBIND_TAG}" \
    https://github.com/wjakob/nanobind.git External/nanobind
  git -C External/nanobind submodule update --init --recursive
else
  echo "    already present"
fi

# Prefer a stable Python if conda 3.14 causes nanobind issues
PYTHON_EXE="${Python_EXECUTABLE:-$(command -v python3)}"
echo "Using Python: ${PYTHON_EXE} ($("${PYTHON_EXE}" -c 'import sys; print(sys.version.split()[0])'))"

echo "==> [4/4] cmake + build (OIDN/DLSS/Python ON)"
# Fresh cache so previous OFF options don't stick
rm -rf build-linux
cmake -S . -B build-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAUSTICA_WITH_VULKAN=ON \
  -DCAUSTICA_WITH_NATIVE_DLSS=ON \
  -DCAUSTICA_WITH_OIDN=ON \
  -DCAUSTICA_WITH_PYTHON=ON \
  -DCAUSTICA_WITH_PHYSX=OFF \
  -DPython_EXECUTABLE="${PYTHON_EXE}" \
  -DDXC_SPIRV_PATH="${DXC}" \
  -DFETCHCONTENT_SOURCE_DIR_MATHLIB="${ROOT}/External/MathLib" \
  -DFETCHCONTENT_SOURCE_DIR_NANOBIND="${ROOT}/External/nanobind" \
  -DFETCHCONTENT_SOURCE_DIR_DLSS="${ROOT}/External/DLSS"

cmake --build build-linux --target caustica -j"$(nproc)"
cmake --build build-linux --target caustica_py -j"$(nproc)" || true

echo
echo "Done."
echo "  editor : ${ROOT}/bin/caustica"
echo "  python : PYTHONPATH=${ROOT}/bin ${PYTHON_EXE} -c 'import caustica; print(caustica.MODE)'"
