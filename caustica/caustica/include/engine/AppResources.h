#pragma once

#include <memory>
#include <typeindex>
#include <type_traits>
#include <unordered_map>

namespace caustica
{

// Type-indexed resource store (Bevy-style World resources at App scope).
class AppResources
{
public:
    template<typename T, typename... Args>
    T& emplace(Args&&... args)
    {
        static_assert(std::is_same_v<T, std::remove_cv_t<T>>, "Resource type must be non-const");
        auto owned = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *owned;
        insertOwned(std::type_index(typeid(T)), std::move(owned));
        return ref;
    }

    template<typename T>
    T& insert(T value)
    {
        return emplace<T>(std::move(value));
    }

    template<typename T>
    T& insertRef(T& resource)
    {
        static_assert(std::is_same_v<T, std::remove_cv_t<T>>, "Resource type must be non-const");
        m_refs[std::type_index(typeid(T))] = &resource;
        return resource;
    }

    template<typename T>
    const T& insertRef(const T& resource)
    {
        m_refs[std::type_index(typeid(T))] = const_cast<T*>(&resource);
        return resource;
    }

    template<typename T>
    [[nodiscard]] T& get()
    {
        return *static_cast<T*>(resourcePtr(std::type_index(typeid(T))));
    }

    template<typename T>
    [[nodiscard]] const T& get() const
    {
        return *static_cast<const T*>(resourcePtr(std::type_index(typeid(T))));
    }

    template<typename T>
    [[nodiscard]] T* tryGet()
    {
        if (void* ptr = tryResourcePtr(std::type_index(typeid(T))))
            return static_cast<T*>(ptr);
        return nullptr;
    }

    template<typename T>
    [[nodiscard]] const T* tryGet() const
    {
        if (const void* ptr = tryResourcePtr(std::type_index(typeid(T))))
            return static_cast<const T*>(ptr);
        return nullptr;
    }

    template<typename T>
    [[nodiscard]] bool contains() const
    {
        return tryResourcePtr(std::type_index(typeid(T))) != nullptr;
    }

    void clear();

private:
    struct IResourceBox
    {
        virtual ~IResourceBox() = default;
        [[nodiscard]] virtual void* ptr() = 0;
    };

    template<typename T>
    struct ResourceBox final : IResourceBox
    {
        explicit ResourceBox(std::unique_ptr<T> valueIn)
            : value(std::move(valueIn))
        {
        }

        void* ptr() override { return value.get(); }

        std::unique_ptr<T> value;
    };

    void insertOwned(std::type_index type, std::unique_ptr<IResourceBox> box);
    [[nodiscard]] void* resourcePtr(std::type_index type);
    [[nodiscard]] const void* resourcePtr(std::type_index type) const;
    [[nodiscard]] void* tryResourcePtr(std::type_index type);
    [[nodiscard]] const void* tryResourcePtr(std::type_index type) const;

    std::unordered_map<std::type_index, std::unique_ptr<IResourceBox>> m_owned;
    std::unordered_map<std::type_index, void*> m_refs;
};

} // namespace caustica
