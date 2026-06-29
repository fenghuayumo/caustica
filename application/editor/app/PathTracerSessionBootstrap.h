#pragma once

#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>
#include <render/SessionDiagnostics.h>
#include <render/WorldRenderer/PathTracingFrameExtension.h>

#include <functional>
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
    caustica::render::SceneLightingPasses& lighting;
    caustica::render::SceneRayTracingResources& rayTracing;
    caustica::render::SceneGaussianSplatPasses& gaussianSplats;
    caustica::render::SessionDiagnostics& diagnostics;
    std::span<caustica::render::IPathTracingFrameExtension* const> frameExtensions;
    std::string preferredScene;

    // Host-specific setup after scene passes are attached (env-map list, UI from cmdline, etc.).
    std::function<void()> onAfterAttachPasses;
};

[[nodiscard]] std::unique_ptr<caustica::EngineRenderer> bootstrapPathTracerSession(
    const PathTracerSessionBootstrapParams& params);

} // namespace caustica::editor
