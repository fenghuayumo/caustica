# Building and running Caustica

This guide reflects the current root `CMakeLists.txt`, application targets, and
`CommandLineOptions` implementation.

## Prerequisites

Common requirements:

- A C++20 compiler and CMake 3.18 or newer.
- A ray-tracing-capable GPU and a recent vendor driver.
- All Git submodules, including `Assets`, `rtxmu`, the RHI headers, and the
  libraries under `External/`.
- Python 3.8 or newer, including development headers, when
  `CAUSTICA_WITH_PYTHON=ON` (the default).

Windows is the primary development platform. The normal Windows configuration
uses Visual Studio 2022, the Windows SDK, and DirectX 12. Linux and WSL use
Vulkan; install the Vulkan SDK and make its SPIR-V-capable `dxc` available.

Clone or repair the checkout with:

```powershell
git clone --recursive https://github.com/fenghuayumo/caustica.git
cd caustica
git submodule update --init --recursive
```

## Windows: DirectX 12

Configure and build the editor:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target caustica
.\bin\caustica.exe --scene default.json
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

DirectX backends, Agility SDK, NVAPI, and Streamline are disabled outside
Windows. Vulkan defaults on. A representative WSL dependency set is:

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build python3-dev \
  xorg-dev libwayland-dev wayland-protocols
```

After installing the Linux Vulkan SDK:

```bash
cmake -S . -B build-linux -G Ninja \
  -DCAUSTICA_WITH_VULKAN=ON \
  -DDXC_SPIRV_PATH="$VULKAN_SDK/bin/dxc"
cmake --build build-linux --config Release --target caustica
```

Native NGX DLSS for Vulkan defaults on when Vulkan is enabled. Disable it for a
portable build that does not fetch or deploy NGX:

```bash
cmake -S . -B build-linux -G Ninja \
  -DCAUSTICA_WITH_VULKAN=ON \
  -DCAUSTICA_WITH_NATIVE_DLSS=OFF \
  -DDXC_SPIRV_PATH="$VULKAN_SDK/bin/dxc"
```

OIDN 2.4.1 is supported on x86-64 Windows and Linux and defaults on. Set
`CAUSTICA_WITH_OIDN=OFF` if it is not wanted.

## Important CMake options

| Option | Default | Notes |
| --- | --- | --- |
| `CAUSTICA_WITH_DX12` | On on Windows | DirectX 12 backend. |
| `CAUSTICA_WITH_VULKAN` | Off on Windows, on elsewhere | Vulkan backend and SPIR-V shaders. |
| `CAUSTICA_WITH_STREAMLINE` | On on Windows | DLSS SR/RR, Frame Generation, and Reflex integration. Forced off elsewhere. |
| `CAUSTICA_WITH_NATIVE_DLSS` | On when Vulkan is enabled | Native NGX path; disabled automatically if Streamline is also enabled. |
| `CAUSTICA_WITH_PYTHON` | On | Embedded scripting and the `caustica_py` extension target. |
| `CAUSTICA_WITH_OIDN` | On | Reference-mode Intel Open Image Denoise. |
| `CAUSTICA_WITH_OPENUSD` | On | Enables USD only when a valid SDK is found at `CAUSTICA_USD_ROOT`; otherwise CMake warns and disables it. |
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
   executable directory containing `ShaderPrecompiled`.
2. A supplied `EngineAppDesc::resourceRoot`, otherwise the runtime directory
   containing `Assets`, then its parent.

For a copied binary distribution, keep the generated shader directories and
runtime libraries with the executable, and put `Assets/` either beside the
executable or one directory above it. A distribution build may use
`caustica.shaders.<api>.pack` beside the executable instead of loose dynamic
shader bins.

## Command line

Run `caustica.exe --help` for the parser-generated complete list. Common
options are:

| Option | Meaning |
| --- | --- |
| `--scene <file>` | Preferred `.scene.json` or scene file. |
| `--width <px> --height <px>` | Initial output size. |
| `--fullscreen` | Start fullscreen. |
| `--backend <dx12\|d3d12\|vulkan\|vk>` | Select a compiled graphics backend. |
| `--vk`, `--vulkan` | Vulkan aliases. |
| `--debug` | Enable graphics debug layers and RHI validation. |
| `--adapterIndex <n>` | Select an adapter by index; `-1` is automatic. |
| `--adapter <text>` | Prefer an adapter whose name contains the text. |
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

After building `caustica_py`, the repository packaging scripts collect the
native module and its runtime payload:

```powershell
python -m pip install .
python -c "import caustica; print(caustica.MODE)"
```

For standalone wheel creation:

```powershell
python support/python/build_wheel.py
python -m pip install dist/caustica-*.whl
```

See the [Python API reference](../py_caustica.md) and
[Python examples](../caustica/Python/Examples/README.md) for renderer usage.
