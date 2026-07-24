#include <render/core/MaterialBindingCache.h>
#include <render/SceneGpuResources.h>

#include <core/log.h>

namespace caustica
{

MaterialBindingCache::MaterialBindingCache(
    caustica::rhi::Device* device,
    caustica::rhi::ShaderType shaderType,
    uint32_t registerSpace,
    bool registerSpaceIsDescriptorSet,
    const std::vector<MaterialResourceBinding>& bindings,
    caustica::rhi::Sampler* sampler,
    caustica::rhi::Texture* fallbackTexture,
    render::SceneGpuResources* sceneGpuResources,
    bool trackLiveness)
    : m_device(device)
    , m_bindingDesc(bindings)
    , m_fallbackTexture(fallbackTexture)
    , m_sampler(sampler)
    , m_sceneGpuResources(sceneGpuResources)
    , m_trackLiveness(trackLiveness)
{
    caustica::rhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = shaderType;
    layoutDesc.registerSpace = registerSpace;
    layoutDesc.registerSpaceIsDescriptorSet = registerSpaceIsDescriptorSet;

    for (const auto& item : bindings)
    {
        caustica::rhi::BindingLayoutItem layoutItem{};
        layoutItem.slot = item.slot;
        layoutItem.size = 1;

        switch (item.resource)
        {
        case MaterialResource::ConstantBuffer:
            layoutItem.type = caustica::rhi::ResourceType::ConstantBuffer;
            break;
        case MaterialResource::DiffuseTexture:
        case MaterialResource::SpecularTexture:
        case MaterialResource::normalTexture:
        case MaterialResource::emissiveTexture:
        case MaterialResource::OcclusionTexture:
        case MaterialResource::transmissionTexture:
        case MaterialResource::OpacityTexture:
            layoutItem.type = caustica::rhi::ResourceType::Texture_SRV;
            break;
        case MaterialResource::Sampler:
            layoutItem.type = caustica::rhi::ResourceType::Sampler;
            break;
        default:
            error("MaterialBindingCache: unknown MaterialResource value (%d)", item.resource);
            return;
        }

        layoutDesc.bindings.push_back(layoutItem);
    }

    m_bindingLayout = m_device->createBindingLayout(layoutDesc);
}

caustica::rhi::BindingLayout* MaterialBindingCache::getLayout() const
{
    return m_bindingLayout;
}

caustica::rhi::BindingSet* MaterialBindingCache::getMaterialBindingSet(const Material* material)
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);

    caustica::rhi::BindingSetHandle& bindingSet = m_bindingSets[material];

    if (bindingSet)
        return bindingSet;

    bindingSet = createMaterialBindingSet(material);

    return bindingSet;
}

void MaterialBindingCache::clear()
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);

    m_bindingSets.clear();
}

caustica::rhi::BindingSetItem MaterialBindingCache::getTextureBindingSetItem(uint32_t slot, const Handle<ImageAsset>& texture) const
{
    return caustica::rhi::BindingSetItem::Texture_SRV(slot, texture && texture->gpu.texture ? texture->gpu.texture.Get() : m_fallbackTexture.Get());
}

caustica::rhi::BindingSetHandle MaterialBindingCache::createMaterialBindingSet(const Material* material)
{
    caustica::rhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.trackLiveness = m_trackLiveness;

    for (const auto& item : m_bindingDesc)
    {
        caustica::rhi::BindingSetItem setItem;

        switch (item.resource)
        {
        case MaterialResource::ConstantBuffer:
        {
            if (m_sceneGpuResources == nullptr)
                return nullptr;
            const auto materialGpuIt =
                m_sceneGpuResources->materialRegistry.find(material->renderResourceId);
            if (materialGpuIt == m_sceneGpuResources->materialRegistry.end()
                || !materialGpuIt->second.constantsBuffer)
            {
                return nullptr;
            }
            setItem = caustica::rhi::BindingSetItem::ConstantBuffer(
                item.slot,
                materialGpuIt->second.constantsBuffer);
            break;
        }

        case MaterialResource::Sampler:
            setItem = caustica::rhi::BindingSetItem::Sampler(
                item.slot,
                m_sampler);
            break;

        case MaterialResource::DiffuseTexture:
            setItem = getTextureBindingSetItem(item.slot, material->baseOrDiffuseTexture);
            break;

        case MaterialResource::SpecularTexture:
            setItem = getTextureBindingSetItem(item.slot, material->metalRoughOrSpecularTexture);
            break;

        case MaterialResource::normalTexture:
            setItem = getTextureBindingSetItem(item.slot, material->normalTexture);
            break;

        case MaterialResource::emissiveTexture:
            setItem = getTextureBindingSetItem(item.slot, material->emissiveTexture);
            break;

        case MaterialResource::OcclusionTexture:
            setItem = getTextureBindingSetItem(item.slot, material->occlusionTexture);
            break;

        case MaterialResource::transmissionTexture:
            setItem = getTextureBindingSetItem(item.slot, material->transmissionTexture);
            break;

        case MaterialResource::OpacityTexture:
            setItem = getTextureBindingSetItem(item.slot, material->opacityTexture);
            break;

        default:
            error("MaterialBindingCache: unknown MaterialResource value (%d)", item.resource);
            return nullptr;
        }

        bindingSetDesc.bindings.push_back(setItem);
    }

    return m_device->createBindingSet(bindingSetDesc, m_bindingLayout);
}

} // namespace caustica
