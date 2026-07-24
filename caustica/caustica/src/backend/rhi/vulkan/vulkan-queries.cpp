#include "vulkan-backend.h"
#include <rhi/common/misc.h>

namespace caustica::rhi::vulkan
{

    EventQueryHandle Device::createEventQuery(void)
    {
        EventQuery *query = new EventQuery();
        return EventQueryHandle::Create(query);
    }

    void Device::setEventQuery(rhi::EventQuery* _query, CommandQueue queue)
    {
        EventQuery* query = checked_cast<EventQuery*>(_query);

        assert(query->commandListID == 0);

        query->queue = queue;
        query->commandListID = m_Queues[uint32_t(queue)]->getLastSubmittedID();
    }

    bool Device::pollEventQuery(rhi::EventQuery* _query)
    {
        EventQuery* query = checked_cast<EventQuery*>(_query);
        
        auto& queue = *m_Queues[uint32_t(query->queue)];

        return queue.pollCommandList(query->commandListID);
    }

    void Device::waitEventQuery(rhi::EventQuery* _query)
    {
        EventQuery* query = checked_cast<EventQuery*>(_query);

        if (query->commandListID == 0)
            return;

        auto& queue = *m_Queues[uint32_t(query->queue)];

        bool success = queue.waitCommandList(query->commandListID, ~0ull);
        assert(success);
        (void)success;
    }

    void Device::resetEventQuery(rhi::EventQuery* _query)
    {
        EventQuery* query = checked_cast<EventQuery*>(_query);

        query->commandListID = 0;
    }


    TimerQueryHandle Device::createTimerQuery(void)
    {
        if (!m_TimerQueryPool)
        {
            std::lock_guard lockGuard(m_Mutex);

            if (!m_TimerQueryPool)
            {
                // set up the timer query pool on first use
                auto poolInfo = vk::QueryPoolCreateInfo()
                    .setQueryType(vk::QueryType::eTimestamp)
                    .setQueryCount(uint32_t(m_TimerQueryAllocator.getCapacity()) * 2); // use 2 Vulkan queries per 1 TimerQuery

                const vk::Result res = m_Context.device.createQueryPool(&poolInfo, m_Context.allocationCallbacks, &m_TimerQueryPool);
                CHECK_VK_FAIL(res)
            }
        }

        int queryIndex = m_TimerQueryAllocator.allocate();

        if (queryIndex < 0)
        {
            m_Context.error("Insufficient query pool space, increase Device::numTimerQueries");
            return nullptr;
        }

        TimerQuery* query = new TimerQuery(m_TimerQueryAllocator);
        query->beginQueryIndex = queryIndex * 2;
        query->endQueryIndex = queryIndex * 2 + 1;

        return TimerQueryHandle::Create(query);
    }

    TimerQuery::~TimerQuery()
    {
        m_QueryAllocator.release(beginQueryIndex / 2);
        beginQueryIndex = -1;
        endQueryIndex = -1;
    }

    void CommandList::beginTimerQuery(rhi::TimerQuery* _query)
    {
        endRenderPass();

        TimerQuery* query = checked_cast<TimerQuery*>(_query);

        assert(query->beginQueryIndex >= 0);
        assert(!query->started);
        assert(m_CurrentCmdBuf);

        query->resolved = false;

        m_CurrentCmdBuf->cmdBuf.resetQueryPool(m_Device->getTimerQueryPool(), query->beginQueryIndex, 2);
        m_CurrentCmdBuf->cmdBuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_Device->getTimerQueryPool(), query->beginQueryIndex);
    }

    void CommandList::endTimerQuery(rhi::TimerQuery* _query)
    {
        endRenderPass();

        TimerQuery* query = checked_cast<TimerQuery*>(_query);

        assert(query->endQueryIndex >= 0);
        assert(!query->started);
        assert(!query->resolved);

        assert(m_CurrentCmdBuf);

        m_CurrentCmdBuf->cmdBuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_Device->getTimerQueryPool(), query->endQueryIndex);
        query->started = true;
    }

    bool Device::pollTimerQuery(rhi::TimerQuery* _query)
    {
        TimerQuery* query = checked_cast<TimerQuery*>(_query);

        if (!query->started)
        {
            return false;
        }

        if (query->resolved)
        {
            return true;
        }

        uint32_t timestamps[2] = { 0, 0 };

        vk::Result res;
        res = m_Context.device.getQueryPoolResults(m_TimerQueryPool,
                                                 query->beginQueryIndex, 2,
                                                 sizeof(timestamps), timestamps,
                                                 sizeof(timestamps[0]), vk::QueryResultFlags());
        assert(res == vk::Result::eSuccess || res == vk::Result::eNotReady || res == vk::Result::eErrorDeviceLost);

        if (res == vk::Result::eNotReady || res == vk::Result::eErrorDeviceLost)
        {
            return false;
        }

        const auto timestampPeriod = m_Context.physicalDeviceProperties.limits.timestampPeriod; // in nanoseconds
        const float scale = 1e-9f * timestampPeriod;

        query->time = float(timestamps[1] - timestamps[0]) * scale;
        query->resolved = true;
        return true;
    }

    float Device::getTimerQueryTime(rhi::TimerQuery* _query)
    {
        TimerQuery* query = checked_cast<TimerQuery*>(_query);

        if (!query->started)
            return 0.f;

        if (!query->resolved)
        {
            while(!pollTimerQuery(query))
                ;
        }

        query->started = false;

        assert(query->resolved);
        return query->time;
    }

    void Device::resetTimerQuery(rhi::TimerQuery* _query)
    {
        TimerQuery* query = checked_cast<TimerQuery*>(_query);

        query->started = false;
        query->resolved = false;
        query->time = 0.f;
    }


    void CommandList::beginMarker(const char* name)
    {
        if (m_Context.extensions.EXT_debug_utils)
        {
            assert(m_CurrentCmdBuf);

            auto label = vk::DebugUtilsLabelEXT()
                            .setPLabelName(name);
            m_CurrentCmdBuf->cmdBuf.beginDebugUtilsLabelEXT(&label);
        }
        else if (m_Context.extensions.EXT_debug_marker)
        {
            assert(m_CurrentCmdBuf);

            auto markerInfo = vk::DebugMarkerMarkerInfoEXT()
                                .setPMarkerName(name);
            m_CurrentCmdBuf->cmdBuf.debugMarkerBeginEXT(&markerInfo);
        }
        
#if CAUSTICA_RHI_WITH_AFTERMATH
        if (m_Device->isAftermathEnabled())
        {
            const size_t aftermathMarker = m_AftermathTracker.pushEvent(name);
            m_CurrentCmdBuf->cmdBuf.setCheckpointNV((const void*)aftermathMarker);
        }
#endif
    }

    void CommandList::endMarker()
    {
        if (m_Context.extensions.EXT_debug_utils)
        {
            assert(m_CurrentCmdBuf);

            m_CurrentCmdBuf->cmdBuf.endDebugUtilsLabelEXT();
        }
        else if (m_Context.extensions.EXT_debug_marker)
        {
            assert(m_CurrentCmdBuf);

            m_CurrentCmdBuf->cmdBuf.debugMarkerEndEXT();
        }
        
#if CAUSTICA_RHI_WITH_AFTERMATH
        m_AftermathTracker.popEvent();
#endif
    }

} // namespace caustica::rhi::vulkan