#pragma once

#include <core/command_line.h>
#include <engine/ISubsystem.h>
#include <render/RenderSessionState.h>
#include <render/SessionDiagnostics.h>
#include <render/WorldRenderer/PathTracingFrameExtension.h>

#include <span>
#include <string>

namespace caustica::editor
{

class SceneEditor;

struct EditorSceneSubsystemConfig
{
    SceneEditor& sceneEditor;
    caustica::render::SessionDiagnostics& diagnostics;
    std::span<caustica::render::IPathTracingFrameExtension* const> frameExtensions;
    std::string preferredScene;

    caustica::render::RenderSessionState* sessionState = nullptr;
    const CommandLineOptions* cmdLine = nullptr;

    bool refreshEnvMapMediaList = true;
    bool applyCmdLineToSessionState = true;
    bool postAppInit = true;
};

// Wires SceneEditor to RenderingSubsystem and drives scene rendering each frame.
class EditorSceneSubsystem : public caustica::ISubsystem
{
public:
    explicit EditorSceneSubsystem(EditorSceneSubsystemConfig config);

    [[nodiscard]] int priority() const override { return 200; }

    void initialize(caustica::EngineInitContext& context) override;

    void onBeginFrame(caustica::GpuDevice& gpuDevice) override;
    void onUpdate(float elapsedTimeSeconds, bool windowFocused) override;
    void onRenderScene(caustica::GpuDevice& gpuDevice) override;

    void onBackBufferResizing() override;

    [[nodiscard]] bool skipRenderPhase() const override;
    [[nodiscard]] bool shouldRenderWhenUnfocused() const override;

private:
    EditorSceneSubsystemConfig m_config;
};

} // namespace caustica::editor
