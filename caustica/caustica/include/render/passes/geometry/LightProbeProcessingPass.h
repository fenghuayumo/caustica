#pragma once


#include <math/math.h>
#include <rhi/nvrhi.h>
#include <memory>
#include <unordered_map>

namespace caustica::render
{
class RenderDevice;
}

namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
    class ICompositeView;
}

namespace caustica::render
{
    class LightProbeProcessingPass
    {
    private:
        nvrhi::DeviceHandle m_device;

        nvrhi::ShaderHandle m_GeometryShader;
        nvrhi::ShaderHandle m_MipPixelShader;
        nvrhi::ShaderHandle m_DiffusePixelShader;
        nvrhi::ShaderHandle m_SpecularPixelShader;
        nvrhi::ShaderHandle m_EnvironmentBrdfPixelShader;
        nvrhi::BufferHandle m_LightProbeCB;

        nvrhi::BindingLayoutHandle m_BindingLayout;

        nvrhi::TextureHandle m_IntermediateTexture;
        uint32_t m_IntermediateTextureSize;

        nvrhi::TextureHandle m_EnvironmentBrdfTexture;
        uint32_t m_EnvironmentBrdfTextureSize;

        caustica::render::RenderDevice* m_renderDevice = nullptr;

        std::unordered_map<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle> m_BlitPsoCache;
        std::unordered_map<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle> m_DiffusePsoCache;
        std::unordered_map<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle> m_SpecularPsoCache;

        struct TextureSubresourcesKey
        {
            nvrhi::TextureHandle texture;
            nvrhi::TextureSubresourceSet subresources;

            bool operator==(const TextureSubresourcesKey& other) const { return texture == other.texture && subresources == other.subresources; }
            bool operator!=(const TextureSubresourcesKey& other) const { return !(*this == other); }

            struct Hash
            {
                size_t operator ()(const TextureSubresourcesKey& s) const {
                    return (std::hash<nvrhi::ITexture*>()(s.texture) << 1)
                        ^ std::hash<nvrhi::TextureSubresourceSet>()(s.subresources);
                }
            };
        };

        std::unordered_map<TextureSubresourcesKey, nvrhi::FramebufferHandle, TextureSubresourcesKey::Hash> m_framebufferCache;
        nvrhi::FramebufferHandle getCachedFramebuffer(nvrhi::ITexture* texture, nvrhi::TextureSubresourceSet subresources);

        std::unordered_map<TextureSubresourcesKey, nvrhi::BindingSetHandle, TextureSubresourcesKey::Hash> m_BindingSetCache;
        nvrhi::BindingSetHandle getCachedBindingSet(nvrhi::ITexture* texture, nvrhi::TextureSubresourceSet subresources);


    public:
        LightProbeProcessingPass(
            nvrhi::IDevice* device,
            std::shared_ptr<caustica::ShaderFactory> shaderFactory,
            caustica::render::RenderDevice& renderDevice,
            uint32_t intermediateTextureSize = 1024,
            nvrhi::Format intermediateTextureFormat = nvrhi::Format::RGBA16_FLOAT
        );

        void blitCubemap(
            nvrhi::ICommandList* commandList,
            nvrhi::ITexture* inCubeMap,
            uint32_t inBaseArraySlice,
            uint32_t inMipLevel,
            nvrhi::ITexture* outCubeMap,
            uint32_t outBaseArraySlice,
            uint32_t outMipLevel);

        void generateCubemapMips(
            nvrhi::ICommandList* commandList,
            nvrhi::ITexture* cubeMap,
            uint32_t baseArraySlice,
            uint32_t sourceMipLevel,
            uint32_t levelsToGenerate);

        void renderDiffuseMap(
            nvrhi::ICommandList* commandList,
            nvrhi::ITexture* inEnvironmentMap,
            nvrhi::TextureSubresourceSet inSubresources,
            nvrhi::ITexture* outDiffuseMap,
            uint32_t outBaseArraySlice,
            uint32_t outMipLevel);

        void renderSpecularMap(
            nvrhi::ICommandList* commandList,
            float roughness,
            nvrhi::ITexture* inEnvironmentMap,
            nvrhi::TextureSubresourceSet inSubresources,
            nvrhi::ITexture* outDiffuseMap,
            uint32_t outBaseArraySlice,
            uint32_t outMipLevel);

        void renderEnvironmentBrdfTexture(
            nvrhi::ICommandList* commandList);

        nvrhi::ITexture* getEnvironmentBrdfTexture();

        void resetCaches();
    };
}