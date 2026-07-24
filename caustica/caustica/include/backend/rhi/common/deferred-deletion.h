#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace caustica::rhi
{
    // Retires CPU-side GPU object cleanup after a queue fence/timeline value completes.
    // Enqueue is free-threaded; flush should run from the render-thread GC path.
    class DeferredDeletionQueue
    {
    public:
        using IsCompleteFn = bool (*)(void* context, uint64_t fenceValue);

        void enqueue(void* fenceContext, uint64_t fenceValue, IsCompleteFn isComplete, std::function<void()> destroy)
        {
            if (!destroy)
                return;

            if (!fenceContext || !isComplete || isComplete(fenceContext, fenceValue))
            {
                destroy();
                return;
            }

            std::lock_guard lock(m_Mutex);
            m_Items.push_back(Item{ fenceContext, fenceValue, isComplete, std::move(destroy) });
        }

        void flush()
        {
            std::vector<Item> ready;
            {
                std::lock_guard lock(m_Mutex);
                std::vector<Item> remaining;
                remaining.reserve(m_Items.size());
                for (auto& item : m_Items)
                {
                    if (item.isComplete && item.isComplete(item.fenceContext, item.fenceValue))
                        ready.push_back(std::move(item));
                    else
                        remaining.push_back(std::move(item));
                }
                m_Items.swap(remaining);
            }

            for (auto& item : ready)
            {
                if (item.destroy)
                    item.destroy();
            }
        }

        void flushAllBlocking()
        {
            std::vector<Item> items;
            {
                std::lock_guard lock(m_Mutex);
                items.swap(m_Items);
            }
            for (auto& item : items)
            {
                if (item.destroy)
                    item.destroy();
            }
        }

    private:
        struct Item
        {
            void* fenceContext = nullptr;
            uint64_t fenceValue = 0;
            IsCompleteFn isComplete = nullptr;
            std::function<void()> destroy;
        };

        std::mutex m_Mutex;
        std::vector<Item> m_Items;
    };
}
