/*
* Copyright (c) 2021-2025, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#if DONUT_WITH_DLSS && DONUT_WITH_DX11

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <render/DLSS.h>
#include <engine/View.h>
#include <core/log.h>

using namespace caustica::render;

static void NVSDK_CONV NgxLogCallback(const char* message, NVSDK_NGX_Logging_Level loggingLevel, NVSDK_NGX_Feature sourceComponent)
{
    caustica::info("NGX: %s", message);
}

class DLSS_DX11 : public DLSS
{
public:
    DLSS_DX11(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
        std::string const& directoryWithExecutable, uint32_t applicationID)
        : DLSS(device, shaderFactory)
    {
        ID3D11Device* d3ddevice = device->getNativeObject(nvrhi::ObjectTypes::D3D11_Device);

        std::wstring executablePathW;
        executablePathW.assign(directoryWithExecutable.begin(), directoryWithExecutable.end());
        
        NVSDK_NGX_FeatureCommonInfo featureCommonInfo = {};
        featureCommonInfo.LoggingInfo.LoggingCallback = NgxLogCallback;
        featureCommonInfo.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
        featureCommonInfo.LoggingInfo.DisableOtherLoggingSinks = true;

        NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init(applicationID, executablePathW.c_str(), d3ddevice, &featureCommonInfo);

        if (result != NVSDK_NGX_Result_Success)
        {
            caustica::warning("Cannot initialize NGX, Result = 0x%08x (%ls)", result, GetNGXResultAsString(result));
            return;
        }

        result = NVSDK_NGX_D3D11_GetCapabilityParameters(&m_parameters);

        if (result != NVSDK_NGX_Result_Success)
            return;

        int dlssAvailable = 0;
        result = m_parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlssAvailable);
        if (result != NVSDK_NGX_Result_Success || !dlssAvailable)
        {
            result = NVSDK_NGX_Result_Fail;
            NVSDK_NGX_Parameter_GetI(m_parameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, (int*)&result);
            caustica::warning("NVIDIA DLSS is not available on this system, FeatureInitResult = 0x%08x (%ls)",
                result, GetNGXResultAsString(result));
            return;
        }

        m_dlssSupported = true;
    }

    void Init(const InitParameters& params) override
    {
        if (!m_dlssSupported)
            return;

        if (m_initParameters.inputWidth == params.inputWidth && m_initParameters.inputHeight == params.inputHeight &&
            m_initParameters.outputWidth == params.outputWidth && m_initParameters.outputHeight == params.outputHeight &&
            m_initParameters.useLinearDepth == params.useLinearDepth &&
            m_initParameters.useAutoExposure == params.useAutoExposure)
            return;

        if (m_dlssHandle)
        {
            m_device->waitForIdle();
            NVSDK_NGX_D3D11_ReleaseFeature(m_dlssHandle);
            m_dlssHandle = nullptr;
            m_dlssInitialized = false;
        }

        ID3D11DeviceContext* d3dcontext = m_device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);

        NVSDK_NGX_DLSS_Create_Params dlssParams = {};
        dlssParams.Feature.InWidth = params.inputWidth;
        dlssParams.Feature.InHeight = params.inputHeight;
        dlssParams.Feature.InTargetWidth = params.outputWidth;
        dlssParams.Feature.InTargetHeight = params.outputHeight;
        dlssParams.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
        dlssParams.InFeatureCreateFlags =
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        dlssParams.InFeatureCreateFlags |= params.useLinearDepth ? 0 : NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        dlssParams.InFeatureCreateFlags |= params.useAutoExposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0;

        NVSDK_NGX_Result result = NGX_D3D11_CREATE_DLSS_EXT(d3dcontext, &m_dlssHandle, m_parameters, &dlssParams);

        if (result != NVSDK_NGX_Result_Success)
        {
            caustica::warning("Failed to create a DLSS feautre, Result = 0x%08x (%ls)", result, GetNGXResultAsString(result));
            return;
        }

        m_dlssInitialized = true;

        m_initParameters = params;
    }
    
    bool Evaluate(
        nvrhi::ICommandList* commandList,
        const EvaluateParameters& params,
        const caustica::PlanarView& view) override
    {
        if (!m_dlssInitialized)
            return false;

        commandList->beginMarker("DLSS");

        bool const useExposureBuffer = params.exposureBuffer != nullptr && params.exposureScale != 0.f;

        if (useExposureBuffer)
        {
            ComputeExposure(commandList, params.exposureBuffer, params.exposureScale);
        }

        ID3D11DeviceContext* d3dcontext = commandList->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);

        commandList->setTextureState(params.inputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(params.outputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(params.depthTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(params.motionVectorsTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        if (useExposureBuffer)
        {
            commandList->setTextureState(m_exposureTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        }
        commandList->commitBarriers();

        NVSDK_NGX_D3D11_DLSS_Eval_Params evalParams = {};
        evalParams.Feature.pInColor = params.inputColorTexture->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource);
        evalParams.Feature.pInOutput = params.outputColorTexture->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource);
        evalParams.Feature.InSharpness = params.sharpness;
        evalParams.pInDepth = params.depthTexture->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource);
        evalParams.pInMotionVectors = params.motionVectorsTexture->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource);
        evalParams.pInExposureTexture = useExposureBuffer ? m_exposureTexture->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource) : nullptr;
        evalParams.InReset = params.resetHistory;
        evalParams.InMVScaleX = params.motionVectorScaleX;
        evalParams.InMVScaleY = params.motionVectorScaleY;
        evalParams.InJitterOffsetX = view.GetPixelOffset().x;
        evalParams.InJitterOffsetY = view.GetPixelOffset().y;
        evalParams.InRenderSubrectDimensions.Width = view.GetViewExtent().width();
        evalParams.InRenderSubrectDimensions.Height = view.GetViewExtent().height();

        NVSDK_NGX_Result result = NGX_D3D11_EVALUATE_DLSS_EXT(d3dcontext, m_dlssHandle, m_parameters, &evalParams);

        commandList->clearState();

        commandList->endMarker();

        if (result != NVSDK_NGX_Result_Success)
        {
            caustica::warning("Failed to evaluate DLSS feature: 0x%08x", result);
            return false;
        }

        return true;
    }

    ~DLSS_DX11() override
    {
        if (m_dlssHandle)
        {
            NVSDK_NGX_D3D11_ReleaseFeature(m_dlssHandle);
            m_dlssHandle = nullptr;
        }

        if (m_parameters)
        {
            NVSDK_NGX_D3D11_DestroyParameters(m_parameters);
            m_parameters = nullptr;
        }

        ID3D11Device* d3ddevice = m_device->getNativeObject(nvrhi::ObjectTypes::D3D11_Device);
        NVSDK_NGX_D3D11_Shutdown1(d3ddevice);
    }
};

std::unique_ptr<DLSS> DLSS::CreateDX11(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
    std::string const& directoryWithExecutable, uint32_t applicationID)
{
    return std::make_unique<DLSS_DX11>(device, shaderFactory, directoryWithExecutable, applicationID);
}

#endif
