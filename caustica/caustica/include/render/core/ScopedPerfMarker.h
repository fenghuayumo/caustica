#pragma once

#include <rhi/rhi.h>

// GPU performance marker using RHI command list begin/end marker.
// Moved from editor/SampleCommon/SampleCommon.h
class ScopedPerfMarker
{
public:
    ScopedPerfMarker(const char* label, caustica::rhi::CommandListHandle cmdList)
        : m_cmdList(cmdList)
    {
        cmdList->beginMarker(label);
    }
    ~ScopedPerfMarker()
    {
        m_cmdList->endMarker();
    }
private:
    caustica::rhi::CommandListHandle m_cmdList;
};
