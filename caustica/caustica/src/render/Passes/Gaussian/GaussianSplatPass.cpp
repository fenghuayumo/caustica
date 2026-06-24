#include <render/Passes/Gaussian/GaussianSplatPass.h>

#include <render/GPUSort/GPUSort.h>
#include <SampleCommon/RenderTargets.h>

#include <core/log.h>
#include <render/Core/FramebufferFactory.h>
#include <assets/loader/ShaderFactory.h>
#include <render/Core/View.h>
#include <shaders/view_cb.h>
#include <rhi/utils.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <utility>

using namespace caustica::math;

namespace
{
    constexpr float kSH_C0 = 0.28209479177387814f;
    constexpr float kGaussianSplatAsRadius = 2.8284271247461903f;
    constexpr float kGaussianSplatKernelMinResponse = 0.0113f;
    constexpr std::array<float, 15> kRdfToRubShFlip = {
        -1.0f, -1.0f, 1.0f, -1.0f, 1.0f,
         1.0f, -1.0f, 1.0f, -1.0f, 1.0f,
        -1.0f, -1.0f, 1.0f, -1.0f, 1.0f
    };

    enum class PlyFormat
    {
        Ascii,
        BinaryLittleEndian,
        Unsupported
    };

    enum class PlyScalarType
    {
        Int8,
        UInt8,
        Int16,
        UInt16,
        Int32,
        UInt32,
        Float32,
        Float64,
        Invalid
    };

    struct PlyProperty
    {
        std::string name;
        PlyScalarType type = PlyScalarType::Invalid;
        bool isList = false;
        PlyScalarType listCountType = PlyScalarType::Invalid;
    };

    struct PlyElement
    {
        std::string name;
        uint64_t count = 0;
        std::vector<PlyProperty> properties;
    };

    struct RawGaussianSplat
    {
        float position[3] = {};
        float scale[3] = { 1.0f, 1.0f, 1.0f };
        float rotation[4] = { 1.0f, 0.0f, 0.0f, 0.0f }; // w, x, y, z
        float color[3] = { 1.0f, 1.0f, 1.0f };
        float alpha = 1.0f;
    };

    float GaussianKernelScale(float density, float kernelMinResponse, uint32_t kernelDegree, bool adaptiveClamping)
    {
        const float responseModulation = adaptiveClamping ? std::max(density, 1e-6f) : 1.0f;
        const float minResponse = std::min(kernelMinResponse / responseModulation, 0.97f);

        if (kernelDegree == 0)
            return std::abs((minResponse - 1.0f) / -0.329630334487f);

        const float b = std::max(float(kernelDegree), 1.0f);
        const float a = -4.5f / std::pow(3.0f, b);
        return std::pow(std::log(minResponse) / a, 1.0f / b);
    }

    float3 GaussianAabbExtent(const GaussianSplatData& splat, float splatScale, uint32_t kernelDegree, bool adaptiveClamp)
    {
        const float3 variance = float3(
            std::max(splat.covariance0.x, 1e-8f),
            std::max(splat.covariance0.w, 1e-8f),
            std::max(splat.covariance1.y, 1e-8f));
        const float kernelScale = GaussianKernelScale(
            splat.centerOpacity.w,
            kGaussianSplatKernelMinResponse,
            kernelDegree,
            adaptiveClamp);
        return float3(
            std::sqrt(variance.x),
            std::sqrt(variance.y),
            std::sqrt(variance.z)) * (std::max(splatScale, 1e-4f) * std::max(kernelScale, 1e-3f));
    }

    float SrgbToLinear(float srgb)
    {
        srgb = std::max(srgb, 0.0f);
        return srgb <= 0.04045f
            ? srgb / 12.92f
            : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
    }

    float3 SrgbToLinear(const float3& srgb)
    {
        return float3(SrgbToLinear(srgb.x), SrgbToLinear(srgb.y), SrgbToLinear(srgb.z));
    }

    float Luminance(const float3& color)
    {
        return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
    }

    nvrhi::rt::GeometryAABB GaussianAabbFromSplat(const GaussianSplatData& splat, float splatScale, uint32_t kernelDegree, bool adaptiveClamp)
    {
        const float3 center = splat.centerOpacity.xyz();
        const float3 extent = GaussianAabbExtent(splat, splatScale, kernelDegree, adaptiveClamp);

        nvrhi::rt::GeometryAABB aabb = {};
        aabb.minX = center.x - extent.x;
        aabb.minY = center.y - extent.y;
        aabb.minZ = center.z - extent.z;
        aabb.maxX = center.x + extent.x;
        aabb.maxY = center.y + extent.y;
        aabb.maxZ = center.z + extent.z;
        return aabb;
    }

    std::vector<nvrhi::rt::GeometryAABB> BuildGaussianAabbs(
        const std::vector<GaussianSplatData>& splats,
        float splatScale,
        uint32_t kernelDegree,
        bool adaptiveClamp)
    {
        std::vector<nvrhi::rt::GeometryAABB> aabbs;
        aabbs.reserve(splats.size());

        for (const GaussianSplatData& splat : splats)
            aabbs.push_back(GaussianAabbFromSplat(splat, splatScale, kernelDegree, adaptiveClamp));

        return aabbs;
    }

    void FillScaleTranslateTransform(nvrhi::rt::AffineTransform& transform, const float3& center, const float3& extent)
    {
        transform[0] = extent.x; transform[1] = 0.0f;     transform[2] = 0.0f;     transform[3] = center.x;
        transform[4] = 0.0f;     transform[5] = extent.y; transform[6] = 0.0f;     transform[7] = center.y;
        transform[8] = 0.0f;     transform[9] = 0.0f;     transform[10] = extent.z; transform[11] = center.z;
    }

    constexpr float kIcosahedronInvPhi = 0.61803398875f;

    const std::array<float3, 12> kUnitIcosahedronVertices = {
        float3(-kIcosahedronInvPhi,  1.0f, 0.0f),
        float3( kIcosahedronInvPhi,  1.0f, 0.0f),
        float3(-kIcosahedronInvPhi, -1.0f, 0.0f),
        float3( kIcosahedronInvPhi, -1.0f, 0.0f),
        float3(0.0f, -kIcosahedronInvPhi,  1.0f),
        float3(0.0f,  kIcosahedronInvPhi,  1.0f),
        float3(0.0f, -kIcosahedronInvPhi, -1.0f),
        float3(0.0f,  kIcosahedronInvPhi, -1.0f),
        float3( 1.0f, 0.0f, -kIcosahedronInvPhi),
        float3( 1.0f, 0.0f,  kIcosahedronInvPhi),
        float3(-1.0f, 0.0f, -kIcosahedronInvPhi),
        float3(-1.0f, 0.0f,  kIcosahedronInvPhi)
    };

    const std::array<uint32_t, 60> kUnitIcosahedronIndices = {
        0, 11, 5,
        0, 5, 1,
        0, 1, 7,
        0, 7, 10,
        0, 10, 11,
        1, 5, 9,
        5, 11, 4,
        11, 10, 2,
        10, 7, 6,
        7, 1, 8,
        3, 9, 4,
        3, 4, 2,
        3, 2, 6,
        3, 6, 8,
        3, 8, 9,
        4, 9, 5,
        2, 4, 11,
        6, 2, 10,
        8, 6, 7,
        9, 8, 1
    };

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });
        return value;
    }

    std::string Trim(std::string value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();

        size_t first = 0;
        while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
            ++first;

        if (first)
            value.erase(0, first);

        return value;
    }

    PlyScalarType ParseScalarType(const std::string& token)
    {
        const std::string type = ToLower(token);

        if (type == "char" || type == "int8")
            return PlyScalarType::Int8;
        if (type == "uchar" || type == "uint8" || type == "uint8_t")
            return PlyScalarType::UInt8;
        if (type == "short" || type == "int16")
            return PlyScalarType::Int16;
        if (type == "ushort" || type == "uint16")
            return PlyScalarType::UInt16;
        if (type == "int" || type == "int32")
            return PlyScalarType::Int32;
        if (type == "uint" || type == "uint32")
            return PlyScalarType::UInt32;
        if (type == "float" || type == "float32")
            return PlyScalarType::Float32;
        if (type == "double" || type == "float64")
            return PlyScalarType::Float64;

        return PlyScalarType::Invalid;
    }

    size_t ScalarTypeSize(PlyScalarType type)
    {
        switch (type)
        {
        case PlyScalarType::Int8:
        case PlyScalarType::UInt8:
            return 1;
        case PlyScalarType::Int16:
        case PlyScalarType::UInt16:
            return 2;
        case PlyScalarType::Int32:
        case PlyScalarType::UInt32:
        case PlyScalarType::Float32:
            return 4;
        case PlyScalarType::Float64:
            return 8;
        default:
            return 0;
        }
    }

    bool ReadScalarBinary(std::istream& stream, PlyScalarType type, double& value)
    {
        switch (type)
        {
        case PlyScalarType::Int8:
        {
            int8_t v = 0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::UInt8:
        {
            uint8_t v = 0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::Int16:
        {
            int16_t v = 0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::UInt16:
        {
            uint16_t v = 0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::Int32:
        {
            int32_t v = 0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::UInt32:
        {
            uint32_t v = 0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::Float32:
        {
            float v = 0.0f;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = double(v);
            return bool(stream);
        }
        case PlyScalarType::Float64:
        {
            double v = 0.0;
            stream.read(reinterpret_cast<char*>(&v), sizeof(v));
            value = v;
            return bool(stream);
        }
        default:
            return false;
        }
    }

    bool ParseScalarAscii(std::istream& stream, PlyScalarType type, double& value)
    {
        std::string token;
        if (!(stream >> token))
            return false;

        try
        {
            switch (type)
            {
            case PlyScalarType::Float32:
            case PlyScalarType::Float64:
                value = std::stod(token);
                return true;
            case PlyScalarType::Int8:
            case PlyScalarType::Int16:
            case PlyScalarType::Int32:
                value = double(std::stoll(token));
                return true;
            case PlyScalarType::UInt8:
            case PlyScalarType::UInt16:
            case PlyScalarType::UInt32:
                value = double(std::stoull(token));
                return true;
            default:
                return false;
            }
        }
        catch (...)
        {
            return false;
        }
    }

    int FindProperty(const std::vector<PlyProperty>& properties, const char* name)
    {
        for (size_t index = 0; index < properties.size(); ++index)
        {
            if (!properties[index].isList && properties[index].name == name)
                return int(index);
        }

        return -1;
    }

    int FindFirstProperty(const std::vector<PlyProperty>& properties, const std::initializer_list<const char*> names)
    {
        for (const char* name : names)
        {
            int index = FindProperty(properties, name);
            if (index >= 0)
                return index;
        }

        return -1;
    }

    bool SkipElementRowBinary(std::istream& stream, const PlyElement& element)
    {
        for (const PlyProperty& property : element.properties)
        {
            if (!property.isList)
            {
                stream.seekg(static_cast<std::streamoff>(ScalarTypeSize(property.type)), std::ios::cur);
                if (!stream)
                    return false;
                continue;
            }

            double countValue = 0.0;
            if (!ReadScalarBinary(stream, property.listCountType, countValue))
                return false;

            const auto count = static_cast<uint64_t>(std::max(0.0, countValue));
            stream.seekg(static_cast<std::streamoff>(count * ScalarTypeSize(property.type)), std::ios::cur);
            if (!stream)
                return false;
        }

        return true;
    }

    float Clamp01(float value)
    {
        return std::min(1.0f, std::max(0.0f, value));
    }

    float Sigmoid(float value)
    {
        return 1.0f / (1.0f + std::exp(-value));
    }

    void NormalizeQuaternion(float rotation[4])
    {
        float lengthSquared = rotation[0] * rotation[0] + rotation[1] * rotation[1] +
            rotation[2] * rotation[2] + rotation[3] * rotation[3];

        if (lengthSquared <= std::numeric_limits<float>::epsilon())
        {
            rotation[0] = 1.0f;
            rotation[1] = rotation[2] = rotation[3] = 0.0f;
            return;
        }

        float invLength = 1.0f / std::sqrt(lengthSquared);
        rotation[0] *= invLength;
        rotation[1] *= invLength;
        rotation[2] *= invLength;
        rotation[3] *= invLength;
    }

    SimpleViewConstants FromPlanarViewConstants(const PlanarViewConstants& view)
    {
        SimpleViewConstants ret;
        ret.matWorldToView = view.matWorldToView;
        ret.matViewToClip = view.matViewToClip;
        ret.matWorldToClipNoOffset = view.matWorldToClipNoOffset;
        ret.matClipToWorldNoOffset = view.matClipToWorldNoOffset;
        ret.matWorldToClip = view.matWorldToClip;
        ret.clipToWindowBias = view.clipToWindowBias;
        ret.clipToWindowScale = view.clipToWindowScale;
        ret.viewportOrigin = view.viewportOrigin;
        ret.viewportSize = view.viewportSize;
        ret.viewportSizeInv = view.viewportSizeInv;
        ret.pixelOffset = view.pixelOffset;
        return ret;
    }

    bool MatrixEquals(const float4x4& a, const float4x4& b)
    {
        for (int index = 0; index < 16; ++index)
        {
            if (a.m_data[index] != b.m_data[index])
                return false;
        }

        return true;
    }

    uint32_t FormatElementSize(GaussianSplatStorageFormat format)
    {
        switch (format)
        {
        case GaussianSplatStorageFormat::Float32:
            return sizeof(float);
        case GaussianSplatStorageFormat::Float16:
            return sizeof(uint16_t);
        case GaussianSplatStorageFormat::Uint8:
            return sizeof(uint8_t);
        default:
            return sizeof(float);
        }
    }

    uint8_t QuantizeUnorm8(float value)
    {
        return uint8_t(std::clamp(std::round(Clamp01(value) * 255.0f), 0.0f, 255.0f));
    }

    uint8_t QuantizeSnormRange8(float value, float minValue, float maxValue)
    {
        const float normalized = std::clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);
        return uint8_t(std::clamp(std::round(normalized * 255.0f), 0.0f, 255.0f));
    }

    void StoreFormattedScalar(std::vector<uint8_t>& data, uint64_t scalarIndex, GaussianSplatStorageFormat format, float value, bool signedRange)
    {
        const uint64_t byteOffset = scalarIndex * FormatElementSize(format);
        switch (format)
        {
        case GaussianSplatStorageFormat::Float32:
        {
            std::memcpy(data.data() + byteOffset, &value, sizeof(value));
            break;
        }
        case GaussianSplatStorageFormat::Float16:
        {
            const float16_t halfValue = Float32ToFloat16(value);
            std::memcpy(data.data() + byteOffset, &halfValue.bits, sizeof(halfValue.bits));
            break;
        }
        case GaussianSplatStorageFormat::Uint8:
        {
            const uint8_t quantized = signedRange
                ? QuantizeSnormRange8(value, -1.0f, 1.0f)
                : QuantizeUnorm8(value);
            data[byteOffset] = quantized;
            break;
        }
        }
    }

    float ShCoefficientAt(const std::vector<float4>& packedCoefficients, uint32_t splatIndex, uint32_t scalarIndex)
    {
        const uint32_t float4Index = splatIndex * GAUSSIAN_SPLAT_SH_FLOAT4_COUNT + scalarIndex / 4u;
        if (float4Index >= packedCoefficients.size())
            return 0.0f;

        const float4 value = packedCoefficients[float4Index];
        switch (scalarIndex & 3u)
        {
        case 0: return value.x;
        case 1: return value.y;
        case 2: return value.z;
        default: return value.w;
        }
    }

    uint64_t AlignRawBufferSize(uint64_t size)
    {
        return std::max<uint64_t>(4, (size + 3u) & ~uint64_t(3u));
    }

    GaussianSplatData ConvertToGpuSplat(const RawGaussianSplat& raw, bool convertRdfToRub)
    {
        float rotation[4] = { raw.rotation[0], raw.rotation[1], raw.rotation[2], raw.rotation[3] };
        float position[3] = { raw.position[0], raw.position[1], raw.position[2] };

        if (convertRdfToRub)
        {
            position[1] = -position[1];
            position[2] = -position[2];
            rotation[2] = -rotation[2];
            rotation[3] = -rotation[3];
        }

        NormalizeQuaternion(rotation);

        const float w = rotation[0];
        const float x = rotation[1];
        const float y = rotation[2];
        const float z = rotation[3];

        const float xx = x * x;
        const float yy = y * y;
        const float zz = z * z;
        const float xy = x * y;
        const float xz = x * z;
        const float yz = y * z;
        const float wx = w * x;
        const float wy = w * y;
        const float wz = w * z;

        const float r00 = 1.0f - 2.0f * (yy + zz);
        const float r01 = 2.0f * (xy - wz);
        const float r02 = 2.0f * (xz + wy);
        const float r10 = 2.0f * (xy + wz);
        const float r11 = 1.0f - 2.0f * (xx + zz);
        const float r12 = 2.0f * (yz - wx);
        const float r20 = 2.0f * (xz - wy);
        const float r21 = 2.0f * (yz + wx);
        const float r22 = 1.0f - 2.0f * (xx + yy);

        const float sx2 = raw.scale[0] * raw.scale[0];
        const float sy2 = raw.scale[1] * raw.scale[1];
        const float sz2 = raw.scale[2] * raw.scale[2];

        float cov00 = r00 * r00 * sx2 + r01 * r01 * sy2 + r02 * r02 * sz2;
        float cov01 = r00 * r10 * sx2 + r01 * r11 * sy2 + r02 * r12 * sz2;
        float cov02 = r00 * r20 * sx2 + r01 * r21 * sy2 + r02 * r22 * sz2;
        float cov11 = r10 * r10 * sx2 + r11 * r11 * sy2 + r12 * r12 * sz2;
        float cov12 = r10 * r20 * sx2 + r11 * r21 * sy2 + r12 * r22 * sz2;
        float cov22 = r20 * r20 * sx2 + r21 * r21 * sy2 + r22 * r22 * sz2;

        GaussianSplatData splat = {};
        splat.centerOpacity = float4(position[0], position[1], position[2], Clamp01(raw.alpha));
        splat.covariance0 = float4(cov00, cov01, cov02, cov11);
        splat.covariance1 = float4(cov12, cov22, 0.0f, 0.0f);
        splat.color = float4(Clamp01(raw.color[0]), Clamp01(raw.color[1]), Clamp01(raw.color[2]), 0.0f);

        return splat;
    }

    bool LoadPlyFile(
        const std::filesystem::path& fileName,
        bool convertRdfToRub,
        std::vector<GaussianSplatData>& splats,
        std::vector<float4>& shCoefficients,
        uint32_t& shDegree)
    {
        std::ifstream file(fileName, std::ios::binary);
        if (!file)
        {
            caustica::error("Failed to open Gaussian splat PLY file: %s", fileName.string().c_str());
            return false;
        }

        std::string line;
        if (!std::getline(file, line) || Trim(line) != "ply")
        {
            caustica::error("Invalid Gaussian splat PLY header: %s", fileName.string().c_str());
            return false;
        }

        PlyFormat format = PlyFormat::Unsupported;
        std::vector<PlyElement> elements;
        PlyElement* currentElement = nullptr;

        while (std::getline(file, line))
        {
            line = Trim(line);
            if (line.empty())
                continue;

            std::istringstream parser(line);
            std::string keyword;
            parser >> keyword;

            if (keyword == "end_header")
                break;

            if (keyword == "comment" || keyword == "obj_info")
                continue;

            if (keyword == "format")
            {
                std::string formatToken;
                parser >> formatToken;
                formatToken = ToLower(formatToken);

                if (formatToken == "ascii")
                    format = PlyFormat::Ascii;
                else if (formatToken == "binary_little_endian")
                    format = PlyFormat::BinaryLittleEndian;
                else
                    format = PlyFormat::Unsupported;

                continue;
            }

            if (keyword == "element")
            {
                PlyElement element;
                parser >> element.name >> element.count;
                elements.push_back(std::move(element));
                currentElement = &elements.back();
                continue;
            }

            if (keyword == "property" && currentElement)
            {
                std::string typeToken;
                parser >> typeToken;

                PlyProperty property;
                if (typeToken == "list")
                {
                    std::string countTypeToken;
                    std::string valueTypeToken;
                    parser >> countTypeToken >> valueTypeToken >> property.name;
                    property.isList = true;
                    property.listCountType = ParseScalarType(countTypeToken);
                    property.type = ParseScalarType(valueTypeToken);
                }
                else
                {
                    parser >> property.name;
                    property.type = ParseScalarType(typeToken);
                }

                if (property.type == PlyScalarType::Invalid ||
                    (property.isList && property.listCountType == PlyScalarType::Invalid))
                {
                    caustica::error("Unsupported PLY property type in %s", fileName.string().c_str());
                    return false;
                }

                currentElement->properties.push_back(std::move(property));
            }
        }

        if (format == PlyFormat::Unsupported)
        {
            caustica::error("Unsupported PLY format in %s", fileName.string().c_str());
            return false;
        }

        auto vertexElementIt = std::find_if(elements.begin(), elements.end(),
            [](const PlyElement& element) { return element.name == "vertex"; });

        if (vertexElementIt == elements.end() || vertexElementIt->count == 0)
        {
            caustica::error("Gaussian splat PLY has no vertex element: %s", fileName.string().c_str());
            return false;
        }

        const PlyElement& vertexElement = *vertexElementIt;
        const auto& properties = vertexElement.properties;

        const int xIndex = FindProperty(properties, "x");
        const int yIndex = FindProperty(properties, "y");
        const int zIndex = FindProperty(properties, "z");
        const int opacityIndex = FindProperty(properties, "opacity");
        const int scale0Index = FindProperty(properties, "scale_0");
        const int scale1Index = FindProperty(properties, "scale_1");
        const int scale2Index = FindProperty(properties, "scale_2");
        const int rot0Index = FindProperty(properties, "rot_0");
        const int rot1Index = FindProperty(properties, "rot_1");
        const int rot2Index = FindProperty(properties, "rot_2");
        const int rot3Index = FindProperty(properties, "rot_3");
        const int fdc0Index = FindProperty(properties, "f_dc_0");
        const int fdc1Index = FindProperty(properties, "f_dc_1");
        const int fdc2Index = FindProperty(properties, "f_dc_2");
        const int redIndex = FindFirstProperty(properties, { "red", "r", "diffuse_red" });
        const int greenIndex = FindFirstProperty(properties, { "green", "g", "diffuse_green" });
        const int blueIndex = FindFirstProperty(properties, { "blue", "b", "diffuse_blue" });

        std::array<int, 45> fRestIndices;
        fRestIndices.fill(-1);
        uint32_t fRestCount = 0;
        for (uint32_t index = 0; index < uint32_t(fRestIndices.size()); ++index)
        {
            std::string propertyName = "f_rest_" + std::to_string(index);
            fRestIndices[index] = FindProperty(properties, propertyName.c_str());
            if (fRestIndices[index] >= 0)
                ++fRestCount;
        }

        if (fRestCount >= 45)
            shDegree = 3;
        else if (fRestCount >= 24)
            shDegree = 2;
        else if (fRestCount >= 9)
            shDegree = 1;
        else
            shDegree = 0;

        const bool hasRequired3dgsProperties =
            xIndex >= 0 && yIndex >= 0 && zIndex >= 0 &&
            opacityIndex >= 0 &&
            scale0Index >= 0 && scale1Index >= 0 && scale2Index >= 0 &&
            rot0Index >= 0 && rot1Index >= 0 && rot2Index >= 0 && rot3Index >= 0 &&
            ((fdc0Index >= 0 && fdc1Index >= 0 && fdc2Index >= 0) ||
             (redIndex >= 0 && greenIndex >= 0 && blueIndex >= 0));

        if (!hasRequired3dgsProperties)
        {
            caustica::error("PLY file does not contain the expected 3DGS attributes: %s", fileName.string().c_str());
            return false;
        }

        splats.clear();
        splats.reserve(size_t(vertexElement.count));
        shCoefficients.clear();
        shCoefficients.reserve(size_t(vertexElement.count) * GAUSSIAN_SPLAT_SH_FLOAT4_COUNT);

        std::vector<double> values(properties.size(), 0.0);

        for (const PlyElement& element : elements)
        {
            if (element.name != "vertex")
            {
                for (uint64_t row = 0; row < element.count; ++row)
                {
                    if (format == PlyFormat::Ascii)
                    {
                        std::getline(file, line);
                    }
                    else if (!SkipElementRowBinary(file, element))
                    {
                        caustica::error("Failed while skipping PLY element in %s", fileName.string().c_str());
                        return false;
                    }
                }
                continue;
            }

            for (uint64_t row = 0; row < element.count; ++row)
            {
                if (format == PlyFormat::Ascii)
                {
                    if (!std::getline(file, line))
                    {
                        caustica::error("Unexpected end of PLY vertex data: %s", fileName.string().c_str());
                        return false;
                    }

                    std::istringstream rowParser(line);
                    for (size_t propertyIndex = 0; propertyIndex < properties.size(); ++propertyIndex)
                    {
                        const PlyProperty& property = properties[propertyIndex];

                        if (property.isList)
                        {
                            double countValue = 0.0;
                            if (!ParseScalarAscii(rowParser, property.listCountType, countValue))
                                return false;
                            for (uint64_t i = 0; i < static_cast<uint64_t>(std::max(0.0, countValue)); ++i)
                            {
                                double ignored = 0.0;
                                if (!ParseScalarAscii(rowParser, property.type, ignored))
                                    return false;
                            }
                        }
                        else if (!ParseScalarAscii(rowParser, property.type, values[propertyIndex]))
                        {
                            caustica::error("Failed to parse PLY vertex row in %s", fileName.string().c_str());
                            return false;
                        }
                    }
                }
                else
                {
                    for (size_t propertyIndex = 0; propertyIndex < properties.size(); ++propertyIndex)
                    {
                        const PlyProperty& property = properties[propertyIndex];

                        if (property.isList)
                        {
                            double countValue = 0.0;
                            if (!ReadScalarBinary(file, property.listCountType, countValue))
                                return false;
                            file.seekg(static_cast<std::streamoff>(static_cast<uint64_t>(std::max(0.0, countValue)) * ScalarTypeSize(property.type)), std::ios::cur);
                            if (!file)
                                return false;
                        }
                        else if (!ReadScalarBinary(file, property.type, values[propertyIndex]))
                        {
                            caustica::error("Failed to read PLY vertex row in %s", fileName.string().c_str());
                            return false;
                        }
                    }
                }

                RawGaussianSplat raw = {};
                raw.position[0] = float(values[xIndex]);
                raw.position[1] = float(values[yIndex]);
                raw.position[2] = float(values[zIndex]);

                raw.scale[0] = std::exp(float(values[scale0Index]));
                raw.scale[1] = std::exp(float(values[scale1Index]));
                raw.scale[2] = std::exp(float(values[scale2Index]));

                raw.rotation[0] = float(values[rot0Index]);
                raw.rotation[1] = float(values[rot1Index]);
                raw.rotation[2] = float(values[rot2Index]);
                raw.rotation[3] = float(values[rot3Index]);

                raw.alpha = Sigmoid(float(values[opacityIndex]));

                if (fdc0Index >= 0)
                {
                    raw.color[0] = 0.5f + kSH_C0 * float(values[fdc0Index]);
                    raw.color[1] = 0.5f + kSH_C0 * float(values[fdc1Index]);
                    raw.color[2] = 0.5f + kSH_C0 * float(values[fdc2Index]);
                }
                else
                {
                    raw.color[0] = float(values[redIndex]) / 255.0f;
                    raw.color[1] = float(values[greenIndex]) / 255.0f;
                    raw.color[2] = float(values[blueIndex]) / 255.0f;
                }

                splats.push_back(ConvertToGpuSplat(raw, convertRdfToRub));

                std::array<float, GAUSSIAN_SPLAT_SH_FLOAT4_COUNT * 4> packedSh = {};
                if (shDegree > 0)
                {
                    const uint32_t coefficientsPerChannel = shDegree == 3 ? 15u : (shDegree == 2 ? 8u : 3u);
                    const uint32_t coefficientCount = coefficientsPerChannel * 3u;

                    for (uint32_t coeff = 0; coeff < coefficientsPerChannel; ++coeff)
                    {
                        const float coordinateFlip = convertRdfToRub ? kRdfToRubShFlip[coeff] : 1.0f;
                        for (uint32_t rgb = 0; rgb < 3; ++rgb)
                        {
                            const uint32_t sourceCoeff = rgb * coefficientsPerChannel + coeff;
                            if (sourceCoeff < coefficientCount && fRestIndices[sourceCoeff] >= 0)
                                packedSh[coeff * 3 + rgb] = float(values[fRestIndices[sourceCoeff]]) * coordinateFlip;
                        }
                    }
                }

                for (uint32_t i = 0; i < GAUSSIAN_SPLAT_SH_FLOAT4_COUNT; ++i)
                {
                    shCoefficients.push_back(float4(
                        packedSh[i * 4 + 0],
                        packedSh[i * 4 + 1],
                        packedSh[i * 4 + 2],
                        packedSh[i * 4 + 3]));
                }
            }
        }

        caustica::info("Loaded %zu Gaussian splats from %s (SH degree %u)", splats.size(), fileName.string().c_str(), shDegree);
        return !splats.empty();
    }
}

GaussianSplatPass::GaussianSplatPass(
    nvrhi::IDevice* device,
    std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_shaderFactory(std::move(shaderFactory))
{
    m_constantBuffer = m_device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(GaussianSplatConstants), "GaussianSplatConstants", 16));

    nvrhi::BindingLayoutDesc rasterRenderLayoutDesc;
    rasterRenderLayoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    rasterRenderLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        nvrhi::BindingLayoutItem::TypedBuffer_SRV(1),
        nvrhi::BindingLayoutItem::RawBuffer_SRV(2),
        nvrhi::BindingLayoutItem::RawBuffer_SRV(3),
        nvrhi::BindingLayoutItem::Texture_SRV(4)
    };
    m_rasterRenderBindingLayout = m_device->createBindingLayout(rasterRenderLayoutDesc);

    nvrhi::BindingLayoutDesc hybridRenderLayoutDesc = rasterRenderLayoutDesc;
    hybridRenderLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::RayTracingAccelStruct(5));
    m_hybridRenderBindingLayout = m_device->createBindingLayout(hybridRenderLayoutDesc);

    nvrhi::BindingLayoutDesc sortLayoutDesc;
    sortLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    sortLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        nvrhi::BindingLayoutItem::TypedBuffer_UAV(0),
        nvrhi::BindingLayoutItem::TypedBuffer_UAV(1),
        nvrhi::BindingLayoutItem::TypedBuffer_UAV(2),
        nvrhi::BindingLayoutItem::TypedBuffer_UAV(3)
    };
    m_sortKeyBindingLayout = m_device->createBindingLayout(sortLayoutDesc);
}

void GaussianSplatPass::SetGpuSort(std::shared_ptr<GPUSort> gpuSort)
{
    m_gpuSort = std::move(gpuSort);
}

void GaussianSplatPass::InvalidateSortCache()
{
    m_sortCacheValid = false;
    m_cachedSortSplatCount = 0;
}

bool GaussianSplatPass::CanReuseSort(const GaussianSplatConstants& constants) const
{
    return m_sortCacheValid
        && m_cachedSortSplatCount == m_splatCount
        && MatrixEquals(m_cachedSortWorldToClipNoOffset, constants.view.matWorldToClipNoOffset)
        && MatrixEquals(m_cachedSortObjectToWorld, constants.objectToWorld);
}

bool GaussianSplatPass::LoadFromFile(const std::filesystem::path& fileName, bool convertRdfToRub)
{
    const std::string extension = ToLower(fileName.extension().string());
    if (extension != ".ply")
    {
        caustica::error("Unsupported Gaussian splat file extension '%s'. This pass currently supports 3DGS .ply files.", extension.c_str());
        return false;
    }

    std::vector<GaussianSplatData> loadedSplats;
    std::vector<float4> loadedShCoefficients;
    uint32_t loadedShDegree = 0;
    if (!LoadPlyFile(fileName, convertRdfToRub, loadedSplats, loadedShCoefficients, loadedShDegree))
        return false;

    m_splats = std::move(loadedSplats);
    m_shCoefficients = std::move(loadedShCoefficients);
    m_emissionProxies.clear();
    m_splatCount = uint32_t(m_splats.size());
    m_shDegree = loadedShDegree;
    m_colorOpacity.clear();
    m_colorOpacity.reserve(m_splats.size());
    for (const GaussianSplatData& splat : m_splats)
        m_colorOpacity.push_back(float4(splat.color.x, splat.color.y, splat.color.z, splat.centerOpacity.w));

    if (m_shCoefficients.empty())
        m_shCoefficients.push_back(float4(0.0f, 0.0f, 0.0f, 0.0f));

    nvrhi::BufferDesc splatBufferDesc;
    splatBufferDesc.byteSize = uint64_t(m_splatCount) * sizeof(GaussianSplatData);
    splatBufferDesc.structStride = sizeof(GaussianSplatData);
    splatBufferDesc.debugName = "GaussianSplatDataBuffer";
    splatBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    splatBufferDesc.keepInitialState = true;
    m_splatBuffer = m_device->createBuffer(splatBufferDesc);

    m_colorBuffer = nullptr;
    m_shBuffer = nullptr;

    nvrhi::BufferDesc uintBufferDesc;
    uintBufferDesc.byteSize = uint64_t(m_splatCount) * sizeof(uint32_t);
    uintBufferDesc.format = nvrhi::Format::R32_UINT;
    uintBufferDesc.canHaveTypedViews = true;
    uintBufferDesc.canHaveUAVs = true;
    uintBufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    uintBufferDesc.keepInitialState = true;
    uintBufferDesc.debugName = "GaussianSplatSortedIndexBuffer";
    m_indexBuffer = m_device->createBuffer(uintBufferDesc);

    uintBufferDesc.debugName = "GaussianSplatSortKeyBuffer";
    m_sortKeyBuffer = m_device->createBuffer(uintBufferDesc);

    nvrhi::BufferDesc sortControlDesc;
    sortControlDesc.byteSize = sizeof(uint32_t);
    sortControlDesc.format = nvrhi::Format::R32_UINT;
    sortControlDesc.canHaveTypedViews = true;
    sortControlDesc.canHaveUAVs = true;
    sortControlDesc.debugName = "GaussianSplatSortControlBuffer";
    sortControlDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    sortControlDesc.keepInitialState = true;
    m_sortControlBuffer = m_device->createBuffer(sortControlDesc);

    nvrhi::BufferDesc drawIndirectDesc;
    drawIndirectDesc.byteSize = sizeof(nvrhi::DrawIndirectArguments);
    drawIndirectDesc.format = nvrhi::Format::R32_UINT;
    drawIndirectDesc.canHaveTypedViews = true;
    drawIndirectDesc.canHaveUAVs = true;
    drawIndirectDesc.isDrawIndirectArgs = true;
    drawIndirectDesc.debugName = "GaussianSplatDrawIndirectBuffer";
    drawIndirectDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    drawIndirectDesc.keepInitialState = true;
    m_drawIndirectBuffer = m_device->createBuffer(drawIndirectDesc);

    nvrhi::BufferDesc aabbBufferDesc;
    aabbBufferDesc.byteSize = uint64_t(m_splatCount) * sizeof(nvrhi::rt::GeometryAABB);
    aabbBufferDesc.debugName = "GaussianSplatAabbBuffer";
    aabbBufferDesc.isAccelStructBuildInput = true;
    aabbBufferDesc.initialState = nvrhi::ResourceStates::AccelStructBuildInput;
    aabbBufferDesc.keepInitialState = true;
    m_splatAabbBuffer = m_device->createBuffer(aabbBufferDesc);

    m_splatTriangleVertexBuffer = nullptr;
    m_splatTriangleIndexBuffer = nullptr;
    m_rasterRenderBindingSet = nullptr;
    m_hybridRenderBindingSet = nullptr;
    m_sortKeyBindingSet = nullptr;
    m_splatBottomLevelAS = nullptr;
    m_splatTopLevelAS = nullptr;
    m_hybridRenderMeshTopLevelAS = nullptr;
    m_sourceFileName = fileName.string();
    m_splatUploadPending = true;
    m_formatUploadPending = true;
    m_accelStructBuildPending = true;
    m_lastBlasCompaction = false;
    m_lastAsUseAABBs = true;
    m_lastAsUseTLASInstances = false;
    m_lastAsSplatScale = 1.0f;
    m_lastAsKernelDegree = 2;
    m_lastAsAdaptiveClamp = true;
    m_cachedEmissionProxyMaxCount = 0;
    m_cachedEmissionProxySplatScale = 1.0f;
    m_cachedEmissionProxyKernelDegree = 0;
    m_cachedEmissionProxyAdaptiveClamp = true;
    m_cachedEmissionProxyTintColor = float3(1.0f);
    m_cachedEmissionProxyAlphaCullThreshold = 0.0f;
    m_emissionProxyBuildPending = true;
    m_shadowPrimitiveCountPerSplat = 1;
    m_randomIndices.clear();
    m_randomIndexUploadPending = true;
    InvalidateSortCache();

    return true;
}

void GaussianSplatPass::BuildEmissionProxies(
    uint32_t maxProxyCount,
    float splatScale,
    uint32_t kernelDegree,
    bool adaptiveClamp,
    float3 tintColor,
    float alphaCullThreshold)
{
    kernelDegree = std::min(kernelDegree, 5u);
    tintColor = float3(
        std::max(tintColor.x, 0.0f),
        std::max(tintColor.y, 0.0f),
        std::max(tintColor.z, 0.0f));
    alphaCullThreshold = std::max(alphaCullThreshold, 0.0f);

    if (!HasSplats() || maxProxyCount == 0)
    {
        m_emissionProxies.clear();
        m_cachedEmissionProxyMaxCount = maxProxyCount;
        m_cachedEmissionProxySplatScale = splatScale;
        m_cachedEmissionProxyKernelDegree = kernelDegree;
        m_cachedEmissionProxyAdaptiveClamp = adaptiveClamp;
        m_cachedEmissionProxyTintColor = tintColor;
        m_cachedEmissionProxyAlphaCullThreshold = alphaCullThreshold;
        m_emissionProxyBuildPending = false;
        return;
    }

    const bool tintChanged =
        std::abs(m_cachedEmissionProxyTintColor.x - tintColor.x) >= 1e-4f ||
        std::abs(m_cachedEmissionProxyTintColor.y - tintColor.y) >= 1e-4f ||
        std::abs(m_cachedEmissionProxyTintColor.z - tintColor.z) >= 1e-4f;

    if (!m_emissionProxyBuildPending
        && m_cachedEmissionProxyMaxCount == maxProxyCount
        && std::abs(m_cachedEmissionProxySplatScale - splatScale) < 1e-4f
        && m_cachedEmissionProxyKernelDegree == kernelDegree
        && m_cachedEmissionProxyAdaptiveClamp == adaptiveClamp
        && !tintChanged
        && std::abs(m_cachedEmissionProxyAlphaCullThreshold - alphaCullThreshold) < 1e-6f)
    {
        return;
    }

    std::vector<GaussianSplatEmissionProxy> candidates;
    candidates.reserve(m_splats.size());

    for (const GaussianSplatData& splat : m_splats)
    {
        const float opacity = std::max(splat.centerOpacity.w, 0.0f);
        if (opacity <= alphaCullThreshold)
            continue;

        const float3 extent = GaussianAabbExtent(splat, splatScale, kernelDegree, adaptiveClamp);
        const float radius = std::max(1e-4f, std::max(extent.x, std::max(extent.y, extent.z)));
        const float3 linearSh0 = SrgbToLinear(float3(
            std::max(splat.color.x, 0.0f),
            std::max(splat.color.y, 0.0f),
            std::max(splat.color.z, 0.0f)) * tintColor);
        const float3 radiance = linearSh0 * opacity;
        const float weight = std::max(0.0f, Luminance(radiance)) * radius * radius;
        if (weight <= 0.0f)
            continue;

        GaussianSplatEmissionProxy proxy;
        proxy.center = splat.centerOpacity.xyz();
        proxy.radius = radius;
        proxy.radiance = radiance;
        proxy.weight = weight;
        candidates.push_back(proxy);
    }

    if (candidates.size() > maxProxyCount)
    {
        auto byDescendingWeight = [](const GaussianSplatEmissionProxy& lhs, const GaussianSplatEmissionProxy& rhs)
        {
            return lhs.weight > rhs.weight;
        };

        std::nth_element(candidates.begin(), candidates.begin() + maxProxyCount, candidates.end(), byDescendingWeight);
        candidates.resize(maxProxyCount);
        std::sort(candidates.begin(), candidates.end(), byDescendingWeight);
    }

    m_emissionProxies = std::move(candidates);
    m_cachedEmissionProxyMaxCount = maxProxyCount;
    m_cachedEmissionProxySplatScale = splatScale;
    m_cachedEmissionProxyKernelDegree = kernelDegree;
    m_cachedEmissionProxyAdaptiveClamp = adaptiveClamp;
    m_cachedEmissionProxyTintColor = tintColor;
    m_cachedEmissionProxyAlphaCullThreshold = alphaCullThreshold;
    m_emissionProxyBuildPending = false;
}

void GaussianSplatPass::BuildAccelerationStructures(
    nvrhi::ICommandList* commandList,
    bool useAABBs,
    bool useTLASInstances,
    bool allowBlasCompaction,
    float splatScale,
    uint32_t kernelDegree,
    bool adaptiveClamp)
{
    if (!HasSplats() || !m_splatAabbBuffer)
        return;

    if (!m_accelStructBuildPending
        && m_lastBlasCompaction == allowBlasCompaction
        && m_lastAsUseAABBs == useAABBs
        && m_lastAsUseTLASInstances == useTLASInstances
        && std::abs(m_lastAsSplatScale - splatScale) < 1e-4f
        && m_lastAsKernelDegree == kernelDegree
        && m_lastAsAdaptiveClamp == adaptiveClamp
        && m_splatBottomLevelAS
        && m_splatTopLevelAS)
    {
        return;
    }

    UploadSplatDataIfNeeded(commandList);

    nvrhi::rt::GeometryDesc geometryDesc;
    m_shadowPrimitiveCountPerSplat = useAABBs ? 1u : uint32_t(kUnitIcosahedronIndices.size() / 3u);

    if (useAABBs)
    {
        std::vector<nvrhi::rt::GeometryAABB> aabbs;
        if (useTLASInstances)
        {
            aabbs.push_back({ -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f });
        }
        else
        {
            aabbs = BuildGaussianAabbs(m_splats, splatScale, kernelDegree, adaptiveClamp);
        }

        commandList->writeBuffer(m_splatAabbBuffer, aabbs.data(), aabbs.size() * sizeof(nvrhi::rt::GeometryAABB));
        commandList->setBufferState(m_splatAabbBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
        commandList->commitBarriers();

        nvrhi::rt::GeometryAABBs aabbGeometry;
        aabbGeometry.buffer = m_splatAabbBuffer;
        aabbGeometry.offset = 0;
        aabbGeometry.count = uint32_t(aabbs.size());
        aabbGeometry.stride = sizeof(nvrhi::rt::GeometryAABB);
        geometryDesc.setAABBs(aabbGeometry);
        geometryDesc.flags = nvrhi::rt::GeometryFlags::NoDuplicateAnyHitInvocation;
    }
    else
    {
        std::vector<float3> vertices;
        std::vector<uint32_t> indices;
        if (useTLASInstances)
        {
            vertices.assign(kUnitIcosahedronVertices.begin(), kUnitIcosahedronVertices.end());
            indices.assign(kUnitIcosahedronIndices.begin(), kUnitIcosahedronIndices.end());
        }
        else
        {
            vertices.reserve(size_t(m_splatCount) * kUnitIcosahedronVertices.size());
            indices.reserve(size_t(m_splatCount) * kUnitIcosahedronIndices.size());
            for (uint32_t splatIndex = 0; splatIndex < m_splatCount; ++splatIndex)
            {
                const GaussianSplatData& splat = m_splats[splatIndex];
                const float3 center = splat.centerOpacity.xyz();
                const float3 extent = GaussianAabbExtent(splat, splatScale, kernelDegree, adaptiveClamp);
                const uint32_t vertexBase = uint32_t(vertices.size());
                for (const float3& unitVertex : kUnitIcosahedronVertices)
                    vertices.push_back(center + unitVertex * extent);
                for (uint32_t index : kUnitIcosahedronIndices)
                    indices.push_back(vertexBase + index);
            }
        }

        nvrhi::BufferDesc vertexDesc;
        vertexDesc.byteSize = uint64_t(vertices.size()) * sizeof(float3);
        vertexDesc.structStride = sizeof(float3);
        vertexDesc.format = nvrhi::Format::RGB32_FLOAT;
        vertexDesc.isVertexBuffer = true;
        vertexDesc.isAccelStructBuildInput = true;
        vertexDesc.initialState = nvrhi::ResourceStates::AccelStructBuildInput;
        vertexDesc.keepInitialState = true;
        vertexDesc.debugName = "GaussianSplatIcosahedronVertexBuffer";
        m_splatTriangleVertexBuffer = m_device->createBuffer(vertexDesc);

        nvrhi::BufferDesc indexDesc;
        indexDesc.byteSize = uint64_t(indices.size()) * sizeof(uint32_t);
        indexDesc.format = nvrhi::Format::R32_UINT;
        indexDesc.isIndexBuffer = true;
        indexDesc.isAccelStructBuildInput = true;
        indexDesc.initialState = nvrhi::ResourceStates::AccelStructBuildInput;
        indexDesc.keepInitialState = true;
        indexDesc.debugName = "GaussianSplatIcosahedronIndexBuffer";
        m_splatTriangleIndexBuffer = m_device->createBuffer(indexDesc);

        commandList->writeBuffer(m_splatTriangleVertexBuffer, vertices.data(), vertices.size() * sizeof(float3));
        commandList->writeBuffer(m_splatTriangleIndexBuffer, indices.data(), indices.size() * sizeof(uint32_t));
        commandList->setBufferState(m_splatTriangleVertexBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
        commandList->setBufferState(m_splatTriangleIndexBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
        commandList->commitBarriers();

        nvrhi::rt::GeometryTriangles triangles;
        triangles.vertexBuffer = m_splatTriangleVertexBuffer;
        triangles.indexBuffer = m_splatTriangleIndexBuffer;
        triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
        triangles.indexFormat = nvrhi::Format::R32_UINT;
        triangles.vertexStride = sizeof(float3);
        triangles.vertexCount = uint32_t(vertices.size());
        triangles.indexCount = uint32_t(indices.size());
        geometryDesc.setTriangles(triangles);
        geometryDesc.flags = nvrhi::rt::GeometryFlags::None;
    }

    nvrhi::rt::AccelStructDesc blasDesc;
    blasDesc.isTopLevel = false;
    blasDesc.debugName = useAABBs ? "GaussianSplatAabbBLAS" : "GaussianSplatIcosahedronBLAS";
    blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace
        | (allowBlasCompaction ? nvrhi::rt::AccelStructBuildFlags::AllowCompaction : nvrhi::rt::AccelStructBuildFlags::AllowUpdate);
    blasDesc.bottomLevelGeometries.push_back(geometryDesc);

    m_splatBottomLevelAS = m_device->createAccelStruct(blasDesc);
    nvrhi::utils::BuildBottomLevelAccelStruct(commandList, m_splatBottomLevelAS, blasDesc);

    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.debugName = "GaussianSplatTLAS";
    tlasDesc.topLevelMaxInstances = useTLASInstances ? m_splatCount : 1;
    tlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace | nvrhi::rt::AccelStructBuildFlags::AllowUpdate;
    m_splatTopLevelAS = m_device->createAccelStruct(tlasDesc);

    std::vector<nvrhi::rt::InstanceDesc> instances;
    instances.resize(useTLASInstances ? m_splatCount : 1u);
    if (useTLASInstances)
    {
        for (uint32_t splatIndex = 0; splatIndex < m_splatCount; ++splatIndex)
        {
            const GaussianSplatData& splat = m_splats[splatIndex];
            nvrhi::rt::InstanceDesc& instanceDesc = instances[splatIndex];
            instanceDesc.bottomLevelAS = m_splatBottomLevelAS;
            instanceDesc.instanceMask = 0xff;
            instanceDesc.instanceID = splatIndex;
            instanceDesc.instanceContributionToHitGroupIndex = 0;
            instanceDesc.flags = nvrhi::rt::InstanceFlags::ForceNonOpaque;
            FillScaleTranslateTransform(instanceDesc.transform, splat.centerOpacity.xyz(), GaussianAabbExtent(splat, splatScale, kernelDegree, adaptiveClamp));
        }
    }
    else
    {
        nvrhi::rt::InstanceDesc& instanceDesc = instances[0];
        instanceDesc.bottomLevelAS = m_splatBottomLevelAS;
        instanceDesc.instanceMask = 0xff;
        instanceDesc.instanceID = 0;
        instanceDesc.instanceContributionToHitGroupIndex = 0;
        instanceDesc.flags = nvrhi::rt::InstanceFlags::ForceNonOpaque;
        std::memcpy(instanceDesc.transform, nvrhi::rt::c_IdentityTransform, sizeof(nvrhi::rt::AffineTransform));
    }

    commandList->buildTopLevelAccelStruct(
        m_splatTopLevelAS,
        instances.data(),
        instances.size(),
        nvrhi::rt::AccelStructBuildFlags::PreferFastTrace | nvrhi::rt::AccelStructBuildFlags::AllowUpdate);

    m_accelStructBuildPending = false;
    m_lastBlasCompaction = allowBlasCompaction;
    m_lastAsUseAABBs = useAABBs;
    m_lastAsUseTLASInstances = useTLASInstances;
    m_lastAsSplatScale = splatScale;
    m_lastAsKernelDegree = kernelDegree;
    m_lastAsAdaptiveClamp = adaptiveClamp;
}

void GaussianSplatPass::ReleaseAccelerationStructures()
{
    m_splatBottomLevelAS = nullptr;
    m_splatTopLevelAS = nullptr;
    m_accelStructBuildPending = HasSplats();
}

void GaussianSplatPass::CreateBindingSets(const RenderTargets& renderTargets, nvrhi::rt::IAccelStruct* meshTopLevelAS)
{
    if (!m_splatBuffer || !m_colorBuffer || !m_shBuffer || !m_indexBuffer || !m_sortKeyBuffer || !m_sortControlBuffer || !m_drawIndirectBuffer)
        return;

    nvrhi::BindingSetDesc rasterRenderBindingSetDesc;
    rasterRenderBindingSetDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_splatBuffer),
        nvrhi::BindingSetItem::TypedBuffer_SRV(1, m_indexBuffer, nvrhi::Format::R32_UINT),
        nvrhi::BindingSetItem::RawBuffer_SRV(2, m_colorBuffer),
        nvrhi::BindingSetItem::RawBuffer_SRV(3, m_shBuffer),
        nvrhi::BindingSetItem::Texture_SRV(4, renderTargets.Depth)
    };
    m_rasterRenderBindingSet = m_device->createBindingSet(rasterRenderBindingSetDesc, m_rasterRenderBindingLayout);

    if (meshTopLevelAS != nullptr)
    {
        nvrhi::BindingSetDesc hybridRenderBindingSetDesc = rasterRenderBindingSetDesc;
        hybridRenderBindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::RayTracingAccelStruct(5, meshTopLevelAS));
        m_hybridRenderBindingSet = m_device->createBindingSet(hybridRenderBindingSetDesc, m_hybridRenderBindingLayout);
        m_hybridRenderMeshTopLevelAS = meshTopLevelAS;
    }
    else
    {
        m_hybridRenderBindingSet = nullptr;
        m_hybridRenderMeshTopLevelAS = nullptr;
    }

    nvrhi::BindingSetDesc sortKeyBindingSetDesc;
    sortKeyBindingSetDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_splatBuffer),
        nvrhi::BindingSetItem::TypedBuffer_UAV(0, m_sortKeyBuffer, nvrhi::Format::R32_UINT),
        nvrhi::BindingSetItem::TypedBuffer_UAV(1, m_indexBuffer, nvrhi::Format::R32_UINT),
        nvrhi::BindingSetItem::TypedBuffer_UAV(2, m_sortControlBuffer, nvrhi::Format::R32_UINT),
        nvrhi::BindingSetItem::TypedBuffer_UAV(3, m_drawIndirectBuffer, nvrhi::Format::R32_UINT)
    };
    m_sortKeyBindingSet = m_device->createBindingSet(sortKeyBindingSetDesc, m_sortKeyBindingLayout);
}

void GaussianSplatPass::CreateStochasticFramebuffer(const RenderTargets& renderTargets)
{
    auto createFramebuffer = [this](
        const nvrhi::TextureHandle& colorTarget,
        nvrhi::TextureHandle& depthBuffer,
        std::shared_ptr<caustica::FramebufferFactory>& framebuffer,
        const char* depthName)
    {
        if (!colorTarget)
            return;

        const nvrhi::TextureDesc& colorDesc = colorTarget->getDesc();
        bool depthMatches = false;
        if (depthBuffer)
        {
            const nvrhi::TextureDesc& depthDesc = depthBuffer->getDesc();
            depthMatches = depthDesc.width == colorDesc.width
                && depthDesc.height == colorDesc.height
                && depthDesc.sampleCount == colorDesc.sampleCount
                && depthDesc.sampleQuality == colorDesc.sampleQuality;
        }

        if (!depthMatches)
        {
            const std::array<nvrhi::Format, 4> depthFormats = {
                nvrhi::Format::D32,
                nvrhi::Format::D24S8,
                nvrhi::Format::D32S8,
                nvrhi::Format::D16
            };
            const nvrhi::FormatSupport depthFeatures =
                nvrhi::FormatSupport::Texture |
                nvrhi::FormatSupport::DepthStencil;

            nvrhi::TextureDesc depthDesc;
            depthDesc.width = colorDesc.width;
            depthDesc.height = colorDesc.height;
            depthDesc.sampleCount = colorDesc.sampleCount;
            depthDesc.sampleQuality = colorDesc.sampleQuality;
            depthDesc.dimension = colorDesc.dimension;
            depthDesc.mipLevels = 1;
            depthDesc.format = nvrhi::utils::ChooseFormat(m_device, depthFeatures, depthFormats.data(), depthFormats.size());
            depthDesc.isTypeless = true;
            depthDesc.isRenderTarget = true;
            depthDesc.isUAV = false;
            depthDesc.useClearValue = true;
            depthDesc.clearValue = nvrhi::Color(0.0f);
            depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
            depthDesc.keepInitialState = true;
            depthDesc.debugName = depthName;
            depthBuffer = m_device->createTexture(depthDesc);
        }

        const bool framebufferMatches = framebuffer
            && !framebuffer->RenderTargets.empty()
            && framebuffer->RenderTargets[0].Get() == colorTarget.Get()
            && framebuffer->DepthTarget.Get() == depthBuffer.Get();
        if (!framebufferMatches)
        {
            framebuffer = std::make_shared<caustica::FramebufferFactory>(m_device);
            framebuffer->RenderTargets = { colorTarget };
            framebuffer->DepthTarget = depthBuffer;
        }
    };

    createFramebuffer(renderTargets.OutputColor, m_stochasticDepthBuffer, m_stochasticFramebuffer, "GaussianSplatStochasticDepth");
    createFramebuffer(renderTargets.ProcessedOutputColor, m_stochasticProcessedDepthBuffer, m_stochasticProcessedFramebuffer, "GaussianSplatStochasticProcessedDepth");
}

void GaussianSplatPass::CreatePipeline(const RenderTargets& renderTargets)
{
    if (!HasSplats())
        return;

    CreateStochasticFramebuffer(renderTargets);

    std::vector<caustica::ShaderMacro> rasterShadowMacros = {
        caustica::ShaderMacro({ "GAUSSIAN_SPLAT_HYBRID_SHADOWS", "0" })
    };
    m_rasterVertexShader = m_shaderFactory->CreateShader("caustica/shaders/render/ProcessingPasses/GaussianSplatRaster.hlsl", "vs_main", &rasterShadowMacros, nvrhi::ShaderType::Vertex);
    m_rasterPixelShader = m_shaderFactory->CreateShader("caustica/shaders/render/ProcessingPasses/GaussianSplatRaster.hlsl", "ps_main", &rasterShadowMacros, nvrhi::ShaderType::Pixel);

    std::vector<caustica::ShaderMacro> hybridShadowMacros = {
        caustica::ShaderMacro({ "GAUSSIAN_SPLAT_HYBRID_SHADOWS", "1" })
    };
    m_hybridVertexShader = m_shaderFactory->CreateShader("caustica/shaders/render/ProcessingPasses/GaussianSplatRaster.hlsl", "vs_main", &hybridShadowMacros, nvrhi::ShaderType::Vertex);
    m_hybridPixelShader = m_shaderFactory->CreateShader("caustica/shaders/render/ProcessingPasses/GaussianSplatRaster.hlsl", "ps_main", &hybridShadowMacros, nvrhi::ShaderType::Pixel);

    std::vector<caustica::ShaderMacro> sortKeyMacros = {
        caustica::ShaderMacro({ "GAUSSIAN_SPLAT_SORT_KEYS", "1" })
    };
    m_sortKeyShader = m_shaderFactory->CreateShader("caustica/shaders/render/ProcessingPasses/GaussianSplatRaster.hlsl", "cs_sort_keys", &sortKeyMacros, nvrhi::ShaderType::Compute);

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_rasterRenderBindingLayout };
    pipelineDesc.VS = m_rasterVertexShader;
    pipelineDesc.PS = m_rasterPixelShader;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    pipelineDesc.renderState.rasterState.depthClipEnable = true;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;

    nvrhi::BlendState::RenderTarget alphaBlend;
    alphaBlend.blendEnable = true;
    alphaBlend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
    alphaBlend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
    alphaBlend.srcBlendAlpha = nvrhi::BlendFactor::One;
    alphaBlend.destBlendAlpha = nvrhi::BlendFactor::One;
    pipelineDesc.renderState.blendState.targets[0] = alphaBlend;

    m_rasterRenderPipeline = m_device->createGraphicsPipeline(
        pipelineDesc,
        renderTargets.ProcessedOutputFramebuffer->GetFramebuffer(nvrhi::AllSubresources));

    pipelineDesc.bindingLayouts = { m_hybridRenderBindingLayout };
    pipelineDesc.VS = m_hybridVertexShader;
    pipelineDesc.PS = m_hybridPixelShader;
    m_hybridRenderPipeline = m_device->createGraphicsPipeline(
        pipelineDesc,
        renderTargets.ProcessedOutputFramebuffer->GetFramebuffer(nvrhi::AllSubresources));

    if (m_stochasticFramebuffer)
    {
        nvrhi::BlendState::RenderTarget opaqueBlend;
        opaqueBlend.blendEnable = false;
        pipelineDesc.renderState.blendState.targets[0] = opaqueBlend;
        pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
        pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;
        pipelineDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;

        pipelineDesc.bindingLayouts = { m_rasterRenderBindingLayout };
        pipelineDesc.VS = m_rasterVertexShader;
        pipelineDesc.PS = m_rasterPixelShader;
        m_stochasticRasterRenderPipeline = m_device->createGraphicsPipeline(
            pipelineDesc,
            m_stochasticFramebuffer->GetFramebuffer(nvrhi::AllSubresources));

        pipelineDesc.bindingLayouts = { m_hybridRenderBindingLayout };
        pipelineDesc.VS = m_hybridVertexShader;
        pipelineDesc.PS = m_hybridPixelShader;
        m_stochasticHybridRenderPipeline = m_device->createGraphicsPipeline(
            pipelineDesc,
            m_stochasticFramebuffer->GetFramebuffer(nvrhi::AllSubresources));
    }

    if (m_stochasticProcessedFramebuffer)
    {
        nvrhi::BlendState::RenderTarget opaqueBlend;
        opaqueBlend.blendEnable = false;
        pipelineDesc.renderState.blendState.targets[0] = opaqueBlend;
        pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
        pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;
        pipelineDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;

        pipelineDesc.bindingLayouts = { m_rasterRenderBindingLayout };
        pipelineDesc.VS = m_rasterVertexShader;
        pipelineDesc.PS = m_rasterPixelShader;
        m_stochasticProcessedRasterRenderPipeline = m_device->createGraphicsPipeline(
            pipelineDesc,
            m_stochasticProcessedFramebuffer->GetFramebuffer(nvrhi::AllSubresources));

        pipelineDesc.bindingLayouts = { m_hybridRenderBindingLayout };
        pipelineDesc.VS = m_hybridVertexShader;
        pipelineDesc.PS = m_hybridPixelShader;
        m_stochasticProcessedHybridRenderPipeline = m_device->createGraphicsPipeline(
            pipelineDesc,
            m_stochasticProcessedFramebuffer->GetFramebuffer(nvrhi::AllSubresources));
    }

    nvrhi::ComputePipelineDesc computePipelineDesc;
    computePipelineDesc.bindingLayouts = { m_sortKeyBindingLayout };
    computePipelineDesc.CS = m_sortKeyShader;
    m_sortKeyPipeline = m_device->createComputePipeline(computePipelineDesc);

    m_rasterRenderBindingSet = nullptr;
    m_hybridRenderBindingSet = nullptr;
    m_hybridRenderMeshTopLevelAS = nullptr;
}

void GaussianSplatPass::UploadSplatDataIfNeeded(nvrhi::ICommandList* commandList)
{
    if (!m_splatUploadPending || m_splats.empty())
        return;

    commandList->writeBuffer(m_splatBuffer, m_splats.data(), m_splats.size() * sizeof(GaussianSplatData));
    m_splatUploadPending = false;
}

void GaussianSplatPass::UploadFormatDataIfNeeded(
    nvrhi::ICommandList* commandList,
    GaussianSplatStorageFormat shFormat,
    GaussianSplatStorageFormat rgbaFormat)
{
    if (!HasSplats())
        return;

    const bool formatChanged = !m_colorBuffer || !m_shBuffer || shFormat != m_currentShFormat || rgbaFormat != m_currentRgbaFormat;
    if (formatChanged)
    {
        m_currentShFormat = shFormat;
        m_currentRgbaFormat = rgbaFormat;
        m_formatUploadPending = true;

        const uint64_t colorByteSize = AlignRawBufferSize(uint64_t(m_splatCount) * 4u * FormatElementSize(rgbaFormat));
        nvrhi::BufferDesc colorDesc;
        colorDesc.byteSize = colorByteSize;
        colorDesc.canHaveRawViews = true;
        colorDesc.debugName = "GaussianSplatRGBAFormatBuffer";
        colorDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        colorDesc.keepInitialState = true;
        m_colorBuffer = m_device->createBuffer(colorDesc);

        constexpr uint32_t kShScalarStride = 45;
        const uint64_t shByteSize = AlignRawBufferSize(uint64_t(m_splatCount) * kShScalarStride * FormatElementSize(shFormat));
        nvrhi::BufferDesc shDesc;
        shDesc.byteSize = shByteSize;
        shDesc.canHaveRawViews = true;
        shDesc.debugName = "GaussianSplatSHFormatBuffer";
        shDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        shDesc.keepInitialState = true;
        m_shBuffer = m_device->createBuffer(shDesc);

        m_rasterRenderBindingSet = nullptr;
        m_hybridRenderBindingSet = nullptr;
        m_hybridRenderMeshTopLevelAS = nullptr;
    }

    if (!m_formatUploadPending)
        return;

    m_packedColorOpacity.assign(size_t(AlignRawBufferSize(uint64_t(m_splatCount) * 4u * FormatElementSize(rgbaFormat))), 0u);
    for (uint32_t splatIndex = 0; splatIndex < m_splatCount; ++splatIndex)
    {
        const float4 color = splatIndex < m_colorOpacity.size()
            ? m_colorOpacity[splatIndex]
            : float4(1.0f, 1.0f, 1.0f, 1.0f);
        const uint64_t base = uint64_t(splatIndex) * 4u;
        StoreFormattedScalar(m_packedColorOpacity, base + 0u, rgbaFormat, color.x, false);
        StoreFormattedScalar(m_packedColorOpacity, base + 1u, rgbaFormat, color.y, false);
        StoreFormattedScalar(m_packedColorOpacity, base + 2u, rgbaFormat, color.z, false);
        StoreFormattedScalar(m_packedColorOpacity, base + 3u, rgbaFormat, color.w, false);
    }

    constexpr uint32_t kShScalarStride = 45;
    m_packedShCoefficients.assign(size_t(AlignRawBufferSize(uint64_t(m_splatCount) * kShScalarStride * FormatElementSize(shFormat))), 0u);
    for (uint32_t splatIndex = 0; splatIndex < m_splatCount; ++splatIndex)
    {
        for (uint32_t scalarIndex = 0; scalarIndex < kShScalarStride; ++scalarIndex)
        {
            StoreFormattedScalar(
                m_packedShCoefficients,
                uint64_t(splatIndex) * kShScalarStride + scalarIndex,
                shFormat,
                ShCoefficientAt(m_shCoefficients, splatIndex, scalarIndex),
                true);
        }
    }

    commandList->writeBuffer(m_colorBuffer, m_packedColorOpacity.data(), m_packedColorOpacity.size());
    commandList->writeBuffer(m_shBuffer, m_packedShCoefficients.data(), m_packedShCoefficients.size());
    m_formatUploadPending = false;
}

void GaussianSplatPass::UploadStochasticSplatIndices(nvrhi::ICommandList* commandList)
{
    if (!m_indexBuffer || m_splatCount == 0)
        return;

    if (m_randomIndices.size() != m_splatCount)
    {
        m_randomIndices.resize(m_splatCount);
        std::iota(m_randomIndices.begin(), m_randomIndices.end(), 0u);
        std::mt19937 rng(0x3d05da7au);
        std::shuffle(m_randomIndices.begin(), m_randomIndices.end(), rng);
        m_randomIndexUploadPending = true;
    }

    if (!m_randomIndexUploadPending)
        return;

    commandList->writeBuffer(m_indexBuffer, m_randomIndices.data(), m_randomIndices.size() * sizeof(uint32_t));
    m_randomIndexUploadPending = false;
}

void GaussianSplatPass::BuildDistanceCulledSplatList(nvrhi::ICommandList* commandList, GaussianSplatSortMode sortMode)
{
    if (!m_sortKeyBindingSet || !m_sortKeyPipeline || !m_sortControlBuffer || !m_drawIndirectBuffer)
        return;

    const uint32_t zero = 0;
    nvrhi::DrawIndirectArguments drawArgs;
    drawArgs.vertexCount = 0;
    drawArgs.instanceCount = 1;
    drawArgs.startVertexLocation = 0;
    drawArgs.startInstanceLocation = 0;
    commandList->writeBuffer(m_sortControlBuffer, &zero, sizeof(zero));
    commandList->writeBuffer(m_drawIndirectBuffer, &drawArgs, sizeof(drawArgs));

    {
        nvrhi::ComputeState state;
        state.pipeline = m_sortKeyPipeline;
        state.bindings = { m_sortKeyBindingSet };

        commandList->setBufferState(m_sortKeyBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_indexBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_sortControlBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_drawIndirectBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();

        commandList->setComputeState(state);
        commandList->dispatch((m_splatCount + 255u) / 256u, 1, 1);
    }

    if (sortMode == GaussianSplatSortMode::GpuSort && m_gpuSort)
    {
        commandList->setBufferState(m_sortKeyBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_indexBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_sortControlBuffer, nvrhi::ResourceStates::CopySource);
        commandList->commitBarriers();

        m_gpuSort->Sort(commandList, m_sortControlBuffer, 0, m_sortKeyBuffer, m_indexBuffer, m_splatCount, false);
    }

    commandList->setBufferState(m_indexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(m_drawIndirectBuffer, nvrhi::ResourceStates::IndirectArgument);
    commandList->commitBarriers();

    m_cachedSortMode = sortMode;
    m_sortCacheValid = false;
    m_randomIndexUploadPending = true;
}

void GaussianSplatPass::UpdateSplatIndices(nvrhi::ICommandList* commandList, const GaussianSplatConstants& constants, GaussianSplatSortMode sortMode)
{
    if (constants.frustumCulling == uint32_t(GaussianSplatFrustumCulling::AtDistanceStage))
    {
        BuildDistanceCulledSplatList(commandList, sortMode);
        return;
    }

    if (sortMode != m_cachedSortMode && sortMode == GaussianSplatSortMode::StochasticSplats)
        m_randomIndexUploadPending = true;

    if (sortMode == GaussianSplatSortMode::StochasticSplats)
    {
        UploadStochasticSplatIndices(commandList);
        m_cachedSortMode = sortMode;
        m_sortCacheValid = false;
        return;
    }

    if (!m_gpuSort || !m_sortKeyBindingSet || !m_sortKeyPipeline || !m_sortControlBuffer || !m_drawIndirectBuffer)
        return;

    if (m_cachedSortMode == sortMode && CanReuseSort(constants))
        return;

    {
        nvrhi::ComputeState state;
        state.pipeline = m_sortKeyPipeline;
        state.bindings = { m_sortKeyBindingSet };

        commandList->setBufferState(m_sortKeyBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_indexBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_sortControlBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_drawIndirectBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();

        commandList->setComputeState(state);
        commandList->dispatch((m_splatCount + 255u) / 256u, 1, 1);
    }

    commandList->writeBuffer(m_sortControlBuffer, &m_splatCount, sizeof(m_splatCount));

    commandList->setBufferState(m_sortKeyBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(m_indexBuffer, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(m_sortControlBuffer, nvrhi::ResourceStates::CopySource);
    commandList->commitBarriers();

    m_gpuSort->Sort(commandList, m_sortControlBuffer, 0, m_sortKeyBuffer, m_indexBuffer, m_splatCount, true);

    m_cachedSortWorldToClipNoOffset = constants.view.matWorldToClipNoOffset;
    m_cachedSortObjectToWorld = constants.objectToWorld;
    m_cachedSortSplatCount = m_splatCount;
    m_cachedSortMode = sortMode;
    m_sortCacheValid = true;
}

void GaussianSplatPass::Render(
    nvrhi::ICommandList* commandList,
    const caustica::IView& view,
    nvrhi::rt::IAccelStruct* meshTopLevelAS,
    const RenderTargets& renderTargets,
    const GaussianSplatRenderSettings& settings)
{
    if (!settings.enabled || !HasSplats())
        return;

    if (settings.sortingMode == GaussianSplatSortMode::GpuSort && !m_gpuSort)
        return;

    const bool stochasticSplats = settings.sortingMode == GaussianSplatSortMode::StochasticSplats;
    const bool distanceStageCulling = settings.frustumCulling == GaussianSplatFrustumCulling::AtDistanceStage;
    const bool stochasticToOutput = stochasticSplats && settings.renderTarget == GaussianSplatRenderTarget::OutputColor;
    nvrhi::TextureHandle stochasticDepthBuffer = stochasticToOutput ? m_stochasticDepthBuffer : m_stochasticProcessedDepthBuffer;
    std::shared_ptr<caustica::FramebufferFactory> stochasticFramebuffer = stochasticToOutput
        ? m_stochasticFramebuffer
        : m_stochasticProcessedFramebuffer;
    if (stochasticSplats && (!stochasticFramebuffer || !stochasticDepthBuffer))
        return;
    if (!stochasticSplats && !m_rasterRenderPipeline)
        return;

    commandList->beginMarker("GaussianSplats");

    UploadSplatDataIfNeeded(commandList);
    UploadFormatDataIfNeeded(commandList, settings.shFormat, settings.rgbaFormat);

    const bool useHybridShadows = settings.shadowsEnabled && meshTopLevelAS != nullptr && m_hybridRenderPipeline;

    if (!m_rasterRenderBindingSet || (useHybridShadows && (!m_hybridRenderBindingSet || m_hybridRenderMeshTopLevelAS != meshTopLevelAS)))
        CreateBindingSets(renderTargets, useHybridShadows ? meshTopLevelAS : nullptr);

    nvrhi::BindingSetHandle renderBindingSet = useHybridShadows ? m_hybridRenderBindingSet : m_rasterRenderBindingSet;
    if (!renderBindingSet)
    {
        commandList->endMarker();
        return;
    }

    PlanarViewConstants planarView = {};
    view.FillPlanarViewConstants(planarView);

    GaussianSplatConstants constants = {};
    constants.view = FromPlanarViewConstants(planarView);
    const float3 cameraPosition = view.GetViewOrigin();
    constants.cameraPosition = float4(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f);
    constants.objectToWorld = settings.objectToWorld;
    constants.splatScale = settings.splatScale;
    constants.alphaScale = settings.alphaScale;
    constants.brightness = settings.brightness;
    constants.splatCount = m_splatCount;
    constants.tintColor = float3(
        std::max(settings.tintColor.x, 0.0f),
        std::max(settings.tintColor.y, 0.0f),
        std::max(settings.tintColor.z, 0.0f));
    constants.alphaCullThreshold = settings.alphaCullThreshold;
    constants.shDegree = m_shDegree;
    constants.depthTest = settings.depthTest ? 1u : 0u;
    constants.shadowsEnabled = useHybridShadows ? 1u : 0u;
    float3 shadowDir = settings.shadowDirectionToLight;
    if (length(shadowDir) < 1e-4f)
        shadowDir = float3(0.0f, 1.0f, 0.0f);
    shadowDir = normalize(shadowDir);
    constants.shadowDirectionToLight = float4(shadowDir.x, shadowDir.y, shadowDir.z, settings.shadowRayOffset);
    constants.shadowStrength = settings.shadowStrength;
    constants.shadowRayTMax = settings.shadowRayTMax;
    constants.shadowMode = useHybridShadows ? settings.shadowMode : GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    constants.shadowSoftSampleCount = std::clamp(settings.shadowSoftSampleCount, 1u, 16u);
    constants.shadowSoftRadius = settings.shadowSoftRadius;
    constants.shadowFrameIndex = settings.shadowFrameIndex;
    constants.sortMode = uint32_t(settings.sortingMode);
    constants.frustumCulling = uint32_t(settings.frustumCulling);
    constants.frustumDilation = settings.frustumDilation;
    constants.minPixelCoverage = settings.minPixelCoverage;
    constants.screenSizeCulling = settings.screenSizeCulling ? 1u : 0u;
    constants.mipSplattingAntialiasing = settings.mipSplattingAntialiasing ? 1u : 0u;
    constants.shFormat = uint32_t(settings.shFormat);
    constants.rgbaFormat = uint32_t(settings.rgbaFormat);
    constants.projectionMethod = uint32_t(settings.projectionMethod);
    constants.stochasticFrameIndex = settings.stochasticFrameIndex;
    commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    UpdateSplatIndices(commandList, constants, settings.sortingMode);

    nvrhi::GraphicsPipelineHandle renderPipeline;
    if (useHybridShadows)
    {
        renderPipeline = stochasticSplats
            ? (stochasticToOutput ? m_stochasticHybridRenderPipeline : m_stochasticProcessedHybridRenderPipeline)
            : m_hybridRenderPipeline;
    }
    else
    {
        renderPipeline = stochasticSplats
            ? (stochasticToOutput ? m_stochasticRasterRenderPipeline : m_stochasticProcessedRasterRenderPipeline)
            : m_rasterRenderPipeline;
    }
    nvrhi::IFramebuffer* framebuffer = stochasticSplats
        ? stochasticFramebuffer->GetFramebuffer(nvrhi::AllSubresources)
        : renderTargets.ProcessedOutputFramebuffer->GetFramebuffer(nvrhi::AllSubresources);
    if (!renderPipeline || !framebuffer)
    {
        commandList->endMarker();
        return;
    }

    commandList->setBufferState(m_indexBuffer, nvrhi::ResourceStates::ShaderResource);
    if (distanceStageCulling)
        commandList->setBufferState(m_drawIndirectBuffer, nvrhi::ResourceStates::IndirectArgument);
    commandList->setTextureState(renderTargets.Depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    nvrhi::TextureHandle colorTarget = stochasticToOutput ? renderTargets.OutputColor : renderTargets.ProcessedOutputColor;
    commandList->setTextureState(colorTarget, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    if (stochasticSplats)
        commandList->setTextureState(stochasticDepthBuffer, nvrhi::AllSubresources, nvrhi::ResourceStates::DepthWrite);
    commandList->commitBarriers();

    if (stochasticSplats)
    {
        const nvrhi::FormatInfo& depthFormatInfo = nvrhi::getFormatInfo(stochasticDepthBuffer->getDesc().format);
        commandList->clearDepthStencilTexture(stochasticDepthBuffer, nvrhi::AllSubresources, true, 0.0f, depthFormatInfo.hasStencil, 0);
    }

    nvrhi::GraphicsState state;
    state.pipeline = renderPipeline;
    state.bindings = { renderBindingSet };
    state.framebuffer = framebuffer;
    state.viewport = view.GetViewportState();
    if (distanceStageCulling)
        state.indirectParams = m_drawIndirectBuffer;
    commandList->setGraphicsState(state);

    if (distanceStageCulling)
    {
        commandList->drawIndirect(0);
    }
    else
    {
        nvrhi::DrawArguments args;
        args.vertexCount = m_splatCount * 6;
        args.instanceCount = 1;
        commandList->draw(args);
    }

    commandList->endMarker();
}
