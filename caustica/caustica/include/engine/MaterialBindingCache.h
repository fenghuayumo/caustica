#pragma once

#include <engine/SceneTypes.h>
#include <rhi/nvrhi.h>
#include <unordered_map>
#include <mutex>

namespace caustica
{
    enum class MaterialResource
    {
        ConstantBuffer,
        Sampler,
        DiffuseTexture,
        SpecularTexture,
        NormalTexture,
        EmissiveTexture,
        OcclusionTexture,
        TransmissionTexture,
        OpacityTexture
    };

    struct MaterialResourceBinding
    {
        MaterialResource resource;
        uint32_t slot; // type depends on resource
    };

    class MaterialBindingCache
    {
    private:
        nvrhi::DeviceHandle m_Device;
        nvrhi::BindingLayoutHandle m_BindingLayout;
        std::unordered_map<const Material*, nvrhi::BindingSetHandle> m_BindingSets;
        std::vector<MaterialResourceBinding> m_BindingDesc;
        nvrhi::TextureHandle m_FallbackTexture;
        nvrhi::SamplerHandle m_Sampler;
        std::mutex m_Mutex;
        bool m_TrackLiveness;

        nvrhi::BindingSetHandle CreateMaterialBindingSet(const Material* material);
        nvrhi::BindingSetItem GetTextureBindingSetItem(uint32_t slot, const std::shared_ptr<LoadedTexture>& texture) const;

    public:
        MaterialBindingCache(
            nvrhi::IDevice* device, 
            nvrhi::ShaderType shaderType, 
            uint32_t registerSpace,
            bool registerSpaceIsDescriptorSet,
            const std::vector<MaterialResourceBinding>& bindings,
            nvrhi::ISampler* sampler,
            nvrhi::ITexture* fallbackTexture,
            bool trackLiveness = true);

        nvrhi::IBindingLayout* GetLayout() const;
        nvrhi::IBindingSet* GetMaterialBindingSet(const Material* material);
        void Clear();
    };
}