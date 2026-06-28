#pragma once

#include <scene/View.h>
#include <scene/SceneTypes.h>
#include <render/Core/GeometryPasses.h>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace caustica
{
    class ShaderFactory;
    class Light;
    class CommonRenderPasses;
    class FramebufferFactory;
    class MaterialBindingCache;
    struct Material;
    struct LightProbe;
}

namespace caustica::render
{
    struct ForwardShadingPassPipelineKey
    {
        caustica::MaterialDomain domain = caustica::MaterialDomain::Opaque;
        nvrhi::RasterCullMode cullMode = nvrhi::RasterCullMode::Back;
        bool frontCounterClockwise = false;
        bool reverseDepth = false;
        nvrhi::VariableRateShadingState shadingRateState{};

        bool operator==(const ForwardShadingPassPipelineKey& other) const
        {
            return domain == other.domain &&
                    cullMode == other.cullMode &&
                    frontCounterClockwise == other.frontCounterClockwise &&
                    reverseDepth == other.reverseDepth &&
                    shadingRateState == other.shadingRateState;
        }

        bool operator!=(const ForwardShadingPassPipelineKey& other) const
        {
            return !(*this == other);
        }
    };
}

namespace std
{
    template<>
    struct hash<std::pair<nvrhi::ITexture*, nvrhi::ITexture*>>
    {
        size_t operator()(const std::pair<nvrhi::ITexture*, nvrhi::ITexture*>& v) const noexcept
        {
            auto h = hash<nvrhi::ITexture*>();
            return h(v.first) ^ (h(v.second) << 8);
        }
    };

    template<> struct hash<caustica::render::ForwardShadingPassPipelineKey>
    {
        std::size_t operator()(caustica::render::ForwardShadingPassPipelineKey const& key) const noexcept
        {
            size_t hash = 0;
            nvrhi::hash_combine(hash, key.domain);
            nvrhi::hash_combine(hash, key.cullMode);
            nvrhi::hash_combine(hash, key.frontCounterClockwise);
            nvrhi::hash_combine(hash, key.reverseDepth);
            nvrhi::hash_combine(hash, key.shadingRateState);
            return hash;
        }
    };
}

namespace caustica::render
{
    class ForwardShadingPass : public IGeometryPass
    {
    public:

        class Context : public GeometryPassContext
        {
        public:
            nvrhi::BindingSetHandle shadingBindingSet;
            nvrhi::BindingSetHandle inputBindingSet;
            ForwardShadingPassPipelineKey keyTemplate;

            uint32_t positionOffset = 0;
            uint32_t texCoordOffset = 0;
            uint32_t normalOffset = 0;
            uint32_t tangentOffset = 0;
        };

        struct CreateParameters
        {
            std::shared_ptr<caustica::MaterialBindingCache> materialBindings;
            bool singlePassCubemap = false;
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
        nvrhi::ShaderHandle m_PixelShaderTransmissive;
        nvrhi::ShaderHandle m_GeometryShader;
        nvrhi::SamplerHandle m_ShadowSampler;
        nvrhi::BindingLayoutHandle m_ViewBindingLayout;
        nvrhi::BindingSetHandle m_ViewBindingSet;
        nvrhi::BindingLayoutHandle m_ShadingBindingLayout;
        nvrhi::BindingLayoutHandle m_InputBindingLayout;
        caustica::ViewType::Enum m_SupportedViewTypes = caustica::ViewType::PLANAR;
        nvrhi::BufferHandle m_ForwardViewCB;
        nvrhi::BufferHandle m_ForwardLightCB;
        bool m_TrackLiveness = true;
        bool m_IsDX11 = false;
        bool m_UseInputAssembler = false;
        std::mutex m_Mutex;

        std::unordered_map<ForwardShadingPassPipelineKey, nvrhi::GraphicsPipelineHandle> m_Pipelines;
        std::unordered_map<std::pair<nvrhi::ITexture*, nvrhi::ITexture*>, nvrhi::BindingSetHandle> m_ShadingBindingSets;
        std::unordered_map<const caustica::BufferGroup*, nvrhi::BindingSetHandle> m_InputBindingSets;
        
        std::shared_ptr<caustica::CommonRenderPasses> m_CommonPasses;
        std::shared_ptr<caustica::MaterialBindingCache> m_MaterialBindings;
        
        virtual nvrhi::ShaderHandle CreateVertexShader(caustica::ShaderFactory& shaderFactory, const CreateParameters& params);
        virtual nvrhi::ShaderHandle CreateGeometryShader(caustica::ShaderFactory& shaderFactory, const CreateParameters& params);
        virtual nvrhi::ShaderHandle CreatePixelShader(caustica::ShaderFactory& shaderFactory, const CreateParameters& params, bool transmissiveMaterial);
        virtual nvrhi::InputLayoutHandle CreateInputLayout(nvrhi::IShader* vertexShader, const CreateParameters& params);
        virtual nvrhi::BindingLayoutHandle CreateViewBindingLayout();
        virtual nvrhi::BindingSetHandle CreateViewBindingSet();
        virtual nvrhi::BindingLayoutHandle CreateShadingBindingLayout();
        virtual nvrhi::BindingSetHandle CreateShadingBindingSet(nvrhi::ITexture* shadowMapTexture, nvrhi::ITexture* diffuse, nvrhi::ITexture* specular, nvrhi::ITexture* environmentBrdf);
        virtual nvrhi::BindingLayoutHandle CreateInputBindingLayout();
        virtual nvrhi::BindingSetHandle CreateInputBindingSet(const caustica::BufferGroup* bufferGroup);
        virtual std::shared_ptr<caustica::MaterialBindingCache> CreateMaterialBindingCache(caustica::CommonRenderPasses& commonPasses);
        virtual nvrhi::GraphicsPipelineHandle CreateGraphicsPipeline(ForwardShadingPassPipelineKey const& key, nvrhi::FramebufferInfo const& framebufferInfo);
        nvrhi::BindingSetHandle GetOrCreateInputBindingSet(const caustica::BufferGroup* bufferGroup);

    public:
        ForwardShadingPass(
            nvrhi::IDevice* device,
            std::shared_ptr<caustica::CommonRenderPasses> commonPasses);

        virtual void Init(
            caustica::ShaderFactory& shaderFactory,
            const CreateParameters& params);

        void ResetBindingCache();
        
        virtual void PrepareLights(
            Context& context,
            nvrhi::ICommandList* commandList,
            const std::vector<std::shared_ptr<caustica::Light>>& lights,
            dm::float3 ambientColorTop,
            dm::float3 ambientColorBottom,
            const std::vector<std::shared_ptr<caustica::LightProbe>>& lightProbes);

        // IGeometryPass implementation

        [[nodiscard]] caustica::ViewType::Enum GetSupportedViewTypes() const override;
        void SetupView(GeometryPassContext& context, nvrhi::ICommandList* commandList, const caustica::IView* view, const caustica::IView* viewPrev) override;
        bool SetupMaterial(GeometryPassContext& context, const caustica::Material* material, nvrhi::RasterCullMode cullMode, nvrhi::GraphicsState& state) override;
        void SetupInputBuffers(GeometryPassContext& context, const caustica::BufferGroup* buffers, nvrhi::GraphicsState& state) override;
        void SetPushConstants(GeometryPassContext& context, nvrhi::ICommandList* commandList, nvrhi::GraphicsState& state, nvrhi::DrawArguments& args) override;
    };

}
