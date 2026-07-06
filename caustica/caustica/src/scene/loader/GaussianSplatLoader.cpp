#include <scene/loader/GaussianSplatLoader.h>

#include <core/log.h>
#include <math/math.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace caustica::math;

namespace
{
    constexpr float kSH_C0 = 0.28209479177387814f;
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

    caustica::GaussianSplatData ConvertToGpuSplat(const RawGaussianSplat& raw, bool convertRdfToRub)
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

        caustica::GaussianSplatData splat = {};
        splat.centerOpacity = float4(position[0], position[1], position[2], Clamp01(raw.alpha));
        splat.covariance0 = float4(cov00, cov01, cov02, cov11);
        splat.covariance1 = float4(cov12, cov22, 0.0f, 0.0f);
        splat.color = float4(Clamp01(raw.color[0]), Clamp01(raw.color[1]), Clamp01(raw.color[2]), 0.0f);

        return splat;
    }

    bool LoadPlyFile(
        const std::filesystem::path& fileName,
        bool convertRdfToRub,
        std::vector<caustica::GaussianSplatData>& splats,
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
        shCoefficients.reserve(size_t(vertexElement.count) * caustica::kGaussianSplatShFloat4Count);

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

                std::array<float, caustica::kGaussianSplatShFloat4Count * 4> packedSh = {};
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

                for (uint32_t i = 0; i < caustica::kGaussianSplatShFloat4Count; ++i)
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

} // namespace

namespace caustica
{

bool loadGaussianSplatPly(
    const std::filesystem::path& filePath,
    bool convertRdfToRub,
    GaussianSplatDataset& outDataset)
{
    const std::string extension = [](std::string ext) {
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });
        return ext;
    }(filePath.extension().string());

    if (extension != ".ply")
    {
        error("Unsupported Gaussian splat file extension '%s'. Only 3DGS .ply files are supported.", extension.c_str());
        return false;
    }

    outDataset.splats.clear();
    outDataset.shCoefficients.clear();
    outDataset.shDegree = 0;
    outDataset.sourcePath = filePath.string();

    if (!LoadPlyFile(filePath, convertRdfToRub, outDataset.splats, outDataset.shCoefficients, outDataset.shDegree))
        return false;

    return !outDataset.splats.empty();
}

} // namespace caustica
