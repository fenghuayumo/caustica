/*
* Copyright (c) 2014-2024, NVIDIA CORPORATION. All rights reserved.
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

#include <engine/SceneTypes.h>
#include <render/GeometryPasses.h>
#include <memory>
#include <mutex>
#include <nvrhi/nvrhi.h>

namespace caustica
{
    class ShaderFactory;
    class CommonRenderPasses;
    class FramebufferFactory;
    class MaterialBindingCache;
    class ICompositeView;
    class IView;
}

namespace caustica::render
{
    class DepthPass : public IGeometryPass
    {
    public:
        union PipelineKey
        {
            struct
            {
                nvrhi::RasterCullMode cullMode : 2;
                bool alphaTested : 1;
                bool frontCounterClockwise : 1;
                bool reverseDepth : 1;
            } bits;
            uint32_t value;

            static constexpr size_t Count = 1 << 5;
        };

        class Context : public GeometryPassContext
        {
        public:
            nvrhi::BindingSetHandle inputBindingSet;
            PipelineKey keyTemplate;

            uint32_t positionOffset = 0;
            uint32_t texCoordOffset = 0;
            
            Context()
            {
                keyTemplate.value = 0;
            }
        };

        struct CreateParameters
        {
            std::shared_ptr<caustica::MaterialBindingCache> materialBindings;
            int depthBias = 0;
            float depthBiasClamp = 0.f;
            float slopeScaledDepthBias = 0.f;
            bool trackLiveness = true;

            // Switches between loading vertex data through the Input Assembler (true) or buffer SRVs (false).
            // Using Buffer SRVs is often faster.
            bool useInputAssembler = false;

            uint32_t numConstantBufferVersions = 16;
        };

    protected:
        nvrhi::DeviceHandle m_Device;
        nvrhi::InputLayoutHandle m_InputLayout;
        nvrhi::ShaderHandle m_VertexShader;
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::BindingLayoutHandle m_InputBindingLayout;
        nvrhi::BindingLayoutHandle m_ViewBindingLayout;
        nvrhi::BufferHandle m_DepthCB;
        nvrhi::BindingSetHandle m_ViewBindingSet;
        nvrhi::GraphicsPipelineHandle m_Pipelines[PipelineKey::Count];
        std::mutex m_Mutex;

        int m_DepthBias = 0;
        float m_DepthBiasClamp = 0.f;
        float m_SlopeScaledDepthBias = 0.f;
        bool m_IsDX11 = false;
        bool m_UseInputAssembler = false;
        bool m_TrackLiveness = true;

        std::unordered_map<const caustica::BufferGroup*, nvrhi::BindingSetHandle> m_InputBindingSets;
        
        std::shared_ptr<caustica::CommonRenderPasses> m_CommonPasses;
        std::shared_ptr<caustica::MaterialBindingCache> m_MaterialBindings;

        virtual nvrhi::ShaderHandle CreateVertexShader(caustica::ShaderFactory& shaderFactory, const CreateParameters& params);
        virtual nvrhi::ShaderHandle CreatePixelShader(caustica::ShaderFactory& shaderFactory, const CreateParameters& params);
        virtual nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params);
        virtual nvrhi::BindingLayoutHandle CreateInputBindingLayout();
        virtual nvrhi::BindingSetHandle CreateInputBindingSet(const caustica::BufferGroup* bufferGroup);
        virtual void CreateViewBindings(nvrhi::BindingLayoutHandle& layout, nvrhi::BindingSetHandle& set, const CreateParameters& params);
        virtual std::shared_ptr<caustica::MaterialBindingCache> CreateMaterialBindingCache(caustica::CommonRenderPasses& commonPasses);
        virtual nvrhi::GraphicsPipelineHandle CreateGraphicsPipeline(PipelineKey key, nvrhi::FramebufferInfo const& framebufferInfo);
        nvrhi::BindingSetHandle GetOrCreateInputBindingSet(const caustica::BufferGroup* bufferGroup);


    public:
        DepthPass(
            nvrhi::IDevice* device,
            std::shared_ptr<caustica::CommonRenderPasses> commonPasses);

        virtual void Init(
            caustica::ShaderFactory& shaderFactory,
            const CreateParameters& params);

        void ResetBindingCache();
        
        // IGeometryPass implementation

        [[nodiscard]] caustica::ViewType::Enum GetSupportedViewTypes() const override;
        void SetupView(GeometryPassContext& context, nvrhi::ICommandList* commandList, const caustica::IView* view, const caustica::IView* viewPrev) override;
        bool SetupMaterial(GeometryPassContext& context, const caustica::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state) override;
        void SetupInputBuffers(GeometryPassContext& context, const caustica::BufferGroup* buffers, nvrhi::GraphicsState& state) override;
        void SetPushConstants(GeometryPassContext& context, nvrhi::ICommandList* commandList, nvrhi::GraphicsState& state, nvrhi::DrawArguments& args) override;
    };

}