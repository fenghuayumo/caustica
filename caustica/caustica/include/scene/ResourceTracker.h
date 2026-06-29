#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <unordered_map>

namespace caustica
{
    // A container that tracks unique resources of the same type used by some entity, for example unique meshes used in a scene.
    // It works by putting the resource shared pointers into a map and associating a reference count with each resource.
    // When the resource is added and released an equal number of times, its reference count reaches zero, and it's removed from the container.
    template<typename T>
    class ResourceTracker
    {
    private:
        std::unordered_map<std::shared_ptr<T>, uint32_t> m_Map;
        using UnderlyingConstIterator = typename std::unordered_map<std::shared_ptr<T>, uint32_t>::const_iterator;

    public:
        class ConstIterator
        {
        private:
            UnderlyingConstIterator m_Iter;
        public:
            ConstIterator(UnderlyingConstIterator iter) : m_Iter(std::move(iter)) {}
            ConstIterator& operator++() { ++m_Iter; return *this; }
            ConstIterator operator++(int) { ConstIterator res = *this; ++m_Iter; return res; }
            bool operator==(ConstIterator other) const { return m_Iter == other.m_Iter; }
            bool operator!=(ConstIterator other) const { return !(*this == other); }
            const std::shared_ptr<T>& operator*() { return m_Iter->first; }
        };

        // Adds a reference to the specified resource.
        // Returns true if this is the first reference, i.e. if the resource has just been added to the tracker.
        bool AddRef(const std::shared_ptr<T>& resource)
        {
            if (!resource) return false;
            uint32_t refCount = ++m_Map[resource];
            return (refCount == 1);
        }

        // Removes a reference from the specified resource.
        // Returns true if this was the last reference, i.e. if the resource has just been removed from the tracker.
        bool Release(const std::shared_ptr<T>& resource)
        {
            if (!resource) return false;
            auto it = m_Map.find(resource);
            if (it == m_Map.end())
            {
                assert(false); // trying to release an object not owned by this tracker
                return false;
            }

            if (it->second == 0)
                assert(false); // zero-reference entries should not be possible; might indicate concurrency issues
            else
                --it->second;

            if (it->second == 0)
            {
                m_Map.erase(it);
                return true;
            }
            return false;
        }

        [[nodiscard]] ConstIterator begin() const { return ConstIterator(m_Map.cbegin()); }
        [[nodiscard]] ConstIterator end() const { return ConstIterator(m_Map.cend()); }
        [[nodiscard]] bool empty() const { return m_Map.empty(); }
        [[nodiscard]] size_t size() const { return m_Map.size(); }
    };
}
