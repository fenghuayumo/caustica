#pragma once

#include <rhi/nvrhi.h>

// GPU performance marker using NVRHI command list begin/end marker.
// Moved from editor/SampleCommon/SampleCommon.h
class ScopedPerfMarker
{
public:
    ScopedPerfMarker(const char* label, nvrhi::CommandListHandle cmdList)
        : m_cmdList(cmdList)
    {
        cmdList->beginMarker(label);
    }
    ~ScopedPerfMarker()
    {
        m_cmdList->endMarker();
    }
private:
    nvrhi::CommandListHandle m_cmdList;
};
