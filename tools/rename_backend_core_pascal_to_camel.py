#!/usr/bin/env python3
"""Rename PascalCase backend/core/remaining APIs to camelCase (exclude rhi)."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

from _backend_core_rename_pairs import RENAMES  # noqa: E402

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
    "__pycache__",
}

EXT = {".h", ".hpp", ".cpp", ".c", ".inl", ".cc", ".cxx"}

# Never rewrite tooling source that embeds Old names as string pairs.
SKIP_FILES = {
    "_backend_core_rename_pairs.py",
    "_render_rename_pairs.py",
    "rename_backend_core_pascal_to_camel.py",
    "rename_render_pascal_to_camel.py",
    "rename_scene_pascal_to_camel.py",
}


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


# COM / DXGI / XAudio / NRD / nvrhi enum names that collide with our camelCase APIs.
_ARROW_RESTORE = [
    ("queryVideoMemoryInfo", "QueryVideoMemoryInfo"),
    ("reportLiveObjects", "ReportLiveObjects"),
    ("getCurrentBackBufferIndex", "GetCurrentBackBufferIndex"),
    # ID3D12DeviceFactory::CreateDevice only (Vulkan-Hpp uses createDevice camelCase).
]
_ENUM_RESTORE = [
    (re.compile(r"ResourceStates::present\b"), "ResourceStates::Present"),
    (re.compile(r"\bnrd::createInstance\b"), "nrd::CreateInstance"),
]
# DXGI Present — only swap-chain receivers (not GpuDevice::present).
_SWAPCHAIN_PRESENT = re.compile(
    r"\b(m_SwapChain|pSwapChain|swapChain|pSwapChain1)\s*->\s*present\s*\("
)
# D3D12 device factory CreateDevice
_FACTORY_CREATE_DEVICE = re.compile(
    r"\b(d3d12DeviceFactory|deviceFactory)\s*->\s*createDevice\s*\("
)


def restore_sdk_false_positives(text: str) -> tuple[str, int]:
    n = 0
    for old, neu in _ARROW_RESTORE:
        text, c = re.subn(rf"->{old}\s*\(", f"->{neu}(", text)
        n += c
    text, c = _SWAPCHAIN_PRESENT.subn(lambda m: f"{m.group(1)}->Present(", text)
    n += c
    text, c = _FACTORY_CREATE_DEVICE.subn(lambda m: f"{m.group(1)}->CreateDevice(", text)
    n += c
    text, c = re.subn(r"\bvoice->stop\s*\(", "voice->Stop(", text)
    n += c
    text, c = re.subn(r"\bvoice->start\s*\(", "voice->Start(", text)
    n += c
    for pat, neu in _ENUM_RESTORE:
        text, c = pat.subn(neu, text)
        n += c
    return text, n


def collect_files() -> list[Path]:
    roots = [
        ROOT / "caustica" / "caustica",
        ROOT / "caustica" / "Python",
        ROOT / "application",
        ROOT / "python",
        ROOT / "support",
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
            if p.name in SKIP_FILES:
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
        new, n_rest = restore_sdk_false_positives(new)
        n += n_rest
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
