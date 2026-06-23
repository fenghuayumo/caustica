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

#pragma once

#if DONUT_WITH_DLSS

#include <memory>
#include <rhi/nvrhi.h>

class RenderTargets;

namespace caustica
{
    class ShaderFactory;
    class PlanarView;
}

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace caustica::render
{

    class DLSS
    {
    public:
        struct InitParameters
        {
            uint32_t inputWidth = 0;
            uint32_t inputHeight = 0;
            uint32_t outputWidth = 0;
            uint32_t outputHeight = 0;
            bool useLinearDepth = false;
            bool useAutoExposure = false;
            bool useRayReconstruction = false;
        };

        struct EvaluateParameters
        {
            nvrhi::TextureHandle depthTexture;
            nvrhi::TextureHandle motionVectorsTexture;
            nvrhi::TextureHandle inputColorTexture;
            nvrhi::TextureHandle outputColorTexture;

            // The exposure buffer returned by ToneMappingPass::GetExposureBuffer(), optional.
            nvrhi::BufferHandle exposureBuffer;

            // DLSS RR (ray reconstruction) specific textures.
            nvrhi::TextureHandle diffuseAlbedo;
            nvrhi::TextureHandle specularAlbedo;
            nvrhi::TextureHandle normalRoughness;

            float exposureScale = 1.f;
            float sharpness = 0.f;
            float motionVectorScaleX = 1.f;
            float motionVectorScaleY = 1.f;
            bool resetHistory = false;
        };

        DLSS(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory);

        [[nodiscard]] bool IsDlssSupported() const;
        [[nodiscard]] bool IsDlssInitialized() const;

        [[nodiscard]] bool IsRayReconstructionSupported() const;
        [[nodiscard]] bool IsRayReconstructionInitialized() const;

        virtual void Init(const InitParameters& params) = 0;

        virtual bool Evaluate(
            nvrhi::ICommandList* commandList,
            const EvaluateParameters& params,
            const caustica::PlanarView& view) = 0;

        virtual ~DLSS() = default;

        static std::unique_ptr<DLSS> Create(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
            std::string const& directoryWithExecutable, uint32_t applicationID = DefaultApplicationID);

        static void GetRequiredVulkanExtensions(std::vector<std::string>& instanceExtensions, std::vector<std::string>& deviceExtensions);

        static const uint32_t DefaultApplicationID = 231313132;

    protected:
        bool m_dlssSupported = false;
        bool m_dlssInitialized = false;

        bool m_rayReconstructionSupported = false;
        bool m_rayReconstructionInitialized = false;

        NVSDK_NGX_Handle* m_dlssHandle = nullptr;
        NVSDK_NGX_Parameter* m_parameters = nullptr;

        InitParameters m_initParameters;

        nvrhi::DeviceHandle m_device;
        nvrhi::ShaderHandle m_exposureShader;
        nvrhi::ComputePipelineHandle m_exposurePipeline;
        nvrhi::TextureHandle m_exposureTexture;
        nvrhi::BufferHandle m_exposureSourceBuffer;
        nvrhi::BindingLayoutHandle m_exposureBindingLayout;
        nvrhi::BindingSetHandle m_exposureBindingSet;
        nvrhi::CommandListHandle m_featureCommandList;

        void ComputeExposure(nvrhi::ICommandList* commandList, nvrhi::IBuffer* toneMapperExposureBuffer, float exposureScale);
        
    #if DONUT_WITH_DX11
        static std::unique_ptr<DLSS> CreateDX11(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
            std::string const& directoryWithExecutable, uint32_t applicationID);
    #endif
    #if DONUT_WITH_DX12
        static std::unique_ptr<DLSS> CreateDX12(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
            std::string const& directoryWithExecutable, uint32_t applicationID);
    #endif
    #if DONUT_WITH_VULKAN
        static std::unique_ptr<DLSS> CreateVK(nvrhi::IDevice* device, caustica::ShaderFactory& shaderFactory,
            std::string const& directoryWithExecutable, uint32_t applicationID);
    #endif
    };
}

#endif
