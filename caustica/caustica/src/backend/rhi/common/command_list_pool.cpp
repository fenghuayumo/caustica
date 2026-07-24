#include <rhi/command_list_pool.h>

#include <cassert>
#include <utility>

namespace caustica::rhi
{
    CommandListPool::CommandListPool(Device* device, CommandListParameters params)
        : m_device(device)
        , m_params(std::move(params))
    {
        assert(m_device);
        m_params.enableImmediateExecution = false;
    }

    CommandListPool::~CommandListPool()
    {
        clear();
    }

    CommandListHandle CommandListPool::acquire(CommandQueue queue)
    {
        assert(m_device);
        assert(queue < CommandQueue::Count);

        std::lock_guard lock(m_mutex);
        auto& freeList = m_free[size_t(queue)];
        if (!freeList.empty())
        {
            CommandListHandle list = std::move(freeList.back());
            freeList.pop_back();
            return list;
        }

        CommandListParameters params = m_params;
        params.queueType = queue;
        return m_device->createCommandList(params);
    }

    void CommandListPool::release(CommandListHandle list)
    {
        if (!list)
            return;

        const CommandQueue queue = list->getDesc().queueType;
        assert(queue < CommandQueue::Count);

        std::lock_guard lock(m_mutex);
        m_free[size_t(queue)].push_back(std::move(list));
    }

    void CommandListPool::clear()
    {
        std::lock_guard lock(m_mutex);
        for (auto& freeList : m_free)
            freeList.clear();
    }

    FrameCommandContext::FrameCommandContext(CommandListPool& pool, CommandQueue queue)
        : m_pool(&pool)
        , m_queue(queue)
    {
    }

    FrameCommandContext::~FrameCommandContext()
    {
        abort();
        if (m_primary)
        {
            m_pool->release(std::move(m_primary));
            m_primary = nullptr;
        }
    }

    CommandList* FrameCommandContext::ensurePrimary()
    {
        assert(m_pool);
        assert(!m_primaryOpen);
        if (!m_primary)
            m_primary = m_pool->acquire(m_queue);
        return m_primary.Get();
    }

    CommandList* FrameCommandContext::beginPrimary()
    {
        assert(m_pool);
        assert(!m_primaryOpen);
        if (!m_primary)
            m_primary = m_pool->acquire(m_queue);
        m_primary->open();
        m_primaryOpen = true;
        m_forks.clear();
        return m_primary.Get();
    }

    uint64_t FrameCommandContext::flushPrimary()
    {
        assert(m_primary);
        assert(m_primaryOpen);

        m_primary->close();
        m_primaryOpen = false;
        const uint64_t instance = m_pool->device()->executeCommandList(m_primary, m_queue);
        m_primary->open();
        m_primaryOpen = true;
        return instance;
    }

    CommandListHandle FrameCommandContext::fork()
    {
        assert(m_pool);
        CommandListHandle list = m_pool->acquire(m_queue);
        assert(list);
        list->open();
        m_forks.push_back(ForkEntry{ list, false });
        return list;
    }

    void FrameCommandContext::closeFork(CommandListHandle list)
    {
        assert(list);
        for (auto& entry : m_forks)
        {
            if (entry.list.Get() == list.Get())
            {
                assert(!entry.closed);
                entry.list->close();
                entry.closed = true;
                return;
            }
        }
        assert(!"closeFork: list was not forked from this context");
    }

    uint64_t FrameCommandContext::submitForks()
    {
        std::vector<CommandList*> toExecute;
        toExecute.reserve(m_forks.size());

        for (auto& entry : m_forks)
        {
            if (!entry.list)
                continue;
            if (!entry.closed)
            {
                entry.list->close();
                entry.closed = true;
            }
            toExecute.push_back(entry.list.Get());
        }

        uint64_t instance = 0;
        if (!toExecute.empty())
            instance = m_pool->device()->executeCommandLists(toExecute.data(), toExecute.size(), m_queue);

        for (auto& entry : m_forks)
        {
            if (entry.list)
                m_pool->release(std::move(entry.list));
        }
        m_forks.clear();
        return instance;
    }

    uint64_t FrameCommandContext::endFrame()
    {
        uint64_t instance = submitForks();

        if (m_primary && m_primaryOpen)
        {
            m_primary->close();
            m_primaryOpen = false;
            instance = m_pool->device()->executeCommandList(m_primary, m_queue);
        }

        return instance;
    }

    void FrameCommandContext::abort()
    {
        for (auto& entry : m_forks)
        {
            if (!entry.list)
                continue;
            if (!entry.closed)
            {
                entry.list->close();
                entry.closed = true;
            }
        }
        releaseForks();

        if (m_primary && m_primaryOpen)
        {
            m_primary->close();
            m_primaryOpen = false;
        }
    }

    void FrameCommandContext::releaseForks()
    {
        for (auto& entry : m_forks)
        {
            if (entry.list)
                m_pool->release(std::move(entry.list));
        }
        m_forks.clear();
    }
}
