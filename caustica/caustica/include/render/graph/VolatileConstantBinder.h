#pragma once

#include <rhi/rhi.h>

#include <cstddef>
#include <vector>

namespace caustica::rg
{

// First-class binder for NVRHI volatile constant buffers (ADR 0001 R2).
//
// Volatile CB GPU addresses are per command-list open session. Parallel GraphBuilder
// waves flush/fork lists, so CPU shadows must be rewritten onto each list before
// passes bind them. Register once per frame; GraphBuilder::recordPass calls apply().
class VolatileConstantBinder
{
public:
    struct Entry
    {
        caustica::rhi::Buffer* buffer = nullptr;
        const void* data = nullptr;
        size_t byteSize = 0;
    };

    void clear() { m_entries.clear(); }

    void bind(caustica::rhi::Buffer* buffer, const void* data, size_t byteSize)
    {
        if (!buffer || !data || byteSize == 0)
            return;
        m_entries.push_back(Entry{ buffer, data, byteSize });
    }

    void apply(caustica::rhi::CommandList* commandList) const
    {
        if (!commandList)
            return;
        for (const Entry& entry : m_entries)
        {
            if (entry.buffer && entry.data && entry.byteSize > 0)
                commandList->writeBuffer(entry.buffer, entry.data, entry.byteSize);
        }
    }

    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }
    [[nodiscard]] size_t size() const noexcept { return m_entries.size(); }
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return m_entries; }

private:
    std::vector<Entry> m_entries;
};

} // namespace caustica::rg
