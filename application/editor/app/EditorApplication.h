#pragma once

#include <memory>
#include <vector>

#include "engine/Application.h"
#include "core/log.h"
#include <core/Timer.h>
#include <core/command_line.h>

#include "ui/SampleUIData.h"
#include "SceneEditor.h"
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>
#include <render/SessionDiagnostics.h>
#include "SceneEditorFrameExtension.h"
#include <render/WorldRenderer/PathTracingFrameExtension.h>

using caustica::FPSLimiter;
constexpr static const int c_SwapchainCount = 3;

#define CAUSTICA_ENABLE_VIDEO_MEMORY_INFO 1

#if CAUSTICA_ENABLE_VIDEO_MEMORY_INFO && defined(_WIN32)
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

// Forward declarations — full definitions only needed in .cpp
namespace caustica::editor {
class EditorUI;
}
class SceneManager;
namespace caustica {
    class EngineRenderer;
    class GpuDevice;
    class RenderCore;
    class Window;
    struct DeviceCreationParameters;
    namespace render {
        class PathTracingWorldRenderer;
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
    caustica::render::SceneLightingPasses& GetLightingPasses() { return m_lightingPasses; }
    const caustica::render::SceneLightingPasses& GetLightingPasses() const { return m_lightingPasses; }
    caustica::render::SceneRayTracingResources& GetRayTracingResources() { return m_rayTracingResources; }
    const caustica::render::SceneRayTracingResources& GetRayTracingResources() const { return m_rayTracingResources; }
    caustica::render::SceneGaussianSplatPasses& GetGaussianSplatPasses() { return m_gaussianSplatPasses; }
    const caustica::render::SceneGaussianSplatPasses& GetGaussianSplatPasses() const { return m_gaussianSplatPasses; }

    SceneManager* GetSceneManager();
    const SceneManager* GetSceneManager() const;

    caustica::RenderCore* GetRenderCore();
    const caustica::RenderCore* GetRenderCore() const;

    caustica::render::PathTracingWorldRenderer* GetWorldRenderer();
    const caustica::render::PathTracingWorldRenderer* GetWorldRenderer() const;

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
    void syncPassesToBackBuffer();

    CommandLineOptions CmdLine;
    SampleUIData sampleUIData;
    caustica::render::SessionDiagnostics m_sessionDiagnostics;
    caustica::render::SceneLightingPasses m_lightingPasses;
    caustica::render::SceneRayTracingResources m_rayTracingResources;
    caustica::render::SceneGaussianSplatPasses m_gaussianSplatPasses;
    SceneEditor m_sceneEditor;
    SceneEditorFrameExtension m_sceneEditorFrameExtension{ m_sceneEditor };
    std::vector<caustica::render::IPathTracingFrameExtension*> m_frameExtensions{ &m_sceneEditorFrameExtension };

    caustica::Callback m_DefaultLogCallback = nullptr;
    FPSLimiter m_FPSLimiter;

    std::unique_ptr<caustica::EngineRenderer> m_engineRenderer;
    std::unique_ptr<EditorUI> m_uiPass;

#if CAUSTICA_ENABLE_VIDEO_MEMORY_INFO && defined(_WIN32)
    Microsoft::WRL::ComPtr<IDXGIAdapter3> m_d3dAdapter;
#endif

    void* m_NVAPIValidationHandle = nullptr;
};

} // namespace caustica::editor
