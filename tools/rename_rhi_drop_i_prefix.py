#!/usr/bin/env python3
"""Drop COM-style I-prefix from caustica::rhi interface types."""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
# Sources live under caustica/caustica/... ; also scan sibling Python/docs.
SCAN_ROOTS = [
    REPO / "caustica" / "caustica",
    REPO / "caustica" / "Python",
    REPO / "docs",
]
SKIP_PARTS = {"External", "bin", "build", ".git", "out"}

# Longest-first to avoid partial replacements.
RENAMES: list[tuple[str, str]] = [
    ("ISamplerFeedbackTexture", "SamplerFeedbackTexture"),
    ("IStagingTexture", "StagingTexture"),
    ("IShaderLibrary", "ShaderLibrary"),
    ("IGraphicsPipeline", "GraphicsPipeline"),
    ("IComputePipeline", "ComputePipeline"),
    ("IMeshletPipeline", "MeshletPipeline"),
    ("IDescriptorTable", "DescriptorTable"),
    ("IDescriptorHeap", "DescriptorHeap"),
    ("IBindingLayout", "BindingLayout"),
    ("IBindingSet", "BindingSet"),
    ("IOpacityMicromap", "OpacityMicromap"),
    ("IInputLayout", "InputLayout"),
    ("IFramebuffer", "Framebuffer"),
    ("IRootSignature", "RootSignature"),
    ("ICommandList", "CommandList"),
    ("IMessageCallback", "MessageCallback"),
    ("IEventQuery", "EventQuery"),
    ("ITimerQuery", "TimerQuery"),
    ("IAccelStruct", "AccelStruct"),
    ("IShaderTable", "ShaderTable"),
    ("IPipeline", "Pipeline"),
    ("ITexture", "Texture"),
    ("IBuffer", "Buffer"),
    ("IShader", "Shader"),
    ("ISampler", "Sampler"),
    ("IDevice", "Device"),
    ("IHeap", "Heap"),
    ("IResource", "Resource"),
]

# Backend concrete classes that share names with public interfaces.
BACKEND_REFCOUNTER_FIXES = [
    "SamplerFeedbackTexture",
    "StagingTexture",
    "ShaderLibrary",
    "GraphicsPipeline",
    "ComputePipeline",
    "MeshletPipeline",
    "DescriptorTable",
    "BindingLayout",
    "BindingSet",
    "OpacityMicromap",
    "InputLayout",
    "Framebuffer",
    "RootSignature",
    "CommandList",
    "EventQuery",
    "TimerQuery",
    "AccelStruct",
    "ShaderTable",
    "Pipeline",
    "Texture",
    "Buffer",
    "Shader",
    "Sampler",
    "Device",
    "Heap",
]


def should_process(path: Path) -> bool:
    if path.suffix.lower() not in {".h", ".hpp", ".cpp", ".c", ".cc", ".inl", ".md"}:
        return False
    return not any(part in SKIP_PARTS for part in path.parts)


def apply_renames(text: str) -> str:
    for old, new in RENAMES:
        text = re.sub(rf"\b{old}\b", new, text)
    return text


def fix_backend_refcounters(text: str) -> str:
    # Fully-qualified extended interfaces → public rhi base.
    text = text.replace(
        "RefCounter<caustica::rhi::d3d12::CommandList>",
        "RefCounter<rhi::CommandList>",
    )
    text = text.replace(
        "RefCounter<caustica::rhi::vulkan::Device>",
        "RefCounter<rhi::Device>",
    )
    text = text.replace(
        "RefCounter<caustica::rhi::d3d12::Device>",
        "RefCounter<rhi::Device>",
    )

    # RootSignature is d3d12-only; no rhi::RootSignature interface after de-COM.
    text = re.sub(
        r"\bRefCounter<(?:rhi::)?RootSignature>",
        "RefCounter<rhi::Resource>",
        text,
    )

    for name in BACKEND_REFCOUNTER_FIXES:
        if name == "RootSignature":
            continue
        text = re.sub(
            rf"\bRefCounter<{name}>",
            f"RefCounter<rhi::{name}>",
            text,
        )
    return text


def patch_d3d12_header(text: str) -> str:
    """Remove d3d12 extended Device/CommandList interfaces; keep Desc + createDevice."""
    # After rename, d3d12.h still has abstract Device/CommandList extending rhi::*.
    # Replace them with notes + handle aliases; extras live on concrete backend types.

    text = re.sub(
        r"class RootSignature : public Resource\s*\{\s*\};\s*"
        r"typedef RefCountPtr<RootSignature> RootSignatureHandle;",
        "class RootSignature;\n"
        "    typedef RefCountPtr<RootSignature> RootSignatureHandle;",
        text,
        count=1,
        flags=re.S,
    )

    text = re.sub(
        r"class CommandList : public caustica::rhi::CommandList\s*"
        r"\{.*?\};\s*"
        r"typedef RefCountPtr<CommandList> CommandListHandle;",
        "// D3D12-specific CommandList methods live on the concrete backend type\n"
        "    // (caustica::rhi::d3d12::CommandList). Use checked_cast when needed.\n"
        "    typedef caustica::rhi::CommandListHandle CommandListHandle;",
        text,
        count=1,
        flags=re.S,
    )

    text = re.sub(
        r"class Device : public caustica::rhi::Device\s*"
        r"\{.*?\};\s*"
        r"typedef RefCountPtr<Device> DeviceHandle;",
        "// D3D12-specific Device methods live on the concrete backend type\n"
        "    // (caustica::rhi::d3d12::Device). Use checked_cast when needed.\n"
        "    typedef caustica::rhi::DeviceHandle DeviceHandle;",
        text,
        count=1,
        flags=re.S,
    )
    return text


def patch_vulkan_header(text: str) -> str:
    text = re.sub(
        r"class Device : public caustica::rhi::Device\s*"
        r"\{.*?\};\s*"
        r"typedef RefCountPtr<Device> DeviceHandle;",
        "// Vulkan-specific Device methods live on the concrete backend type\n"
        "    // (caustica::rhi::vulkan::Device). Use checked_cast when needed.\n"
        "    typedef caustica::rhi::DeviceHandle DeviceHandle;",
        text,
        count=1,
        flags=re.S,
    )
    return text


def strip_override_on_backend_extras(text: str, kind: str) -> str:
    """After folding extended interfaces, d3d12/vulkan extras are no longer overrides."""
    if kind == "d3d12":
        methods = [
            "allocateUploadBuffer",
            "commitDescriptorHeaps",
            "getBufferGpuVA",
            "updateGraphicsVolatileBuffers",
            "updateComputeVolatileBuffers",
            "buildRootSignature",
            "createHandleForNativeGraphicsPipeline",
            "createHandleForNativeMeshletPipeline",
            "getDescriptorHeap",
        ]
    elif kind == "vulkan":
        methods = [
            "getQueueSemaphore",
            "queueWaitForSemaphore",
            "queueSignalSemaphore",
            "queueGetCompletedInstance",
        ]
    else:
        return text

    for method in methods:
        # bool foo(...) override;  /  Type foo(...) override
        text = re.sub(
            rf"(\b{method}\s*\([^\)]*\)[^\{{;]*?)\s+override\b",
            r"\1",
            text,
        )
    return text


def main() -> int:
    files: list[Path] = []
    for root in SCAN_ROOTS:
        if not root.exists():
            continue
        files.extend(p for p in root.rglob("*") if p.is_file() and should_process(p))

    changed = 0
    for path in files:
        original = path.read_text(encoding="utf-8", errors="surrogateescape")
        text = apply_renames(original)

        rel = path.as_posix()
        if "/rhi/d3d12/" in rel or rel.endswith("d3d12-backend.h"):
            text = fix_backend_refcounters(text)
            text = strip_override_on_backend_extras(text, "d3d12")
        elif "/rhi/d3d11/" in rel or rel.endswith("d3d11-backend.h"):
            text = fix_backend_refcounters(text)
        elif "/rhi/vulkan/" in rel or rel.endswith("vulkan-backend.h"):
            text = fix_backend_refcounters(text)
            text = strip_override_on_backend_extras(text, "vulkan")

        if rel.endswith("/backend/rhi/d3d12.h"):
            text = patch_d3d12_header(text)
        if rel.endswith("/backend/rhi/vulkan.h"):
            text = patch_vulkan_header(text)

        if text != original:
            path.write_text(text, encoding="utf-8", newline="\n")
            changed += 1
            print(f"updated: {path.relative_to(REPO).as_posix()}")

    print(f"done: {changed}/{len(files)} files changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
