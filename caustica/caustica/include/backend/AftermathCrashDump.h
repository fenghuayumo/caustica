#pragma once
#include <filesystem>
#include <set>
#include <rhi/common/aftermath.h>

namespace caustica
{
    class GpuDevice;

    // AftermathCrashDump contains all interactions with the Aftermath crash dump SDK,
    // and gathers all relevant information to package together with the dump
    class AftermathCrashDump
    {
    public:
        static void WaitForCrashDump(uint32_t maxTimeoutSeconds = 60);
        static uint64_t GetShaderHashForBinary(std::pair<const void*, size_t> shaderBinary, nvrhi::GraphicsAPI api);

        AftermathCrashDump(GpuDevice& deviceManager);

        void EnableCrashDumpTracking();
        // markers are stored with Aftermath as hashed 64bit values
        // this method resolves the hash back to the original human-readable text
        const std::string& ResolveMarker(uint64_t markerHash);

        GpuDevice& GetGpuDevice();
        std::filesystem::path GetDumpFolder();
    private:
        static void InitializeAftermathCrashDump(AftermathCrashDump* dumper);

        GpuDevice& m_deviceManager;
        std::filesystem::path m_dumpFolder;
    };
} // end namespace caustica