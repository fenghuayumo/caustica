#!/usr/bin/env python3
"""Extract render methods from PathTracerApp.cpp into PathTracingRenderer.cpp"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "caustica/editor/app/PathTracerApp.cpp"
OUT_CPP = ROOT / "caustica/editor/render/PathTracingRenderer.cpp"

METHOD_MAP = {
    "CreateRenderPasses": "createRenderPasses",
    "PreUpdateLighting": "preUpdateLighting",
    "UpdateLighting": "updateLighting",
    "PreUpdatePathTracing": "preUpdatePathTracing",
    "PostUpdatePathTracing": "postUpdatePathTracing",
    "UpdatePathTracerConstants": "updatePathTracerConstants",
    "RtxdiSetupFrame": "rtxdiSetupFrame",
    "StreamlinePreRender": "streamlinePreRender",
    "NativeDLSSPreRender": "nativeDLSSPreRender",
    "PreRender": "preRender",
    "PostProcessPreToneMapping": "postProcessPreToneMapping",
    "PostProcessPostToneMapping": "postProcessPostToneMapping",
    "RenderGaussianSplats": "renderGaussianSplats",
    "AccumulateGaussianSplats": "accumulateGaussianSplats",
    "Render": "render",
    "RecreateBindingSet": "recreateBindingSet",
    "PathTrace": "pathTrace",
    "Denoise": "denoise",
    "EvaluateNativeDLSS": "evaluateNativeDLSS",
    "PostProcessAA": "postProcessAA",
    "ResetReferenceOIDN": "resetReferenceOIDN",
    "ApplyReferenceOIDN": "applyReferenceOIDN",
    "DenoisedScreenshot": "denoisedScreenshot",
}

text = SRC.read_text(encoding="utf-8")

pattern = re.compile(
    r"^(?P<sig>(?:void|bool|dm::float2|std::string|int|float|uint2|auto)\s+)"
    r"PathTracerApp::(?P<name>\w+)\s*\([^)]*\)\s*(?:const\s*)?\{",
    re.MULTILINE,
)

spans = {}
for m in pattern.finditer(text):
    name = m.group("name")
    if name not in METHOD_MAP:
        continue
    i = m.end() - 1
    depth = 0
    while i < len(text):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                spans[name] = (m.start(), i + 1, m.group("sig"))
                break
        i += 1

missing = [n for n in METHOD_MAP if n not in spans]
if missing:
    raise SystemExit(f"Missing methods: {missing}")

# Helper function before PreRender
helper = ""
helper_m = re.search(
    r"(SimpleViewConstants FromPlanarViewConstants\(PlanarViewConstants & view\)\s*\{.*?\n\})",
    text,
    re.DOTALL,
)
if helper_m:
    helper = helper_m.group(1) + "\n\n"

includes = '''#include "render/PathTracingRenderer.h"
#include "PathTracerApp.h"
#include "PathTracerApp.h"

#include <render/Core/PostProcessAA.h>
#include <render/Core/SceneGeometryUpdate.h>
#include <render/Core/LightingUpdate.h>
#include <render/Core/PTPipelineBaker.h>
#include <render/Core/ComputePipelineBaker.h>
#include <render/Core/BindingCache.h>
#include <render/Core/View.h>
#include <render/Core/FramebufferFactory.h>
#include <render/Core/AccelerationStructureUtil.h>
#include <render/Passes/Lighting/Distant/EnvMapImportanceSamplingBaker.h>
#include <render/Passes/Lighting/MaterialsBaker.h>
#include <render/Passes/OMM/OmmBaker.h>
#include <render/Passes/Debug/ZoomTool.h>
#include <render/Passes/PostProcess/DenoisingGuidesBaker.h>
#include <render/Passes/Denoisers/OidnDenoiser.h>
#include <render/Passes/Gaussian/GaussianSplatPass.h>
#include <render/GPUSort/GPUSort.h>
#include <backend/GpuDevice.h>
#include <core/path_utils.h>
#include <core/log.h>
#include <math/math.h>
#include <shaders/view_cb.h>
#include <rhi/utils.h>

#include "SampleCommon/SampleBaseApp.h"
#include "SampleCommon/CaptureScriptManager.h"
#include "SampleGame/GameScene.h"

#if CAUSTICA_WITH_STREAMLINE
#include <engine/StreamlineInterface.h>
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/Passes/Geometry/DLSS.h>
#endif

using namespace caustica;
using namespace caustica::math;
using namespace caustica::render;

'''

def transform_body(body: str, old_name: str, new_name: str, sig: str) -> str:
    body = body.replace(f"PathTracerApp::{old_name}", f"PathTracingRenderer::{new_name}")
    # member access via m_app
    replacements = [
        (r"\bGetDevice\(\)", "m_app.GetDevice()"),
        (r"\bGetGpuDevice\(\)", "m_app.GetGpuDevice()"),
        (r"\bGetFrameIndex\(\)", "m_app.GetFrameIndex()"),
        (r"\bGetMaterialSpecializationShader\(\)", "m_app.GetMaterialSpecializationShader()"),
        (r"\bCreateRTPipelines\(\)", "m_app.CreateRTPipelines()"),
        (r"\bFillPTPipelineGlobalMacros\(", "m_app.FillPTPipelineGlobalMacros("),
        (r"\bSampleRenderCode\(", "m_app.SampleRenderCode("),
        (r"\bOnRenderTargetsRecreated\(\)", "m_app.OnRenderTargetsRecreated()"),
        (r"\bAddCustomBindings\(", "m_app.AddCustomBindings("),
        (r"\bCollectUncompressedTextures\(\)", "m_app.CollectUncompressedTextures()"),
        (r"\bHandleDroppedFiles\(\)", "m_app.HandleDroppedFiles()"),
        (r"\bFindMaterial\(", "m_app.FindMaterial("),
        (r"\bFindNodeByInstanceIndex\(", "m_app.FindNodeByInstanceIndex("),
        (r"\bUpdateViews\(", "m_app.UpdateViews("),
        (r"\bRecreateAccelStructs\(", "m_app.RecreateAccelStructs("),
        (r"\bUploadSubInstanceData\(", "m_app.UploadSubInstanceData("),
        (r"\bGetPrimaryGaussianSplatObject\(", "m_app.GetPrimaryGaussianSplatObject("),
        (r"\bGetGaussianSplatObjectToWorld\(", "m_app.GetGaussianSplatObjectToWorld("),
    ]
    for pat, repl in replacements:
        body = re.sub(pat, repl, body)

    # m_ -> m_app.m_  (careful order - after m_app. already done)
    body = re.sub(r"(?<!m_app\.)\bm_([a-zA-Z])", r"m_app.m_\1", body)

    # Fix double m_app.m_app.
    body = body.replace("m_app.m_app.", "m_app.")

    # Fix this captures in lambdas
    body = body.replace("[this]", "[&app = m_app]")

    return body


def extract_function(name: str) -> str:
    start, end, sig = spans[name]
    chunk = text[start:end]
    new_name = METHOD_MAP[name]
    # replace signature line only at start
    chunk = re.sub(
        rf"^{re.escape(sig)}PathTracerApp::{name}",
        f"{sig}PathTracingRenderer::{new_name}",
        chunk,
        count=1,
        flags=re.MULTILINE,
    )
    return transform_body(chunk, name, new_name, sig)


parts = [includes, "PathTracingRenderer::PathTracingRenderer(PathTracerApp& app)\n    : m_app(app)\n{\n}\n\n"]

# onBackBufferResizing from BackBufferResizing body (skip SceneRender:: call)
if "BackBufferResizing" not in METHOD_MAP:
    pass

parts.append(helper)

order = list(METHOD_MAP.keys())
for name in order:
    parts.append(extract_function(name))
    parts.append("\n")

# onBackBufferResizing manual
bb_start, bb_end, bb_sig = spans.get("BackBufferResizing", (None, None, None))
if bb_start is None:
    # extract manually from file for BackBufferResizing - not in METHOD_MAP, add separately
    m = pattern.finditer(text)
    for match in m:
        if match.group("name") == "BackBufferResizing":
            i = match.end() - 1
            depth = 0
            while i < len(text):
                if text[i] == "{":
                    depth += 1
                elif text[i] == "}":
                    depth -= 1
                    if depth == 0:
                        body = text[match.start():i + 1]
                        body = body.replace("PathTracerApp::BackBufferResizing", "PathTracingRenderer::onBackBufferResizing")
                        body = re.sub(r"^\s*SceneRender::BackBufferResizing\(\);\s*\n", "", body, flags=re.MULTILINE)
                        body = transform_body(body, "BackBufferResizing", "onBackBufferResizing", "void ")
                        parts.insert(1, body + "\n\n")
                        break
                i += 1
            break

OUT_CPP.write_text("".join(parts), encoding="utf-8")
print(f"Wrote {OUT_CPP} ({OUT_CPP.stat().st_size} bytes)")
