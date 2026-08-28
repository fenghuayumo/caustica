#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace caustica::ecs
{

// Dense process-wide type ids. EnTT identifies components purely by template
// parameter, but the system scheduler needs a compact runtime id it can pack
// into a bitset to test read/write overlap between systems.
using TypeId = uint32_t;

inline constexpr TypeId kInvalidTypeId = ~TypeId{ 0 };

class TypeIdRegistry
{
public:
    TypeId intern(const std::type_info& info)
    {
        const std::type_index key(info);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (const auto it = m_ids.find(key); it != m_ids.end())
            return it->second;

        const TypeId id = static_cast<TypeId>(m_names.size());
        m_ids.emplace(key, id);
        m_names.push_back(info.name());
        return id;
    }

    [[nodiscard]] const char* name(TypeId id) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return id < m_names.size() ? m_names[id] : "<unknown>";
    }

    [[nodiscard]] std::size_t count() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_names.size();
    }

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::type_index, TypeId> m_ids;
    std::vector<const char*> m_names;
};

inline TypeIdRegistry& typeIdRegistry()
{
    static TypeIdRegistry registry;
    return registry;
}

template<typename T>
[[nodiscard]] TypeId typeId()
{
    using Raw = std::remove_cv_t<std::remove_reference_t<T>>;
    static const TypeId id = typeIdRegistry().intern(typeid(Raw));
    return id;
}

// Growable bitset over TypeId. Access sets are tiny (a handful of components per
// system), so the word vector rarely grows past one or two entries.
class AccessMask
{
public:
    void set(TypeId id)
    {
        const std::size_t word = id / kBitsPerWord;
        if (word >= m_words.size())
            m_words.resize(word + 1, 0);
        m_words[word] |= Word{ 1 } << (id % kBitsPerWord);
    }

    [[nodiscard]] bool test(TypeId id) const
    {
        const std::size_t word = id / kBitsPerWord;
        if (word >= m_words.size())
            return false;
        return (m_words[word] & (Word{ 1 } << (id % kBitsPerWord))) != 0;
    }

    [[nodiscard]] bool empty() const
    {
        for (Word word : m_words)
        {
            if (word != 0)
                return false;
        }
        return true;
    }

    [[nodiscard]] bool intersects(const AccessMask& other) const
    {
        const std::size_t shared = m_words.size() < other.m_words.size()
            ? m_words.size()
            : other.m_words.size();
        for (std::size_t i = 0; i < shared; ++i)
        {
            if ((m_words[i] & other.m_words[i]) != 0)
                return true;
        }
        return false;
    }

    void merge(const AccessMask& other)
    {
        if (other.m_words.size() > m_words.size())
            m_words.resize(other.m_words.size(), 0);
        for (std::size_t i = 0; i < other.m_words.size(); ++i)
            m_words[i] |= other.m_words[i];
    }

    void clear() { m_words.clear(); }

    template<typename Fn>
    void forEach(Fn&& fn) const
    {
        for (std::size_t word = 0; word < m_words.size(); ++word)
        {
            Word bits = m_words[word];
            while (bits != 0)
            {
                const Word lowest = bits & (~bits + 1);
                TypeId bit = 0;
                Word probe = lowest;
                while (probe > 1)
                {
                    probe >>= 1;
                    ++bit;
                }
                fn(static_cast<TypeId>(word * kBitsPerWord + bit));
                bits ^= lowest;
            }
        }
    }

private:
    using Word = uint64_t;
    static constexpr std::size_t kBitsPerWord = 64;

    std::vector<Word> m_words;
};

} // namespace caustica::ecs
