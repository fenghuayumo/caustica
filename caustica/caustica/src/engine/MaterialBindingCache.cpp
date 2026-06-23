#include <engine/MaterialBindingCache.h>
#include <core/log.h>

using namespace caustica;

MaterialBindingCache::MaterialBindingCache(
    nvrhi::IDevice* device, 
    nvrhi::ShaderType shaderType, 
    uint32_t registerSpace,
    bool registerSpaceIsDescriptorSet,
    const std::vector<MaterialResourceBinding>& bindings,
    nvrhi::ISampler* sampler,
    nvrhi::ITexture* fallbackTexture,
    bool trackLiveness)
    : m_Device(device)
    , m_BindingDesc(bindings)
    , m_FallbackTexture(fallbackTexture)
    , m_Sampler(sampler)
    , m_TrackLiveness(trackLiveness)
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
            caustica::error("MaterialBindingCache: unknown MaterialResource value (%d)", item.resource);
            return;
        }

        layoutDesc.bindings.push_back(layoutItem);
    }

    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);
}

nvrhi::IBindingLayout* caustica::MaterialBindingCache::GetLayout() const
{
    return m_BindingLayout;
}

nvrhi::IBindingSet* caustica::MaterialBindingCache::GetMaterialBindingSet(const Material* material)
{
    std::lock_guard<std::mutex> lockGuard(m_Mutex);

    nvrhi::BindingSetHandle& bindingSet = m_BindingSets[material];

    if (bindingSet)
        return bindingSet;

    bindingSet = CreateMaterialBindingSet(material);

    return bindingSet;
}

void caustica::MaterialBindingCache::Clear()
{
    std::lock_guard<std::mutex> lockGuard(m_Mutex);

    m_BindingSets.clear();
}

nvrhi::BindingSetItem MaterialBindingCache::GetTextureBindingSetItem(uint32_t slot, const std::shared_ptr<LoadedTexture>& texture) const
{
    return nvrhi::BindingSetItem::Texture_SRV(slot, texture && texture->texture ? texture->texture.Get() : m_FallbackTexture.Get());
}

nvrhi::BindingSetHandle caustica::MaterialBindingCache::CreateMaterialBindingSet(const Material* material)
{
    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.trackLiveness = m_TrackLiveness;

    for (const auto& item : m_BindingDesc)
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
                m_Sampler);
            break;

        case MaterialResource::DiffuseTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->baseOrDiffuseTexture);
            break;

        case MaterialResource::SpecularTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->metalRoughOrSpecularTexture);
            break;

        case MaterialResource::NormalTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->normalTexture);
            break;

        case MaterialResource::EmissiveTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->emissiveTexture);
            break;

        case MaterialResource::OcclusionTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->occlusionTexture);
            break;

        case MaterialResource::TransmissionTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->transmissionTexture);
            break;

        case MaterialResource::OpacityTexture:
            setItem = GetTextureBindingSetItem(item.slot, material->opacityTexture);
            break;

        default:
            caustica::error("MaterialBindingCache: unknown MaterialResource value (%d)", item.resource);
            return nullptr;
        }

        bindingSetDesc.bindings.push_back(setItem);
    }

    return m_Device->createBindingSet(bindingSetDesc, m_BindingLayout);
}
