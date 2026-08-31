#!/usr/bin/env python
"""Convert NVIDIA/OIIO .tx textures to PNG next to the source files."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


DEFAULT_OIIOTOOL = Path(r"D:\ProgramTool\OpenUSD\bin\oiiotool.exe")


def find_oiiotool(explicit: Path | None) -> Path:
    if explicit and explicit.is_file():
        return explicit
    found = shutil.which("oiiotool")
    if found:
        return Path(found)
    if DEFAULT_OIIOTOOL.is_file():
        return DEFAULT_OIIOTOOL
    raise SystemExit("oiiotool.exe not found. Pass --oiiotool or add it to PATH.")


def convert_one(oiiotool: Path, src: Path, dst: Path) -> tuple[Path, str]:
    if dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime and dst.stat().st_size > 0:
        return dst, "skip"
    dst.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    extra = str(oiiotool.parent)
    lib = str(oiiotool.parent.parent / "lib")
    env["PATH"] = extra + os.pathsep + lib + os.pathsep + env.get("PATH", "")
    cmd = [str(oiiotool), str(src), "-o", str(dst)]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if proc.returncode != 0 or not dst.exists() or dst.stat().st_size == 0:
        err = (proc.stderr or proc.stdout or "unknown oiiotool error").strip()
        raise RuntimeError(f"{src.name}: {err}")
    return dst, "ok"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", type=Path, required=True, help="Folder of .tx files")
    parser.add_argument("--dst", type=Path, help="Output folder (default: next to each .tx)")
    parser.add_argument("--oiiotool", type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()

    src_dir = args.src.resolve()
    if not src_dir.is_dir():
        raise SystemExit(f"Texture folder not found: {src_dir}")

    sources = sorted(src_dir.rglob("*.tx"))
    if not sources:
        print(f"No .tx files in {src_dir}; nothing to convert")
        return 0
    oiiotool = find_oiiotool(args.oiiotool)

    print(f"Converting {len(sources)} .tx files with {oiiotool}")
    ok = skipped = failed = 0
    errors: list[str] = []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = []
        for src in sources:
            dst = (
                args.dst.resolve() / src.relative_to(src_dir).with_suffix(".png")
                if args.dst
                else src.with_suffix(".png")
            )
            futures.append(pool.submit(convert_one, oiiotool, src, dst))
        for i, fut in enumerate(as_completed(futures), 1):
            try:
                path, status = fut.result()
                if status == "skip":
                    skipped += 1
                else:
                    ok += 1
                if i % 25 == 0 or i == len(futures):
                    print(f"  {i}/{len(futures)}  last={path.name} ({status})")
            except Exception as exc:
                failed += 1
                errors.append(str(exc))
                print(f"  FAIL {exc}")

    print(f"Done: converted={ok} skipped={skipped} failed={failed}")
    if errors:
        print("\n".join(errors[:20]), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
