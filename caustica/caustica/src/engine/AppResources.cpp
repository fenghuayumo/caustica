#include <engine/AppResources.h>

#include <cassert>

namespace caustica
{

void AppResources::insertOwned(std::type_index type, std::unique_ptr<IResourceBox> box)
{
    m_owned[type] = std::move(box);
    m_refs.erase(type);
}

void* AppResources::resourcePtr(std::type_index type)
{
    if (void* ptr = tryResourcePtr(type))
        return ptr;

    assert(false && "App resource not found");
    return nullptr;
}

const void* AppResources::resourcePtr(std::type_index type) const
{
    if (const void* ptr = tryResourcePtr(type))
        return ptr;

    assert(false && "App resource not found");
    return nullptr;
}

void* AppResources::tryResourcePtr(std::type_index type)
{
    if (auto refIt = m_refs.find(type); refIt != m_refs.end())
        return refIt->second;

    if (auto ownedIt = m_owned.find(type); ownedIt != m_owned.end())
        return ownedIt->second->ptr();

    return nullptr;
}

const void* AppResources::tryResourcePtr(std::type_index type) const
{
    if (auto refIt = m_refs.find(type); refIt != m_refs.end())
        return refIt->second;

    if (auto ownedIt = m_owned.find(type); ownedIt != m_owned.end())
        return ownedIt->second->ptr();

    return nullptr;
}

void AppResources::clear()
{
    m_owned.clear();
    m_refs.clear();
}

} // namespace caustica
