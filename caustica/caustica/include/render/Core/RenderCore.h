#pragma once

#include <render/Core/AccelStructManager.h>
#include <render/Core/CameraController.h>
#include <render/Core/RenderPipeline.h>

#include <render/Core/PostProcessAA.h>
#include <render/Core/HdrPostProcess.h>
#include <render/Core/SceneGeometryUpdate.h>
#include <render/Core/LightingUpdate.h>

#include <memory>

namespace caustica
{
class Scene;
class ShaderFactory;

// Shared render orchestrator (editor sample shell remains global ::PathTracerApp).
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

    void postProcessAA(PostProcessAAParams& params);
    void hdrPostProcess(HdrPostProcessParams& params);
    void updateSceneGeometry(UpdateSceneGeometryParams& params);
    void preUpdateLighting(PreUpdateLightingParams& params);
    void updateLighting(UpdateLightingParams& params);
    void updateLightingEnd(UpdateLightingEndParams& params);

private:
    nvrhi::IDevice* m_device = nullptr;

    AccelStructManager              m_accelStructs;
    CameraController                m_camera;
    std::unique_ptr<RenderPipeline> m_pipeline;
};

} // namespace caustica
