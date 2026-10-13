# GPU capture regression, 2026-10-13

Status: host-side failure handling and screenshot readback synchronization have
been repaired. The dense-scene GPU fault is still reproducible; this work does
not establish that sensor capture is stable on this driver.

## Environment and evidence

Tests used the extension in `bin/caustica.cp311-win_amd64.pyd`, Python 3.11,
Windows, an RTX 5090 D v2, and NVIDIA driver 596.36. The installed Windows Vulkan
validation and crash diagnostic layers are 1.4.328. The older Linux layer and
Xid 109 described in the original report were not tested on this host.

- Broad Python suite: 54 isolated processes, 30 passed and 24 failed. Vulkan
  failed the three camera-parent cases and three sensor stress cases. DX12
  failed 18 sensor correctness cases, including black output. A successful
  process exit is therefore insufficient to establish correct rendered output.
- Final Vulkan binding/screenshot repeat: 40/40 processes passed, comprising
  20 binding-surface runs and 20 screenshot runs with ten captures each. The
  screenshot runs used 333 x 257 to exercise non-aligned readback pitches and
  retained NumPy arrays across later frames and engine teardown.
- DeviceFactory and RenderGraph CTest targets passed.
- Final binding and screenshot checks with Vulkan validation enabled both
  passed without reported validation errors; a screenshot was visually checked.
- The negative device-loss probe reproduced the dense-scene loss, rejected
  subsequent frames/captures/readbacks, and completed shutdown with retained
  Python scene/camera wrappers. This verifies failure handling, not GPU stability.

Detailed results and logs are in:

```
build/vulkan-python-tests/continue-regression/results.json
build/vulkan-python-tests/final-binding-screenshot/results.json
build/vulkan-python-tests/loss-recovery-final-verified/process.log
```

## Repairs

Screenshot and framebuffer readback now wait for the render thread before
resolving the output texture. GPU-idle waits alone did not protect a texture
pointer that the render thread could still replace.

Vulkan device loss is latched across queue/timestamp operations. Lost timer
queries resolve to zero instead of leaving `getTimerQueryTime()` in an infinite
poll loop. Timeline polling no longer propagates a device-loss exception out of
the upload allocator. Vulkan AS device addresses now come from the AS-address
query rather than assuming they equal the backing-buffer address.

Failed frozen captures throw to Python instead of returning empty or incomplete
AOV products. CPU camera/framebuffer overrides are restored on exceptions, and
the lost session rejects later frames. Screenshot/readback failures also stop
the session. `get_pixels()` reports device loss rather than suggesting another
`step_frame()` on the lost device.

## Remaining GPU investigation

The dense reproducer contains a plane and 256 cubes, renders at 640 x 480, and
spawns three cameras. The fault can occur during `step_frame()` after the cameras
are spawned, before `capture_sensor_outputs()` starts. Using the same sensor
view as the main camera also reproduced loss; rendering the default main view
passed three ten-frame runs.

The crash diagnostic layer reports an instruction-pointer fault and an invalid
read in a page associated with a VertexBuffer. Its unfinished commands include
`vkCmdTraceRaysKHR` in MainPathTrace/PathTrace. This is stronger evidence than
the earlier checkpoint alone, but does not prove whether the read is due to
shader indexing, resource lifetime, or driver/compiler behavior. Command-buffer
reuse warnings from the diagnostic layer also need independent confirmation.

Artifacts:

```
build/vulkan-python-tests/diagnostics/original/cdl_dump.yaml
build/vulkan-python-tests/diagnostics/original/vendor_binary.dat
build/vulkan-python-tests/dense-frozen.gfxr
build/vulkan-python-tests/dense-frozen-replay.log
```

GFXReconstruct recorded a successful full headless session with three RT
pipelines. Its frame count is zero because the application does not present a
swapchain; API calls were still captured. Replay failed with capture/replay and
NV-extension validation errors, so it is not a validated standalone reproducer.
Nsight Systems produced no Vulkan trace data and cannot support a GPU timing
conclusion. GPU-assisted validation completed two dense captures, with no
reported application validation error, but its instrumentation materially
changes compilation and execution timing.

Tile versus full-view dispatch, a bounded raygen loop, removal of redundant
Gaussian queries, robust-buffer access, optional NV pipeline changes, and a
NonUniform SPIR-V decoration experiment did not eliminate the fault. These
trial changes were withdrawn; temporary binary modifications were restored.
The original working-tree tiling and submission-isolation changes were retained.

## Reproduce

From the repository root in PowerShell:

```powershell
$env:PYTHONPATH = (Resolve-Path bin).Path
python caustica/Python/Tests/binding_regression_suite.py --backend both --runs 3 --output-dir build/binding-regression
python caustica/Python/Tests/vulkan_capture_stress_test.py --scene build/vulkan-python-tests/dense.scene.json --width 640 --height 480 --cameras 3 --runs 10 --captures 20 --timeout 60 --output-dir build/dense-capture
```

`binding_regression_suite.py` runs cases sequentially in separate processes,
records per-case logs and commands, and treats GPU faults and validation errors
as failures even if the child exits zero. `device_loss_recovery_test.py` is a
separate manual negative test; run it with a timeout and a known failing scene.
Exit 77 means no fault was reproduced and the recovery check is inconclusive.
