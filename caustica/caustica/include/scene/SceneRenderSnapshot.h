#pragma once

#include <scene/SceneRenderData.h>

#include <atomic>
#include <cstdint>

namespace caustica::scene
{
    struct SceneRenderPublishState
    {
        bool structureChanged = false;
        bool transformsChanged = false;
        uint32_t frameIndex = 0;
    };

    // Double-buffered render extract. The logic thread writes writeBuffer(); the render
    // thread reads readBuffer() after publish().
    class SceneRenderSnapshot
    {
    public:
        [[nodiscard]] const SceneRenderData& readBuffer() const
        {
            return m_buffers[m_readIndex.load(std::memory_order_acquire)];
        }

        [[nodiscard]] SceneRenderData& writeBuffer()
        {
            return m_buffers[m_writeIndex];
        }

        [[nodiscard]] SceneRenderPublishState& pendingState()
        {
            return m_pendingState;
        }

        [[nodiscard]] const SceneRenderPublishState& publishedState() const
        {
            return m_publishedState;
        }

        void publish()
        {
            m_publishedState = m_pendingState;
            const uint32_t newRead = m_writeIndex;
            m_writeIndex = m_readIndex.load(std::memory_order_relaxed);
            m_readIndex.store(newRead, std::memory_order_release);
        }

        void clear()
        {
            m_buffers[0].clear();
            m_buffers[1].clear();
            m_pendingState = {};
            m_publishedState = {};
            m_readIndex.store(0, std::memory_order_release);
            m_writeIndex = 1;
        }

    private:
        SceneRenderData m_buffers[2];
        std::atomic<uint32_t> m_readIndex{0};
        uint32_t m_writeIndex = 1;
        SceneRenderPublishState m_pendingState;
        SceneRenderPublishState m_publishedState;
    };

} // namespace caustica::scene
