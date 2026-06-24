#pragma once

#include <math/math.h>
#include <core/vfs/VFS.h>

#include <memory>
#include <cstring>


namespace caustica::chunk
{

struct MeshNode
{
    char const * name;

    uint32_t parentId,
             siblingId,
             instanceId;

    caustica::math::affine3 transform,
                         ctm;
    caustica::math::box3 bbox;
    caustica::math::float3 center;
};

static_assert(sizeof(MeshNode) == 152);

struct MeshInstance
{
    char const * name;

    uint32_t minfoId,
             nodeId;

    caustica::math::affine3 transform;
    caustica::math::box3 bbox;
    caustica::math::float3 center;
    uint32_t padding;
};

static_assert(sizeof(MeshInstance) == 104);

struct MeshInfoBase
{
    char const * name,
               * materialName;

    uint32_t materialId;
    caustica::math::box3 bbox;
    uint32_t padding;
};

static_assert(sizeof(MeshInfoBase) == 48);

struct MeshInfo : public MeshInfoBase
{
    uint32_t firstVertex,
             numVertices,
             firstIndex,
             numIndices;
};

static_assert(sizeof(MeshInfo) == 64);

struct MeshletInfo : public MeshInfoBase
{
    uint32_t firstMeshlet,
             numMeshlets;
};

static_assert(sizeof(MeshletInfo) == 56);

struct MeshSetBase
{

public:

    MeshSetBase() { memset(this, 0, sizeof(MeshSetBase)); }

    enum Type {
        UNDEFINED=0,
        MESH,
        MESHLET
    } type;

    char const * name;

    struct VertexStreams {
        caustica::math::float3 const * position;

        uint32_t const * normal,
                       * tangent,
                       * bitangent;

        caustica::math::float2 const * texcoord0,
                                  * texcoord1;
    } streams;

    uint32_t nverts;

    uint32_t nmeshInfos;

    MeshInstance const * instances;
    uint32_t ninstances;

    MeshNode const * nodes;
    uint32_t nnodes,
             rootId;

    caustica::math::box3 bbox;

    std::shared_ptr<caustica::IBlob const> blob;
};

struct MeshSet : public MeshSetBase
{

    MeshSet() { memset(this, 0, sizeof(MeshSet)); }

    uint32_t const * indices;
    uint32_t nindices;

    MeshInfo const * meshInfos;
};

struct MeshletSet : public MeshSetBase
{

    MeshletSet() { memset(this, 0, sizeof(MeshletSet)); }

    uint32_t maxVerts,
             maxPrims;

    uint32_t const * indices32;
    uint32_t nindices32;

    uint8_t const * indices8;
    uint32_t nindices8;

    uint32_t const * meshlets;
    uint32_t nmeshlets;
    uint8_t meshletSize; // size of meshlet header (in uint32_t)

    MeshletInfo const * meshInfos;
};

std::shared_ptr<caustica::IBlob const> serialize(MeshSetBase const & mset);

std::shared_ptr<MeshSetBase const> deserialize(std::weak_ptr<caustica::IBlob const> blob, char const * assetpath);

}
