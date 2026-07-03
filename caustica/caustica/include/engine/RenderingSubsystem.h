#pragma once

#include <engine/EngineRenderer.h>
#include <engine/ISubsystem.h>

#include <memory>

namespace caustica
{

// Owns EngineRenderer and shared GPU render infrastructure for a session.
class RenderingSubsystem : public ISubsystem
{
public:
    [[nodiscard]] int priority() const override { return 100; }

    void initialize(EngineInitContext& context) override;
    void shutdown() override;

    void onRenderEnd(GpuDevice& gpuDevice) override;

    bool initializeRenderer(GpuDevice& gpuDevice,
        std::shared_ptr<SceneTypeFactory> sceneTypeFactory,
        EngineSceneCallbacks sceneCallbacks = {});

    void createPathTracer(const PathTracerSessionParams& session);

    [[nodiscard]] EngineRenderer* renderer() { return m_renderer.get(); }
    [[nodiscard]] const EngineRenderer* renderer() const { return m_renderer.get(); }

private:
    std::unique_ptr<EngineRenderer> m_renderer;
};

} // namespace caustica
