#pragma once

#include <rhi/nvrhi.h>
#include <math/math.h>
#include <memory>
#include <unordered_map>
#include <vector>
#include <rtxdi/ImportanceSamplingContext.h>
#include <render/Core/ComputePass.h>
#include "RayTracingPass.h"
#include "RtxdiResources.h"
#include "RtxdiApplicationSettings.h"

#include <shaders/PathTracer/Lighting/LightingTypes.hlsli>
#include <shaders/PathTracer/Lighting/EnvMap.hlsli>
#include <render/Passes/Gaussian/GaussianSplatEmissionProxy.h>

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

#include <scene/Scene.h>

struct RtxdiUserSettings
{
    rtxdi::CheckerboardMode checkerboardMode = rtxdi::CheckerboardMode::Off;

	struct
	{
		rtxdi::ReSTIRDI_ResamplingMode resamplingMode = GetReSTIRDI_ResamplingMode();
		ReSTIRDI_InitialSamplingParameters initialSamplingParams = getReSTIRDIInitialSamplingParams();
		ReSTIRDI_TemporalResamplingParameters temporalResamplingParams = getReSTIRDITemporalResamplingParams();
		ReSTIRDI_SpatialResamplingParameters spatialResamplingParams = getReSTIRDISpatialResamplingParams();
		ReSTIRDI_ShadingParameters shadingParams = getReSTIRDIShadingParams();
	} restirDI;

	struct  
	{
		rtxdi::ReSTIRGI_ResamplingMode resamplingMode = GetReSTIRGI_ResamplingMode();
		ReSTIRGI_TemporalResamplingParameters temporalResamplingParams = getReSTIRGITemporalResamplingParams();
		ReSTIRGI_SpatialResamplingParameters spatialResamplingParams = getReSTIRGISpatialResamplingParams();
		ReSTIRGI_FinalShadingParameters finalShadingParams = getReSTIRGIFinalShadingParams();
	} restirGI;

    struct
    {
        rtxdi::ReSTIRPT_ResamplingMode resamplingMode = GetReSTIRPT_ResamplingMode();
        RTXDI_PTInitialSamplingParameters initialSamplingParams = getReSTIRPTInitialSamplingParams();
        RTXDI_PTTemporalResamplingParameters temporalResamplingParams = getReSTIRPTTemporalResamplingParams();
        RTXDI_PTReconnectionParameters reconnectionParams = getReSTIRPTReconnectionParams();
        RTXDI_PTHybridShiftPerFrameParameters hybridShiftParams = getReSTIRPTHybridShiftParams();
        RTXDI_BoilingFilterParameters boilingFilterParams = getReSTIRPTBoilingFilterParams();
        RTXDI_PTSpatialResamplingParameters spatialResamplingParams = getReSTIRPTSpatialResamplingParams();
    } restirPT;

	struct 
	{
		rtxdi::ReGIRStaticParameters regirStaticParams = {};
		rtxdi::ReGIRDynamicParameters regirDynamicParameters = getReGIRDynamicParams();
	} regir;

	struct
	{
		int numIndirectSamples = 6;
	} regirIndirect;

	float rayEpsilon = 1.0e-4f;
	bool reStirGIEnableTemporalResampling = true;
	bool reStirGIVaryAgeThreshold = true;
};

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
		nvrhi::IDevice* device,
		std::shared_ptr<caustica::ShaderFactory> shaderFactory,
		caustica::render::RenderDevice& renderDevice,
		nvrhi::BindingLayoutHandle bindlessLayout);
	~RtxdiPass();

	void Reset();
    void PrepareResources(
        nvrhi::CommandListHandle commandList,
        const RenderTargets& renderTargets,
        std::shared_ptr<EnvMapProcessor> envMap,
        EnvMapSceneParams envMapSceneParams,
        const std::shared_ptr<caustica::Scene> scene,
        std::shared_ptr<class MaterialGpuCache> materialGpuCache,
        std::shared_ptr<class OpacityMicromapBuilder> opacityMicromapBuilder,
        nvrhi::BufferHandle subInstanceDataBuffer,
        const RtxdiBridgeParameters& bridgeParams,
        const nvrhi::BindingLayoutHandle extraBindingLayout,
        std::shared_ptr<ShaderDebug> shaderDebug);
	void BeginFrame(
		nvrhi::CommandListHandle commandList,
		const RenderTargets& renderTargets,
		const nvrhi::BindingLayoutHandle extraBindingLayout,
		nvrhi::BindingSetHandle extraBindingSet);
	void execute(
		nvrhi::CommandListHandle commandList,
		nvrhi::BindingSetHandle extraBindingSet, bool skipFinal);
	void ExecuteGI(nvrhi::CommandListHandle commandList,
		nvrhi::BindingSetHandle extraBindingSet, bool skipFinal);
    void ExecuteFusedDIGIFinal(nvrhi::CommandListHandle commandList,
        nvrhi::BindingSetHandle extraBindingSet);
    void ExecutePT(nvrhi::CommandListHandle commandList,
        nvrhi::BindingSetHandle extraBindingSet);
	void EndFrame();
	
	std::shared_ptr<RtxdiResources> GetRTXDIResources() { return m_rtxdiResources; }
	nvrhi::BufferHandle GetRTXDIConstants() { return m_rtxdiConstantBuffer; }

private:
	void CheckContextStaticParameters();
	void UpdateContextDynamicParameters();
	void CreatePipelines(nvrhi::BindingLayoutHandle extraBindingLayout = nullptr, bool useRayQuery = true);
	void CreateBindingSet(const RenderTargets& renderTargets);

	std::unique_ptr<rtxdi::ImportanceSamplingContext> m_ImportanceSamplingContext;
    std::unique_ptr<rtxdi::ReSTIRPTContext> m_ReSTIRPTContext;
	std::shared_ptr<RtxdiResources> m_rtxdiResources;
	std::unique_ptr<PrepareLightsPass> m_PrepareLightsPass;
	std::unique_ptr<GenerateMipsPass> m_LocalLightPdfMipmapPass;

	nvrhi::DeviceHandle m_device; 
	std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
	caustica::render::RenderDevice& m_renderDevice;
	std::shared_ptr<caustica::Scene> m_Scene;
	nvrhi::BindingLayoutHandle m_bindingLayout;
	nvrhi::BindingLayoutHandle m_bindlessLayout;
	nvrhi::BindingSetHandle m_bindingSet;
	nvrhi::BindingSetHandle m_PrevBindingSet;
	nvrhi::BufferHandle m_rtxdiConstantBuffer;

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

	void ExecuteComputePass(
		nvrhi::CommandListHandle& commandList, 
		ComputePass& pass, 
		const char* passName, 
		dm::int3 dispatchSize, 
		nvrhi::BindingSetHandle extraBindingSet = nullptr);

	void ExecuteRayTracingPass(
		nvrhi::CommandListHandle& commandList,
		RayTracingPass& pass,
		const char* passName,
		dm::int2 dispatchSize, 
		nvrhi::IBindingSet* extraBindingSet = nullptr
	);

	caustica::ShaderMacro GetReGirMacro(const rtxdi::ReGIRStaticParameters& regirParameters);

	void FillConstants(nvrhi::CommandListHandle commandList);
	void FillSharedConstants(struct RtxdiBridgeConstants& bridgeConstants) const;
	void FillDIConstants(ReSTIRDI_Parameters& diParams);
	void FillGIConstants(ReSTIRGI_Parameters& giParams);
    void FillPTConstants(RTXDI_PTParameters& ptParams);
	void FillReGIRConstant(ReGIR_Parameters& regirParams);
	void FillReGirIndirectConstants(ReGirIndirectConstants& regirIndirectConstants);

	RtxdiBridgeParameters m_BridgeParameters;
	uint32_t m_CurrentReservoirIndex;
	uint32_t m_PreviousReservoirIndex;
};

