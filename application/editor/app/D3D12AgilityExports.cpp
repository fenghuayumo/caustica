// Agility SDK redistributable exports required by the host binary (exe / pyd).
// https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/

#if defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)

#include <Windows.h>

extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = CAUSTICA_D3D_AGILITY_SDK_VERSION;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

#endif
