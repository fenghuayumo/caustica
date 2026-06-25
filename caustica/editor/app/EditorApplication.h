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

namespace caustica { class ShaderFactory; }

class PathTracerApp;
class SampleUI;

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

    bool IsSERSupported() const;
    bool QueryVideoMemoryInfo(uint64_t& outBudget, uint64_t& outCurrentUsage, uint64_t& outAvailableForReservation, uint64_t& outCurrentReservation);

protected:
	void onUpdate(float elapsedTimeSeconds, bool windowFocused) override;
	void onRender() override;
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
	void syncPassesToBackBuffer();

	caustica::Callback m_DefaultLogCallback = nullptr;
	FPSLimiter m_FPSLimiter;
	CommandLineOptions m_CmdLine;
	SampleUIData m_sampleUIData;

	std::shared_ptr<caustica::ShaderFactory> m_ShaderFactory;
	std::unique_ptr<PathTracerApp> m_scenePass;
	std::unique_ptr<SampleUI> m_uiPass;

#if CAUSTICA_ENABLE_VIDEO_MEMORY_INFO && defined(_WIN32)
    Microsoft::WRL::ComPtr<IDXGIAdapter3> m_d3dAdapter;
#endif

    void* m_NVAPIValidationHandle = nullptr;
};
