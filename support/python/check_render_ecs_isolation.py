#!/usr/bin/env python3
"""Fail when render implementation code reaches into the live ECS world."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


FORBIDDEN = (
    ("SceneEntityWorld type", re.compile(r"\bSceneEntityWorld\b")),
    ("Scene::getEntityWorld", re.compile(r"\bgetEntityWorld\s*\(")),
    ("direct ecs::World access", re.compile(r"(?:\.|->)\s*world\s*\(")),
    ("logic ECS refresh", re.compile(r"\brefreshEntityWorldForFrame\s*\(")),
    ("logic-only GPU extract", re.compile(r"\bextractRenderDataForGpuSetup\s*\(")),
    ("render snapshot publish", re.compile(r"\bextractAndPublishRenderSnapshot\s*\(")),
    ("live scene mesh tracker", re.compile(r"\bgetMeshes\s*\(")),
    ("live scene material tracker", re.compile(r"\bgetMaterials\s*\(")),
    ("live scene geometry count", re.compile(r"\bgetGeometryCount\s*\(")),
    ("logic-only scene unload", re.compile(r"\bprepareForUnload\s*\(")),
    ("ECS component type", re.compile(r"\b(?:scene::)?[A-Z][A-Za-z0-9_]*Component\b")),
)

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}


COMMENTS_AND_LITERALS = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    flags=re.DOTALL,
)


def strip_comments_and_literals(source: str) -> str:
    """Blank C/C++ comments and ordinary literals while preserving positions."""
    def blank(match: re.Match[str]) -> str:
        return "".join("\n" if character == "\n" else " " for character in match.group(0))

    return COMMENTS_AND_LITERALS.sub(blank, source)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--render-root",
        required=True,
        action="append",
        type=pathlib.Path,
        help="Render source/include root to scan; may be repeated",
    )
    args = parser.parse_args()

    violations: list[str] = []
    for render_root_arg in args.render_root:
        render_root = render_root_arg.resolve()
        if not render_root.is_dir():
            print(
                f"error: render directory does not exist: {render_root}",
                file=sys.stderr,
            )
            return 2

        for path in sorted(render_root.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue

            original = path.read_text(encoding="utf-8", errors="replace")
            source = strip_comments_and_literals(original)
            original_lines = original.splitlines()
            relative = path.relative_to(render_root)
            for description, pattern in FORBIDDEN:
                for match in pattern.finditer(source):
                    line_number = source.count("\n", 0, match.start()) + 1
                    line = original_lines[line_number - 1].strip()
                    violations.append(
                        f"{render_root.name}/{relative}:{line_number}: "
                        f"{description}: {line}"
                    )

    if violations:
        print(
            "Render/ECS isolation check failed. Move logic/ECS code out of src/render "
            "and publish immutable render data during Extract:",
            file=sys.stderr,
        )
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1

    print("Render/ECS isolation check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
