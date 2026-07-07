#pragma once

#include <assets/AssetId.h>

#include <cstddef>
#include <memory>

namespace caustica
{

template <typename T>
class Handle
{
public:
    Handle() = default;
    Handle(std::nullptr_t) {}
    Handle(AssetId id, std::shared_ptr<T> asset)
        : m_id(id)
        , m_asset(std::move(asset))
    {}

    [[nodiscard]] AssetId id() const { return m_id; }
    [[nodiscard]] T* get() const { return m_asset.get(); }
    [[nodiscard]] const std::shared_ptr<T>& shared() const { return m_asset; }
    [[nodiscard]] bool isValid() const { return m_id.isValid() && m_asset != nullptr; }

    explicit operator bool() const { return isValid(); }
    T& operator*() const { return *m_asset; }
    T* operator->() const { return m_asset.get(); }

    bool operator==(const Handle& other) const { return m_id == other.m_id; }
    bool operator!=(const Handle& other) const { return !(*this == other); }
    bool operator==(std::nullptr_t) const { return !isValid(); }
    bool operator!=(std::nullptr_t) const { return isValid(); }
    bool operator<(const Handle& other) const { return m_id < other.m_id; }

    Handle& operator=(std::nullptr_t)
    {
        m_id = AssetId::invalid();
        m_asset.reset();
        return *this;
    }

private:
    AssetId m_id = AssetId::invalid();
    std::shared_ptr<T> m_asset;
};

} // namespace caustica
