#pragma once

#include <rhi/rhi.h>
#include <math/math.h>
#include <memory>
#include <unordered_map>
#include <vector>
#include <rtxdi/ImportanceSamplingContext.h>
#include <render/core/ComputePass.h>
#include "RayTracingPass.h"
#include "RtxdiResources.h"
#include "RtxdiApplicationSettings.h"

#include <shaders/PathTracer/Lighting/LightingTypes.hlsli>
#include <shaders/PathTracer/Lighting/EnvMap.hlsli>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>

class RenderTargets;
class PrepareLightsPass;
class GenerateMipsPass;
class EnvMapProcessor;
struct ReGirIndirectConstants;
class ShaderDebug;

namespace caustica
{
	class ShaderFactory;
	namespace render { class RenderDevice; }
	struct ShaderMacro;
	class PlanarView;
}

#include <render/passes/rtxdi/RtxdiUserSettings.h>
#include <render/SceneGpuResources.h>
#include <render/core/PathTracerSettings.h>

namespace caustica::scene { class SceneRenderData; }

struct RtxdiBridgeParameters
{
	uint32_t frameIndex;
	caustica::math::uint2 frameDims;
	caustica::math::float3 cameraPosition;

	RtxdiUserSettings userSettings;

    bool usingLightSampling;
    bool usingReGIR;

    const std::vector<GaussianSplatEmissionProxy>* gaussianSplatEmissionProxies = nullptr;
    caustica::math::float4x4 gaussianSplatEmissionObjectToWorld = caustica::math::float4x4::identity();
    float gaussianSplatEmissionIntensity = 0.0f;
};

class RtxdiPass
{
public:
	RtxdiPass(
		caustica::rhi::IDevice* device,
		std::shared_ptr<caustica::ShaderFactory> shaderFactory,
		caustica::render::RenderDevice& renderDevice,
		caustica::rhi::BindingLayoutHandle bindlessLayout);
	~RtxdiPass();

	void reset();

	// Per-frame RTXDI resource prep (formerly WorldRenderer::rtxdiSetupFrame).
	struct SetupParams
	{
		caustica::rhi::CommandListHandle commandList;
		const RenderTargets* renderTargets = nullptr;
		EnvMapProcessor* environment = nullptr;
		EnvMapSceneParams envMapSceneParams{};
		const caustica::scene::SceneRenderData* renderData = nullptr;
		caustica::rhi::IDescriptorTable* descriptorTable = nullptr;
		caustica::render::SceneGpuFrameHandles gpuHandles{};
		std::shared_ptr<class MaterialGpuCache> materials;
		std::shared_ptr<class OpacityMicromapBuilder> opacityMaps;
		caustica::rhi::BufferHandle subInstanceDataBuffer;
		caustica::rhi::BindingLayoutHandle bindingLayout;
		std::shared_ptr<ShaderDebug> shaderDebug;

		uint32_t frameIndex = 0;
		caustica::math::uint2 frameDims{};
		caustica::math::float3 cameraPosition{};
		RtxdiUserSettings userSettings{};
		bool usingLightSampling = false;
		bool usingReGIR = false;
		bool environmentMapImportanceSampling = false;
		bool resetRealtimeCaches = false;

		const std::vector<GaussianSplatEmissionProxy>* gaussianSplatEmissionProxies = nullptr;
		caustica::math::float4x4 gaussianSplatEmissionObjectToWorld = caustica::math::float4x4::identity();
		float gaussianSplatEmissionIntensity = 0.0f;
	};

	void setupFrame(const SetupParams& params);

    void prepareResources(
        caustica::rhi::CommandListHandle commandList,
        const RenderTargets& renderTargets,
        EnvMapProcessor* envMap,
        EnvMapSceneParams envMapSceneParams,
        const caustica::scene::SceneRenderData* renderData,
        size_t geometryInstanceCount,
        caustica::rhi::IDescriptorTable* descriptorTable,
        const caustica::render::SceneGpuFrameHandles& gpuHandles,
        std::shared_ptr<class MaterialGpuCache> materialGpuCache,
        std::shared_ptr<class OpacityMicromapBuilder> opacityMicromapBuilder,
        caustica::rhi::BufferHandle subInstanceDataBuffer,
        const RtxdiBridgeParameters& bridgeParams,
        const caustica::rhi::BindingLayoutHandle extraBindingLayout,
        std::shared_ptr<ShaderDebug> shaderDebug);
	void beginFrame(
		caustica::rhi::CommandListHandle commandList,
		const RenderTargets& renderTargets,
		const caustica::rhi::BindingLayoutHandle extraBindingLayout,
		caustica::rhi::BindingSetHandle extraBindingSet);

	// Fine-grained begin-frame stages (also composed by beginFrame).
	void prepareLights(caustica::rhi::CommandListHandle commandList);
	void writeBridgeConstants(caustica::rhi::CommandListHandle commandList);
	void generatePdfMips(caustica::rhi::CommandListHandle commandList);
	void presampleLights(caustica::rhi::CommandListHandle commandList, caustica::rhi::BindingSetHandle extraBindingSet);
	void presampleEnvMap(caustica::rhi::CommandListHandle commandList, caustica::rhi::BindingSetHandle extraBindingSet);
	void presampleReGIR(caustica::rhi::CommandListHandle commandList, caustica::rhi::BindingSetHandle extraBindingSet);

	void execute(
		caustica::rhi::CommandListHandle commandList,
		caustica::rhi::BindingSetHandle extraBindingSet, bool skipFinal);
	void executeGI(caustica::rhi::CommandListHandle commandList,
		caustica::rhi::BindingSetHandle extraBindingSet, bool skipFinal);
    void executeFusedDIGIFinal(caustica::rhi::CommandListHandle commandList,
        caustica::rhi::BindingSetHandle extraBindingSet);
    void executePT(caustica::rhi::CommandListHandle commandList,
        caustica::rhi::BindingSetHandle extraBindingSet);
	// Fused DIGI / GI / PT sequence for one frame. No WorldRenderer command-list swap.
	void executeFrame(
		caustica::rhi::ICommandList* commandList,
		caustica::rhi::BindingSetHandle globalBindingSet,
		const PathTracerSettings& settings);
	void endFrame();
	
	std::shared_ptr<RtxdiResources> getRTXDIResources() { return m_rtxdiResources; }
	caustica::rhi::BufferHandle getRTXDIConstants() { return m_rtxdiConstantBuffer; }

private:
	void checkContextStaticParameters();
	void updateContextDynamicParameters();
	void createPipelines(caustica::rhi::BindingLayoutHandle extraBindingLayout = nullptr, bool useRayQuery = true);
	void createBindingSet(const RenderTargets& renderTargets);

	std::unique_ptr<rtxdi::ImportanceSamplingContext> m_ImportanceSamplingContext;
    std::unique_ptr<rtxdi::ReSTIRPTContext> m_ReSTIRPTContext;
	std::shared_ptr<RtxdiResources> m_rtxdiResources;
	std::unique_ptr<PrepareLightsPass> m_PrepareLightsPass;
	std::unique_ptr<GenerateMipsPass> m_LocalLightPdfMipmapPass;

	caustica::rhi::DeviceHandle m_device;
	std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
	caustica::render::RenderDevice& m_renderDevice;
	caustica::rhi::IDescriptorTable* m_descriptorTable = nullptr;
	caustica::rhi::BindingLayoutHandle m_bindingLayout;
	caustica::rhi::BindingLayoutHandle m_bindlessLayout;
	caustica::rhi::BindingSetHandle m_bindingSet;
	caustica::rhi::BindingSetHandle m_PrevBindingSet;
	caustica::rhi::BufferHandle m_rtxdiConstantBuffer;

	ComputePass m_PresampleLightsPass;
	ComputePass m_PresampleEnvMapPass;
	ComputePass m_PresampleReGIRPass;
	ComputePass m_FinalSamplingPass;
	RayTracingPass m_GenerateInitialSamplesPass;
	RayTracingPass m_SpatialResamplingPass;
	RayTracingPass m_TemporalResamplingPass;

	RayTracingPass m_GITemporalResamplingPass;
	RayTracingPass m_GISpatialResamplingPass;
	RayTracingPass m_GIFinalShadingPass;

    RayTracingPass m_FusedDIGIFinalShadingPass;
    RayTracingPass m_PTGenerateInitialSamplesPass;
    RayTracingPass m_PTTemporalResamplingPass;
    RayTracingPass m_PTSpatialResamplingPass;
    RayTracingPass m_PTFinalShadingPass;

	void executeComputePass(
		caustica::rhi::CommandListHandle& commandList, 
		ComputePass& pass, 
		const char* passName, 
		dm::int3 dispatchSize, 
		caustica::rhi::BindingSetHandle extraBindingSet = nullptr);

	void executeRayTracingPass(
		caustica::rhi::CommandListHandle& commandList,
		RayTracingPass& pass,
		const char* passName,
		dm::int2 dispatchSize, 
		caustica::rhi::IBindingSet* extraBindingSet = nullptr
	);

	caustica::ShaderMacro getReGirMacro(const rtxdi::ReGIRStaticParameters& regirParameters);

	void fillConstants(caustica::rhi::CommandListHandle commandList);
	void fillSharedConstants(struct RtxdiBridgeConstants& bridgeConstants) const;
	void fillDIConstants(ReSTIRDI_Parameters& diParams);
	void fillGIConstants(ReSTIRGI_Parameters& giParams);
    void fillPTConstants(RTXDI_PTParameters& ptParams);
	void fillReGIRConstant(ReGIR_Parameters& regirParams);
	void fillReGirIndirectConstants(ReGirIndirectConstants& regirIndirectConstants);

	RtxdiBridgeParameters m_BridgeParameters;
	uint32_t m_CurrentReservoirIndex;
	uint32_t m_PreviousReservoirIndex;
};

