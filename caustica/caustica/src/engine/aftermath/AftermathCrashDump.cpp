#include <sstream>

#include <core/log.h>
#include <backend/AftermathCrashDump.h>
#include <core/path_utils.h>

#include <GFSDK_Aftermath_GpuCrashDump.h>
#include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>

static void DumpFileCallback(const void* pGpuCrashDump, const uint32_t gpuCrashDumpSize, void* pUserData)
{
    caustica::AftermathCrashDump *dumper = reinterpret_cast<caustica::AftermathCrashDump*>(pUserData);
    std::filesystem::create_directory(dumper->getDumpFolder());

    auto nativeFS = std::make_unique<caustica::NativeFileSystem>();
    std::filesystem::path dumpPath = dumper->getDumpFolder() / "crash.nv-gpudmp";
    nativeFS->writeFile(dumpPath, pGpuCrashDump, gpuCrashDumpSize);

    GFSDK_Aftermath_GpuCrashDump_Decoder decoder = {};
    GFSDK_Aftermath_Result result = GFSDK_Aftermath_GpuCrashDump_CreateDecoder(GFSDK_Aftermath_Version_API, pGpuCrashDump, gpuCrashDumpSize, &decoder);
    if (!GFSDK_Aftermath_SUCCEED(result))
    {
        caustica::error("Aftermath crash dump decoder failed create with error 0x%.8x", result);
        return;
    }
    uint32_t numActiveShaders = 0;
    GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfoCount(decoder, &numActiveShaders);
    if (numActiveShaders > 0)
    {
        std::vector<GFSDK_Aftermath_GpuCrashDump_ShaderInfo> shaderInfos{ numActiveShaders };
        GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfo(decoder, numActiveShaders, shaderInfos.data());
        for (auto& shaderInfo : shaderInfos)
        {
            if (!shaderInfo.isInternal)
            {
                GFSDK_Aftermath_ShaderBinaryHash shaderHash = {};
                GFSDK_Aftermath_GetShaderHashForShaderInfo(decoder, &shaderInfo, &shaderHash);
                caustica::rhi::AftermathCrashDumpHelper& crashDumpHelper = dumper->getGpuDevice().getDevice()->getAftermathCrashDumpHelper();
                caustica::rhi::BinaryBlob shaderLookupResult = crashDumpHelper.findShaderBinary(shaderHash.hash, caustica::AftermathCrashDump::getShaderHashForBinary);
                if (shaderLookupResult.second > 0)
                {
                    std::stringstream ss;
                    ss << std::hex << shaderHash.hash << ".bin";
                    std::filesystem::path shaderPath = dumper->getDumpFolder() / ss.str();
                    nativeFS->writeFile(shaderPath, shaderLookupResult.first, shaderLookupResult.second);
                }
            }
        }
    }
    GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
}

static void ShaderDebugInfoCallback(const void* pShaderDebugInfo, const uint32_t shaderDebugInfoSize, void* pUserData)
{
    caustica::AftermathCrashDump* dumper = reinterpret_cast<caustica::AftermathCrashDump*>(pUserData);
    std::filesystem::create_directory(dumper->getDumpFolder());

    auto nativeFS = std::make_unique<caustica::NativeFileSystem>();
    // the hash used for nsight is stored in the shader debug info file in address 0x20-0x40
    // the name (in terms of uint64s at byte addresses) is 0x28 0x20 - 0x38 0x30
    // which in terms of uint64 addresses is 0x5 0x4 - 0x7 0x6
    const uint64_t* ptr64 = reinterpret_cast<const uint64_t*>(pShaderDebugInfo);
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(8) << ptr64[5] << std::setw(8) << ptr64[4]
        << "-" << std::setw(8) << ptr64[7] << std::setw(8) << ptr64[6] << ".nvdbg";
    std::filesystem::path dumpPath = dumper->getDumpFolder() / ss.str();
    nativeFS->writeFile(dumpPath, pShaderDebugInfo, shaderDebugInfoSize);
}

static void DescriptionCallback(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addDescription, void* pUserData)
{
    caustica::AftermathCrashDump* dumper = reinterpret_cast<caustica::AftermathCrashDump*>(pUserData);
    addDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, dumper->getGpuDevice().getWindowTitle());
}

// this callback should call into the RHI device which has the necessary information
static void ResolveMarkerCallback(const void* pMarkerData, const uint32_t markerDataSize, void* pUserData, void** ppResolvedMarkerData, uint32_t* pResolvedMarkerDataSize)
{
    caustica::AftermathCrashDump* dumper = reinterpret_cast<caustica::AftermathCrashDump*>(pUserData);
    const uint64_t markerAsHash = reinterpret_cast<const uint64_t>(pMarkerData);
    // as long as the device is not yet destroyed, these references should be ok to pass back
    const std::string& resolvedMarker = dumper->resolveMarker(markerAsHash);
    *ppResolvedMarkerData = (void*) resolvedMarker.data();
    *pResolvedMarkerDataSize = uint32_t(resolvedMarker.length());
}

void caustica::AftermathCrashDump::waitForCrashDump(uint32_t maxTimeoutSeconds)
{
    std::chrono::time_point startTime = std::chrono::system_clock::now();
    bool timedOut = false;
    while (!timedOut)
    {
        GFSDK_Aftermath_CrashDump_Status crashDumpStatus = GFSDK_Aftermath_CrashDump_Status_Unknown;
        GFSDK_Aftermath_GetCrashDumpStatus(&crashDumpStatus);
        if (crashDumpStatus == GFSDK_Aftermath_CrashDump_Status_Finished)
        {
            break;
        }
        auto elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - startTime).count();
        timedOut = elapsedSeconds > maxTimeoutSeconds;
    }
}

uint64_t caustica::AftermathCrashDump::getShaderHashForBinary(std::pair<const void*, size_t> shaderBinary, caustica::rhi::GraphicsAPI api)
{
#if CAUSTICA_WITH_VULKAN
    if (api == caustica::rhi::GraphicsAPI::VULKAN)
    {
        GFSDK_Aftermath_SpirvCode spirv = {};
        spirv.pData = shaderBinary.first;
        spirv.size = uint32_t(shaderBinary.second);
        GFSDK_Aftermath_ShaderBinaryHash hash = {};
        GFSDK_Aftermath_GetShaderHashSpirv(GFSDK_Aftermath_Version_API, &spirv, &hash);
        return hash.hash;
    }
#endif
#if CAUSTICA_WITH_DX11 || CAUSTICA_WITH_DX12
    if (api == caustica::rhi::GraphicsAPI::D3D11 || api == caustica::rhi::GraphicsAPI::D3D12)
    {
        D3D12_SHADER_BYTECODE dxil = {};
        dxil.pShaderBytecode = shaderBinary.first;
        dxil.BytecodeLength = shaderBinary.second;
        GFSDK_Aftermath_ShaderBinaryHash hash = {};
        GFSDK_Aftermath_GetShaderHash(GFSDK_Aftermath_Version_API, &dxil, &hash);
        return hash.hash;
    }
#endif
    return 0;
}

static bool AftermathInitialized = false;
void caustica::AftermathCrashDump::initializeAftermathCrashDump(AftermathCrashDump* dumper)
{
    // if already initialized, reinit with new crash dumper
    if (AftermathInitialized)
    {
        GFSDK_Aftermath_DisableGpuCrashDumps();
    }

    uint32_t watchedApis = GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_None;
#if CAUSTICA_WITH_DX11 || CAUSTICA_WITH_DX12
    watchedApis |= GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_DX;
#endif
#if CAUSTICA_WITH_VULKAN
    watchedApis |= GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan;
#endif
    uint32_t featureFlags = GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks;
    GFSDK_Aftermath_Result result = GFSDK_Aftermath_EnableGpuCrashDumps(
        GFSDK_Aftermath_Version_API,
        watchedApis,
        featureFlags,
        DumpFileCallback,
        ShaderDebugInfoCallback,
        DescriptionCallback,
        ResolveMarkerCallback,
        dumper);
    if (!GFSDK_Aftermath_SUCCEED(result))
    {
        caustica::error("Aftermath crash dump enable failed with error 0x%.8x", result);
    }
}


caustica::AftermathCrashDump::AftermathCrashDump(GpuDevice& deviceManager) :
    m_deviceManager(deviceManager)
{
}

void caustica::AftermathCrashDump::enableCrashDumpTracking()
{
    initializeAftermathCrashDump(this);
    // create a unique path to store the dump files based on date/time
    // using date/time at creation time since we need to use the same value for different callbacks but they will be called at different times
    std::stringstream folder;
    std::time_t t = std::time(nullptr);
    std::tm tm = *std::localtime(&t);
    folder << "crash_" << std::put_time(&tm, "%Y-%m-%d-%H_%M_%S");
    m_dumpFolder = caustica::getDirectoryWithExecutable() / folder.str();
}

const std::string& caustica::AftermathCrashDump::resolveMarker(uint64_t markerHash)
{
    auto [found, markerString] = m_deviceManager.getDevice()->getAftermathCrashDumpHelper().resolveMarker(markerHash);
    return markerString;
}

caustica::GpuDevice& caustica::AftermathCrashDump::getGpuDevice()
{
    return m_deviceManager;
}

std::filesystem::path caustica::AftermathCrashDump::getDumpFolder()
{
    return m_dumpFolder;
}
