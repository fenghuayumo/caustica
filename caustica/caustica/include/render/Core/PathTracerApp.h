#pragma once

#include <render/Core/AccelStructManager.h>
#include <render/Core/CameraController.h>
#include <render/Core/RenderPipeline.h>

#include <render/Core/PathTracerPostProcess.h>
#include <render/Core/PathTracerSceneGeometry.h>

#include <memory>

namespace caustica
{
class Scene;
class ShaderFactory;

// Engine render orchestrator (editor shell remains global ::PathTracerApp).
class PathTracerRenderCore
{
public:
    explicit PathTracerRenderCore(nvrhi::IDevice* device);

    void initializeRenderPipeline(std::shared_ptr<ShaderFactory> shaderFactory);

    [[nodiscard]] AccelStructManager& accelStructs() { return m_accelStructs; }
    [[nodiscard]] CameraController&   camera() { return m_camera; }
    [[nodiscard]] RenderPipeline*     pipeline() { return m_pipeline.get(); }

    [[nodiscard]] const AccelStructManager& accelStructs() const { return m_accelStructs; }
    [[nodiscard]] const CameraController&   camera() const { return m_camera; }
    [[nodiscard]] const RenderPipeline*     pipeline() const { return m_pipeline.get(); }

    void onSceneLoaded(Scene& scene, bool& accelRebuildRequested);
    void onSceneUnloading();

    void postProcessAA(PostProcessAAParams& params);
    void updateSceneGeometry(UpdateSceneGeometryParams& params);

private:
    nvrhi::IDevice* m_device = nullptr;

    AccelStructManager              m_accelStructs;
    CameraController                m_camera;
    std::unique_ptr<RenderPipeline> m_pipeline;
};

} // namespace caustica
