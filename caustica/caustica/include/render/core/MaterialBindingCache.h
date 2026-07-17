#pragma once

#include <scene/SceneTypes.h>
#include <rhi/nvrhi.h>

#include <mutex>
#include <unordered_map>
#include <vector>

namespace caustica
{
namespace render { struct SceneGpuResources; }

enum class MaterialResource
{
    ConstantBuffer,
    Sampler,
    DiffuseTexture,
    SpecularTexture,
    normalTexture,
    emissiveTexture,
    OcclusionTexture,
    transmissionTexture,
    OpacityTexture
};

struct MaterialResourceBinding
{
    MaterialResource resource;
    uint32_t slot; // type depends on resource
};

class MaterialBindingCache
{
public:
    MaterialBindingCache(
        nvrhi::IDevice* device,
        nvrhi::ShaderType shaderType,
        uint32_t registerSpace,
        bool registerSpaceIsDescriptorSet,
        const std::vector<MaterialResourceBinding>& bindings,
        nvrhi::ISampler* sampler,
        nvrhi::ITexture* fallbackTexture,
        render::SceneGpuResources* sceneGpuResources,
        bool trackLiveness = true);

    nvrhi::IBindingLayout* getLayout() const;
    nvrhi::IBindingSet* getMaterialBindingSet(const Material* material);
    void clear();

private:
    nvrhi::BindingSetHandle createMaterialBindingSet(const Material* material);
    nvrhi::BindingSetItem getTextureBindingSetItem(uint32_t slot, const Handle<ImageAsset>& texture) const;

    nvrhi::DeviceHandle m_device;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    std::unordered_map<const Material*, nvrhi::BindingSetHandle> m_bindingSets;
    std::vector<MaterialResourceBinding> m_bindingDesc;
    nvrhi::TextureHandle m_fallbackTexture;
    nvrhi::SamplerHandle m_sampler;
    render::SceneGpuResources* m_sceneGpuResources = nullptr;
    std::mutex m_mutex;
    bool m_trackLiveness;
};

} // namespace caustica
