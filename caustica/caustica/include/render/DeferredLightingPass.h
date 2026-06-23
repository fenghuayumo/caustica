/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
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

#include <math/math.h>
#include <nvrhi/nvrhi.h>
#include <memory>
#include <unordered_map>
#include <engine/BindingCache.h>

namespace caustica
{
    class ShaderFactory;
    class Light;
    class CommonRenderPasses;
    class FramebufferFactory;
    class ICompositeView;
    class IView;
    struct LightProbe;
}

namespace caustica::render
{
    class GBufferRenderTargets;
    
    class DeferredLightingPass
    {
    private:
        nvrhi::DeviceHandle m_Device;

        nvrhi::ShaderHandle m_ComputeShader;
        nvrhi::SamplerHandle m_ShadowSampler;
        nvrhi::SamplerHandle m_ShadowSamplerComparison;
        nvrhi::BufferHandle m_DeferredLightingCB;
        nvrhi::ComputePipelineHandle m_Pso;

        nvrhi::BindingLayoutHandle m_BindingLayout;
        caustica::BindingCache m_BindingSets;

        std::shared_ptr<caustica::CommonRenderPasses> m_CommonPasses;

    protected:

        virtual nvrhi::ShaderHandle CreateComputeShader(
            caustica::ShaderFactory& shaderFactory);

    public:
        struct Inputs
        {
            nvrhi::ITexture* depth = nullptr;
            nvrhi::ITexture* gbufferNormals = nullptr;
            nvrhi::ITexture* gbufferDiffuse = nullptr;
            nvrhi::ITexture* gbufferSpecular = nullptr;
            nvrhi::ITexture* gbufferEmissive = nullptr;
            nvrhi::ITexture* indirectDiffuse = nullptr;
            nvrhi::ITexture* indirectSpecular = nullptr;
            nvrhi::ITexture* shadowChannels = nullptr;
            nvrhi::ITexture* ambientOcclusion = nullptr;
            nvrhi::ITexture* output = nullptr;

            const std::vector<std::shared_ptr<caustica::Light>>* lights = nullptr;
            const std::vector<std::shared_ptr<caustica::LightProbe>>* lightProbes = nullptr;

            dm::float3 ambientColorTop = 0.f;
            dm::float3 ambientColorBottom = 0.f;

            // Fills the GBuffer-related textures (depth, normals, etc.) from the provided structure.
            void SetGBuffer(const GBufferRenderTargets& targets);
        };

        DeferredLightingPass(
            nvrhi::IDevice* device,
            std::shared_ptr<caustica::CommonRenderPasses> commonPasses);

        virtual void Init(const std::shared_ptr<caustica::ShaderFactory>& shaderFactory);

        void Render(
            nvrhi::ICommandList* commandList,
            const caustica::ICompositeView& compositeView,
            const Inputs& inputs,
            dm::float2 randomOffset = dm::float2::zero());

        void ResetBindingCache();
    };
}
