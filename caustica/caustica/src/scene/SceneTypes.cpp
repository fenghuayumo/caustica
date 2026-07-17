#include <scene/SceneObjects.h>
#include <scene/SceneEcs.h>
#include <core/json.h>
#include <json/json-forwards.h>

using namespace caustica::math;
#include <shaders/light_cb.h>
#include <shaders/bindless.h>

using namespace caustica;
using namespace caustica::scene;

// =============================================================================
// Vertex attribute descriptors and material helpers (unchanged)
// =============================================================================

nvrhi::VertexAttributeDesc caustica::getVertexAttributeDesc(VertexAttribute attribute, const char* name, uint32_t bufferIndex)
{
    nvrhi::VertexAttributeDesc result = {};
    result.name = name;
    result.bufferIndex = bufferIndex;
    result.arraySize = 1;

    switch (attribute)
    {
    case VertexAttribute::Position:
    case VertexAttribute::PrevPosition:
        result.format = nvrhi::Format::RGB32_FLOAT;
        result.elementStride = sizeof(float3);
        break;
    case VertexAttribute::TexCoord1:
    case VertexAttribute::TexCoord2:
        result.format = nvrhi::Format::RG32_FLOAT;
        result.elementStride = sizeof(float2);
        break;
    case VertexAttribute::Normal:
    case VertexAttribute::Tangent:
        result.format = nvrhi::Format::RGBA8_SNORM;
        result.elementStride = sizeof(uint32_t);
        break;
    case VertexAttribute::Transform:
        result.format = nvrhi::Format::RGBA32_FLOAT;
        result.arraySize = 3;
        result.offset = offsetof(InstanceData, transform);
        result.elementStride = sizeof(InstanceData);
        result.isInstanced = true;
        break;
    case VertexAttribute::PrevTransform:
        result.format = nvrhi::Format::RGBA32_FLOAT;
        result.arraySize = 3;
        result.offset = offsetof(InstanceData, prevTransform);
        result.elementStride = sizeof(InstanceData);
        result.isInstanced = true;
        break;

    default:
        assert(!"unknown attribute");
    }

    return result;
}
const char* caustica::materialDomainToString(MaterialDomain domain)
{
    switch (domain)
    {
    case MaterialDomain::Opaque: return "Opaque";
    case MaterialDomain::AlphaTested: return "AlphaTested";
    case MaterialDomain::AlphaBlended: return "AlphaBlended";
    case MaterialDomain::Transmissive: return "Transmissive";
    case MaterialDomain::TransmissiveAlphaTested: return "TransmissiveAlphaTested";
    case MaterialDomain::TransmissiveAlphaBlended: return "TransmissiveAlphaBlended";
    case MaterialDomain::Count: return "Count";
    default: return "<Invalid>";
    }
}
