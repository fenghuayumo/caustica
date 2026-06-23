#pragma once

#include <memory>

#include "backend/GpuDevice.h"
#include "engine/Application.h"
#include "core/log.h"
#include <platform/window.h>

#include "SampleCommon.h"
#include "CommandLine.h"

#define RTXPT_ENABLE_VIDEO_MEMORY_INFO 1

#if RTXPT_ENABLE_VIDEO_MEMORY_INFO && defined(_WIN32)
#include <dxgi1_4.h>    // IDXGIAdapter3 (DXGI 1.4)
#include <wrl/client.h> // Microsoft::WRL::ComPtr
#endif

namespace caustica
{
	class ShaderFactory;
}

class Sample;
class SampleUI;

class SampleBaseApp
{
public:
	SampleBaseApp();
	~SampleBaseApp();

	enum class InitReturnCodes
	{
		Success,
		FailProcessingCommandLine,
		FailToCreateDevice,
		FailDeviceFeatureSupport
	};

	InitReturnCodes Init(int argc, const char* const* argv);
	void End();

	void RunMainLoop();

    bool IsSERSupported() const;
    bool QueryVideoMemoryInfo(uint64_t& outBudget, uint64_t& outCurrentUsage, uint64_t& outAvailableForReservation, uint64_t& outCurrentReservation); // RTXPT_ENABLE_VIDEO_MEMORY_INFO for this to work, otherwise return false

private:
	virtual std::unique_ptr<Sample> CreateMainRenderPass(caustica::GpuDevice& deviceManager, const CommandLineOptions& cmdLineOptions) = 0;

	// Initialization methods
	void RegisterDonutCallback();
	void SampleLogCallback(caustica::Severity severity, const char* message);
	caustica::DeviceCreationParameters GetDefaultDeviceParams() const;
	bool ProcessCommandLine(int argc, char const* const* argv,
		caustica::DeviceCreationParameters& deviceParams, std::string& preferredScene);
	bool InitDeviceAndWindow(const caustica::DeviceCreationParameters& deviceParams);
	bool CheckDeviceFeatureSupport(const caustica::DeviceCreationParameters& deviceParams);
	void CreateShaderFactory();

	caustica::Callback m_DonutDefaultCallback = nullptr;
	FPSLimiter m_FPSLimiter;

	CommandLineOptions m_CmdLine;

	std::unique_ptr<caustica::GpuDevice> m_GpuDevice;
	std::unique_ptr<caustica::Window>       m_Window;
	std::unique_ptr<caustica::Application>      m_AppLoop;
	std::shared_ptr<caustica::ShaderFactory> m_ShaderFactory;
	std::unique_ptr<Sample> m_MainSceneRender; // 3d render of the scene. Where Path Tracing happens
	std::unique_ptr<SampleUI> m_UIRender;

#if RTXPT_ENABLE_VIDEO_MEMORY_INFO && defined(_WIN32)
    Microsoft::WRL::ComPtr<IDXGIAdapter3>   m_d3dAdapter;
#endif

    void * m_NVAPIValidationHandle = nullptr;
};
