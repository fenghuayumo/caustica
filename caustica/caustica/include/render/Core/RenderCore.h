#pragma once

#include <render/Core/AccelStructManager.h>
#include <render/Core/CameraController.h>
#include <render/Core/RenderPipeline.h>

#include <memory>

namespace caustica
{
class Scene;
class ShaderFactory;

// Shared render orchestrator owned by GpuRenderSubsystem (accel structs, camera, render pipeline).
// Scene passes wire AccelStructManager directly; WorldRenderer uses camera + accelStructs via PathTracingContext.
class RenderCore
{
public:
    explicit RenderCore(nvrhi::IDevice* device);

    void initializeRenderPipeline(std::shared_ptr<ShaderFactory> shaderFactory);

    [[nodiscard]] AccelStructManager& accelStructs() { return m_accelStructs; }
    [[nodiscard]] CameraController&   camera() { return m_camera; }
    [[nodiscard]] RenderPipeline*     pipeline() { return m_pipeline.get(); }

    [[nodiscard]] const AccelStructManager& accelStructs() const { return m_accelStructs; }
    [[nodiscard]] const CameraController&   camera() const { return m_camera; }
    [[nodiscard]] const RenderPipeline*     pipeline() const { return m_pipeline.get(); }

    void onSceneLoaded(Scene& scene, bool& accelRebuildRequested);
    void onSceneUnloading();

private:
    nvrhi::IDevice* m_device = nullptr;

    AccelStructManager              m_accelStructs;
    CameraController                m_camera;
    std::unique_ptr<RenderPipeline> m_pipeline;
};

} // namespace caustica
