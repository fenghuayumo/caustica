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
        bool lightsChanged = false;
        uint32_t frameIndex = 0;
        uint64_t structureGeneration = 0;
    };

    // Frame-indexed triple buffer for pipelined extract (RenderThread::kMaxInFlightFrames == 2).
    // The logic thread writes via extractAndPublishRenderSnapshot(); the render thread reads the
    // slot for its render-phase frame index.
    class SceneRenderSnapshot
    {
    public:
        static constexpr uint32_t kSlotCount = 3;

        [[nodiscard]] static uint32_t slotForFrame(uint32_t frameIndex) { return frameIndex % kSlotCount; }

        [[nodiscard]] SceneRenderData& bufferForFrame(uint32_t frameIndex)
        {
            return m_buffers[slotForFrame(frameIndex)];
        }

        [[nodiscard]] const SceneRenderData& readBufferForFrame(uint32_t frameIndex) const
        {
            return m_buffers[slotForFrame(frameIndex)];
        }

        [[nodiscard]] const SceneRenderData& readBufferForFrameOrLatest(uint32_t frameIndex) const
        {
            const uint32_t slot = slotForFrame(frameIndex);
            if (m_extractedFrameIndex[slot].load(std::memory_order_acquire) == frameIndex)
                return m_buffers[slot];

            // A skipped Extract leaves unrelated data in this frame's ring slot.
            // Reuse the most recently published packet instead of replaying a
            // three-frame-old camera/settings packet (including one-shot resets).
            const uint32_t latest = m_latestExtractedFrameIndex.load(std::memory_order_acquire);
            if (latest != UINT32_MAX)
                return m_buffers[slotForFrame(latest)];

            // Before the first publish all buffers are default-initialized.
            return m_buffers[slot];
        }

        [[nodiscard]] SceneRenderData& writeBufferForFrame(uint32_t frameIndex)
        {
            return m_buffers[slotForFrame(frameIndex)];
        }

        [[nodiscard]] SceneRenderPublishState& pendingState()
        {
            return m_pendingState;
        }

        [[nodiscard]] const SceneRenderPublishState& publishedStateForFrame(uint32_t frameIndex) const
        {
            return m_publishedStates[slotForFrame(frameIndex)];
        }

        void publish(uint32_t frameIndex)
        {
            const uint32_t slot = slotForFrame(frameIndex);
            m_pendingState.frameIndex = frameIndex;
            m_publishedStates[slot] = m_pendingState;
            m_extractedFrameIndex[slot].store(frameIndex, std::memory_order_release);
            m_latestExtractedFrameIndex.store(frameIndex, std::memory_order_release);
        }

        [[nodiscard]] bool wasExtractedForFrame(uint32_t frameIndex) const
        {
            const uint32_t slot = slotForFrame(frameIndex);
            return m_extractedFrameIndex[slot].load(std::memory_order_acquire) == frameIndex;
        }

        [[nodiscard]] uint32_t latestExtractedFrameIndex() const
        {
            return m_latestExtractedFrameIndex.load(std::memory_order_acquire);
        }

        void clear()
        {
            for (uint32_t i = 0; i < kSlotCount; ++i)
            {
                m_buffers[i].clear();
                m_publishedStates[i] = {};
                m_extractedFrameIndex[i].store(UINT32_MAX, std::memory_order_release);
            }
            m_pendingState = {};
            m_latestExtractedFrameIndex.store(UINT32_MAX, std::memory_order_release);
        }

    private:
        SceneRenderData m_buffers[kSlotCount];
        SceneRenderPublishState m_pendingState;
        SceneRenderPublishState m_publishedStates[kSlotCount];
        std::atomic<uint32_t> m_extractedFrameIndex[kSlotCount]{UINT32_MAX, UINT32_MAX, UINT32_MAX};
        std::atomic<uint32_t> m_latestExtractedFrameIndex{UINT32_MAX};
    };

} // namespace caustica::scene
