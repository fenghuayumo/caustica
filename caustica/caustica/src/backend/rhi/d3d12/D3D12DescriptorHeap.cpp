#include "D3D12Backend.h"

namespace caustica::rhi::d3d12
{

    StaticDescriptorHeap::StaticDescriptorHeap(const Context& context)
        : m_context(context)
    {
    }

    HRESULT StaticDescriptorHeap::allocateResources(D3D12_DESCRIPTOR_HEAP_TYPE heapType, uint32_t numDescriptors, bool shaderVisible)
    {
        m_heap = nullptr;
        m_shaderVisibleHeap = nullptr;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = heapType;
        heapDesc.NumDescriptors = numDescriptors;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        HRESULT hr = m_context.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap));

        if (FAILED(hr))
            return hr;

        if (shaderVisible)
        {
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

            hr = m_context.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_shaderVisibleHeap));

            if (FAILED(hr))
                return hr;

            m_startCpuHandleShaderVisible = m_shaderVisibleHeap->GetCPUDescriptorHandleForHeapStart();
            m_startGpuHandleShaderVisible = m_shaderVisibleHeap->GetGPUDescriptorHandleForHeapStart();
        }

        m_numDescriptors = heapDesc.NumDescriptors;
        m_heapType = heapDesc.Type;
        m_startCpuHandle = m_heap->GetCPUDescriptorHandleForHeapStart();
        m_stride = m_context.device->GetDescriptorHandleIncrementSize(heapDesc.Type);
        m_allocatedDescriptors.resize(m_numDescriptors);

        return S_OK;
    }

    static uint32_t nextPowerOf2(uint32_t v)
    {
        // https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2

        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v++;

        return v;
    }

    HRESULT StaticDescriptorHeap::Grow(uint32_t minRequiredSize)
    {
        uint32_t oldSize = m_numDescriptors;
        uint32_t newSize = nextPowerOf2(minRequiredSize);

        bool const isShaderVisible = m_shaderVisibleHeap != nullptr;

        if (isShaderVisible)
        {
            uint32_t const maxSize = (m_heapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                ? D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 // Not a power of 2!
                : D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;

            newSize = std::min(newSize, maxSize);

            if (newSize < minRequiredSize)
                return E_OUTOFMEMORY;
        }

        RefCountPtr<ID3D12DescriptorHeap> oldHeap = m_heap;

        HRESULT hr = allocateResources(m_heapType, newSize, isShaderVisible);

        if (FAILED(hr))
            return hr;

        m_context.device->CopyDescriptorsSimple(oldSize, m_startCpuHandle, oldHeap->GetCPUDescriptorHandleForHeapStart(), m_heapType);

        if (m_shaderVisibleHeap != nullptr)
        {
            m_context.device->CopyDescriptorsSimple(oldSize, m_startCpuHandleShaderVisible, oldHeap->GetCPUDescriptorHandleForHeapStart(), m_heapType);
        }

        return S_OK;
    }

    DescriptorIndex StaticDescriptorHeap::allocateDescriptors(uint32_t count)
    {
        std::lock_guard lockGuard(m_mutex);

        DescriptorIndex foundIndex = 0;
        uint32_t freeCount = 0;
        bool found = false;

        auto tryFind = [&](DescriptorIndex begin, DescriptorIndex end) {
            freeCount = 0;
            for (DescriptorIndex index = begin; index < end; index++)
            {
                if (m_allocatedDescriptors[index])
                    freeCount = 0;
                else
                    freeCount += 1;

                if (freeCount >= count)
                {
                    foundIndex = index - count + 1;
                    found = true;
                    return;
                }
            }
        };

        // Prefer the search hint, then scan from the start so freed slots are reused
        // before we grow (and potentially fail at the D3D12 shader-visible limit).
        tryFind(m_searchStart, m_numDescriptors);
        if (!found && m_searchStart > 0)
            tryFind(0, m_searchStart);

        if (!found)
        {
            foundIndex = m_numDescriptors;

            if (FAILED(Grow(m_numDescriptors + count)))
            {
                m_context.error("Failed to grow a descriptor heap!");
                return c_InvalidDescriptorIndex;
            }
        }

        for (DescriptorIndex index = foundIndex; index < foundIndex + count; index++)
        {
            m_allocatedDescriptors[index] = true;
        }

        m_numAllocatedDescriptors += count;

        m_searchStart = foundIndex + count;
        return foundIndex;
    }

    DescriptorIndex StaticDescriptorHeap::allocateDescriptor()
    {
        return allocateDescriptors(1);
    }

    void StaticDescriptorHeap::releaseDescriptors(DescriptorIndex baseIndex, uint32_t count)
    {
        std::lock_guard lockGuard(m_mutex);

        if (count == 0)
            return;

        for (DescriptorIndex index = baseIndex; index < baseIndex + count; index++)
        {
#ifdef _DEBUG
            if (!m_allocatedDescriptors[index])
            {
                m_context.error("Attempted to release an un-allocated descriptor");
            }
#endif

            m_allocatedDescriptors[index] = false;
        }

        m_numAllocatedDescriptors -= count;

        if (m_searchStart > baseIndex)
            m_searchStart = baseIndex;
    }

    void StaticDescriptorHeap::releaseDescriptor(DescriptorIndex index)
    {
        releaseDescriptors(index, 1);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE StaticDescriptorHeap::getCpuHandle(DescriptorIndex index)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_startCpuHandle;
        handle.ptr += index * m_stride;
        return handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE StaticDescriptorHeap::getCpuHandleShaderVisible(DescriptorIndex index)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_startCpuHandleShaderVisible;
        handle.ptr += index * m_stride;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE StaticDescriptorHeap::getGpuHandle(DescriptorIndex index)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = m_startGpuHandleShaderVisible;
        handle.ptr += index * m_stride;
        return handle;
    }

    ID3D12DescriptorHeap* StaticDescriptorHeap::getHeap() const
    {
        return m_heap;
    }

    ID3D12DescriptorHeap* StaticDescriptorHeap::getShaderVisibleHeap() const
    {
        return m_shaderVisibleHeap;
    }

    void StaticDescriptorHeap::copyToShaderVisibleHeap(DescriptorIndex index, uint32_t count)
    {
        m_context.device->CopyDescriptorsSimple(count, getCpuHandleShaderVisible(index), getCpuHandle(index), m_heapType);
    }

} // namespace caustica::rhi::d3d12
