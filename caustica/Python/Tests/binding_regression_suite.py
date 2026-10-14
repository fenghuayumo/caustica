#!/usr/bin/env python3
"""Run Python GPU regressions sequentially in isolated, timeout-bounded processes.

Example (use the freshly built native module):
  cmake -E env PYTHONPATH=bin python caustica/Python/Tests/binding_regression_suite.py \
      --backend both --runs 10 --output-dir build/binding-regression
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


TESTS = (
    "binding_surface", "sensor_warmup", "sensor_aov_consistency",
    "sensor_product_resolution", "sensor_camera_parent", "sensor_world_poses",
    "sensor_robot_fk", "screenshot", "sensor_stress", "dense_sensor_stress", "engine_recreate",
)
GPU_FAULTS = ("device removed", "device lost", "devicelost", "gpu device lost",
              "vk_error_device_lost", "dxgi_error_device", "xid 109", "fence wait aborted")


def run_case(command: list[str], output: Path, timeout: float) -> dict:
    started = time.monotonic()
    output.mkdir(parents=True, exist_ok=True)
    log_path = output / "process.log"
    with log_path.open("w", encoding="utf-8") as log:
        try:
            process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                     timeout=timeout, env=os.environ.copy())
            result = {"status": "passed" if process.returncode == 0 else "failed",
                      "returncode": process.returncode}
        except subprocess.TimeoutExpired:
            result = {"status": "timeout", "returncode": None}
    lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    faults = [line for line in lines if any(token in line.lower() for token in GPU_FAULTS)]
    validation = [line for line in lines
                  if "Validation Error:" in line or "[Vulkan:" in line and line.startswith("ERROR:")]
    if faults or validation:
        if result["status"] == "passed":
            result["status"] = "failed"
    result.update(seconds=round(time.monotonic() - started, 3), command=command,
                  gpu_faults=faults, validation_errors=validation,
                  log=str(log_path), last_lines=lines[-12:])
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--backend", choices=("vulkan", "dx12", "both"), default="vulkan")
    parser.add_argument("--tests", nargs="+", choices=TESTS, default=list(TESTS))
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--captures", type=int, default=10)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()
    if min(args.runs, args.captures) < 1 or args.timeout <= 0:
        parser.error("runs, captures and timeout must be positive")
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    test_dir = Path(__file__).resolve().parent
    backends = ("vulkan", "dx12") if args.backend == "both" else (args.backend,)
    summary = {"python": sys.executable, "pythonpath": os.environ.get("PYTHONPATH"),
               "debug": args.debug, "runs": args.runs, "captures": args.captures,
               "results": []}
    for backend in backends:
        for name in args.tests:
            for run in range(args.runs):
                output = args.output_dir / backend / name / f"run_{run:03d}"
                if name in ("screenshot", "sensor_stress", "dense_sensor_stress"):
                    command = [sys.executable, "-u", str(test_dir / "vulkan_capture_stress_test.py"),
                               "--worker", "--backend", backend,
                               "--mode", "readback" if name == "screenshot" else "sensor",
                               "--captures", str(args.captures), "--width", "333", "--height", "257"]
                    if name == "dense_sensor_stress":
                        command.extend(["--dense-grid", "16", "--cameras", "3",
                                        "--width", "640", "--height", "480",
                                        "--camera-height", "2.8",
                                        "--churn-camera", "--check-content"])
                    elif name == "screenshot":
                        command.append("--check-content")
                else:
                    command = [sys.executable, "-u", str(test_dir / f"{name}_test.py")]
                    if backend == "vulkan":
                        command.append("--vulkan")
                command.extend(["--output-dir", str(output)])
                if args.debug:
                    command.append("--debug")
                result = run_case(command, output, args.timeout)
                result.update(backend=backend, test=name, run=run)
                summary["results"].append(result)
                summary["passed"] = sum(item["status"] == "passed" for item in summary["results"])
                summary["failed"] = len(summary["results"]) - summary["passed"]
                (args.output_dir / "results.json").write_text(
                    json.dumps(summary, indent=2), encoding="utf-8")
                print(f"{backend}/{name}/{run}: {result['status']} ({result['seconds']}s)", flush=True)
    return int(summary["failed"] != 0)


if __name__ == "__main__":
    raise SystemExit(main())
