#pragma once

#include <rhi/rhi_types.h>

#include <array>
#include <mutex>
#include <vector>

namespace caustica::rhi
{
    // Thread-safe pool of deferred CommandList objects.
    // Acquire/release are free-threaded; execute/present/GC remain render-thread-only.
    class CommandListPool
    {
    public:
        explicit CommandListPool(Device* device, CommandListParameters params = CommandListParameters());
        ~CommandListPool();

        CommandListPool(const CommandListPool&) = delete;
        CommandListPool& operator=(const CommandListPool&) = delete;

        [[nodiscard]] Device* device() const { return m_device; }
        [[nodiscard]] const CommandListParameters& parameters() const { return m_params; }

        CommandListHandle acquire(CommandQueue queue = CommandQueue::Graphics);
        void release(CommandListHandle list);
        void clear();

    private:
        Device* m_device = nullptr;
        CommandListParameters m_params{};
        std::mutex m_mutex;
        std::array<std::vector<CommandListHandle>, size_t(CommandQueue::Count)> m_free;
    };

    // Frame owner for one primary deferred list + optional forked worker lists.
    // Render thread: beginPrimary / fork / submitForks / endFrame / flushPrimary.
    // Workers: record only into lists returned by fork().
    class FrameCommandContext
    {
    public:
        explicit FrameCommandContext(CommandListPool& pool, CommandQueue queue = CommandQueue::Graphics);
        ~FrameCommandContext();

        FrameCommandContext(const FrameCommandContext&) = delete;
        FrameCommandContext& operator=(const FrameCommandContext&) = delete;

        [[nodiscard]] CommandListPool& pool() { return *m_pool; }
        [[nodiscard]] Device* device() const { return m_pool->device(); }
        [[nodiscard]] CommandQueue queue() const { return m_queue; }

        // Ensure a closed primary exists (for pre-frame open/close/execute helpers).
        CommandList* ensurePrimary();

        // Open primary for the frame (acquires if needed).
        CommandList* beginPrimary();
        [[nodiscard]] CommandListHandle primaryHandle() const { return m_primary; }
        [[nodiscard]] CommandList* primary() const { return m_primary.Get(); }
        [[nodiscard]] bool primaryOpen() const { return m_primaryOpen; }

        // close -> execute -> open (init sync-points). Does not waitForIdle.
        uint64_t flushPrimary();

        CommandListHandle fork();
        void closeFork(CommandListHandle list);
        uint64_t submitForks();

        // Close+execute primary; keep the handle for the next frame.
        uint64_t endFrame();

        void abort();

    private:
        void releaseForks();

        CommandListPool* m_pool = nullptr;
        CommandQueue m_queue = CommandQueue::Graphics;
        CommandListHandle m_primary;
        bool m_primaryOpen = false;

        struct ForkEntry
        {
            CommandListHandle list;
            bool closed = false;
        };
        std::vector<ForkEntry> m_forks;
    };
}
