#pragma once

#include <cstdint>
#include <cassert>

namespace caustica::rhi
{
    template<typename T> T align(T size, T alignment)
    {
        return (size + alignment - 1) & ~(alignment - 1);
    }

    template<typename T, typename U> [[nodiscard]] bool arraysAreDifferent(const T& a, const U& b)
    {
        if (a.size() != b.size())
            return true;

        for (uint32_t i = 0; i < uint32_t(a.size()); i++)
        {
            if (a[i] != b[i])
                return true;
        }

        return false;
    }

    template<typename T, typename U> [[nodiscard]] uint32_t arrayDifferenceMask(const T& a, const U& b)
    {
        assert(a.size() <= 32);
        assert(b.size() <= 32);

        if (a.size() != b.size())
            return ~0u;

        uint32_t mask = 0;
        for (uint32_t i = 0; i < uint32_t(a.size()); i++)
        {
            if (a[i] != b[i])
                mask |= (1 << i);
        }

        return mask;
    }

    inline uint32_t hash_to_u32(size_t hash)
    {
        return uint32_t(hash) ^ (uint32_t(hash >> 32));
    }

    // A type cast that is safer than static_cast in debug builds, and is a simple static_cast in release builds.
    // Used for downcasting various ISomething* pointers to their implementation classes in the backends.
    template <typename T, typename U>
    T checked_cast(U u)
    {
        static_assert(!std::is_same<T, U>::value, "Redundant checked_cast");
#ifdef _DEBUG
        if (!u) return nullptr;
        T t = dynamic_cast<T>(u);
        if (!t) assert(!"Invalid type cast");  // NOLINT(clang-diagnostic-string-conversion)
        return t;
#else
        return static_cast<T>(u);
#endif
    }
} // namespace caustica::rhi 