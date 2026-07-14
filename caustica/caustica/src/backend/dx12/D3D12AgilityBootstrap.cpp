#include <backend/dx12/D3D12AgilityBootstrap.h>

#include <core/log.h>
#include <core/path_utils.h>

namespace caustica::dx12
{

namespace
{

std::string getAgilitySdkPath()
{
    std::string sdkPath = (getRuntimeDirectory() / "D3D12").string();
    if (!sdkPath.empty() && sdkPath.back() != '\\' && sdkPath.back() != '/')
        sdkPath += "\\";
    return sdkPath;
}

} // namespace

bool enableExperimentalShaderModels(ID3D12DeviceFactory* factory)
{
    static const UUID D3D12ExperimentalShaderModels = {
        0x76f5573e, 0xf13a, 0x40f5, {0xb2, 0x97, 0x81, 0xce, 0x9e, 0x18, 0x93, 0x3f}
    };
    UUID features[] = { D3D12ExperimentalShaderModels };

    const HRESULT hr = factory
        ? factory->EnableExperimentalFeatures(_countof(features), features, nullptr, nullptr)
        : D3D12EnableExperimentalFeatures(_countof(features), features, nullptr, nullptr);
    if (FAILED(hr))
    {
        if (factory && hr == E_NOINTERFACE)
            return false;
        caustica::warning(
            "D3D12 experimental shader models could not be enabled, HRESULT = 0x%08x",
            unsigned(hr));
        return false;
    }
    return true;
}

AgilityBootstrapResult bootstrapAgilitySdk()
{
    AgilityBootstrapResult result;
    const std::string sdkPath = getAgilitySdkPath();

    Microsoft::WRL::ComPtr<ID3D12SDKConfiguration1> sdkConfig1;
    HRESULT hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdkConfig1));
    if (SUCCEEDED(hr))
    {
        Microsoft::WRL::ComPtr<ID3D12DeviceFactory> factory;
        hr = sdkConfig1->CreateDeviceFactory(
            CAUSTICA_D3D_AGILITY_SDK_VERSION,
            sdkPath.c_str(),
            IID_PPV_ARGS(&factory));

        if (SUCCEEDED(hr) && factory)
        {
            enableExperimentalShaderModels(factory.Get());
            result.deviceFactory = std::move(factory);
            return result;
        }

        caustica::warning(
            "ID3D12SDKConfiguration1::CreateDeviceFactory('%s') failed, HRESULT = 0x%08x",
            sdkPath.c_str(),
            unsigned(hr));
    }
    else
    {
        caustica::warning(
            "D3D12GetInterface(ID3D12SDKConfiguration1) failed, HRESULT = 0x%08x",
            unsigned(hr));
    }

    // Fallback for older runtimes. Works when the host process has not already
    // locked D3D12 to the system SDK (see D3D12AgilityExports in the host binary).
    Microsoft::WRL::ComPtr<ID3D12SDKConfiguration> sdkConfig;
    hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdkConfig));
    if (FAILED(hr))
    {
        caustica::warning(
            "D3D12GetInterface(ID3D12SDKConfiguration) failed, HRESULT = 0x%08x",
            unsigned(hr));
        return result;
    }

    hr = sdkConfig->SetSDKVersion(CAUSTICA_D3D_AGILITY_SDK_VERSION, sdkPath.c_str());
    if (FAILED(hr))
    {
        caustica::warning(
            "ID3D12SDKConfiguration::SetSDKVersion('%s') failed, HRESULT = 0x%08x",
            sdkPath.c_str(),
            unsigned(hr));
        return result;
    }

    enableExperimentalShaderModels(nullptr);
    return result;
}

} // namespace caustica::dx12
