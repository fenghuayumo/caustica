#pragma once

#include <cstdint>

namespace caustica
{
    enum struct SceneContentFlags : uint32_t
    {
        None = 0,
        OpaqueMeshes = 0x01,
        AlphaTestedMeshes = 0x02,
        BlendedMeshes = 0x04,
        Lights = 0x08,
        Cameras = 0x10,
        Animations = 0x20
    };

    inline SceneContentFlags operator | (SceneContentFlags a, SceneContentFlags b) { return SceneContentFlags(uint32_t(a) | uint32_t(b)); }
    inline SceneContentFlags operator & (SceneContentFlags a, SceneContentFlags b) { return SceneContentFlags(uint32_t(a) & uint32_t(b)); }
    inline SceneContentFlags operator ~ (SceneContentFlags a) { return SceneContentFlags(~uint32_t(a)); }
    inline SceneContentFlags operator |= (SceneContentFlags& a, SceneContentFlags b) { a = SceneContentFlags(uint32_t(a) | uint32_t(b)); return a; }
    inline SceneContentFlags operator &= (SceneContentFlags& a, SceneContentFlags b) { a = SceneContentFlags(uint32_t(a) & uint32_t(b)); return a; }
    inline bool operator !(SceneContentFlags a) { return uint32_t(a) == 0; }
    inline bool operator ==(SceneContentFlags a, uint32_t b) { return uint32_t(a) == b; }
    inline bool operator !=(SceneContentFlags a, uint32_t b) { return uint32_t(a) != b; }
}
