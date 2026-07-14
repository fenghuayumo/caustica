#!/usr/bin/env python3
"""Rename PascalCase render APIs to camelCase across first-party sources."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Generated pair list (also kept next to this script for review).
from _render_rename_pairs import RENAMES  # noqa: E402

SKIP_DIR_PARTS = {
    "third_party",
    "thirdparty",
    "External",
    "bin",
    ".git",
    "build",
    "build_release",
    "build_imguizmo",
    "out",
    "ShaderPrecompiled",
    "rhi",
    ".vs",
    "CMakeFiles",
    "rtxmu",
}

EXT = {".h", ".hpp", ".cpp", ".c", ".inl", ".cc", ".cxx"}


def should_skip(path: Path) -> bool:
    if set(path.parts) & SKIP_DIR_PARTS:
        return True
    text = str(path).replace("\\", "/")
    if "/backend/rhi/" in text or "/third_party/" in text or "/thirdparty/" in text:
        return True
    return False


def sort_key(item: tuple[str, str]) -> tuple[int, str]:
    return (-len(item[0]), item[0])


def build_patterns(renames: list[tuple[str, str]]) -> list[tuple[re.Pattern[str], str]]:
    out: list[tuple[re.Pattern[str], str]] = []
    for old, new in sorted(renames, key=sort_key):
        if old == new:
            continue
        out.append((re.compile(rf"\b{re.escape(old)}\b"), new))
    return out


def transform(text: str, patterns: list[tuple[re.Pattern[str], str]]) -> tuple[str, int]:
    total = 0
    for pat, new in patterns:
        text, n = pat.subn(new, text)
        total += n
    return text, total


def collect_files() -> list[Path]:
    roots = [
        ROOT / "caustica" / "caustica",
        ROOT / "caustica" / "Python",
        ROOT / "application",
        ROOT / "python",
        ROOT / "support",
        ROOT / "tools",
    ]
    files: list[Path] = []
    for root in roots:
        if not root.exists():
            continue
        for p in root.rglob("*"):
            if not p.is_file() or p.suffix.lower() not in EXT:
                continue
            if should_skip(p):
                continue
            # Don't rewrite the rename tooling itself if it embeds Old names as strings in pairs
            if p.name in {"_render_rename_pairs.py", "rename_render_pascal_to_camel.py",
                          "rename_scene_pascal_to_camel.py"}:
                continue
            files.append(p)
    return files


def main() -> int:
    dry = "--dry-run" in sys.argv
    patterns = build_patterns(RENAMES)
    files = collect_files()
    changed_files = 0
    total_repls = 0

    for path in files:
        raw_bytes = path.read_bytes()
        newline = b"\r\n" if b"\r\n" in raw_bytes else b"\n"
        raw = raw_bytes.decode("utf-8", errors="surrogateescape")
        raw_norm = raw.replace("\r\n", "\n").replace("\r", "\n")
        new, n = transform(raw_norm, patterns)
        if n == 0 or new == raw_norm:
            continue
        changed_files += 1
        total_repls += n
        rel_out = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
        print(f"{rel_out}: {n} replacements")
        if not dry:
            out = new.encode("utf-8", errors="surrogateescape")
            if newline == b"\r\n":
                out = out.replace(b"\n", b"\r\n")
            path.write_bytes(out)

    print(f"---\nfiles={changed_files} replacements={total_repls} dry_run={dry} patterns={len(patterns)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
