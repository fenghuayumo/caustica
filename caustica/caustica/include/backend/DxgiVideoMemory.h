#pragma once

#if defined(_WIN32) && (CAUSTICA_WITH_DX11 || CAUSTICA_WITH_DX12)

struct IDXGIAdapter;

namespace caustica
{
struct VideoMemoryInfo;

bool QueryDxgiAdapterVideoMemory(IDXGIAdapter* adapter, VideoMemoryInfo& out);
} // namespace caustica

#endif
