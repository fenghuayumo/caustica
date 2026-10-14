# GPU capture follow-up, 2026-10-14

This continues the [2026-10-13 investigation](gpu_capture_regression.md).
Validation uses Windows, Python 3.11, an RTX 5090 D v2, and NVIDIA driver 596.36.
Linux Xid 109 has not been tested on this host.

## Repairs and evidence

Scene structure changes can replace buffers and recycle bindless descriptors
while submitted ray dispatches still read the old scene. Keeping the previous
TLAS/SBT alive does not retain every resource reached through the descriptor
table. SceneGpuUpdater now drains outstanding GPU readers before structural
updates or vertex-buffer recreation; runtime mesh uploads synchronize before
texture/material/mesh descriptor changes. Ordinary transform updates retain
their normal queue ordering.

With identical shader sources, removing the SceneGpuUpdater wait reproduced
device loss in 3/3 Vulkan dense-scene processes. Restoring it passed 10/10
processes with 10 three-camera captures each; DX12 passed 5/5. These preliminary
checks verified stability and array ownership but did not reject black RGB.

DX12's ComputePipelineRegistry still selected an older shader pack even when
the RT compiler and ShaderFactory selected newly cooked loose shaders. The
compute registry now applies the loose-manifest freshness rule too, keeping
shared frame/output layouts consistent. Before the change the AOV test's HDR
mean/max were zero; afterward they were 0.18101/1.35352 and the framebuffer's
mean RGB was 73.949. The AOV consistency test passed with validation enabled.

A subsequent broad suite exposed a separate access violation after successful
image checks in the second engine session in one Python process. An existing
symbolized native crash log points to StreamlineIntegration::shutdown calling
slDLSSGSetOptions; this is supporting evidence rather than a newly captured
stack for that intermediate failed process. Streamline SDK feature helpers
retain function-local static pointers into plugins unloaded by slShutdown.
The integration now resolves feature entry points against the current session,
covering DLSS, DLSS-G, DLSS-RR, Reflex, NIS, and DeepDVC without editing vendor
headers. Python debug sessions enable native INFO logs to expose shader/PSO
selection and teardown diagnostics.

App::runFrame now returns failure and stops the session when the RHI has already
reported device loss, instead of returning a successful Python step afterward.
This check does not add a GPU wait to every ordinary frame.

The final screenshot probe also found that UTF-8 Chinese directory/filenames
were interpreted through Windows' ANSI code page: save_screenshot returned
true, but Pillow could not open the requested path. Python screenshot entry
points now construct UTF-8 filesystem paths, EngineApp forwards UTF-8 bytes,
and the STB writer uses wide Windows file APIs. The repeated-session test
includes Chinese screenshot directories and filenames.

The validated working tree also contains full-view path-tracing dispatches in
place of the previous 64-pixel tiling. Stress results apply to this complete
working tree and the rebuilt extension, not to an older distributed module.

## Regressions

The isolated Python runner records commands, exit codes, GPU/validation errors,
and timeout logs. Checks now include:

- A generated plane plus 257 cubes, three 640 x 480 sensor views, and a lit scene
  viewed above the cubes so RGB/depth content checks have meaningful expectations.
- Per-tick camera spawn/despawn to repeatedly exercise scene structure changes.
- Preserved frame identity and simulation time across frozen captures.
- Nonempty RGB, finite/nonempty depth, and retained NumPy arrays after later
  captures and engine teardown.
- Odd-pitch 333 x 257 screenshots decoded with Pillow and compared pixel-for-pixel
  with get_pixels(), including rejection of all-black framebuffer RGB.
- Eight engine create/capture/screenshot/shutdown cycles in one process, with
  earlier scene/output wrappers and arrays retained across subsequent sessions.

Test dependencies are NumPy and Pillow. Reproduce from PowerShell:

```powershell
$env:PYTHONPATH = (Resolve-Path bin).Path
python caustica/Python/Tests/binding_regression_suite.py --backend both --runs 3 --captures 20 --timeout 180 --output-dir build/capture-investigation/verified-binding-suite
python caustica/Python/Tests/binding_regression_suite.py --backend both --tests dense_sensor_stress screenshot --runs 20 --captures 50 --timeout 180 --output-dir build/capture-investigation/verified-stress-suite
```

The intermediate broad run passed 59/60 processes and exposed the Streamline
teardown crash. It was superseded by the verified batches below.

## Verification

- Broad suite: 66/66 isolated processes passed on the repaired GPU/compute/SDK
  paths, including 48 same-process engine recreation cycles.
- Expanded stress: 80/80 processes passed, with 6,000 dense-scene sensor products
  and 2,000 screenshots. No GPU-loss or validation-error log was detected. PNG
  checks compare all RGBA channels to framebuffer readback.
- Seven native CTest targets passed: DeviceFactory, RenderGraph, SceneEcs,
  SceneSerializer, TaskGraph, SchedulePlan, and ShaderDependencyIndex. The last
  target initially detected old DXIL debug-variant records retained by the
  additive cook manifest; both debug modes were recooked before it passed.
- The desktop executable, Python extension, and both DXIL/SPIR-V shader packs
  were rebuilt. The broad/stress tests cover fresh loose shaders superseding an
  obsolete DXIL pack. After the Unicode repair, the final validation batch
  passed 6/6 processes on the rebuilt packs, including 16 additional engine
  recreation cycles, Chinese screenshot paths, dense sensor views, and ASCII
  screenshots. Native logs confirm both RT and compute loaded the new packs;
  no validation error or GPU-loss message was detected.
- Screenshot dimensions and the decoded odd-width DX12 image were also inspected
  visually; the cube and floor are correctly rendered without row-pitch artifacts.

Experimental logs, screenshots, GPU recordings, temporary probes, and result
directories under `build/capture-investigation` and `build/vulkan-python-tests`
are disposable and excluded from this commit. Permanent regression scripts and
the evidence summary above are retained; the reproduction commands recreate
their output directories. Requested cleanup was blocked by the execution
environment's deletion policy, so these local directories may still exist.
