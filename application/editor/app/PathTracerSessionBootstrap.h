#pragma once

#include <render/SessionDiagnostics.h>
#include <render/WorldRenderer/PathTracingFrameExtension.h>

#include <memory>
#include <span>
#include <string>

namespace caustica
{
class EngineRenderer;
class GpuDevice;
}

namespace caustica::editor
{

class SceneEditor;

// Shared wiring for EditorApplication and RenderSession: attaches engine
// infrastructure to SceneEditor and creates the path tracer session.
struct PathTracerSessionBootstrapParams
{
    caustica::GpuDevice& gpuDevice;
    SceneEditor& sceneEditor;
    caustica::render::SessionDiagnostics& diagnostics;
    std::span<caustica::render::IPathTracingFrameExtension* const> frameExtensions;
    std::string preferredScene;
};

[[nodiscard]] std::unique_ptr<caustica::EngineRenderer> bootstrapPathTracerSession(
    const PathTracerSessionBootstrapParams& params);

} // namespace caustica::editor
