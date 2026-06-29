#pragma once

#include "PathTracerSessionBootstrap.h"

#include <core/command_line.h>
#include <render/RenderSessionState.h>
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
class Application;
}

namespace caustica::editor
{

class SceneEditor;

// Shared post-GPU wiring for EditorApplication and Python RenderSession.
struct PathTracerSessionHostParams
{
    caustica::GpuDevice& gpuDevice;
    SceneEditor& sceneEditor;
    caustica::render::SessionDiagnostics& diagnostics;
    std::span<caustica::render::IPathTracingFrameExtension* const> frameExtensions;
    std::string preferredScene;

    caustica::render::RenderSessionState* sessionState = nullptr;
    const CommandLineOptions* cmdLine = nullptr;
    caustica::Application* frameDriver = nullptr;

    bool refreshEnvMapMediaList = true;
    bool syncBackBuffer = true;
    bool applyCmdLineToSessionState = true;
    bool postAppInit = true;
};

void prepareSceneEditorGpuSession(SceneEditor& sceneEditor, caustica::GpuDevice& gpuDevice);

[[nodiscard]] std::unique_ptr<caustica::EngineRenderer> startupPathTracerSessionHost(
    const PathTracerSessionHostParams& params);

} // namespace caustica::editor
