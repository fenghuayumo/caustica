#if CAUSTICA_WITH_DLSS && CAUSTICA_WITH_VULKAN

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers_vk.h>

#include <render/passes/geometry/DLSS.h>
#include <scene/View.h>
#include <core/log.h>
#include <rhi/vulkan.h>

using namespace caustica::render;

static void NVSDK_CONV NgxLogCallback(const char* message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature sourceComponent)
{
    caustica::info("NGX: %s", message);
}

class DLSS_VK : public DLSS
{
public:
    DLSS_VK(caustica::rhi::Device* device, caustica::ShaderFactory& shaderFactory,
        std::string const& directoryWithExecutable, uint32_t applicationID)
        : DLSS(device, shaderFactory)
    {
        VkInstance vkInstance = device->getNativeObject(caustica::rhi::ObjectTypes::VK_Instance);
        VkPhysicalDevice vkPhysicalDevice = device->getNativeObject(caustica::rhi::ObjectTypes::VK_PhysicalDevice);
        VkDevice vkDevice= device->getNativeObject(caustica::rhi::ObjectTypes::VK_Device);

        std::wstring executablePathW;
        executablePathW.assign(directoryWithExecutable.begin(), directoryWithExecutable.end());

        NVSDK_NGX_FeatureCommonInfo featureCommonInfo = {};
        featureCommonInfo.LoggingInfo.LoggingCallback = NgxLogCallback;
        featureCommonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
        featureCommonInfo.LoggingInfo.DisableOtherLoggingSinks = true;

        NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init(applicationID,
            executablePathW.c_str(), vkInstance, vkPhysicalDevice, vkDevice, nullptr, nullptr, &featureCommonInfo);

        if (result != NVSDK_NGX_Result_Success)
        {
            caustica::warning("Cannot initialize NGX, Result = 0x%08x (%ls)", result, GetNGXResultAsString(result));
            return;
        }
        
        result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&m_parameters);

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
            NVSDK_NGX_VULKAN_ReleaseFeature(m_dlssHandle);
            m_dlssHandle = nullptr;
            m_dlssInitialized = false;
            m_rayReconstructionInitialized = false;
        }

        m_featureCommandList->open();
        VkCommandBuffer vkCmdBuf = m_featureCommandList->getNativeObject(caustica::rhi::ObjectTypes::VK_CommandBuffer);

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

            result = NVSDK_NGX_VULKAN_CreateFeature(vkCmdBuf, NVSDK_NGX_Feature_RayReconstruction, m_parameters, &m_dlssHandle);
        }
        else
        {
            result = NVSDK_NGX_VULKAN_CreateFeature(vkCmdBuf, NVSDK_NGX_Feature_SuperSampling, m_parameters, &m_dlssHandle);
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

    static void FillTextureResource(NVSDK_NGX_Resource_VK& resource, caustica::rhi::Texture* texture)
    {
        const caustica::rhi::TextureDesc& desc = texture->getDesc();
        resource.ReadWrite = desc.isUAV;
        resource.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;

        auto& viewInfo = resource.Resource.ImageViewInfo;
        viewInfo.Image = texture->getNativeObject(caustica::rhi::ObjectTypes::VK_Image);
        viewInfo.ImageView = texture->getNativeView(caustica::rhi::ObjectTypes::VK_ImageView);
        viewInfo.Format = VkFormat(caustica::rhi::vulkan::convertFormat(desc.format));
        viewInfo.Width = desc.width;
        viewInfo.Height = desc.height;
        viewInfo.SubresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.SubresourceRange.baseArrayLayer = 0;
        viewInfo.SubresourceRange.layerCount = 1;
        viewInfo.SubresourceRange.baseMipLevel = 0;
        viewInfo.SubresourceRange.levelCount = 1;
    }

    bool evaluate(
        caustica::rhi::CommandList* commandList,
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

        VkCommandBuffer vkCmdBuf = commandList->getNativeObject(caustica::rhi::ObjectTypes::VK_CommandBuffer);

        NVSDK_NGX_Resource_VK inColorResource;
        NVSDK_NGX_Resource_VK outColorResource;
        NVSDK_NGX_Resource_VK depthResource;
        NVSDK_NGX_Resource_VK motionVectorResource;
        NVSDK_NGX_Resource_VK exposureResource;
        NVSDK_NGX_Resource_VK diffuseAlbedoResource;
        NVSDK_NGX_Resource_VK specularAlbedoResource;
        NVSDK_NGX_Resource_VK normalRougnessResource;
        FillTextureResource(inColorResource, params.inputColorTexture);
        FillTextureResource(outColorResource, params.outputColorTexture);
        FillTextureResource(depthResource, params.depthTexture);
        FillTextureResource(motionVectorResource, params.motionVectorsTexture);
        if (m_rayReconstructionInitialized)
        {
            FillTextureResource(diffuseAlbedoResource, params.diffuseAlbedo);
            FillTextureResource(specularAlbedoResource, params.specularAlbedo);
            FillTextureResource(normalRougnessResource, params.normalRoughness);
        }
        if (useExposureBuffer)
        {
            FillTextureResource(exposureResource, m_exposureTexture);
        }

        commandList->setTextureState(params.inputColorTexture, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::ShaderResource);
        commandList->setTextureState(params.outputColorTexture, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(params.depthTexture, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::ShaderResource);
        commandList->setTextureState(params.motionVectorsTexture, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::ShaderResource);
        if (m_rayReconstructionInitialized)
        {
            commandList->setTextureState(params.diffuseAlbedo, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::ShaderResource);
            commandList->setTextureState(params.specularAlbedo, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::ShaderResource);
            commandList->setTextureState(params.normalRoughness, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::ShaderResource);
        }
        if (useExposureBuffer)
        {
            commandList->setTextureState(m_exposureTexture, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::ShaderResource);
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
        m_parameters->Set(NVSDK_NGX_Parameter_Color, &inColorResource);
        m_parameters->Set(NVSDK_NGX_Parameter_Output, &outColorResource);
        m_parameters->Set(NVSDK_NGX_Parameter_Depth, &depthResource);
        m_parameters->Set(NVSDK_NGX_Parameter_MotionVectors, &motionVectorResource);
        m_parameters->Set(NVSDK_NGX_Parameter_ExposureTexture, useExposureBuffer ? &exposureResource : nullptr);

        if (m_rayReconstructionInitialized)
        {
            m_parameters->Set(NVSDK_NGX_Parameter_DiffuseAlbedo, &diffuseAlbedoResource);
            m_parameters->Set(NVSDK_NGX_Parameter_SpecularAlbedo, &specularAlbedoResource);
            m_parameters->Set(NVSDK_NGX_Parameter_GBuffer_Normals, &normalRougnessResource);
            m_parameters->Set(NVSDK_NGX_Parameter_GBuffer_Roughness, &normalRougnessResource);
        }

        NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_EvaluateFeature_C(vkCmdBuf, m_dlssHandle, m_parameters, NULL);

        commandList->clearState();

        commandList->endMarker();

        if (result != NVSDK_NGX_Result_Success)
        {
            caustica::warning("Failed to evaluate DLSS feature: 0x%08x", result);
            return false;
        }

        return true;
    }

    ~DLSS_VK() override
    {
        if (m_dlssHandle)
        {
            NVSDK_NGX_VULKAN_ReleaseFeature(m_dlssHandle);
            m_dlssHandle = nullptr;
        }

        if (m_parameters)
        {
            NVSDK_NGX_VULKAN_DestroyParameters(m_parameters);
            m_parameters = nullptr;
        }

        VkDevice vkDevice = m_device->getNativeObject(caustica::rhi::ObjectTypes::VK_Device);
        NVSDK_NGX_VULKAN_Shutdown1(vkDevice);
    }
};

std::unique_ptr<DLSS> DLSS::createVK(caustica::rhi::Device* device, caustica::ShaderFactory& shaderFactory,
    std::string const& directoryWithExecutable, uint32_t applicationID)
{
    return std::make_unique<DLSS_VK>(device, shaderFactory, directoryWithExecutable, applicationID);
}

void DLSS::getRequiredVulkanExtensions(std::vector<std::string>& instanceExtensions, std::vector<std::string>& deviceExtensions)
{
    unsigned int instanceExtCount = 0;
    unsigned int deviceExtCount = 0;
    const char** pInstanceExtensions = nullptr;
    const char** pDeviceExtensions = nullptr;
    NVSDK_NGX_VULKAN_RequiredExtensions(&instanceExtCount, &pInstanceExtensions, &deviceExtCount, &pDeviceExtensions);

    for (unsigned int i = 0; i < instanceExtCount; i++)
    {
        instanceExtensions.push_back(pInstanceExtensions[i]);
    }

    for (unsigned int i = 0; i < deviceExtCount; i++)
    {
        // VK_EXT_buffer_device_address is incompatible with Vulkan 1.2 and causes a validation error
        if (!strcmp(pDeviceExtensions[i], VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
            continue;

        deviceExtensions.push_back(pDeviceExtensions[i]);
    }
}

#endif
