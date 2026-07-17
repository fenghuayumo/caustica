#pragma once

#include <cstdint>
#include <functional>
#include <limits>

namespace caustica::scene
{

struct MeshRenderResourceTag;
struct GeometryRenderResourceTag;
struct MaterialRenderResourceTag;

template <typename Tag>
class RenderResourceId
{
public:
    static constexpr uint64_t Invalid = std::numeric_limits<uint64_t>::max();

    RenderResourceId() = default;

    static RenderResourceId make(uint32_t index, uint16_t generation)
    {
        return RenderResourceId(
            uint64_t(index) | (uint64_t(generation) << 32));
    }

    [[nodiscard]] bool isValid() const { return m_value != Invalid; }
    [[nodiscard]] explicit operator bool() const { return isValid(); }
    [[nodiscard]] uint32_t getIndex() const { return uint32_t(m_value); }
    [[nodiscard]] uint16_t getGeneration() const { return uint16_t(m_value >> 32); }
    [[nodiscard]] uint64_t getValue() const { return m_value; }

    bool operator==(RenderResourceId other) const { return m_value == other.m_value; }
    bool operator!=(RenderResourceId other) const { return m_value != other.m_value; }

    struct Hash
    {
        size_t operator()(RenderResourceId id) const
        {
            return std::hash<uint64_t>{}(id.m_value);
        }
    };

private:
    explicit RenderResourceId(uint64_t value) : m_value(value) {}
    uint64_t m_value = Invalid;
};

using MeshRenderResourceId = RenderResourceId<MeshRenderResourceTag>;
using GeometryRenderResourceId = RenderResourceId<GeometryRenderResourceTag>;
using MaterialRenderResourceId = RenderResourceId<MaterialRenderResourceTag>;

} // namespace caustica::scene
