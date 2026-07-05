#!/usr/bin/env python3
"""Rename shaders/render/ subdirectories and update all references."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(r"D:\ProgramCode\C++\Render\caustica")
SHADER_RENDER = ROOT / "caustica" / "caustica" / "shaders" / "render"
SEARCH_ROOTS = [ROOT / "caustica", ROOT / "application", ROOT / "scripts"]
SKIP_DIRS = {"third_party", "External", "thirdparty", ".git", "build", "build_release", "bin"}
TEXT_SUFFIXES = {".h", ".cpp", ".py", ".cmake", ".txt", ".md", ".json", ".cfg", ".hlsl", ".hlsli"}

PATH_REPLACEMENTS = [
    ("shaders/render/lighting/distant/", "shaders/render/lighting/distant/"),
    ("shaders/render/lighting/", "shaders/render/lighting/"),
    ("shaders/render/processingPasses/", "shaders/render/processingPasses/"),
    ("shaders/render/toneMapper/", "shaders/render/toneMapper/"),
    ("shaders/render/gpuSort/", "shaders/render/gpuSort/"),
    ("shaders/render/rtxdi/", "shaders/render/rtxdi/"),
    ("shaders/render/misc/", "shaders/render/misc/"),
    ("shaders/render/nrd/", "shaders/render/nrd/"),
]

HLSL_RELATIVE = [
    ("../rtxdi/", "../rtxdi/"),
    ("../nrd/", "../nrd/"),
]


def case_rename(src: Path, dst_name: str) -> None:
    if not src.exists():
        print(f"  skip (missing): {src.relative_to(ROOT)}")
        return
    dst = src.parent / dst_name
    tmp = src.parent / f"_ren_{dst_name}"
    rel_src = src.relative_to(ROOT)
    rel_dst = dst.relative_to(ROOT)
    print(f"  {rel_src} -> {rel_dst}")
    subprocess.run(["git", "mv", str(src), str(tmp)], cwd=ROOT, check=True)
    subprocess.run(["git", "mv", str(tmp), str(dst)], cwd=ROOT, check=True)


def git_mv(src: Path, dst: Path) -> None:
    if not src.exists():
        print(f"  skip (missing): {src.relative_to(ROOT)}")
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    print(f"  {src.relative_to(ROOT)} -> {dst.relative_to(ROOT)}")
    subprocess.run(["git", "mv", str(src), str(dst)], cwd=ROOT, check=True)


def rename_shader_directories() -> None:
    print("=== Renaming shader directories ===")
    base = SHADER_RENDER

    # Lighting/Distant must move out before Lighting -> lighting
    git_mv(base / "Lighting" / "Distant", base / "_lighting_distant")
    case_rename(base / "Lighting", "lighting")
    git_mv(base / "_lighting_distant", base / "lighting" / "distant")

    case_rename(base / "GPUSort", "gpuSort")
    case_rename(base / "Misc", "misc")
    case_rename(base / "NRD", "nrd")
    case_rename(base / "ProcessingPasses", "processingPasses")
    case_rename(base / "RTXDI", "rtxdi")
    case_rename(base / "ToneMapper", "toneMapper")


def iter_text_files():
    for base in SEARCH_ROOTS:
        if not base.exists():
            continue
        for p in base.rglob("*"):
            if not p.is_file():
                continue
            if p.suffix not in TEXT_SUFFIXES:
                continue
            if any(s in p.parts for s in SKIP_DIRS):
                continue
            yield p


def update_references() -> int:
    print("\n=== Updating path references ===")
    changed = 0
    for p in iter_text_files():
        try:
            text = p.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        new_text = text
        for old, new in PATH_REPLACEMENTS:
            new_text = new_text.replace(old, new)
        for old, new in HLSL_RELATIVE:
            new_text = new_text.replace(old, new)
        if new_text != text:
            p.write_text(new_text, encoding="utf-8", newline="\n")
            print(f"  updated: {p.relative_to(ROOT)}")
            changed += 1
    return changed


def main() -> int:
    rename_shader_directories()
    n = update_references()
    print(f"\nDone. Updated {n} files.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
