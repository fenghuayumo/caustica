#pragma once

#include <memory>

#include "engine/Application.h"
#include "core/log.h"
#include <core/Timer.h>
#include <core/command_line.h>

#include "ui/SampleUIData.h"
#include "SceneEditor.h"
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>
#include <render/WorldRenderer/WorldRendererServices.h>

using caustica::FPSLimiter;
constexpr static const int c_SwapchainCount = 3;

#define CAUSTICA_ENABLE_VIDEO_MEMORY_INFO 1

#if CAUSTICA_ENABLE_VIDEO_MEMORY_INFO && defined(_WIN32)
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

// Forward declarations — full definitions only needed in .cpp
namespace caustica::editor {
class SampleUI;
}
class SceneManager;
namespace caustica {
    class GpuDevice;
    class ShaderFactory;
    class BindingCache;
    class CommonRenderPasses;
    class DescriptorTableManager;
    class BindlessTable;
    class RenderCore;
    class TextureLoader;
    class Window;
    struct DeviceCreationParameters;
    namespace render {
        class PathTracingWorldRenderer;
        struct WorldRendererServices;
    }
}

namespace caustica::editor {

// Desktop editor — owns Application lifecycle and composes SceneEditor.
class EditorApplication : public caustica::Application
{
public:
    EditorApplication();
    ~EditorApplication() override;

    enum class StartupResult
    {
        Success,
        FailProcessingCommandLine,
        FailToCreateDevice,
        FailDeviceFeatureSupport
    };

    bool init(int argc, const char* const* argv) override;
    void shutdown() override;

    StartupResult startup(int argc, const char* const* argv);

    SampleUIData& GetSampleUIData() { return sampleUIData; }
    const SampleUIData& GetSampleUIData() const { return sampleUIData; }

    SceneEditor& GetSceneEditor() { return m_sceneEditor; }
    const SceneEditor& GetSceneEditor() const { return m_sceneEditor; }
    SceneLightingPasses& GetLightingPasses() { return m_lightingPasses; }
    const SceneLightingPasses& GetLightingPasses() const { return m_lightingPasses; }
    SceneRayTracingResources& GetRayTracingResources() { return m_rayTracingResources; }
    const SceneRayTracingResources& GetRayTracingResources() const { return m_rayTracingResources; }
    SceneGaussianSplatPasses& GetGaussianSplatPasses() { return m_gaussianSplatPasses; }
    const SceneGaussianSplatPasses& GetGaussianSplatPasses() const { return m_gaussianSplatPasses; }

    SceneEditor* GetScenePass() { return &m_sceneEditor; }
    const SceneEditor* GetScenePass() const { return &m_sceneEditor; }
    SceneEditor* GetAdvancedPathTracer() { return &m_sceneEditor; }
    const SceneEditor* GetAdvancedPathTracer() const { return &m_sceneEditor; }

    SceneManager* GetSceneManager() { return m_sceneManager.get(); }
    const SceneManager* GetSceneManager() const { return m_sceneManager.get(); }

    caustica::RenderCore* GetRenderCore() { return m_renderCore.get(); }
    const caustica::RenderCore* GetRenderCore() const { return m_renderCore.get(); }

    caustica::render::PathTracingWorldRenderer* GetWorldRenderer() { return m_worldRenderer.get(); }
    const caustica::render::PathTracingWorldRenderer* GetWorldRenderer() const { return m_worldRenderer.get(); }

    bool IsSERSupported() const;
    bool QueryVideoMemoryInfo(uint64_t& outBudget, uint64_t& outCurrentUsage, uint64_t& outAvailableForReservation, uint64_t& outCurrentReservation);

protected:
    void onUpdate(float elapsedTimeSeconds, bool windowFocused) override;
    void onRender() override;
    void onEvent(caustica::Event& event) override;
    void onBackBufferResizing() override;
    void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;
    void onDisplayScaleChanged(float scaleX, float scaleY) override;
    bool shouldRenderWhenUnfocused() const override;

private:
    void RegisterLogCallback();
    void SampleLogCallback(caustica::Severity severity, const char* message);
    caustica::DeviceCreationParameters GetDefaultDeviceParams() const;
    bool ProcessCommandLine(int argc, char const* const* argv,
        caustica::DeviceCreationParameters& deviceParams, std::string& preferredScene);
    bool InitDeviceAndWindow(const caustica::DeviceCreationParameters& deviceParams);
    bool CheckDeviceFeatureSupport(const caustica::DeviceCreationParameters& deviceParams);
    void CreateShaderFactory();
    void initRenderInfrastructurePhase1();
    void initRenderInfrastructurePhase2(nvrhi::IBindingLayout* bindlessLayout);
    caustica::render::WorldRendererPipelineHooks buildWorldRendererHooks();
    caustica::render::WorldRendererServices buildWorldRendererServices();
    void initWorldRenderer(nvrhi::IBindingLayout* bindlessLayout);
    void initSceneServices();
    void syncPassesToBackBuffer();

    CommandLineOptions CmdLine;
    SampleUIData sampleUIData;
    SceneLightingPasses m_lightingPasses;
    SceneRayTracingResources m_rayTracingResources;
    SceneGaussianSplatPasses m_gaussianSplatPasses;
    SceneEditor m_sceneEditor;

    caustica::Callback m_DefaultLogCallback = nullptr;
    FPSLimiter m_FPSLimiter;

    std::shared_ptr<caustica::ShaderFactory> m_ShaderFactory;
    std::shared_ptr<caustica::CommonRenderPasses> m_commonPasses;
    std::unique_ptr<caustica::BindingCache> m_bindingCache;
    std::shared_ptr<caustica::DescriptorTableManager> m_descriptorTable;
    std::unique_ptr<caustica::BindlessTable> m_bindlessTable;
    std::shared_ptr<caustica::TextureLoader> m_textureCache;
    std::unique_ptr<caustica::render::WorldRendererServices> m_worldRendererServices;
    std::unique_ptr<caustica::RenderCore>      m_renderCore;
    std::unique_ptr<SceneManager>              m_sceneManager;
    std::unique_ptr<caustica::render::PathTracingWorldRenderer> m_worldRenderer;
    std::unique_ptr<SampleUI> m_uiPass;

#if CAUSTICA_ENABLE_VIDEO_MEMORY_INFO && defined(_WIN32)
    Microsoft::WRL::ComPtr<IDXGIAdapter3> m_d3dAdapter;
#endif

    void* m_NVAPIValidationHandle = nullptr;
};

} // namespace caustica::editor
