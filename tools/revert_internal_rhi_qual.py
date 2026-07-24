#!/usr/bin/env python3
"""Revert rhi:: qualification on backend-internal method signatures (concrete types)."""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DIRS = [
    REPO / "caustica/caustica/src/backend/rhi/d3d11",
    REPO / "caustica/caustica/src/backend/rhi/d3d12",
    REPO / "caustica/caustica/src/backend/rhi/vulkan",
]

# Methods that must keep concrete backend pointer types in their signatures.
INTERNAL_METHODS = {
    "CommandList",  # constructor Class::Class
    "bindFramebuffer",
    "bindGraphicsPipeline",
    "bindMeshletPipeline",
    "setComputeBindings",
    "setGraphicsBindings",
    "createSRV",
    "createUAV",
    "createRTV",
    "createDSV",
    "setRayGenerationShader",
    "addMissShader",
    "addHitGroup",
    "addCallableShader",
    "verifyExport",
    "getShaderTableState",
    "buildTopLevelAccelStructInternal",
    "mapBuffer",  # private Device helpers that take concrete Buffer*
}

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


def unqualify(args: str) -> str:
    for name in TYPES:
        args = re.sub(rf"\brhi::{name}(\s*\*)", rf"{name}\1", args)
    return args


def process(text: str) -> str:
    # Constructor: CommandList::CommandList(rhi::Device* ...
    text = re.sub(
        r"\b(CommandList|Device)::\1\s*\(([^)]*)\)",
        lambda m: f"{m.group(1)}::{m.group(1)}({unqualify(m.group(2))})",
        text,
    )

    for method in INTERNAL_METHODS:
        if method in {"CommandList", "Device"}:
            continue
        pattern = re.compile(
            rf"(\b(?:CommandList|Device|ShaderTable|Texture|Buffer|SamplerFeedbackTexture|"
            rf"GraphicsPipeline|MeshletPipeline|RayTracingPipeline)::\s*{method}\s*\()([^)]*)(\))"
        )
        text = pattern.sub(lambda m: m.group(1) + unqualify(m.group(2)) + m.group(3), text)

    return text


def main() -> None:
    changed = 0
    for d in DIRS:
        for path in d.glob("*.cpp"):
            original = path.read_text(encoding="utf-8")
            text = process(original)
            if text != original:
                path.write_text(text, encoding="utf-8", newline="\n")
                changed += 1
                print(f"updated: {path.name}")
    print(f"done: {changed}")


if __name__ == "__main__":
    main()
