#include <render/Core/MaterialBindingCache.h>

#include <core/log.h>

namespace caustica
{

MaterialBindingCache::MaterialBindingCache(
    nvrhi::IDevice* device,
    nvrhi::ShaderType shaderType,
    uint32_t registerSpace,
    bool registerSpaceIsDescriptorSet,
    const std::vector<MaterialResourceBinding>& bindings,
    nvrhi::ISampler* sampler,
    nvrhi::ITexture* fallbackTexture,
    bool trackLiveness)
    : m_device(device)
    , m_bindingDesc(bindings)
    , m_fallbackTexture(fallbackTexture)
    , m_sampler(sampler)
    , m_trackLiveness(trackLiveness)
{
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = shaderType;
    layoutDesc.registerSpace = registerSpace;
    layoutDesc.registerSpaceIsDescriptorSet = registerSpaceIsDescriptorSet;

    for (const auto& item : bindings)
    {
        nvrhi::BindingLayoutItem layoutItem{};
        layoutItem.slot = item.slot;
        layoutItem.size = 1;

        switch (item.resource)
        {
        case MaterialResource::ConstantBuffer:
            layoutItem.type = nvrhi::ResourceType::ConstantBuffer;
            break;
        case MaterialResource::DiffuseTexture:
        case MaterialResource::SpecularTexture:
        case MaterialResource::NormalTexture:
        case MaterialResource::EmissiveTexture:
        case MaterialResource::OcclusionTexture:
        case MaterialResource::TransmissionTexture:
        case MaterialResource::OpacityTexture:
            layoutItem.type = nvrhi::ResourceType::Texture_SRV;
            break;
        case MaterialResource::Sampler:
            layoutItem.type = nvrhi::ResourceType::Sampler;
            break;
        default:
            error("MaterialBindingCache: unknown MaterialResource value (%d)", item.resource);
            return;
        }

        layoutDesc.bindings.push_back(layoutItem);
    }

    m_bindingLayout = m_device->createBindingLayout(layoutDesc);
}

nvrhi::IBindingLayout* MaterialBindingCache::getLayout() const
{
    return m_bindingLayout;
}

nvrhi::IBindingSet* MaterialBindingCache::getMaterialBindingSet(const Material* material)
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);

    nvrhi::BindingSetHandle& bindingSet = m_bindingSets[material];

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

nvrhi::BindingSetItem MaterialBindingCache::getTextureBindingSetItem(uint32_t slot, const std::shared_ptr<LoadedTexture>& texture) const
{
    return nvrhi::BindingSetItem::Texture_SRV(slot, texture && texture->texture ? texture->texture.Get() : m_fallbackTexture.Get());
}

nvrhi::BindingSetHandle MaterialBindingCache::createMaterialBindingSet(const Material* material)
{
    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.trackLiveness = m_trackLiveness;

    for (const auto& item : m_bindingDesc)
    {
        nvrhi::BindingSetItem setItem;

        switch (item.resource)
        {
        case MaterialResource::ConstantBuffer:
            setItem = nvrhi::BindingSetItem::ConstantBuffer(
                item.slot,
                material->materialConstants);
            break;

        case MaterialResource::Sampler:
            setItem = nvrhi::BindingSetItem::Sampler(
                item.slot,
                m_sampler);
            break;

        case MaterialResource::DiffuseTexture:
            setItem = getTextureBindingSetItem(item.slot, material->baseOrDiffuseTexture);
            break;

        case MaterialResource::SpecularTexture:
            setItem = getTextureBindingSetItem(item.slot, material->metalRoughOrSpecularTexture);
            break;

        case MaterialResource::NormalTexture:
            setItem = getTextureBindingSetItem(item.slot, material->normalTexture);
            break;

        case MaterialResource::EmissiveTexture:
            setItem = getTextureBindingSetItem(item.slot, material->emissiveTexture);
            break;

        case MaterialResource::OcclusionTexture:
            setItem = getTextureBindingSetItem(item.slot, material->occlusionTexture);
            break;

        case MaterialResource::TransmissionTexture:
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
