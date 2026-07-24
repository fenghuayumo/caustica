#pragma once

#include <scene/SceneTypes.h>
#include <rhi/rhi.h>

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
        caustica::rhi::IDevice* device,
        caustica::rhi::ShaderType shaderType,
        uint32_t registerSpace,
        bool registerSpaceIsDescriptorSet,
        const std::vector<MaterialResourceBinding>& bindings,
        caustica::rhi::ISampler* sampler,
        caustica::rhi::ITexture* fallbackTexture,
        render::SceneGpuResources* sceneGpuResources,
        bool trackLiveness = true);

    caustica::rhi::IBindingLayout* getLayout() const;
    caustica::rhi::IBindingSet* getMaterialBindingSet(const Material* material);
    void clear();

private:
    caustica::rhi::BindingSetHandle createMaterialBindingSet(const Material* material);
    caustica::rhi::BindingSetItem getTextureBindingSetItem(uint32_t slot, const Handle<ImageAsset>& texture) const;

    caustica::rhi::DeviceHandle m_device;
    caustica::rhi::BindingLayoutHandle m_bindingLayout;
    std::unordered_map<const Material*, caustica::rhi::BindingSetHandle> m_bindingSets;
    std::vector<MaterialResourceBinding> m_bindingDesc;
    caustica::rhi::TextureHandle m_fallbackTexture;
    caustica::rhi::SamplerHandle m_sampler;
    render::SceneGpuResources* m_sceneGpuResources = nullptr;
    std::mutex m_mutex;
    bool m_trackLiveness;
};

} // namespace caustica
