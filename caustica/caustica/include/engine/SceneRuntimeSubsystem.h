#pragma once

#include <core/command_line.h>
#include <engine/ISubsystem.h>
#include <render/RenderSessionState.h>
#include <render/SessionDiagnostics.h>

#include <string>

namespace caustica
{

class SceneRuntime;

struct SceneRuntimeSubsystemConfig
{
    SceneRuntime& sceneRuntime;
    render::SessionDiagnostics& diagnostics;
    std::string preferredScene;

    render::RenderSessionState* sessionState = nullptr;
    const CommandLineOptions* cmdLine = nullptr;

    bool refreshEnvMapMediaList = true;
    bool applyCmdLineToSessionState = true;
};

// Runtime subsystem: wires SceneRuntime to GpuRenderSubsystem and drives the
// path-tracing frame loop each tick. No editor / ImGui dependencies.
class SceneRuntimeSubsystem : public ISubsystem
{
public:
    explicit SceneRuntimeSubsystem(SceneRuntimeSubsystemConfig config);

    [[nodiscard]] int priority() const override { return 200; }

    void initialize(EngineInitContext& context) override;

    void onBeginFrame(GpuDevice& gpuDevice) override;
    void onUpdate(float elapsedTimeSeconds, bool windowFocused) override;
    void onRenderScene(GpuDevice& gpuDevice) override;

    void onBackBufferResizing() override;

    [[nodiscard]] bool skipRenderPhase() const override;
    [[nodiscard]] bool shouldRenderWhenUnfocused() const override;

protected:
    virtual void onInitializePost(EngineInitContext& context);
    virtual void onBeforeBeginFrame();
    virtual void prepareSceneFrame();

    SceneRuntimeSubsystemConfig m_config;
};

} // namespace caustica
