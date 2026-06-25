#pragma once

#include <memory>

#include "backend/GpuDevice.h"
#include "engine/Application.h"
#include "core/log.h"
#include <platform/window.h>

#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/Core/ScopedPerfMarker.h>
#include <render/Core/TextureUtils.h>

#include <SampleUI.h>

using caustica::FPSLimiter;
constexpr static const int c_SwapchainCount = 3;

#define CAUSTICA_ENABLE_VIDEO_MEMORY_INFO 1

#if CAUSTICA_ENABLE_VIDEO_MEMORY_INFO && defined(_WIN32)
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

#include <engine/SceneManager.h>
#include <render/Core/RenderCore.h>
#include <assets/cache/TextureCache.h>
#include <render/Core/CommonRenderPasses.h>
#include <render/Core/BindingCache.h>
#include <render/Core/DescriptorTableManager.h>
#include <render/WorldRenderer/WorldRendererServices.h>

namespace caustica { class ShaderFactory; }
namespace caustica::render { class PathTracingWorldRenderer; }

#include "PathTracerApp.h"

// Desktop editor executable 
// Owns GpuDevice/Window, drives scene + UI render passes, runs the message loop.
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

	StartupResult startup(int argc, const char* const* argv);
	void shutdown() override;

	SampleUIData& GetSampleUIData() { return m_sampleUIData; }
	const SampleUIData& GetSampleUIData() const { return m_sampleUIData; }

	PathTracerApp* GetScenePass() { return m_scenePass.get(); }
	const PathTracerApp* GetScenePass() const { return m_scenePass.get(); }

	PathTracerApp* GetAdvancedPathTracer() { return m_scenePass.get(); }
	const PathTracerApp* GetAdvancedPathTracer() const { return m_scenePass.get(); }

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
	caustica::render::WorldRendererServices buildWorldRendererServices();
	void initWorldRenderer(nvrhi::IBindingLayout* bindlessLayout);
	void initSceneServices();
	void syncPassesToBackBuffer();

	caustica::Callback m_DefaultLogCallback = nullptr;
	FPSLimiter m_FPSLimiter;
	CommandLineOptions m_CmdLine;
	SampleUIData m_sampleUIData;

	std::shared_ptr<caustica::ShaderFactory> m_ShaderFactory;
	std::shared_ptr<caustica::CommonRenderPasses> m_commonPasses;
	std::unique_ptr<caustica::BindingCache> m_bindingCache;
	std::shared_ptr<caustica::DescriptorTableManager> m_descriptorTable;
	std::shared_ptr<caustica::TextureCache> m_textureCache;
	std::unique_ptr<caustica::render::WorldRendererServices> m_worldRendererServices;
	std::unique_ptr<caustica::RenderCore>      m_renderCore;
	std::unique_ptr<SceneManager>            m_sceneManager;
	std::unique_ptr<PathTracerApp>             m_scenePass;
	std::unique_ptr<caustica::render::PathTracingWorldRenderer> m_worldRenderer;
	std::unique_ptr<SampleUI> m_uiPass;

#if CAUSTICA_ENABLE_VIDEO_MEMORY_INFO && defined(_WIN32)
    Microsoft::WRL::ComPtr<IDXGIAdapter3> m_d3dAdapter;
#endif

    void* m_NVAPIValidationHandle = nullptr;
};
