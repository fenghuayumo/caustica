#pragma once

#if defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)

#include <d3d12.h>
#include <wrl/client.h>

namespace caustica::dx12
{

struct AgilityBootstrapResult
{
    Microsoft::WRL::ComPtr<ID3D12DeviceFactory> deviceFactory;
};

// load the Agility SDK redistributable from <runtime>/D3D12/ and enable
// experimental shader models via the factory when possible.
AgilityBootstrapResult BootstrapAgilitySdk();

bool EnableExperimentalShaderModels(ID3D12DeviceFactory* factory);

} // namespace caustica::dx12

#endif
