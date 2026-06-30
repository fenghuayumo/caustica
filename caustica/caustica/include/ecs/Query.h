#pragma once

#include <ecs/ChangeDetection.h>
#include <ecs/QueryFilters.h>

#include <entt/entity/registry.hpp>

#include <tuple>
#include <type_traits>
#include <utility>

namespace caustica::ecs::detail
{

template<typename... Ts>
struct concat;

template<typename... As, typename... Bs>
struct concat<std::tuple<As...>, std::tuple<Bs...>>
{
    using type = std::tuple<As..., Bs...>;
};

template<typename Tuple, typename T>
struct tuple_contains;

template<typename T>
struct tuple_contains<std::tuple<>, T> : std::false_type
{
};

template<typename T, typename Head, typename... Tail>
struct tuple_contains<std::tuple<Head, Tail...>, T>
    : std::bool_constant<std::is_same_v<Head, T> || tuple_contains<std::tuple<Tail...>, T>::value>
{
};

template<typename Tuple, typename T>
using tuple_push_unique_t = std::conditional_t<
    tuple_contains<Tuple, T>::value,
    Tuple,
    typename concat<Tuple, std::tuple<T>>::type>;

template<typename Tuple>
struct query_traits
{
    using fetch = Tuple;
    using with_extra = std::tuple<>;
    using without = std::tuple<>;
    using changed = std::tuple<>;
    using added = std::tuple<>;
};

template<typename Head, typename Rest>
struct with_extra_for
{
    using type = typename Rest::with_extra;
};

template<typename T, typename Rest>
struct with_extra_for<With<T>, Rest>
{
    using type = typename concat<std::tuple<T>, typename Rest::with_extra>::type;
};

template<typename Head, typename Rest>
struct without_for
{
    using type = typename Rest::without;
};

template<typename T, typename Rest>
struct without_for<Without<T>, Rest>
{
    using type = typename concat<std::tuple<T>, typename Rest::without>::type;
};

template<typename Head, typename Rest>
struct changed_for
{
    using type = typename Rest::changed;
};

template<typename T, typename Rest>
struct changed_for<Changed<T>, Rest>
{
    using type = typename concat<std::tuple<T>, typename Rest::changed>::type;
};

template<typename Head, typename Rest>
struct added_for
{
    using type = typename Rest::added;
};

template<typename T, typename Rest>
struct added_for<Added<T>, Rest>
{
    using type = typename concat<std::tuple<T>, typename Rest::added>::type;
};

template<typename Head, typename Rest>
struct fetch_for
{
    using type = typename concat<std::tuple<Head>, typename Rest::fetch>::type;
};

template<typename Head, typename Rest>
struct fetch_for_head;

template<typename T, typename Rest>
struct fetch_for_head<With<T>, Rest>
{
    using type = typename Rest::fetch;
};

template<typename T, typename Rest>
struct fetch_for_head<Without<T>, Rest>
{
    using type = typename Rest::fetch;
};

template<typename T, typename Rest>
struct fetch_for_head<Changed<T>, Rest>
{
    using type = typename Rest::fetch;
};

template<typename T, typename Rest>
struct fetch_for_head<Added<T>, Rest>
{
    using type = typename Rest::fetch;
};

template<typename Head, typename Rest>
struct fetch_for_head
{
    using type = typename fetch_for<Head, Rest>::type;
};

template<typename Head, typename... Tail>
struct query_traits<std::tuple<Head, Tail...>>
{
private:
    using rest_traits = query_traits<std::tuple<Tail...>>;

public:
    using fetch = typename fetch_for_head<Head, rest_traits>::type;
    using with_extra = typename with_extra_for<Head, rest_traits>::type;
    using without = typename without_for<Head, rest_traits>::type;
    using changed = typename changed_for<Head, rest_traits>::type;
    using added = typename added_for<Head, rest_traits>::type;
};

template<typename Tuple, typename... Extra>
struct merge_fetch_tuple;

template<typename Tuple>
struct merge_fetch_tuple<Tuple>
{
    using type = Tuple;
};

template<typename Tuple, typename Head, typename... Tail>
struct merge_fetch_tuple<Tuple, Head, Tail...>
{
    using type = typename merge_fetch_tuple<tuple_push_unique_t<Tuple, Head>, Tail...>::type;
};

template<typename Tuple, typename... Extra>
struct merge_fetch_tuple<Tuple, std::tuple<Extra...>>
    : merge_fetch_tuple<Tuple, Extra...>
{
};

template<typename Fetch, typename WithExtra, typename Changed, typename Added>
struct finalize_fetch
{
    using with_merged = typename merge_fetch_tuple<Fetch, WithExtra>::type;
    using with_changed = typename merge_fetch_tuple<with_merged, Changed>::type;
    using type = typename merge_fetch_tuple<with_changed, Added>::type;
};

template<typename... Args>
struct query_descriptor
{
    using traits = query_traits<std::tuple<Args...>>;
    using fetch = typename finalize_fetch<
        typename traits::fetch,
        typename traits::with_extra,
        typename traits::changed,
        typename traits::added>::type;
    using without = typename traits::without;
    using changed = typename traits::changed;
    using added = typename traits::added;
};

template<typename Tuple, std::size_t... Is>
bool passes_changed_filters(
    const ChangeDetection* changeDetection,
    const entt::registry& registry,
    Entity entity,
    std::index_sequence<Is...>)
{
    if (!changeDetection)
        return true;
    return (... && changeDetection->isChangedThisFrame<std::tuple_element_t<Is, Tuple>>(entity, registry));
}

template<typename Tuple, std::size_t... Is>
bool passes_added_filters(
    const ChangeDetection* changeDetection,
    const entt::registry& registry,
    Entity entity,
    std::index_sequence<Is...>)
{
    if (!changeDetection)
        return true;
    return (... && changeDetection->isAddedThisFrame<std::tuple_element_t<Is, Tuple>>(entity, registry));
}

template<typename ChangedTuple, typename AddedTuple>
bool passes_tick_filters(const ChangeDetection* changeDetection, const entt::registry& registry, Entity entity)
{
    if constexpr (std::tuple_size_v<ChangedTuple> > 0)
    {
        if (!passes_changed_filters<ChangedTuple>(
                changeDetection,
                registry,
                entity,
                std::make_index_sequence<std::tuple_size_v<ChangedTuple>>{}))
            return false;
    }

    if constexpr (std::tuple_size_v<AddedTuple> > 0)
    {
        if (!passes_added_filters<AddedTuple>(
                changeDetection,
                registry,
                entity,
                std::make_index_sequence<std::tuple_size_v<AddedTuple>>{}))
            return false;
    }

    return true;
}

template<typename Desc, typename Func, typename FetchTuple, typename WithoutTuple>
struct EachInvoker;

template<typename Desc, typename Func, typename... Fetch, typename... Without>
struct EachInvoker<Desc, Func, std::tuple<Fetch...>, std::tuple<Without...>>
{
    static void run(entt::registry& registry, const ChangeDetection* changeDetection, Func&& func)
    {
        if constexpr (sizeof...(Without) == 0)
        {
            registry.view<Fetch...>().each([&](Entity entity, Fetch&... components) {
                if (!passes_tick_filters<typename Desc::changed, typename Desc::added>(changeDetection, registry, entity))
                    return;
                func(entity, components...);
            });
        }
        else
        {
            registry.view<Fetch...>(entt::exclude<Without...>).each([&](Entity entity, Fetch&... components) {
                if (!passes_tick_filters<typename Desc::changed, typename Desc::added>(changeDetection, registry, entity))
                    return;
                func(entity, components...);
            });
        }
    }
};

template<typename Desc, typename Func>
void each_query(entt::registry& registry, const ChangeDetection* changeDetection, Func&& func)
{
    EachInvoker<Desc, Func, typename Desc::fetch, typename Desc::without>::run(
        registry,
        changeDetection,
        std::forward<Func>(func));
}

template<typename Desc, typename Func>
void each_query_const(const entt::registry& registry, const ChangeDetection* changeDetection, Func&& func)
{
    EachInvoker<Desc, Func, typename Desc::fetch, typename Desc::without>::run(
        const_cast<entt::registry&>(registry),
        changeDetection,
        std::forward<Func>(func));
}

} // namespace caustica::ecs::detail
