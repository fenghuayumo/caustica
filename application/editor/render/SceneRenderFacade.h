#pragma once

#include <render/WorldRenderer/WorldRendererServices.h>

#include <filesystem>
#include <memory>

#include "render/SceneGaussianSplatPasses.h"
#include "render/SceneLightingPasses.h"
#include "render/SceneRayTracingResources.h"

namespace caustica
{
class BindingCache;
class CommonRenderPasses;
class DescriptorTableManager;
class GpuDevice;
class RenderCore;
class ShaderFactory;
class TextureLoader;
} // namespace caustica

class SceneManager;

namespace caustica::render
{
class PathTracingWorldRenderer;
}

namespace caustica::editor
{

class EditorUIState;
class SceneEditor;

// Inputs shared by WorldRendererServices construction (editor-owned runtime state).
struct SceneRenderFacadeServicesParams
{
    caustica::GpuDevice& gpuDevice;
    SceneManager& sceneManager;
    caustica::RenderCore& renderCore;
    PathTracerSettings& settings;
    std::shared_ptr<caustica::ShaderFactory>& shaderFactory;
    std::shared_ptr<caustica::CommonRenderPasses>& commonPasses;
    caustica::BindingCache& bindingCache;
    std::shared_ptr<caustica::TextureLoader>& textureCache;
    std::shared_ptr<caustica::DescriptorTableManager>& descriptorTable;
    SceneEditor& editor;
};

// Orchestrates lighting, RT, and gaussian-splat render slices for the editor.
class SceneRenderFacade
{
public:
    [[nodiscard]] bool isWorldRendererAttached() const
    {
        return m_rayTracing.isAttached() && m_gaussianSplats.isAttached();
    }

    SceneLightingPasses& lightingPasses() { return m_lighting; }
    const SceneLightingPasses& lightingPasses() const { return m_lighting; }
    SceneRayTracingResources& rayTracingResources() { return m_rayTracing; }
    const SceneRayTracingResources& rayTracingResources() const { return m_rayTracing; }
    SceneGaussianSplatPasses& gaussianSplatPasses() { return m_gaussianSplats; }
    const SceneGaussianSplatPasses& gaussianSplatPasses() const { return m_gaussianSplats; }

    void attachSceneEditor(SceneEditor& editor);
    void refreshEnvironmentMapMediaList(const std::filesystem::path& assetsFolder,
        const std::filesystem::path& currentScenePath);

    void initWorldRenderer(
        caustica::GpuDevice& gpuDevice,
        SceneManager& sceneManager,
        caustica::RenderCore& renderCore,
        caustica::render::PathTracingWorldRenderer& worldRenderer,
        PathTracerSettings& settings,
        EditorUIState& editor,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
        const std::shared_ptr<caustica::CommonRenderPasses>& commonPasses,
        caustica::BindingCache& bindingCache);

    caustica::render::WorldRendererPipelineHooks buildHooks(SceneEditor& editor);
    caustica::render::WorldRendererServices buildWorldRendererServices(const SceneRenderFacadeServicesParams& params);

private:
    SceneLightingPasses m_lighting;
    SceneRayTracingResources m_rayTracing;
    SceneGaussianSplatPasses m_gaussianSplats;
};

} // namespace caustica::editor
