# RTX Path Tracing v1.8.1

![Title](./Docs/r-title.png)

## What's new in 1.8.0 and 1.8.1
 * Performance optimizations (25-30%+ perf gain over 1.7.x with comparable or better quality)
 * New performance/quality presets in "Display and performance" allow for better scaling from low end to high end GPUs 
 * New modes in `bistro-programmer-art` scene for stress testing animation and dynamic lighting scenarios (see [1](https://github.com/NVIDIA-RTX/caustica-Assets/blob/main/Screenshots/1.8.0/1_Balanced_4k.png), [2](https://github.com/NVIDIA-RTX/caustica-Assets/blob/main/Screenshots/1.8.0/2_Balanced_4k.png), [3](https://github.com/NVIDIA-RTX/caustica-Assets/blob/main/Screenshots/1.8.0/3_Balanced_4k.png), [4](https://github.com/NVIDIA-RTX/caustica-Assets/blob/main/Screenshots/1.8.0/4_Balanced_4k.png))
 * Fixes for a number of DLSS-RR denoising stability issues in reflected/refracted surfaces
 * DirectX SER and Opacity Micromaps are now available when Agility SDK 1.619 is enabled (on by default in 1.8.1, requires [DXR 1.2](https://devblogs.microsoft.com/directx/shader-model-6-9-retail-and-more/) driver support; to disable, remove Agility SDK 1.619 from CMake settings)

 
See [Releases](https://github.com/NVIDIA-RTX/caustica/releases) for more detail and downloads.

## Overview

RTX Path Tracing is a code sample that strives to embody years of ray tracing and neural graphics research and experience. It is intended as a starting point for a clean path tracer integration, as a reference for various integrated SDKs, and/or for learning and experimentation. 

It is a pure path tracer that does not rely on rasterization. In its main configuration, all light transport is evaluated within a single ray tracing pass, leveraging light-sampling caches for real-time performance. It further employs path-space layer decomposition and guide-buffer generation to support real-time denoising (DLSS-RR) and other techniques.

The base path tracing implementation derives from NVIDIA’s [Falcor Research Path Tracer](https://github.com/NVIDIAGameWorks/Falcor), ported to approachable C++/HLSL [Donut framework](https://github.com/NVIDIAGameWorks/donut). 

GTC presentation [How to Build a Real-time Path Tracer](https://www.nvidia.com/gtc/session-catalog/?tab.catalogallsessionstab=16566177511100015Kus&search.industry=option_1559593201839#/session/1666651593475001NN25) provides a high level introduction to most of the features, although it is pretty much out of date by now.



## Features

* DirectX 12 and Vulkan back-ends
* Reference and real-time modes
* Simple BSDF model that is easy(ish) to extend
* Simple asset pipeline based on glTF 2.0 (support for a subset of glTF extensions including animation)
* Volumes and nested dielectrics with priority
* Support for analytic lights (directional, spot, point), emissive triangles and environment map lighting
* NEE lighting with feedback-based, temporaly adaptive guided importance sampling (NEE-AT)
* Path tracing features such as: Low-discrepancy sample generator based on [Practical Hash-based Owen Scrambling](https://jcgt.org/published/0009/04/01/paper.pdf), use of [RayCones](https://research.nvidia.com/publication/2021-04_improved-shader-and-texture-level-detail-using-ray-cones) for texture MIP selection, RR early ray termination, firefly filter and similar 
* Basic post-processing features such as: TAA, tone mapping, bloom and similar
* Reference mode 'photo-mode screenshot' with simple [OptiX denoiser](https://developer.nvidia.com/optix-denoiser) integration
* [Shader Execution Reordering](https://developer.nvidia.com/blog/improve-shader-performance-and-in-game-frame-rates-with-shader-execution-reordering/) for significant increase in execution performance
* [RTXDI](https://github.com/NVIDIA-RTX/RTXDI) integration for ReSTIR DI (light importance sampling) and and ReSTIR GI (indirect lighting)
* [OMM](https://github.com/NVIDIA-RTX/OMM) integration for fast ray traced alpha testing
* [NRD](https://github.com/NVIDIA-RTX/NRD) ReLAX and ReBLUR denoiser integration with up to 3-layer path space decomposition
* [RTXTF](https://github.com/NVIDIA-RTX/RTXTF) integration for Stochastic Texture Filtering
* [Streamline](https://github.com/NVIDIAGameWorks/Streamline/) integration for DLSS (DLSS RR, DLSS SR, DLSS AA, DLSS FG & MFG)


## Requirements

- Windows 10 20H1 (version 2004-10.0.19041) or newer
- DXR Capable GPU (DirectX Raytracing 1.1 API, or higher; if DXR 1.2 not available please disable Agility SDK 1.619 in CMake settings)
- GeForce Game Ready Driver 595.71 or newer
- DirectX 12 or Vulkan API
- CMake v4.02+
- Visual Studio 2022 (v143 build tools) or later with Windows 10 SDK version 10.0.20348.0 or 10.0.26100.0 or later


## Known Issues

* By default, Agility SDK 1.619 is enabled and requires DXR 1.2 (Shader Model 6.9). Our current setup can only switch to older shader models at build configuration time, so if your GPU does not support DXR 1.2 with Shader Model 6.9, set RTXPT_D3D_AGILITY_SDK_PATH, RTXPT_D3D_AGILITY_SDK_VERSION and RTXPT_D3D_AGILITY_SDK_VERSION_NAME CMake variables to empty before cleaning the `/bin` folder, re-configuring/re-generating and rebuilding the project.
* Enabling Vulkan support requires a couple of manual steps, see [below](#building-vulkan)
* SER and OMM support on Vulkan is currently work in progress
* Running Vulkan on AMD GPUs may trigger a TDR during TLAS building in scenes with null TLAS instances
* Enabling debug layer on Vulkan will show a number of warnings and errors - fixes are work in progress
* We recommend using *NVIDIA Nsight Graphics* graphics for frame capture and analysis. If using other GPU performance tuning and debugging tools such as *PIX on Windows*, it is advisable to disable NVRHI_WITH_NVAPI and CAUSTICA_WITH_STREAMLINE variables in CMake to avoid compatibility issues. Please note: disabling these settings results in lower performance and missing features
* There is a known issue resulting in LIVE_DEVICE DirectX warnings reported at shutdown when Streamline is enabled in Debug builds
* There is a known issue resulting in black or incorrect transparencies/reflection on some AMD systems with latest drivers; this is most likely a driver error and has been reported


## Folder Structure

|						| |  
| -						| - |
| /bin					| default CMake folder for binaries and compiled shaders
| /build				| default CMake folder for build files
| /Assets				| models, textures, scene files  
| /Docs					| documentation 
| /External				| external libraries and SDKs, including Donut, Streamline, NRD, RTXDI, and OMM
| /Support				| optional command line tools (denoiser, texture compressor, etc)
| /caustica				| **RTX Path Tracing core; Sample.cpp/.h/.hlsl contain entry points**
| /caustica/PathTracer		| **Core path tracing shaders**


## Build

Windows is the primary supported platform. Linux/WSL builds use the Vulkan backend and can enable OIDN reference-mode denoising.

1. Clone the repository **with all submodules recursively**:
   
   `git clone --recursive https://github.com/NVIDIA-RTX/caustica.git`

2. Use CMake to configure the build and generate the project files.
   
   ```
   cd caustica
   cmake CMakeLists.txt -B ./build
   ```

   Use `-G "some tested VS version"` if specific Visual Studio or other environment version required. Make sure the x64 platform is used. 

3. Build the solution generated by CMake in the `./build/` folder.

   In example, if using Visual Studio, open the generated solution `build/RTXPathTracing.sln` and build it.

4. Select and run the `caustica` project. Binaries get built to the `bin` folder. Assets/media are loaded from `Assets` folder.

   If making a binary build, the `Assets` and `Support` folders can be placed into `bin` next to executable and packed up together (i.e. the sample app will search for both `Assets/` and `../Assets/`).

## Python Extension Install

caustica also builds a standalone Python extension module for offline rendering
and automation. After building the `caustica_py` target, install it into the
active Python environment from the repository root:

```
python -m pip install .
python -c "import caustica; print(caustica.MODE)"
```

The pip build assembles a local binary wheel from `bin/`, including the native
extension, runtime DLLs/so files, shaders, and a minimal asset payload. The
payload can be adjusted with environment variables:

| Variable | Default | Values |
| --- | --- | --- |
| `caustica_WHEEL_VERSION` | `0.2.0` | Any PEP 440 version |
| `caustica_WHEEL_ASSETS` | `minimal` | `minimal`, `full`, `none` |
| `caustica_WHEEL_DYNAMIC_SHADERS` | `bin` | `bin`, `full`, `none` |
| `caustica_WHEEL_SHADER_API` | `d3d12` on Windows, `vulkan` elsewhere | `d3d12`, `vulkan`, `both` |
| `CAUSTICA_WHEEL_SHADER_PACK` | `true` | `true`, `false` |

By default, wheel builds package compiled shader binaries into
`caustica.shaders.<api>.pack` instead of shipping the loose
`ShaderPrecompiled/` and `ShaderDynamic/Bin/` file trees.

You can also build a wheel explicitly:

```
python support/python/build_wheel.py
python -m pip install dist/caustica-*.whl
```

For a standalone executable distribution, generate a shader pack next to the
binary after building and warming the dynamic shader cache:

```
python support/python/package_shaders.py --shader-api d3d12
```


## Building Vulkan

Due to interaction with various included libraries, Vulkan support is not enabled by default on Windows and needs a couple of additional tweaks on the user side; please find the recommended steps below:
 * Install Vulkan SDK (we tested with VulkanSDK-1.3.290.0) and clear CMake cache (if applicable) to make sure the correct dxc.exe path from Vulkan SDK is set for SPIRV compilation
 * Set CAUSTICA_WITH_VULKAN and NVRHI_WITH_VULKAN CMake variables to ON. DXC_SPIRV_PATH should already have automatically picked up the location of the DXC compiler in the Vulkan SDK during config; if not, please set it manually
 * To run with Vulkan use `--vk` command line parameter

## Building Linux / WSL

Linux and WSL builds default to Vulkan and disable Windows-only integrations such as DirectX 12 Agility SDK, NVAPI, and Streamline. DLSS/DLSS-RR uses the native NVIDIA NGX Vulkan path when `RTXPT_WITH_NATIVE_DLSS=ON` (default for Linux Vulkan builds), and OIDN is downloaded from the official x86_64 Linux package when `RTXPT_WITH_OIDN=ON`.

Recommended WSL setup:

```
sudo apt update
sudo apt install -y build-essential cmake ninja-build python3-dev xorg-dev libwayland-dev wayland-protocols
```

Install the Linux Vulkan SDK and make sure `dxc` is on `PATH` or set `DXC_SPIRV_PATH` explicitly. Then configure and build:

```
cmake -S . -B build-linux -G Ninja \
  -DCAUSTICA_WITH_VULKAN=ON \
  -DNVRHI_WITH_VULKAN=ON \
  -DRTXPT_WITH_NATIVE_DLSS=ON \
  -DRTXPT_WITH_OIDN=ON \
  -DDXC_SPIRV_PATH="$VULKAN_SDK/bin/dxc"

cmake --build build-linux --config Release
```

On Linux, CMake fetches NVIDIA's DLSS SDK through Donut and copies the DLSS/DLSS-RR runtime `.so` files next to the executable. Use `-DRTXPT_WITH_NATIVE_DLSS=OFF` if you need a build without NGX/DLSS. Streamline features that are not part of native NGX DLSS, such as Reflex and DLSS Frame Generation, remain Windows/Streamline-only in this codebase.
 

 ## DirectX 12 Agility SDK
 RTX PT optionally integrates [DirectX 12 Agility SDK](https://devblogs.microsoft.com/directx/directx12agility/). If RTXPT_DOWNLOAD_AND_ENABLE_AGILITY_SDK CMake variable is set to TRUE, the version 717-preview will be automatically downloaded via CMake script and required build variables will be set. If different version is required, please set correct RTXPT_D3D_AGILITY_SDK_PATH and RTXPT_D3D_AGILITY_SDK_VERSION.

Version 717-preview enables native DirectX support for [Shader Execution Reordering](https://devblogs.microsoft.com/directx/ser/) and [Opacity Micromaps](https://devblogs.microsoft.com/directx/omm/). For testing this on Nvidia hardware, a preview driver is required and can be downloaded from https://developer.nvidia.com/downloads/shadermodel6-9-preview-driver 



 ## User Interface

Once the application is running, most of the SDK features can be accessed via the UI window on the left hand side and drop-down controls in the top-center. 

![UI](./Docs/r-ui.png)

Camera can be moved using W/S/A/D keys and rotated by dragging with the left mouse cursor.


## Command Line

- `--scene` loads a specific .scene.json file; example: `--scene programmer-art.scene.json`
- `--width` and `--height` to set the window size; example: `--width 3840 --height 2160`
- `--fullscreen` to start in full screen mode; example: `--width 3840 --height 2160 --fullscreen`
- `--debug` to enable the graphics API debug layer or runtime, and additional validation layers.
- `--vk` to enable Vulkan (see [building-vulkan](#building-vulkan))
 

## Developer Documentation

We are working on more detailed SDK developer documentation - watch this space!


## Contact

RTX Path Tracing is under active development. Please report any issues directly through GitHub issue tracker, and for any information, suggestions or general requests please feel free to contact us at pathtracing-sdk-support@nvidia.com!

## Thanks

Many thanks to the developers of the following open-source libraries or projects that make this project possible:
 * dear imgui (https://github.com/ocornut/imgui)
 * DirectX Shader Compiler (https://github.com/microsoft/DirectXShaderCompiler)
 * cgltf, Single-file glTF 2.0 loader (https://github.com/jkuhlmann/cgltf)
 * Krzysztof Narkowicz's Real-time BC6H compression on GPU (https://github.com/knarkowicz/GPURealTimeBC6H)
 * okdshin's https://github.com/okdshin/PicoSHA2
 * ...and any we might have forgotten (please let us know) :)

## Citation
If you use RTX Path Tracing in a research project leading to a publication, please cite the project.
The BibTex entry is

```bibtex
@online{caustica,
   title   = {{{NVIDIA}}\textregistered{} {RTX Path Tracing}},
   author  = {{NVIDIA}},
   year    = 2023,
   url     = {https://github.com/NVIDIA-RTX/caustica},
   urldate = {2024-01-26},
}
```

## License

See [LICENSE.txt](LICENSE.txt)

This project includes NVAPI software. All uses of NVAPI software are governed by the license terms specified here: https://github.com/NVIDIA/nvapi/blob/main/License.txt.
