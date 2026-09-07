#include <rhi/common/aftermath.h>

namespace caustica::rhi
{
    AftermathMarkerTracker::AftermathMarkerTracker() :
        m_eventStack{},
        m_eventHashes{},
        m_oldestHashIndex(0),
        m_eventStrings{}
    {
    }

    size_t AftermathMarkerTracker::pushEvent(const char* name)
    {
        m_eventStack.append(name);
        std::string eventString = m_eventStack.generic_string();
        size_t hash = std::hash<std::string>{}(eventString);
        if (m_eventStrings.find(hash) == m_eventStrings.end())
        {
            m_eventStrings.erase(m_eventHashes[m_oldestHashIndex]);
            m_eventStrings[hash] = eventString;
            m_eventHashes[m_oldestHashIndex] = hash;
            m_oldestHashIndex = (m_oldestHashIndex + 1) % MaxEventStrings;
        }
        return hash;
    }

    void AftermathMarkerTracker::popEvent()
    {
        m_eventStack = m_eventStack.parent_path();
    }

    const static std::string NotFoundMarkerString = "ERROR: could not resolve marker";

    std::pair<bool, std::reference_wrapper<const std::string>> AftermathMarkerTracker::getEventString(size_t hash)
    {
        auto const& found = m_eventStrings.find(hash);
        if (found != m_eventStrings.end())
        {
            return std::make_pair<bool, std::reference_wrapper<const std::string>>(true, found->second);
        }
        else
        {
            // could technically return a string literal according to the spec, but compiler complains, so using static
            return std::make_pair(false, NotFoundMarkerString);
        }
    }

    AftermathCrashDumpHelper::AftermathCrashDumpHelper():
        m_markerTrackers{},
        m_shaderBinaryLookupCallbacks{}
    {
    }

    void AftermathCrashDumpHelper::registerAftermathMarkerTracker(AftermathMarkerTracker* tracker)
    {
        m_markerTrackers.insert(tracker);
    }

    void AftermathCrashDumpHelper::unRegisterAftermathMarkerTracker(AftermathMarkerTracker* tracker)
    {
        // it's possible that a destroyed command list's markers might still be executing on the GPU,
        // so will keep the last few of them around to search in case of a crash
        const static size_t NumDestroyedMarkerTrackers = 2;
        if (m_destroyedMarkerTrackers.size() >= NumDestroyedMarkerTrackers)
        {
            m_destroyedMarkerTrackers.pop_front();
        }
        // copying by value to keep the tracker contents after command list is destroyed
        m_destroyedMarkerTrackers.push_back(*tracker);
        m_markerTrackers.erase(tracker);
    }

    void AftermathCrashDumpHelper::registerShaderBinaryLookupCallback(void* client, ShaderBinaryLookupCallback lookupCallback)
    {
        m_shaderBinaryLookupCallbacks[client] = lookupCallback;
    }

    void AftermathCrashDumpHelper::unRegisterShaderBinaryLookupCallback(void* client)
    {
        m_shaderBinaryLookupCallbacks.erase(client);
    }

    ResolvedMarker AftermathCrashDumpHelper::ResolveMarker(size_t markerHash)
    {
        for (auto markerTracker : m_markerTrackers)
        {
            auto [found, markerString] = markerTracker->getEventString(markerHash);
            if (found)
                return std::make_pair(found, markerString);
        }
        for (auto markerTracker : m_destroyedMarkerTrackers)
        {
            auto [found, markerString] = markerTracker.getEventString(markerHash);
            if (found)
                return std::make_pair(found, markerString);
        }
        return std::make_pair(false, NotFoundMarkerString);
    }

    BinaryBlob AftermathCrashDumpHelper::findShaderBinary(uint64_t shaderHash, ShaderHashGeneratorFunction hashGenerator)
    {
        for (auto shaderLookupClientCallback : m_shaderBinaryLookupCallbacks)
        {
            auto [ptr, size] = shaderLookupClientCallback.second(shaderHash, hashGenerator);
            if (size > 0)
            {
                return std::make_pair(ptr, size);
            }
        }
        return std::make_pair(nullptr, 0);
    }



} // namespace caustica::rhi
