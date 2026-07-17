#pragma once

#include <filesystem>
#include <memory>

#include <render/AppDiagnostics.h>
#include <render/RenderRuntimeState.h>
#include <render/core/AccelStructManager.h>
#include <render/core/CameraController.h>
#include <render/core/PathTracerSettings.h>
#include <render/PathTracerScenePasses.h>
#include <render/worldRenderer/PathTracingContext.h>

namespace caustica
{

class Scene;

namespace render
{
class WorldRenderer;
}

class GpuDevice;
struct RenderInfra;

// Owns path-tracing GPU runtime: scene passes, accel structs, render-thread camera,
// PathTracingContext, and WorldRenderer. Logic camera stays on SessionCamera;
// session Scene is bound on load (not via SceneManager); shared caches on RenderInfra.
struct PathTracingRuntime
{
    PathTracingRuntime();
    ~PathTracingRuntime();

    PathTracingRuntime(const PathTracingRuntime&) = delete;
    PathTracingRuntime& operator=(const PathTracingRuntime&) = delete;
    PathTracingRuntime(PathTracingRuntime&&) = delete;
    PathTracingRuntime& operator=(PathTracingRuntime&&) = delete;

    struct CreateParams
    {
        GpuDevice& gpuDevice;
        RenderInfra& renderInfra;
        ::PathTracerSettings& settings;
        render::RenderRuntimeState& runtimeState;
        render::AppDiagnostics& diagnostics;
        double& sceneTime;
    };

    bool create(const CreateParams& params);
    void destroy();

    void bindSessionScene(std::shared_ptr<Scene> scene, std::filesystem::path scenePath);
    void clearSessionScene();

    [[nodiscard]] render::WorldRenderer* worldRenderer() const { return m_worldRenderer.get(); }
    [[nodiscard]] CameraController& renderCamera() { return m_renderCamera; }
    [[nodiscard]] const CameraController& renderCamera() const { return m_renderCamera; }
    [[nodiscard]] AccelStructManager& accelStructs() { return m_accelStructs; }
    [[nodiscard]] const AccelStructManager& accelStructs() const { return m_accelStructs; }

    [[nodiscard]] render::SceneLightingPasses& lightingPasses() { return m_scenePasses.lighting; }
    [[nodiscard]] const render::SceneLightingPasses& lightingPasses() const { return m_scenePasses.lighting; }
    [[nodiscard]] render::SceneRayTracingResources& rayTracingResources() { return m_scenePasses.rayTracing; }
    [[nodiscard]] const render::SceneRayTracingResources& rayTracingResources() const { return m_scenePasses.rayTracing; }
    [[nodiscard]] render::SceneGaussianSplatPasses& gaussianSplatPasses() { return m_scenePasses.gaussianSplats; }
    [[nodiscard]] const render::SceneGaussianSplatPasses& gaussianSplatPasses() const { return m_scenePasses.gaussianSplats; }

    render::PathTracerScenePasses m_scenePasses;
    CameraController m_renderCamera;
    AccelStructManager m_accelStructs;
    std::unique_ptr<render::PathTracingContext> m_pathTracingContext;
    std::unique_ptr<render::WorldRenderer> m_worldRenderer;
};

} // namespace caustica
