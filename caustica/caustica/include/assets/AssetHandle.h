#pragma once

#include <assets/AssetId.h>
#include <cstdint>

// =============================================================================
// AssetHandle<T> — type-safe, generation-tracked reference to an asset.
//
// Wraps an AssetId with a generation counter. The generation is incremented
// each time the asset is reloaded (hot-reload), invalidating old handles.
// This prevents use-after-reload bugs without runtime overhead.
// =============================================================================

namespace caustica
{

template <typename AssetType>
class AssetHandle
{
public:
    AssetHandle() = default;

    AssetHandle(AssetId id, uint32_t generation = 0)
        : m_Id(id), m_Generation(generation)
    {}

    [[nodiscard]] const AssetId& GetId() const { return m_Id; }
    [[nodiscard]] uint32_t GetGeneration() const { return m_Generation; }
    [[nodiscard]] bool IsValid() const { return m_Id.IsValid(); }
    [[nodiscard]] explicit operator bool() const { return IsValid(); }

    void Invalidate() { m_Id = AssetId::Invalid(); m_Generation = 0; }

    bool operator==(const AssetHandle& other) const
    {
        return m_Id == other.m_Id && m_Generation == other.m_Generation;
    }
    bool operator!=(const AssetHandle& other) const { return !(*this == other); }
    bool operator<(const AssetHandle& other) const
    {
        if (m_Id != other.m_Id) return m_Id < other.m_Id;
        return m_Generation < other.m_Generation;
    }

private:
    AssetId  m_Id;
    uint32_t m_Generation = 0;
};

} // namespace caustica

namespace std
{
    template <typename T>
    struct hash<caustica::AssetHandle<T>>
    {
        size_t operator()(const caustica::AssetHandle<T>& h) const
        {
            return caustica::AssetId::Hash()(h.GetId()) ^ (size_t(h.GetGeneration()) << 16);
        }
    };
}
