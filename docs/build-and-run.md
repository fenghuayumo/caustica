# Building and running Caustica

This guide reflects the current root `CMakeLists.txt`, application targets, and
`CommandLineOptions` implementation.

## Prerequisites

Common requirements:

- A C++20 compiler and CMake 3.18 or newer.
- A ray-tracing-capable GPU and a recent vendor driver.
- All Git submodules, including the `Assets` pack (`fenghuayumo/caustica-assets`), the RHI headers, and the
  libraries under `External/`.
- Python 3.8 or newer, including development headers, when
  `CAUSTICA_WITH_PYTHON=ON` (the default).

Windows is the primary development platform. The normal Windows configuration
uses Visual Studio 2022, the Windows SDK, and DirectX 12. Linux and WSL use
Vulkan only; install a C++20 compiler, Ninja, window-system headers, and a
SPIR-V-capable `dxc` (typically from the LunarG Vulkan SDK).

Clone or repair the checkout with:

```powershell
git clone --recursive https://github.com/fenghuayumo/caustica/
cd caustica
git submodule update --init --recursive
```

On Linux, also enable Git LFS before initializing `Assets/` (large textures and
meshes):

```bash
sudo apt install -y git-lfs
git lfs install
git submodule update --init --recursive
```

## Windows: DirectX 12

Configure and build the editor:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target caustica
.\bin\caustica.exe --scene default.scene.json
```

Debug executables have a `D` suffix, for example `causticaD.exe`. CMake places
executables, the Python extension, runtime libraries, and cooked shaders under
`bin/`.

Useful build targets:

| Target | Purpose |
| --- | --- |
| `caustica` | Desktop editor and interactive renderer. |
| `caustica_thin_client` | Minimal `EngineApp` host without editor UI. |
| `caustica_py` | Native module used by `import caustica`. Requires Python support. |
| `causticaPathTracerShaders` | Cook and verify the path-tracing shader matrix. |
| `causticaShaderPack` | Package cooked shader bins into a load-only shader pack. |

The application targets depend on the required engine, NRD, OMM, and dynamic
shader targets, so a first build can take substantially longer than an
incremental C++ rebuild.

## Windows: Vulkan

Vulkan is off by default on Windows. Install the Vulkan SDK, clear an old CMake
cache if it selected the wrong compiler, then configure with the public backend
option:

```powershell
cmake -S . -B build-vk -G "Visual Studio 17 2022" -A x64 `
  -DCAUSTICA_WITH_VULKAN=ON
cmake --build build-vk --config Release --target caustica
.\bin\caustica.exe --backend vulkan
```

`DXC_SPIRV_PATH` can be set explicitly if CMake cannot find the Vulkan SDK
`dxc`. Do not set `RHI_WITH_VULKAN` or `CAUSTICA_RHI_WITH_VULKAN`: the root
build derives internal RHI backend variables from `CAUSTICA_WITH_VULKAN`.

When both DirectX 12 and Vulkan are compiled in, DirectX 12 remains the runtime
default on Windows. `--backend vulkan`, `--vk`, and `--vulkan` select Vulkan.

## Linux and WSL

Linux and WSL are **Vulkan-only**. DirectX 12, the Agility SDK, NVAPI, and
Streamline are disabled. DLSS Super Resolution and Ray Reconstruction use the
native NGX path (`CAUSTICA_WITH_NATIVE_DLSS`, on by default with Vulkan). Frame
Generation and Reflex are Streamline features and are not available.

### Packages (Debian / Ubuntu / WSL)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git git-lfs python3-dev \
  pkg-config xorg-dev libxkbcommon-dev libwayland-dev wayland-protocols \
  libvulkan-dev vulkan-tools
```

`xorg-dev` pulls in the X11 libraries GLFW needs. Wayland headers are optional
but recommended. `libvulkan-dev` is the loader/headers for linking; a recent
NVIDIA driver still has to provide a ray-tracing-capable `libvulkan.so` at
runtime.

### SPIR-V DXC

Shader cooking requires a **SPIR-V-capable** DirectX Shader Compiler. CMake
looks for `dxc` on `PATH` and under `$VULKAN_SDK/bin`. If it is missing,
configure fails.

Recommended sources, in order:

1. **LunarG Vulkan SDK** — install from https://vulkan.lunarg.com/sdk/home,
   then `source /path/to/vulkan/setup-env.sh` (or export `VULKAN_SDK`). The SDK
   `bin/dxc` is the usual choice.
2. Pass an explicit compiler:
   `-DDXC_SPIRV_PATH=/path/to/dxc`
3. Some NVIDIA sample trees ship the same tool at
   `bin/ShaderDynamic/Tools/vk/x64/dxc`.

Confirm the binary understands SPIR-V:

```bash
"$VULKAN_SDK/bin/dxc" -help | head
```

### Configure and build

Ninja is a single-config generator. Always set `CMAKE_BUILD_TYPE` (CMake
defaults it to `Release` if you omit it). From the repository root:

```bash
cmake -S . -B build-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAUSTICA_WITH_VULKAN=ON \
  -DCAUSTICA_WITH_NATIVE_DLSS=ON \
  -DDXC_SPIRV_PATH="$VULKAN_SDK/bin/dxc"

cmake --build build-linux --target caustica
./bin/caustica --scene kitchen.scene.json
```

Useful targets:

| Target | Output |
| --- | --- |
| `caustica` | Editor binary `bin/caustica` |
| `caustica_thin_client` | Minimal C++ host |
| `caustica_py` | Python module `bin/caustica.cpython-*-linux-gnu.so` |

The first build cooks SPIR-V shaders into `bin/ShaderBin/` and can take several
minutes. Incremental C++ rebuilds are much faster. Native DLSS copies
`libnvidia-ngx-dlss.so.*` and `libnvidia-ngx-dlssd.so.*` next to the binary;
OIDN copies its `.so` files there as well.

### Python extension

Build the importable module (requires `CAUSTICA_WITH_PYTHON=ON`, the default)
with the **same** Python interpreter you will import from:

```bash
cmake --build build-linux --target caustica_py
PYTHONPATH="$PWD/bin" python3 -c "import caustica; print(caustica.__file__, caustica.MODE)"
```

Or install a local wheel from the repository root:

```bash
python3 -m pip install .
python3 -c "import caustica; print(caustica.MODE)"
```

Linux builds have no DirectX 12 backend. `Renderer(vulkan=False)` and
`enumerate_adapters(vulkan=False)` fall back to Vulkan with a warning. Prefer
`vulkan=True` / `--vulkan` in scripts.

Use a conda or venv Python by pointing CMake at it:

```bash
cmake -S . -B build-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPython_EXECUTABLE="$(which python)" \
  -DDXC_SPIRV_PATH="$VULKAN_SDK/bin/dxc"
```

### Optional configurations

Disable native NGX DLSS (no Git fetch of the DLSS SDK, no NGX `.so` deploy):

```bash
cmake -S . -B build-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAUSTICA_WITH_NATIVE_DLSS=OFF \
  -DDXC_SPIRV_PATH="$VULKAN_SDK/bin/dxc"
```

OIDN 2.4.1 defaults on for x86-64 Linux (CMake downloads the official tarball
into `External/OIDN`). Set `-DCAUSTICA_WITH_OIDN=OFF` to skip it. OpenUSD
defaults **off** on Linux; point `-DCAUSTICA_USD_ROOT=` at a Linux SDK to
enable it. Set `-DCAUSTICA_WITH_PYTHON=OFF` if you do not have Python
development files.

### Troubleshooting

| Symptom | What to do |
| --- | --- |
| `DXC for SPIR-V was not found` | Install the Vulkan SDK or pass `-DDXC_SPIRV_PATH` |
| Empty `External/cxxopts` / missing headers | `git submodule update --init --recursive` |
| Asset pack warning / missing scenes | `git lfs install && git submodule update --init Assets` |
| `recompile with -fPIC` when linking `caustica_py` | Reconfigure with Python enabled (PIC is turned on automatically) |
| `import caustica` → undefined symbol | Rebuild `caustica_py` after a clean configure; Linux uses `--no-undefined` |
| `libgomp.so.1` RPATH warning with conda | Harmless if you import with the same conda Python CMake found |
| `libOpenImageDenoise_core.so` not found after copying `bin/` | In-tree builds resolve OIDN via RPATH into `External/OIDN/.../lib`. For a relocated `bin/`, set `LD_LIBRARY_PATH` to that directory (or keep the OIDN lib dir on the path) |
| No DLSS at runtime | Confirm `bin/libnvidia-ngx-dlss.so.*` exists and the NVIDIA driver is recent |

WSL needs a working GPU bridge (WSLg / NVIDIA CUDA on WSL) and a Vulkan-capable
driver inside the distro (`vulkaninfo` should list a discrete NVIDIA adapter).

## Important CMake options

| Option | Default | Notes |
| --- | --- | --- |
| `CAUSTICA_WITH_DX12` | On on Windows | DirectX 12 backend. |
| `CAUSTICA_WITH_VULKAN` | Off on Windows, on elsewhere | Vulkan backend and SPIR-V shaders. |
| `CAUSTICA_WITH_STREAMLINE` | On on Windows, off elsewhere | DLSS SR/RR, Frame Generation, and Reflex integration. Forced off on Linux. |
| `CAUSTICA_WITH_NATIVE_DLSS` | On when Vulkan is enabled | Native NGX path used on Linux and on Windows Vulkan-without-Streamline builds. Disabled automatically if Streamline is also enabled. |
| `CAUSTICA_WITH_PYTHON` | On | Embedded scripting and the `caustica_py` extension target. Turns on `-fPIC` for static libraries (required to link the Linux `.so`). |
| `CAUSTICA_WITH_OIDN` | On | Reference-mode Intel Open Image Denoise (x86-64 Windows and Linux packages). |
| `CAUSTICA_WITH_OPENUSD` | On on Windows, off elsewhere | Enables USD only when a valid SDK is found at `CAUSTICA_USD_ROOT`; otherwise CMake warns and disables it. |
| `CAUSTICA_RHI_WITH_NVAPI` | On on Windows | NVIDIA RHI extensions. Disable for some capture/debugger workflows. |
| `CAUSTICA_WITH_AFTERMATH` | Off | NVIDIA Aftermath crash dumps. |
| `CAUSTICA_DISTRIBUTION_BUILD` | Off | Removes debug shader/program symbols and enables distribution defaults. |
| `CAUSTICA_WITH_RUNTIME_SHADER_COMPILATION` | Off | Deploy shader sources and DXC for runtime compilation. Enable only for development fallback; cooked bins remain the normal path. |

If Python is not needed or its development package is unavailable:

```powershell
cmake -S . -B build -DCAUSTICA_WITH_PYTHON=OFF
```

To enable native OpenUSD loading, point both cache paths at a compatible SDK
layout:

```powershell
cmake -S . -B build `
  -DCAUSTICA_WITH_OPENUSD=ON `
  -DCAUSTICA_USD_ROOT=D:/SDK/OpenUSD `
  -DCAUSTICA_USD_PYTHON_DIR=D:/SDK/Python311
```

## DirectX 12 Agility SDK

`CAUSTICA_D3D_AGILITY_SDK_VERSION_NAME` controls download and activation. Its
current default is `1.619.0`; accepted cache choices also include the preview
versions listed by CMake. The root build derives
`CAUSTICA_D3D_AGILITY_SDK_PATH` and `CAUSTICA_D3D_AGILITY_SDK_VERSION`.

Select another listed version:

```powershell
cmake -S . -B build `
  -DCAUSTICA_D3D_AGILITY_SDK_VERSION_NAME=1.719.0-preview
```

Disable Agility SDK and use the system D3D12 runtime:

```powershell
cmake -S . -B build -DCAUSTICA_D3D_AGILITY_SDK_VERSION_NAME=
```

Clear or recreate the build directory when switching Agility versions. OMM is
enabled for DirectX 12 when the configured Agility version is 619 or newer;
actual DXR 1.2 and Shader Model 6.9 support still depends on the GPU and driver.

## Runtime files and asset lookup

`EngineApp` discovers runtime data in this order:

1. A supplied `EngineAppDesc::runtimeDirectory`, otherwise the module or
   executable directory containing `ShaderBin`.
2. A supplied `EngineAppDesc::resourceRoot`, otherwise the runtime directory
   containing `Assets`, then its parent.
3. The asset pack root: `EngineAppDesc::assetPackRoot`, `--assets`,
   `CAUSTICA_ASSETS_DIR`, `<resourceRoot>/Assets`, then `assets-builtin/`.

Scene files live under `Assets/scenes/`. `--scene kitchen.scene.json` still
resolves by filename. Media paths in scene JSON are pack-relative
(`models/...`, `env/...`).

For a copied binary distribution, keep the generated shader directories and
runtime libraries with the executable, and put the asset pack either in
`Assets/` beside the executable or one directory above it, or point
`CAUSTICA_ASSETS_DIR` at the pack. A distribution build may use
`caustica.shaders.<api>.pack` beside the executable instead of loose
shader bins.

## Command line

Run `caustica --help` (or `caustica.exe --help` on Windows) for the
parser-generated complete list. Common
options are:

| Option | Meaning |
| --- | --- |
| `--scene <file>` | Preferred `.scene.json` or scene file (resolved from the asset pack). |
| `--assets <dir>` | Asset pack directory (overrides `CAUSTICA_ASSETS_DIR` and `Assets/`). |
| `--width <px> --height <px>` | Initial output size. |
| `--fullscreen` | Start fullscreen. |
| `--backend <dx12\|d3d12\|vulkan\|vk>` | Select a compiled graphics backend. |
| `--vk`, `--vulkan` | Vulkan aliases. |
| `--debug` | Enable graphics debug layers and RHI validation. |
| `--gpu <selector>` | Select a GPU with `auto`, `index:N`, `name:text`, `uuid:hex`, or `luid:hex`. `auto` chooses the highest-scoring suitable hardware adapter. |
| `--noWindow` | Use a headless device with offscreen back buffers. |
| `--syncRender` | Disable the dedicated render thread. |
| `--stopAnimations` | Start with scene animation disabled. |
| `--noSER` | Disable Shader Execution Reordering. |
| `--pythonScript <file>` | Run a Python script after scene load. |
| `--pythonExpr <expr>` | Run an inline Python expression after scene load. |

Render overrides include `--useNEE`, `--NEEType`, `--useReSTIRDI`,
`--useReSTIRGI`, `--useReSTIRPT`, `--realtimeSamplesPerPixel`,
`--referenceSamplesPerPixel`, `--realtimeAA`, and the
`--overrideToRealtimeMode` / `--overrideToReferenceMode` pair.

Capture automation is exposed through `--captureSimple`, `--captureSequence`,
`--capturePath`, `--sequenceWarmupStart`, `--sequenceRecordStart`,
`--sequenceFPS`, and `--sequenceFrameCount`.

## Python extension

After building `caustica_py`, the native module lands in `bin/` as
`caustica.pyd` (Windows) or `caustica.cpython-*-linux-gnu.so` (Linux). The
repository packaging scripts collect that module and its runtime payload:

```powershell
python -m pip install .
python -c "import caustica; print(caustica.MODE)"
```

Without installing a wheel, Linux can import from the build output:

```bash
cmake --build build-linux --target caustica_py
PYTHONPATH="$PWD/bin" python3 -c "import caustica; print(caustica.MODE)"
```

For standalone wheel creation:

```powershell
python support/python/build_wheel.py
python -m pip install dist/caustica-*.whl
```

The canonical Caustica version is stored in the repository root `VERSION` file
and is shared by the editor executable, native Python extension, and wheel
metadata.

See the [Python API reference](../py_caustica.md) and
[Python examples](../examples/python/README.md) for renderer usage.
