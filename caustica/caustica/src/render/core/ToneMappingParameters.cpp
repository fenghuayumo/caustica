#include <render/core/ToneMappingParameters.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
    using caustica::math::float3;
    using caustica::math::float4;

    float component(const float3& value, uint32_t channel)
    {
        return channel == 0 ? value.x : (channel == 1 ? value.y : value.z);
    }

    float3 lerp(const float3& a, const float3& b, float t)
    {
        return a * (1.0f - t) + b * t;
    }

    std::string trim(const std::string& value)
    {
        const size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        const size_t last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }
}

bool ToneMappingParameters::loadCameraLut(const std::string& path, std::string* error)
{
    try
    {
        std::ifstream stream(std::filesystem::u8path(path));
        if (!stream)
            throw std::runtime_error("Cannot open camera LUT: " + path);

        uint32_t declared1DSize = 0;
        uint32_t declared3DSize = 0;
        float3 domainMin(0.0f);
        float3 domainMax(1.0f);
        std::vector<float3> source;
        std::string rawLine;
        uint32_t lineNumber = 0;

        while (std::getline(stream, rawLine))
        {
            ++lineNumber;
            const size_t comment = rawLine.find('#');
            const std::string line = trim(rawLine.substr(0, comment));
            if (line.empty())
                continue;

            std::istringstream fields(line);
            std::string keyword;
            fields >> keyword;
            if (lineNumber == 1 && keyword.size() >= 3 &&
                static_cast<unsigned char>(keyword[0]) == 0xef &&
                static_cast<unsigned char>(keyword[1]) == 0xbb &&
                static_cast<unsigned char>(keyword[2]) == 0xbf)
                keyword.erase(0, 3);
            std::transform(keyword.begin(), keyword.end(), keyword.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (keyword == "TITLE")
                continue;
            if (keyword == "LUT_1D_SIZE")
            {
                if (!(fields >> declared1DSize) || declared1DSize < 2)
                    throw std::runtime_error("Invalid LUT_1D_SIZE at line " + std::to_string(lineNumber));
                continue;
            }
            if (keyword == "LUT_3D_SIZE")
            {
                if (!(fields >> declared3DSize) || declared3DSize < 2 || declared3DSize > 64)
                    throw std::runtime_error("LUT_3D_SIZE must be in [2, 64] at line " + std::to_string(lineNumber));
                continue;
            }
            if (keyword == "DOMAIN_MIN" || keyword == "DOMAIN_MAX")
            {
                float3 value;
                if (!(fields >> value.x >> value.y >> value.z))
                    throw std::runtime_error("Invalid " + keyword + " at line " + std::to_string(lineNumber));
                if (keyword == "DOMAIN_MIN") domainMin = value;
                else domainMax = value;
                continue;
            }

            std::istringstream values(line);
            float3 entry;
            std::string trailing;
            if (!(values >> entry.x >> entry.y >> entry.z) || (values >> trailing))
                throw std::runtime_error("Unsupported .cube content at line " + std::to_string(lineNumber));
            source.push_back(entry);
        }

        if ((declared1DSize == 0) == (declared3DSize == 0))
            throw std::runtime_error("A .cube file must declare exactly one of LUT_1D_SIZE or LUT_3D_SIZE");
        const uint64_t expectedEntries = declared1DSize != 0
            ? declared1DSize
            : uint64_t(declared3DSize) * declared3DSize * declared3DSize;
        if (source.size() != expectedEntries)
            throw std::runtime_error(
                "LUT size declares " + std::to_string(expectedEntries) +
                " entries, but " + std::to_string(source.size()) + " were found");
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            if (!std::isfinite(component(domainMin, channel)) ||
                !std::isfinite(component(domainMax, channel)) ||
                component(domainMax, channel) <= component(domainMin, channel))
                throw std::runtime_error("DOMAIN_MAX must be finite and greater than DOMAIN_MIN");
        }
        for (const float3& entry : source)
        {
            if (!std::isfinite(entry.x) || !std::isfinite(entry.y) || !std::isfinite(entry.z))
                throw std::runtime_error("Camera LUT entries must be finite");
        }

        cameraLutDomainMin = domainMin;
        cameraLutDomainMax = domainMax;
        cameraLutPath = path;
        cameraLutEnabled = true;
        cameraLutPreset = CameraLutPreset::CustomFile;
        if (declared3DSize != 0)
        {
            auto table = std::make_shared<std::vector<float4>>(expectedEntries);
            // .cube data is B-fastest; Texture3D memory is R-fastest.
            for (uint32_t r = 0; r < declared3DSize; ++r)
                for (uint32_t g = 0; g < declared3DSize; ++g)
                    for (uint32_t b = 0; b < declared3DSize; ++b)
                    {
                        const uint64_t sourceIndex = (uint64_t(r) * declared3DSize + g) * declared3DSize + b;
                        const uint64_t textureIndex = (uint64_t(b) * declared3DSize + g) * declared3DSize + r;
                        (*table)[textureIndex] = float4(source[sourceIndex], 1.0f);
                    }
            cameraLut3D = std::move(table);
            cameraLut3DSize = declared3DSize;
            cameraLutIs3D = true;
        }
        else
        {
            std::array<float3, CameraLutSize> resampled{};
            for (uint32_t i = 0; i < CameraLutSize; ++i)
            {
                const float sourcePosition =
                    (static_cast<float>(i) / static_cast<float>(CameraLutSize - 1)) *
                    static_cast<float>(declared1DSize - 1);
                const uint32_t lower = static_cast<uint32_t>(sourcePosition);
                const uint32_t upper = std::min(lower + 1, declared1DSize - 1);
                resampled[i] = lerp(source[lower], source[upper], sourcePosition - static_cast<float>(lower));
            }
            cameraLut = resampled;
            cameraLut3D.reset();
            cameraLut3DSize = 0;
            cameraLutIs3D = false;
        }
        ++cameraLutRevision;
        return true;
    }
    catch (const std::exception& exception)
    {
        if (error)
            *error = exception.what();
        return false;
    }
}

void ToneMappingParameters::applyCameraLutPreset(CameraLutPreset preset)
{
    clearCameraLut();
    if (preset == CameraLutPreset::None || preset == CameraLutPreset::CustomFile)
        return;

    for (uint32_t i = 0; i < CameraLutSize; ++i)
    {
        const float x = static_cast<float>(i) / static_cast<float>(CameraLutSize - 1);
        const float contrast = x * x * (3.0f - 2.0f * x);
        switch (preset)
        {
        case CameraLutPreset::Neutral:
            cameraLut[i] = float3(x);
            break;
        case CameraLutPreset::SoftContrast:
            cameraLut[i] = float3(contrast);
            break;
        case CameraLutPreset::WarmFilm:
            cameraLut[i] = float3(std::pow(contrast, 0.96f), contrast, std::pow(contrast, 1.06f));
            break;
        case CameraLutPreset::CoolFilm:
            cameraLut[i] = float3(std::pow(contrast, 1.06f), contrast, std::pow(contrast, 0.96f));
            break;
        default:
            break;
        }
    }
    cameraLutPreset = preset;
    cameraLutEnabled = true;
    ++cameraLutRevision;
}

void ToneMappingParameters::clearCameraLut()
{
    for (uint32_t i = 0; i < CameraLutSize; ++i)
    {
        const float x = static_cast<float>(i) / static_cast<float>(CameraLutSize - 1);
        cameraLut[i] = caustica::math::float3(x);
    }
    cameraLutDomainMin = caustica::math::float3(0.0f);
    cameraLutDomainMax = caustica::math::float3(1.0f);
    cameraLutPath.clear();
    cameraLutEnabled = false;
    cameraLutPreset = CameraLutPreset::None;
    cameraLutIs3D = false;
    cameraLut3DSize = 0;
    cameraLut3D.reset();
    ++cameraLutRevision;
}
