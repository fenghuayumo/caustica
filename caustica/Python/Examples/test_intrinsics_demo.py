#!/usr/bin/env python
"""Demonstrate set_camera_intrinsics issue in caustica.

This script creates two renders using set_camera_intrinsics with different cx/cy values
to demonstrate whether the cx/cy parameters are being respected.

Usage:
    python test_intrinsics_demo.py --out-dir ./intrinsics_test
    python test_intrinsics_demo.py --fx 346.0 --fy 346.0 --cx 320 --cy 240 --cx-offset 160 --cy-offset 360
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3] if len(Path(__file__).resolve().parents) > 3 else Path("/home/ybq/data0/Dizhou_caustica")


def configure_import_path() -> None:
    """Configure Python path to find caustica module."""
    try:
        import caustica  # noqa: F401
        return
    except ImportError:
        pass

    candidates = [
        REPO_ROOT / "bin",
        REPO_ROOT / "build-linux" / "bin",
        REPO_ROOT / "build" / "caustica" / "Release",
        Path(__file__).resolve().parent,
        Path("/home/ybq/data0/Dizhou_caustica"),
    ]
    for candidate in candidates:
        if glob.glob(str(candidate / "caustica*.pyd")) or glob.glob(str(candidate / "caustica*.so")):
            sys.path.insert(0, str(candidate))
            os.environ["PATH"] = str(candidate) + os.pathsep + os.environ.get("PATH", "")
            os.chdir(candidate)
            return

    searched = "\n".join(f"  {p}" for p in candidates)
    raise RuntimeError(f"Could not find caustica Python module. Searched:\n{searched}")


def build_test_scene() -> str:
    """Build a simple test scene WITHOUT camera (camera will be set via code).
    
    This matches the approach in load_from_h5_caustica.py where camera is configured
    programmatically rather than in the scene JSON.
    """
    scene = {
        "models": ["builtin:plane_cube"],
        "graph": [
            {
                "name": "Ground",
                "model": 0,
                "translation": [0.0, 0.0, 0.0],
                "scaling": [2.0, 1.0, 2.0],
            },
            {
                "name": "Lights",
                "children": [
                    {
                        "name": "Sun",
                        "type": "DirectionalLight",
                        "rotation": [-0.23053891, -0.15879166, -0.68904659, 0.66846975],
                        "angularSize": 1.5,
                        "color": [1.0, 0.96, 0.9],
                        "irradiance": 3.0,
                    }
                ],
            },
            # Note: No camera defined in scene JSON - we set it via code like load_from_h5_caustica.py
            {
                "name": "SampleSettings",
                "type": "SampleSettings",
                "realtimeMode": False,
                "startingCamera": -1,  # -1 means no default camera, we control it via code
            },
        ],
    }
    return json.dumps(scene, indent=2)


def init_camera_from_pose(renderer, K, width, height, camera_pose):
    """Initialize camera from pose matrix and intrinsics - matches load_from_h5_caustica.py approach.
    
    Args:
        renderer: caustica renderer instance
        K: 3x3 camera intrinsics matrix
        width: image width
        height: image height
        camera_pose: 4x4 camera-to-world pose matrix
    """
    import numpy as np
    
    # Extract position and orientation from pose matrix (same as load_from_h5_caustica.py)
    cam_pos = camera_pose[:3, 3]
    cam_dir = camera_pose[:3, 2]  # Forward direction (Z axis)
    cam_up = camera_pose[:3, 1]   # Up direction (Y axis)
    
    # Set camera extrinsics
    renderer.set_camera(cam_pos.tolist(), cam_dir.tolist(), cam_up.tolist())
    
    # Set camera intrinsics
    fx = float(K[0, 0])
    fy = float(K[1, 1])
    cx = float(K[0, 2])
    cy = float(K[1, 2])
    
    print(f"[init_camera] Pose set: pos={cam_pos}, dir={cam_dir}, up={cam_up}")
    print(f"[init_camera] Intrinsics: fx={fx:.3f}, fy={fy:.3f}, cx={cx:.3f}, cy={cy:.3f}")
    
    if hasattr(renderer, "set_camera_intrinsics"):
        renderer.set_camera_intrinsics(fx, fy, cx, cy, float(width), float(height))
    elif hasattr(renderer, "app") and hasattr(renderer.app, "set_camera_intrinsics"):
        renderer.app.set_camera_intrinsics(fx, fy, cx, cy, float(width), float(height))
    else:
        raise RuntimeError("set_camera_intrinsics not available!")


def build_camera_pose(position: list, look_at: list, up: list) -> 'np.ndarray':
    """Build a 4x4 camera-to-world pose matrix.
    
    Args:
        position: Camera position [x, y, z]
        look_at: Point to look at [x, y, z]
        up: Up vector [x, y, z]
    
    Returns:
        4x4 camera-to-world transformation matrix
    """
    import numpy as np
    
    pos = np.array(position, dtype=np.float64)
    target = np.array(look_at, dtype=np.float64)
    up_vec = np.array(up, dtype=np.float64)
    
    # Build camera coordinate system
    forward = target - pos
    forward = forward / np.linalg.norm(forward)
    
    right = np.cross(forward, up_vec)
    right = right / np.linalg.norm(right)
    
    up_new = np.cross(right, forward)
    
    # Build 4x4 pose matrix (camera-to-world)
    pose = np.eye(4, dtype=np.float64)
    pose[:3, 0] = right
    pose[:3, 1] = up_new
    pose[:3, 2] = forward  # caustica uses pose[:3,2] directly as camera direction
    pose[:3, 3] = pos
    
    return pose


def render_with_intrinsics(renderer, width: int, height: int, fx: float, fy: float, cx: float, cy: float, output_path: Path) -> bool:
    """Render using set_camera_intrinsics method - matches load_from_h5_caustica.py approach."""
    print(f"\n[Render] Using set_camera_intrinsics() with cx={cx:.1f}, cy={cy:.1f}")
    
    # Build camera pose matrix (same approach as load_from_h5_caustica.py)
    camera_pose = build_camera_pose(
        position=[0.0, 1.6, 5.0],
        look_at=[0.0, 0.0, 0.0],
        up=[0.0, 1.0, 0.0]
    )
    
    # Build K matrix
    import numpy as np
    K = np.array([
        [fx, 0.0, cx],
        [0.0, fy, cy],
        [0.0, 0.0, 1.0]
    ], dtype=np.float64)
    
    # Use the same function as load_from_h5_caustica.py
    init_camera_from_pose(renderer, K, width, height, camera_pose)
    
    # Render
    renderer.settings.reset_accumulation = True
    frames = renderer.step_until_accumulated()
    
    if renderer.save_screenshot(str(output_path)):
        print(f"[OK] Saved: {output_path} ({frames} frames)")
        return True
    else:
        print(f"[ERROR] Failed to save: {output_path}")
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description="Test set_camera_intrinsics functionality")
    parser.add_argument("--out-dir", type=str, default="./intrinsics_test", help="Output directory for test images")
    parser.add_argument("--width", type=int, default=640, help="Image width")
    parser.add_argument("--height", type=int, default=480, help="Image height")
    parser.add_argument("--spp", type=int, default=16, help="Samples per pixel")
    parser.add_argument("--fx", type=float, default=346.0, help="Focal length x")
    parser.add_argument("--fy", type=float, default=346.0, help="Focal length y")
    parser.add_argument("--cx", type=float, default=320.0, help="Principal point x (center)")
    parser.add_argument("--cy", type=float, default=240.0, help="Principal point y (center)")
    parser.add_argument("--cx-offset", type=float, default=600.0, help="Principal point x (offset test)")
    parser.add_argument("--cy-offset", type=float, default=600.0, help="Principal point y (offset test)")
    parser.add_argument("--vulkan", action="store_true", default=False, help="Use Vulkan backend")
    args = parser.parse_args()

    # Setup
    configure_import_path()
    import caustica

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    launch_cwd = Path.cwd()
    
    print("=" * 60)
    print("caustica set_camera_intrinsics Test")
    print("=" * 60)
    print(f"Resolution: {args.width}x{args.height}")
    print(f"Baseline intrinsics: fx={args.fx:.3f}, fy={args.fy:.3f}, cx={args.cx:.3f}, cy={args.cy:.3f}")
    print(f"Offset intrinsics: fx={args.fx:.3f}, fy={args.fy:.3f}, cx={args.cx_offset:.3f}, cy={args.cy_offset:.3f}")
    print(f"Output directory: {out_dir.resolve()}")
    print("=" * 60)

    # Build scene
    scene = build_test_scene()

    # Create renderer
    print("\n[Setup] Creating renderer...")
    renderer = caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=True,
        vulkan=args.vulkan,
        scene=scene,
        realtime=False,
        accumulation_target=args.spp,
    )

    try:
        # Configure renderer
        s = renderer.settings
        s.bounce_count = 4
        s.enable_tone_mapping = True
        s.realtime_aa = int(caustica.RealtimeAA.Off)

        # Test 1: Intrinsics with baseline cx/cy
        output1 = out_dir / "test_01_intrinsics_baseline1.png"
        success1 = render_with_intrinsics(renderer, args.width, args.height, args.fx, args.fy, args.cx, args.cy, output1)

        # # # Test 2: Intrinsics with offset cx/cy (should look different if cx/cy work)
        # # output2 = out_dir / "test_02_intrinsics_offset.png"
        # # success2 = render_with_intrinsics(renderer, args.width, args.height, args.fx, args.fy, args.cx_offset, args.cy_offset, output2)

        # # Summary
        # print("\n" + "=" * 60)
        # print("Test Summary")
        # print("=" * 60)
        
        # if success1 and success2:
        #     print("\nComparison:")
        #     print(f"  - test_01_intrinsics_baseline.png: cx={args.cx:.1f}, cy={args.cy:.1f}")
        #     print(f"  - test_02_intrinsics_offset.png: cx={args.cx_offset:.1f}, cy={args.cy_offset:.1f}")
        #     print("\nExpected behavior:")
        #     print("  - If cx/cy work correctly:")
        #     print("    * test_02 should show a different viewpoint (offset projection center)")
        #     print("  - If cx/cy do NOT work:")
        #     print("    * Both images will look the same (cx/cy ignored)")
        
        # print("\nOutput files:")
        # for f in [output1, output2]:
        #     if f.exists():
        #         print(f"  ✓ {f.name}")
        #     else:
        #         print(f"  ✗ {f.name} (failed)")

        # return 0 if (success1 and success2) else 1

    finally:
        renderer.close()


if __name__ == "__main__":
    raise SystemExit(main())
