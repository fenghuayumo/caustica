#pragma once

namespace caustica::ecs
{

// Bevy-style query filters. Fetch components are listed directly; filters constrain the match set.
//
//   world.each<GlobalTransformComponent, With<ParentComponent>>(...);
//   world.each<SkinnedMeshReferenceComponent, Changed<LocalTransformComponent>>(...);

template<typename T>
struct With
{
    using Type = T;
};

template<typename T>
struct Without
{
    using Type = T;
};

template<typename T>
struct Changed
{
    using Type = T;
};

template<typename T>
struct Added
{
    using Type = T;
};

template<typename T>
struct is_with : std::false_type
{
};

template<typename T>
struct is_with<With<T>> : std::true_type
{
};

template<typename T>
struct is_without : std::false_type
{
};

template<typename T>
struct is_without<Without<T>> : std::true_type
{
};

template<typename T>
struct is_changed : std::false_type
{
};

template<typename T>
struct is_changed<Changed<T>> : std::true_type
{
};

template<typename T>
struct is_added : std::false_type
{
};

template<typename T>
struct is_added<Added<T>> : std::true_type
{
};

template<typename T>
inline constexpr bool is_query_filter_v =
    is_with<T>::value || is_without<T>::value || is_changed<T>::value || is_added<T>::value;

} // namespace caustica::ecs
