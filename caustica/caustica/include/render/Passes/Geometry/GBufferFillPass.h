#pragma once

#include <scene/View.h>
#include <scene/SceneTypes.h>
#include <render/Core/GeometryPasses.h>
#include <memory>
#include <mutex>

namespace caustica::render
{
class RenderDevice;
}

namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
    class MaterialBindingCache;
    struct Material;
}

namespace caustica::render
{
    class GBufferFillPass : public IGeometryPass
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
            uint32_t prevPositionOffset = 0;
            uint32_t texCoordOffset = 0;
            uint32_t normalOffset = 0;
            uint32_t tangentOffset = 0;

            Context()
            {
                keyTemplate.value = 0;
            }
        };

        struct CreateParameters
        {
            std::shared_ptr<caustica::MaterialBindingCache> materialBindings;
            bool enableSinglePassCubemap = false;
            bool enableDepthWrite = true;
            bool enableMotionVectors = false;
            bool trackLiveness = true;

            // Switches between loading vertex data through the Input Assembler (true) or buffer SRVs (false).
            // Using Buffer SRVs is often faster.
            bool useInputAssembler = false;

            uint32_t stencilWriteMask = 0;
            uint32_t numConstantBufferVersions = 16;
        };

    protected:
        nvrhi::DeviceHandle m_device;
        nvrhi::InputLayoutHandle m_InputLayout;
        nvrhi::ShaderHandle m_VertexShader;
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::ShaderHandle m_PixelShaderAlphaTested;
        nvrhi::ShaderHandle m_GeometryShader;
        nvrhi::BindingLayoutHandle m_InputBindingLayout;
        nvrhi::BindingLayoutHandle m_ViewBindingLayout;
        nvrhi::BindingSetHandle m_ViewBindings;
        nvrhi::BufferHandle m_GBufferCB;
        caustica::ViewType::Enum m_SupportedViewTypes = caustica::ViewType::PLANAR;
        nvrhi::GraphicsPipelineHandle m_Pipelines[PipelineKey::Count];
        std::mutex m_mutex;

        std::unordered_map<const caustica::BufferGroup*, nvrhi::BindingSetHandle> m_InputBindingSets;

        caustica::render::RenderDevice* m_renderDevice = nullptr;
        std::shared_ptr<caustica::MaterialBindingCache> m_MaterialBindings;

        bool m_EnableDepthWrite = true;
        bool m_EnableMotionVectors = false;
        bool m_IsDX11 = false;
        bool m_UseInputAssembler = false;
        uint32_t m_StencilWriteMask = 0;
        
        virtual nvrhi::ShaderHandle CreateVertexShader(caustica::ShaderFactory& shaderFactory, const CreateParameters& params);
        virtual nvrhi::ShaderHandle CreateGeometryShader(caustica::ShaderFactory& shaderFactory, const CreateParameters& params);
        virtual nvrhi::ShaderHandle CreatePixelShader(caustica::ShaderFactory& shaderFactory, const CreateParameters& params, bool alphaTested);
        virtual nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params);
        virtual nvrhi::BindingLayoutHandle CreateInputBindingLayout();
        virtual nvrhi::BindingSetHandle CreateInputBindingSet(const caustica::BufferGroup* bufferGroup);
        virtual void CreateViewBindings(nvrhi::BindingLayoutHandle& layout, nvrhi::BindingSetHandle& set, const CreateParameters& params);
        virtual std::shared_ptr<caustica::MaterialBindingCache> CreateMaterialBindingCache(caustica::render::RenderDevice& renderDevice);
        virtual nvrhi::GraphicsPipelineHandle CreateGraphicsPipeline(PipelineKey key, nvrhi::FramebufferInfo const& framebufferInfo);
        nvrhi::BindingSetHandle GetOrCreateInputBindingSet(const caustica::BufferGroup* bufferGroup);
        
    public:
        GBufferFillPass(nvrhi::IDevice* device, caustica::render::RenderDevice& renderDevice);

        virtual void Init(
            caustica::ShaderFactory& shaderFactory,
            const CreateParameters& params);

        void ResetBindingCache();
        
        // IGeometryPass implementation

        [[nodiscard]] caustica::ViewType::Enum getSupportedViewTypes() const override;
        void setupView(GeometryPassContext& context, nvrhi::ICommandList* commandList, const caustica::IView* view, const caustica::IView* viewPrev) override;
        bool setupMaterial(GeometryPassContext& context, const caustica::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state) override;
        void setupInputBuffers(GeometryPassContext& context, const caustica::BufferGroup* buffers, nvrhi::GraphicsState& state) override;
        void setPushConstants(GeometryPassContext& context, nvrhi::ICommandList* commandList, nvrhi::GraphicsState& state, nvrhi::DrawArguments& args) override;
    };

    class MaterialIDPass : public GBufferFillPass
    {
    protected:
        nvrhi::ShaderHandle CreatePixelShader(caustica::ShaderFactory& shaderFactory, const CreateParameters& params, bool alphaTested) override;

    public:
        using GBufferFillPass::GBufferFillPass;

        void Init(
            caustica::ShaderFactory& shaderFactory,
            const CreateParameters& params) override;
    };
}