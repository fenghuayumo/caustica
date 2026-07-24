#pragma once

#include <rhi/rhi_types.h>
#include <unordered_map>
#include <functional>
#include <deque>
#include <set>
#include <unordered_map>
#include <filesystem>

namespace caustica::rhi
{
    typedef std::pair<bool, std::reference_wrapper<const std::string>> ResolvedMarker;
    typedef std::pair<const void*, size_t> BinaryBlob;
    typedef std::function<uint64_t(BinaryBlob, caustica::rhi::GraphicsAPI)> ShaderHashGeneratorFunction;
    typedef std::function<BinaryBlob(uint64_t, ShaderHashGeneratorFunction)> ShaderBinaryLookupCallback;

    // Aftermath will return the payload of the last marker the GPU executed, so in cases of nested regimes,
    // we want the marker payloads to represent the whole "stack" of regimes, not just the last one
    // AftermathMarkerTracker pushes/pops regimes to this stack
    // The payload itself is a 64bit value, so AftermathMarkerTracker stores the mappings of strings<->hashes
    // There should be one AftermathMarkerTracker per graphics API-level command list
    class AftermathMarkerTracker
    {
    public:
        AftermathMarkerTracker();

        size_t pushEvent(const char* name);
        void popEvent();

        ResolvedMarker getEventString(size_t hash);
    private:
        // using a filesystem path to track the event stack since that automatically inserts "/" separators
        // and is easy to push/pop entries
        std::filesystem::path m_EventStack;
        
        // Some apps have unique marker text on every frame (for example, by appending the frame number to the marker)
        // In these cases, we want to cap the max number of strings stored to prevent memory usage from growing
        const static size_t MaxEventStrings = 128;
        std::array<size_t, MaxEventStrings> m_EventHashes;
        size_t m_OldestHashIndex;
        std::unordered_map<size_t, std::string> m_EventStrings;
    };

    // AftermathCrashDumpHelper tracks all caustica::rhi::Device-level constructs that we need when generating a crash dump
    // It provides two services: resolving a marker hash to the original string, and getting the specific shader bytecode
    // of a requested shader hash
    // There should be one AftermathCrashDumpHelper per caustica::rhi::Device
    // All command lists will register their AftermathMarkerTrackers with the AftermathCrashDumpHelper
    // Any shader bytecode loading and management code (e.g. caustica's ShaderFactory) should register a shader binary lookup callback
    class AftermathCrashDumpHelper
    {
    public:
        AftermathCrashDumpHelper();
        
        void registerAftermathMarkerTracker(AftermathMarkerTracker* tracker);
        void unRegisterAftermathMarkerTracker(AftermathMarkerTracker* tracker);
        void registerShaderBinaryLookupCallback(void* client, ShaderBinaryLookupCallback lookupCallback);
        void unRegisterShaderBinaryLookupCallback(void* client);

        ResolvedMarker ResolveMarker(size_t markerHash);
        BinaryBlob findShaderBinary(uint64_t shaderHash, ShaderHashGeneratorFunction hashGenerator);
    private:
        std::set<AftermathMarkerTracker*> m_MarkerTrackers;
        // Command lists that are deleted on the CPU-side could still be executing (and crashing) GPU side,
        // so we keep around a small number of recently destroyed marker trackers just in case
        std::deque<AftermathMarkerTracker> m_DestroyedMarkerTrackers;
        std::unordered_map<void*, ShaderBinaryLookupCallback> m_ShaderBinaryLookupCallbacks;
    };
} // namespace caustica::rhi