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

#include <engine/BindingCache.h>
#include <nvrhi/nvrhi.h>
#include <memory>
#include <unordered_map>


namespace caustica
{
    class ShaderFactory;
    class ShadowMap;
    class CommonRenderPasses;
    class FramebufferFactory;
    class ICompositeView;
}

namespace caustica::render
{
    class BloomPass
    {
    private:
        std::shared_ptr<caustica::CommonRenderPasses> m_CommonPasses;
        std::shared_ptr<caustica::FramebufferFactory> m_FramebufferFactory;

        nvrhi::DeviceHandle m_Device;

        struct PerViewData
        {
            nvrhi::GraphicsPipelineHandle bloomBlurPso;

            nvrhi::TextureHandle textureDownscale1;
            nvrhi::FramebufferHandle framebufferDownscale1;
            nvrhi::TextureHandle textureDownscale2;
            nvrhi::FramebufferHandle framebufferDownscale2;

            nvrhi::TextureHandle texturePass1Blur;
            nvrhi::FramebufferHandle framebufferPass1Blur;
            nvrhi::TextureHandle texturePass2Blur;
            nvrhi::FramebufferHandle framebufferPass2Blur;

            nvrhi::BindingSetHandle bloomBlurBindingSetPass1;
            nvrhi::BindingSetHandle bloomBlurBindingSetPass2;
            nvrhi::BindingSetHandle bloomBlurBindingSetPass3;
            nvrhi::BindingSetHandle blitFromDownscale1BindingSet;
            nvrhi::BindingSetHandle compositeBlitBindingSet;
        };

        std::vector<PerViewData> m_PerViewData;
        nvrhi::BufferHandle m_BloomHBlurCB;
        nvrhi::BufferHandle m_BloomVBlurCB;
        nvrhi::ShaderHandle m_BloomBlurPixelShader;
        nvrhi::BindingLayoutHandle m_BloomBlurBindingLayout;
        nvrhi::BindingLayoutHandle m_BloomApplyBindingLayout;

        caustica::BindingCache m_BindingCache;

    public:
        BloomPass(
            nvrhi::IDevice* device,
            const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
            std::shared_ptr<caustica::CommonRenderPasses> commonPasses,
            std::shared_ptr<caustica::FramebufferFactory> framebufferFactory,
            const caustica::ICompositeView& compositeView);

        void Render(
            nvrhi::ICommandList* commandList,
            const std::shared_ptr<caustica::FramebufferFactory>& framebufferFactory,
            const caustica::ICompositeView& compositeView,
            nvrhi::ITexture* sourceDestTexture,
            float sigmaInPixels,
            float blendFactor);
    };
}
