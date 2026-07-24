#!/usr/bin/env python3
"""Qualify interface pointer types in backend overrides as rhi::Type*."""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BACKEND_DIRS = [
    REPO / "caustica/caustica/src/backend/rhi/d3d11",
    REPO / "caustica/caustica/src/backend/rhi/d3d12",
    REPO / "caustica/caustica/src/backend/rhi/vulkan",
]

# Types that exist both as rhi:: interfaces and backend concrete classes.
TYPES = [
    "SamplerFeedbackTexture",
    "StagingTexture",
    "ShaderLibrary",
    "GraphicsPipeline",
    "ComputePipeline",
    "MeshletPipeline",
    "DescriptorTable",
    "BindingLayout",
    "BindingSet",
    "InputLayout",
    "Framebuffer",
    "CommandList",
    "EventQuery",
    "TimerQuery",
    "Texture",
    "Buffer",
    "Shader",
    "Sampler",
    "Device",
    "Heap",
]

# Method names that implement public rhi::Device / rhi::CommandList APIs
# (definitions in .cpp lack the override keyword).
API_METHOD_PATTERN = re.compile(
    r"\b(?:Device|CommandList|Texture|Buffer|Shader|ShaderLibrary|Sampler|"
    r"StagingTexture|SamplerFeedbackTexture|Framebuffer|BindingSet|"
    r"DescriptorTable|GraphicsPipeline|ComputePipeline|MeshletPipeline|"
    r"AccelStruct|OpacityMicromap|RayTracingPipeline|ShaderTable|"
    r"StaticDescriptorHeap|RootSignature|Heap|InputLayout|"
    r"EventQuery|TimerQuery|BindlessLayout)::\w+"
)


def qualify_params(arglist: str) -> str:
    for name in TYPES:
        # Foo* / const Foo* / Foo * but not rhi::Foo / rt::Foo / d3d12::Foo / ID3D12Foo
        arglist = re.sub(
            rf"(?<![:\w])(const\s+)?{name}(\s*\*)",
            rf"\1rhi::{name}\2",
            arglist,
        )
    return arglist


def process_header(text: str) -> str:
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    for line in lines:
        if "override" in line and "(" in line:
            m = re.match(r"^(\s*[^\(]+\()(.*)(\)[^\)]*)$", line.rstrip("\n\r"))
            if m:
                prefix, args, suffix = m.group(1), m.group(2), m.group(3)
                # Keep newline style
                nl = "\n" if line.endswith("\n") else ""
                if line.endswith("\r\n"):
                    nl = "\r\n"
                line = prefix + qualify_params(args) + suffix + nl
        out.append(line)
    return "".join(out)


def process_cpp(text: str) -> str:
    # Qualify parameter lists of backend API method definitions.
    def repl(match: re.Match[str]) -> str:
        full = match.group(0)
        # Only touch caustica rhi method defs: Type Class::method(args)
        return full[: match.start(1) - match.start(0)] + qualify_params(match.group(1)) + full[match.end(1) - match.start(0) :]

    # Match: ReturnType Class::method(args)  [optional qualifiers] {
    pattern = re.compile(
        r"(?m)^[ \t]*[\w:<>\s\*&]+?\b(?:Device|CommandList)::\w+\s*\(([^;{}]*)\)\s*(?:const\s*)?(?:override\s*)?\{"
    )

    def repl_block(m: re.Match[str]) -> str:
        args = m.group(1)
        new_args = qualify_params(args)
        if new_args == args:
            return m.group(0)
        return m.group(0).replace(f"({args})", f"({new_args})", 1)

    text = pattern.sub(repl_block, text)

    # Also Device/CommandList defs that are multi-line or end with `;` not handled —
    # handle common form ending before `{` on same or we already did `{`.
    # Declarations in cpp are rare.

    # Method definitions where body `{` is on next line
    pattern2 = re.compile(
        r"(?m)^([ \t]*[\w:<>\s\*&]+?\b(?:Device|CommandList)::\w+\s*\()([^;{}]*)(\)\s*(?:const\s*)?\n\s*\{)"
    )

    def repl2(m: re.Match[str]) -> str:
        return m.group(1) + qualify_params(m.group(2)) + m.group(3)

    text = pattern2.sub(repl2, text)
    return text


def main() -> None:
    changed = 0
    for d in BACKEND_DIRS:
        for path in list(d.glob("*.h")) + list(d.glob("*.cpp")):
            original = path.read_text(encoding="utf-8")
            if path.suffix == ".h":
                text = process_header(original)
            else:
                text = process_cpp(original)
            if text != original:
                path.write_text(text, encoding="utf-8", newline="\n")
                changed += 1
                print(f"updated: {path.relative_to(REPO).as_posix()}")
    print(f"done: {changed} files")


if __name__ == "__main__":
    main()
