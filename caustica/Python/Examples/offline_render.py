#!/usr/bin/env python
"""
Offline / headless reference rendering using the caustica Python extension.

Usage:
    # Make sure caustica.pyd is on your PYTHONPATH (e.g. by `cd` into the bin folder
    # next to the freshly built caustica.exe), then:

    python offline_render.py --scene bistro-programmer-art.scene.json \
                             --width 1280 --height 720 --spp 256 \
                             --out my_frame.png

The renderer is created with `headless=True`, which uses offscreen back
buffers without creating an OS window or swap chain. This makes it easy to
script reference renders of variations of a scene from Python.
"""

import argparse
import os
import sys
import time

try:
    import caustica
except ImportError as exc:                              # pragma: no cover
    sys.stderr.write(
        "Failed to import caustica.  Make sure caustica.pyd is on PYTHONPATH:\n"
        "    set PYTHONPATH=<path-to>/bin/Release;%PYTHONPATH%\n"
        f"Original error: {exc}\n"
    )
    raise


def render(args):
    print(f"[caustica] Mode: {caustica.MODE}")
    print(f"[caustica] Creating Renderer ({args.width}x{args.height}, headless={args.headless}) ...")

    renderer = caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=args.headless,
        vulkan=args.vulkan,
        adapter_index=args.adapter_index,
        scene=args.scene,
        accumulation_target=args.spp,
    )

    print(f"[caustica] Loaded scene: {renderer.app.scene_name}")
    scene = renderer.app.scene
    print(f"[caustica] Materials in scene: {scene.material_count}")
    print(f"[caustica] Lights in scene   : {scene.light_count}")

    # --- Configure rendering --------------------------------------------------
    s = renderer.settings
    s.realtime_mode = False                  # accumulation / reference mode
    s.accumulation_target = args.spp
    s.accumulation_prewarm_realtime_caches = False
    s.bounce_count = args.bounces
    s.use_nee = True
    s.enable_tone_mapping = True
    s.realtime_aa = 0                        # disable DLSS for offline
    s.use_restir_di = False
    s.use_restir_gi = False
    if args.gaussian_splat:
        s.enable_gaussian_splats = True
        s.gaussian_splat_depth_test = args.gaussian_splat_depth_test
        s.gaussian_splat_scale = args.gaussian_splat_scale
        s.gaussian_splat_alpha_scale = args.gaussian_splat_alpha_scale
        s.gaussian_splat_brightness = args.gaussian_splat_brightness
        s.gaussian_splat_alpha_cull_threshold = args.gaussian_splat_alpha_cull
        if not renderer.load_gaussian_splats(
            args.gaussian_splat,
            args.gaussian_splat_convert_rdf_to_donut,
        ):
            raise RuntimeError(f"Failed to load Gaussian splat: {args.gaussian_splat}")
    s.oidn_enabled = args.oidn
    s.oidn_use_gpu = args.oidn_gpu
    s.oidn_quality = args.oidn_quality
    s.oidn_passes = args.oidn_passes
    s.oidn_prefilter = args.oidn_prefilter
    if args.oidn:
        s.oidn_apply()

    # --- Optional camera override --------------------------------------------
    if args.camera_pos and args.camera_dir:
        renderer.set_camera(args.camera_pos, args.camera_dir, args.camera_up)
    if args.fov is not None:
        renderer.set_camera_fov(args.fov)

    # --- Optional material override ------------------------------------------
    if args.material_overrides:
        for spec in args.material_overrides:
            name, color = spec
            mat = scene.find_material(name)
            if mat is not None:
                mat.base_color = color
                print(f"[caustica] Overrode '{name}' base_color to {color}")
            else:
                print(f"[caustica] WARNING: material '{name}' not found, skipping")

    # --- Render --------------------------------------------------------------
    print(f"[caustica] Rendering {args.spp} samples per pixel ...")
    t_start = time.time()
    frames = renderer.step_until_accumulated()
    elapsed = time.time() - t_start
    print(f"[caustica] Done in {elapsed:.2f}s ({frames} frames executed)")

    # --- Save ----------------------------------------------------------------
    out_path = os.path.abspath(args.out)
    if renderer.save_screenshot(out_path):
        print(f"[caustica] Saved: {out_path}")
    else:
        sys.stderr.write(f"[caustica] Failed to save screenshot to {out_path}\n")

    renderer.close()


def main():
    parser = argparse.ArgumentParser(description="caustica offline rendering driver.")
    parser.add_argument("--scene",   default="bistro-programmer-art.scene.json",
                        help="Scene file relative to the Assets folder.")
    parser.add_argument("--width",   type=int, default=1280)
    parser.add_argument("--height",  type=int, default=720)
    parser.add_argument("--spp",     type=int, default=256,
                        help="Reference-mode samples-per-pixel target.")
    parser.add_argument("--bounces", type=int, default=8)
    parser.add_argument("--out",     default="frame.png",
                        help="Output image path (PNG/JPG/BMP/TGA).")
    parser.add_argument("--vulkan",  action="store_true",
                        help="Use Vulkan instead of DX12.")
    parser.add_argument("--adapter-index", type=int, default=-1,
                        help="DXGI adapter index. Default -1 lets caustica choose.")
    parser.add_argument("--no-headless", dest="headless",
                        action="store_false", default=True,
                        help="Show the window while rendering (debugging).")
    parser.add_argument("--oidn", action="store_true",
                        help="Run OIDN after reference accumulation reaches --spp.")
    parser.add_argument("--oidn-gpu", dest="oidn_gpu",
                        action=argparse.BooleanOptionalAction, default=True,
                        help="Use OIDN GPU device when available.")
    parser.add_argument("--oidn-quality", type=int, default=2,
                        help="OIDN quality: 0=Fast, 1=Balanced, 2=High.")
    parser.add_argument("--oidn-passes", type=int, default=2,
                        help="OIDN guides: 0=ColorOnly, 1=Albedo, 2=AlbedoNormal.")
    parser.add_argument("--oidn-prefilter", type=int, default=2,
                        help="OIDN guide prefilter: 0=None, 1=Fast, 2=Accurate.")

    parser.add_argument("--camera-pos", type=float, nargs=3, metavar=("X","Y","Z"))
    parser.add_argument("--camera-dir", type=float, nargs=3, metavar=("X","Y","Z"))
    parser.add_argument("--camera-up",  type=float, nargs=3, metavar=("X","Y","Z"),
                        default=(0.0, 1.0, 0.0))
    parser.add_argument("--fov", type=float, default=None,
                        help="Vertical FOV in degrees.")

    parser.add_argument("--material-override",
                        dest="material_overrides", action="append", nargs=4,
                        metavar=("NAME","R","G","B"),
                        help="Override a material's base color, e.g. --material-override Floor 0.8 0.6 0.4")
    parser.add_argument("--gaussian-splat", default=None,
                        help="3DGS .ply file to rasterize over the scene.")
    parser.add_argument("--gaussian-splat-convert-rdf-to-donut",
                        action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--gaussian-splat-depth-test",
                        action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--gaussian-splat-scale", type=float, default=1.0)
    parser.add_argument("--gaussian-splat-alpha-scale", type=float, default=1.0)
    parser.add_argument("--gaussian-splat-brightness", type=float, default=1.0)
    parser.add_argument("--gaussian-splat-alpha-cull", type=float, default=1.0 / 255.0)
    args = parser.parse_args()

    if args.material_overrides:
        args.material_overrides = [
            (spec[0], (float(spec[1]), float(spec[2]), float(spec[3])))
            for spec in args.material_overrides
        ]

    if args.camera_pos: args.camera_pos = tuple(args.camera_pos)
    if args.camera_dir: args.camera_dir = tuple(args.camera_dir)
    if args.camera_up : args.camera_up  = tuple(args.camera_up)

    render(args)


if __name__ == "__main__":
    main()
