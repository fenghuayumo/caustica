#!/usr/bin/env python3
"""Replace extracted PathTracerApp render methods with delegating stubs."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "caustica/editor/app/PathTracerApp.cpp"
text = SRC.read_text(encoding="utf-8")

METHODS = [
    "BackBufferResizing",
    "CreateRenderPasses",
    "PreUpdateLighting",
    "UpdateLighting",
    "PreUpdatePathTracing",
    "PostUpdatePathTracing",
    "UpdatePathTracerConstants",
    "RtxdiSetupFrame",
    "StreamlinePreRender",
    "NativeDLSSPreRender",
    "PreRender",
    "PostProcessPreToneMapping",
    "PostProcessPostToneMapping",
    "RenderGaussianSplats",
    "AccumulateGaussianSplats",
    "Render",
    "RecreateBindingSet",
    "PathTrace",
    "Denoise",
    "EvaluateNativeDLSS",
    "PostProcessAA",
    "ResetReferenceOIDN",
    "ApplyReferenceOIDN",
    "DenoisedScreenshot",
]

STUBS = {
    "BackBufferResizing": """void PathTracerApp::BackBufferResizing()
{
    SceneRender::BackBufferResizing();
    m_pathTracingRenderer->onBackBufferResizing();
}
""",
    "Render": """void PathTracerApp::Render(nvrhi::IFramebuffer* framebuffer)
{
    if (m_sceneManager->getScene() == nullptr)
    {
        assert(false);
        return;
    }
    m_progressLoading.Stop();
    m_asyncLoadingInProgress = false;
    HandleDroppedFiles();
    m_pathTracingRenderer->render(framebuffer);
}
""",
    "PreRender": """void PathTracerApp::PreRender()
{
    m_pathTracingRenderer->preRender();
}
""",
    "StreamlinePreRender": """void PathTracerApp::StreamlinePreRender()
{
#if CAUSTICA_WITH_STREAMLINE
    m_pathTracingRenderer->streamlinePreRender();
#endif
}
""",
    "NativeDLSSPreRender": """void PathTracerApp::NativeDLSSPreRender()
{
#if CAUSTICA_WITH_NATIVE_DLSS
    m_pathTracingRenderer->nativeDLSSPreRender();
#endif
}
""",
    "PathTrace": """void PathTracerApp::PathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants& constants)
{
    m_pathTracingRenderer->pathTrace(framebuffer, constants);
}
""",
    "Denoise": """void PathTracerApp::Denoise(nvrhi::IFramebuffer* framebuffer)
{
    m_pathTracingRenderer->denoise(framebuffer);
}
""",
    "PostProcessAA": """void PathTracerApp::PostProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset)
{
    m_pathTracingRenderer->postProcessAA(framebuffer, reset);
}
""",
    "RecreateBindingSet": """void PathTracerApp::RecreateBindingSet()
{
    m_pathTracingRenderer->recreateBindingSet();
}
""",
}

# Remove helper FromPlanarViewConstants - moved to renderer
text = re.sub(
    r"SimpleViewConstants FromPlanarViewConstants\(PlanarViewConstants & view\)\s*\{.*?\n\}\n\n",
    "",
    text,
    count=1,
    flags=re.DOTALL,
)

pattern = re.compile(
    r"^(?P<ret>(?:void|bool|dm::float2|std::string|int|float|uint2|auto)\s+)"
    r"PathTracerApp::(?P<name>\w+)\s*\([^)]*\)\s*(?:const\s*)?\{",
    re.MULTILINE,
)

# Collect spans in reverse order for safe deletion
spans = []
for m in pattern.finditer(text):
    name = m.group("name")
    if name not in METHODS:
        continue
    i = m.end() - 1
    depth = 0
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                spans.append((m.start(), i + 1, name))
                break
        i += 1

for start, end, name in reversed(spans):
    if name in STUBS:
        replacement = STUBS[name]
    else:
        # remove entirely - only used from renderer now
        replacement = ""
    text = text[:start] + replacement + text[end:]

SRC.write_text(text, encoding="utf-8")
print(f"Stripped {len(spans)} methods from PathTracerApp.cpp")
