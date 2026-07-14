#include <backend/DxgiVideoMemory.h>

#include <backend/GpuDevice.h>

#if defined(_WIN32) && (CAUSTICA_WITH_DX11 || CAUSTICA_WITH_DX12)

#include <dxgi1_4.h>
#include <wrl/client.h>

namespace caustica
{

bool queryDxgiAdapterVideoMemory(IDXGIAdapter* adapter, VideoMemoryInfo& out)
{
    if (!adapter)
        return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))))
        return false;

    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return false;

    out.budget = info.Budget;
    out.currentUsage = info.CurrentUsage;
    out.availableForReservation = info.AvailableForReservation;
    out.currentReservation = info.CurrentReservation;
    return true;
}

} // namespace caustica

#endif
