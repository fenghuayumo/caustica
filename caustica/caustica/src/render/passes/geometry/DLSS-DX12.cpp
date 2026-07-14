#if CAUSTICA_WITH_DLSS && CAUSTICA_WITH_DX12

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers.h>

#include <render/passes/geometry/DLSS.h>
#include <scene/View.h>
#include <core/log.h>

using namespace caustica::render;

static void NVSDK_CONV NgxLogCallback(const char* message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature sourceComponent)
{
    caustica::info("NGX: %s", message);
}

class DLSS_DX12 : public DLSS
{
public:
    DLSS_DX12(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
        std::string const& directoryWithExecutable, uint32_t applicationID)
        : DLSS(device, shaderFactory)
    {
        ID3D12Device* d3ddevice = device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);

        std::wstring executablePathW;
        executablePathW.assign(directoryWithExecutable.begin(), directoryWithExecutable.end());
        
        NVSDK_NGX_FeatureCommonInfo featureCommonInfo = {};
        featureCommonInfo.LoggingInfo.LoggingCallback = NgxLogCallback;
        featureCommonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
        featureCommonInfo.LoggingInfo.DisableOtherLoggingSinks = true;

        NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init(applicationID, executablePathW.c_str(), d3ddevice, &featureCommonInfo);

        if (result != NVSDK_NGX_Result_Success)
        {
            caustica::warning("Cannot initialize NGX, Result = 0x%08x (%ls)", result, GetNGXResultAsString(result));
            return;
        }

        result = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_parameters);

        if (result != NVSDK_NGX_Result_Success)
            return;

        // DLSS
        {
            int available = 0;
            result = m_parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
            m_dlssSupported = available && result == NVSDK_NGX_Result_Success;
            if (!m_dlssSupported)
            {
                result = NVSDK_NGX_Result_Fail;
                NVSDK_NGX_Parameter_GetI(m_parameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, (int*)&result);
                caustica::warning("NVIDIA DLSS is not available on this system, FeatureInitResult = 0x%08x (%ls)", result, GetNGXResultAsString(result));
            }
        }

        // DLSS RR
        {
            int available = 0;
            result = m_parameters->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
            m_rayReconstructionSupported = available && result == NVSDK_NGX_Result_Success;
            if (!m_rayReconstructionSupported)
            {
                result = NVSDK_NGX_Result_Fail;
                NVSDK_NGX_Parameter_GetI(m_parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, (int*)&result);
                caustica::warning("NVIDIA DLSSRR is not available on this system, FeatureInitResult = 0x%08x (%ls)", result, GetNGXResultAsString(result));
            }
        }
    }

    void init(const InitParameters& params) override
    {
        if (params.useRayReconstruction && !m_rayReconstructionSupported)
            return;
        else if (!m_dlssSupported)
            return;

        if (m_initParameters.inputWidth == params.inputWidth && m_initParameters.inputHeight == params.inputHeight &&
            m_initParameters.outputWidth == params.outputWidth && m_initParameters.outputHeight == params.outputHeight &&
            m_initParameters.useLinearDepth == params.useLinearDepth &&
            m_initParameters.useAutoExposure == params.useAutoExposure &&
            m_initParameters.useRayReconstruction == params.useRayReconstruction)
            return;

        if (m_dlssHandle)
        {
            m_device->waitForIdle();
            NVSDK_NGX_D3D12_ReleaseFeature(m_dlssHandle);
            m_dlssHandle = nullptr;
            m_dlssInitialized = false;
            m_rayReconstructionInitialized = false;
        }

        m_featureCommandList->open();
        ID3D12GraphicsCommandList* d3dcmdlist = m_featureCommandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);

        m_parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        m_parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        m_parameters->Set(NVSDK_NGX_Parameter_Width, params.inputWidth);
        m_parameters->Set(NVSDK_NGX_Parameter_Height, params.inputHeight);
        m_parameters->Set(NVSDK_NGX_Parameter_OutWidth, params.outputWidth);
        m_parameters->Set(NVSDK_NGX_Parameter_OutHeight, params.outputHeight);

        int inFeatureCreateFlags =
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        inFeatureCreateFlags |= params.useLinearDepth ? 0 : NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        inFeatureCreateFlags |= params.useAutoExposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0;
        m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, inFeatureCreateFlags);
        m_parameters->Set(NVSDK_NGX_Parameter_Use_HW_Depth, params.useLinearDepth ? NVSDK_NGX_DLSS_Depth_Type_Linear : NVSDK_NGX_DLSS_Depth_Type_HW);

        NVSDK_NGX_Result result;
        if (params.useRayReconstruction)
        {
            m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode, NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
            m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, NVSDK_NGX_DLSS_Roughness_Mode_Packed);

            result = NVSDK_NGX_D3D12_CreateFeature(d3dcmdlist, NVSDK_NGX_Feature_RayReconstruction, m_parameters, &m_dlssHandle);
        }
        else
        {
            result = NVSDK_NGX_D3D12_CreateFeature(d3dcmdlist, NVSDK_NGX_Feature_SuperSampling, m_parameters, &m_dlssHandle);
        }

        m_featureCommandList->close();
        m_device->executeCommandList(m_featureCommandList);

        if (result != NVSDK_NGX_Result_Success)
        {
            caustica::warning("Failed to create a DLSS feautre, Result = 0x%08x (%ls)", result, GetNGXResultAsString(result));
            return;
        }

        if (params.useRayReconstruction)
            m_rayReconstructionInitialized = true;
        else
            m_dlssInitialized = true;

        m_initParameters = params;
    }

    bool evaluate(
        nvrhi::ICommandList* commandList,
        const EvaluateParameters& params,
        const caustica::PlanarView& view) override
    {
        if (!m_dlssInitialized && !m_rayReconstructionInitialized)
            return false;

        commandList->beginMarker(m_rayReconstructionInitialized ? "DLSS_RR" : "DLSS");

        bool const useExposureBuffer = params.exposureBuffer != nullptr && params.exposureScale != 0.f && !m_rayReconstructionInitialized;
        if (useExposureBuffer)
        {
            computeExposure(commandList, params.exposureBuffer, params.exposureScale);
        }

        ID3D12GraphicsCommandList* d3dcmdlist = commandList->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList);

        commandList->setTextureState(params.inputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(params.outputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(params.depthTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(params.motionVectorsTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        if (m_rayReconstructionInitialized)
        {
            commandList->setTextureState(params.diffuseAlbedo, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(params.specularAlbedo, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(params.normalRoughness, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        }
        if (useExposureBuffer)
        {
            commandList->setTextureState(m_exposureTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        }
        commandList->commitBarriers();

        m_parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, view.getPixelOffset().x);
        m_parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, view.getPixelOffset().y);
        m_parameters->Set(NVSDK_NGX_Parameter_Reset, params.resetHistory);
        m_parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, params.motionVectorScaleX);
        m_parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, params.motionVectorScaleY);
        m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, view.getViewExtent().width());
        m_parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, view.getViewExtent().height());

        // Common buffers
        m_parameters->Set(NVSDK_NGX_Parameter_Color, (ID3D12Resource*)params.inputColorTexture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
        m_parameters->Set(NVSDK_NGX_Parameter_Output, (ID3D12Resource*)params.outputColorTexture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
        m_parameters->Set(NVSDK_NGX_Parameter_Depth, (ID3D12Resource*)params.depthTexture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
        m_parameters->Set(NVSDK_NGX_Parameter_MotionVectors, (ID3D12Resource*)params.motionVectorsTexture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
        m_parameters->Set(NVSDK_NGX_Parameter_ExposureTexture, useExposureBuffer ? (ID3D12Resource*)m_exposureTexture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource) : nullptr);

        if (m_rayReconstructionInitialized)
        {
            m_parameters->Set(NVSDK_NGX_Parameter_DiffuseAlbedo, (ID3D12Resource*)params.diffuseAlbedo->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
            m_parameters->Set(NVSDK_NGX_Parameter_SpecularAlbedo, (ID3D12Resource*)params.specularAlbedo->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
            m_parameters->Set(NVSDK_NGX_Parameter_GBuffer_Normals, (ID3D12Resource*)params.normalRoughness->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
            m_parameters->Set(NVSDK_NGX_Parameter_GBuffer_Roughness, (ID3D12Resource*)params.normalRoughness->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource));
        }

        NVSDK_NGX_Result result = NVSDK_NGX_D3D12_EvaluateFeature_C(d3dcmdlist, m_dlssHandle, m_parameters, NULL);

        commandList->clearState();

        commandList->endMarker();

        if (result != NVSDK_NGX_Result_Success)
        {
            caustica::warning("Failed to evaluate DLSS feature: 0x%08x", result);
            return false;
        }

        return true;
    }

    ~DLSS_DX12() override
    {
        if (m_dlssHandle)
        {
            NVSDK_NGX_D3D12_ReleaseFeature(m_dlssHandle);
            m_dlssHandle = nullptr;
        }

        if (m_parameters)
        {
            NVSDK_NGX_D3D12_DestroyParameters(m_parameters);
            m_parameters = nullptr;
        }

        ID3D12Device* d3ddevice = m_device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
        NVSDK_NGX_D3D12_Shutdown1(d3ddevice);
    }
};

std::unique_ptr<DLSS> DLSS::createDX12(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
    std::string const& directoryWithExecutable, uint32_t applicationID)
{
    return std::make_unique<DLSS_DX12>(device, shaderFactory, directoryWithExecutable, applicationID);
}

#endif
