/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

namespace caustica
{

// Linear / arena allocator.
// Fast allocation with no per-allocation overhead; all memory is freed at once via reset().
// Thread-compatible but NOT thread-safe — each thread should use its own arena.
struct Arena
{
    uint8_t* base     = nullptr;
    size_t   capacity = 0;
    size_t   used     = 0;
};

// Allocate an arena with the given capacity (in bytes)
inline Arena* ArenaAlloc(size_t capacity)
{
    Arena* arena    = static_cast<Arena*>(malloc(sizeof(Arena)));
    arena->base     = static_cast<uint8_t*>(malloc(capacity));
    arena->capacity = capacity;
    arena->used     = 0;
    return arena;
}

// Release the arena and all its memory
inline void ArenaRelease(Arena* arena)
{
    if (arena)
    {
        free(arena->base);
        free(arena);
    }
}

// Allocate aligned memory from the arena
inline void* ArenaPush(Arena* arena, size_t size, size_t alignment = 8)
{
    size_t offset = (arena->used + alignment - 1) & ~(alignment - 1);
    if (offset + size > arena->capacity)
        return nullptr; // Out of memory
    void* ptr = arena->base + offset;
    arena->used = offset + size;
    return ptr;
}

// Push a zero-initialized object onto the arena
template <typename T>
inline T* ArenaPushZero(Arena* arena)
{
    T* ptr = static_cast<T*>(ArenaPush(arena, sizeof(T), alignof(T)));
    if (ptr)
        memset(ptr, 0, sizeof(T));
    return ptr;
}

// Reset the arena (free all allocations at once)
inline void ArenaClear(Arena* arena)
{
    arena->used = 0;
}

// Convenience constants
inline constexpr size_t Kilobytes(size_t n) { return n * 1024; }
inline constexpr size_t Megabytes(size_t n) { return n * 1024 * 1024; }

} // namespace caustica
