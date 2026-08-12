#!/usr/bin/env python
"""Render a scene, read the RGBA8 framebuffer, and save it as a PNG."""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path


def write_rgba8_png(path: Path, width: int, height: int, pixels: bytes) -> None:
    expected_size = width * height * 4
    if len(pixels) != expected_size:
        raise ValueError(f"expected {expected_size} RGBA8 bytes, got {len(pixels)}")

    def chunk(kind: bytes, data: bytes) -> bytes:
        payload = kind + data
        return struct.pack(">I", len(data)) + payload + struct.pack(">I", zlib.crc32(payload))

    rows = b"".join(
        b"\0" + pixels[y * width * 4 : (y + 1) * width * 4]
        for y in range(height)
    )
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, level=6))
    png += chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scene", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--spp", type=int, default=8)
    parser.add_argument("--mode", choices=("reference", "realtime"), default="reference")
    parser.add_argument("--oidn", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--load-timeout", type=float, default=600.0)
    parser.add_argument("--warmup-frames", type=int, default=4)
    args = parser.parse_args()

    import caustica

    scene = args.scene if args.scene.startswith("builtin:") else str(Path(args.scene).resolve())
    renderer = caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=True,
        scene=scene,
        realtime=args.mode == "realtime",
        accumulation_target=args.spp,
    )
    if not renderer.scene_ready and not renderer.wait_until_ready(
        timeout_seconds=args.load_timeout,
        warmup_frames=args.warmup_frames,
    ):
        raise RuntimeError(f"scene did not become ready in {args.load_timeout:.1f} seconds")

    if args.mode == "reference":
        renderer.app.set_reference_mode(
            spp=args.spp,
            oidn=args.oidn,
            oidn_quality=2,
            oidn_passes=2,
            oidn_prefilter=2,
        )
        renderer.settings.oidn_use_gpu = True
        if args.oidn:
            renderer.settings.oidn_apply()
        frames = renderer.step_until_accumulated()
    else:
        renderer.app.set_realtime_mode(standalone_denoiser=True, realtime_aa=1)
        frames = args.spp
        if not renderer.step_n(frames):
            raise RuntimeError("realtime render failed")
    framebuffer = renderer.get_framebuffer()
    pixels = framebuffer.pixels

    assert framebuffer.shape == (args.height, args.width, 4)
    assert framebuffer.format == "RGBA8"
    assert framebuffer.dtype == "uint8"
    assert len(pixels) == args.width * args.height * 4
    assert len(set(pixels)) > 1, "framebuffer is unexpectedly constant"

    output = Path(args.out).resolve()
    write_rgba8_png(output, framebuffer.width, framebuffer.height, pixels)
    print(
        f"rendered_frames={frames} shape={framebuffer.shape} bytes={len(pixels)} "
        f"range=[{min(pixels)}, {max(pixels)}] output={output}",
        flush=True,
    )
    renderer.close()


if __name__ == "__main__":
    main()
