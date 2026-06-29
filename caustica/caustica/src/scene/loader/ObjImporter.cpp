#include <scene/loader/ObjImporter.h>
#include <assets/loader/TextureLoader.h>
#include <scene/SceneEcs.h>
#include <scene/SceneEcsLegacyAdapter.h>
#include <scene/SceneGraph.h>
#include <core/log.h>
#include <core/vfs/VFS.h>

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <vector>

using namespace caustica::math;
using namespace caustica;
namespace
{
    std::string TrimString(const std::string& text)
    {
        const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
        const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
        return begin < end ? std::string(begin, end) : std::string();
    }

    std::vector<std::string> SplitWhitespace(const std::string& text)
    {
        std::vector<std::string> result;
        std::istringstream stream(text);
        std::string token;
        while (stream >> token)
            result.push_back(token);
        return result;
    }

    std::string ToLowerString(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    std::string StripMatchingQuotes(const std::string& text)
    {
        if (text.size() >= 2)
        {
            const char first = text.front();
            const char last = text.back();
            if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
                return text.substr(1, text.size() - 2);
        }
        return text;
    }

    bool IsFloatToken(const std::string& text)
    {
        if (text.empty())
            return false;

        char* end = nullptr;
        std::strtof(text.c_str(), &end);
        return end != text.c_str() && *end == '\0';
    }

    std::string JoinTokens(const std::vector<std::string>& tokens, size_t begin)
    {
        std::string result;
        for (size_t i = begin; i < tokens.size(); ++i)
        {
            if (!result.empty())
                result += ' ';
            result += tokens[i];
        }
        return result;
    }

    enum class ObjTextureChannel : uint8_t
    {
        Red,
        Green,
        Blue,
        Alpha,
        Luminance
    };

    struct ObjTextureReference
    {
        std::filesystem::path path;
        ObjTextureChannel channel = ObjTextureChannel::Red;
        float bumpMultiplier = 1.0f;
        bool hasBumpMultiplier = false;

        [[nodiscard]] bool empty() const { return path.empty(); }
    };

    ObjTextureChannel ParseObjTextureChannel(const std::string& text)
    {
        const std::string channel = ToLowerString(text);
        if (channel == "g" || channel == "green")
            return ObjTextureChannel::Green;
        if (channel == "b" || channel == "blue")
            return ObjTextureChannel::Blue;
        if (channel == "a" || channel == "alpha" || channel == "m" || channel == "matte")
            return ObjTextureChannel::Alpha;
        if (channel == "l" || channel == "lum" || channel == "luminance" || channel == "z")
            return ObjTextureChannel::Luminance;
        return ObjTextureChannel::Red;
    }

    ObjTextureReference ExtractObjTextureReference(const std::vector<std::string>& tokens)
    {
        ObjTextureReference result;
        size_t i = 1;
        while (i < tokens.size())
        {
            const std::string option = ToLowerString(tokens[i]);
            if (option.empty() || option[0] != '-' || IsFloatToken(option))
            {
                result.path = StripMatchingQuotes(JoinTokens(tokens, i));
                return result;
            }

            if (option == "-o" || option == "-s" || option == "-t")
            {
                ++i;
                for (int skipped = 0; i < tokens.size() && skipped < 3 && IsFloatToken(tokens[i]); ++skipped)
                    ++i;
            }
            else if (option == "-mm")
            {
                i += 3;
            }
            else if (option == "-imfchan")
            {
                i += 2;
                if (i <= tokens.size())
                    result.channel = ParseObjTextureChannel(tokens[i - 1]);
            }
            else if (option == "-bm")
            {
                i += 2;
                if (i <= tokens.size())
                {
                    result.bumpMultiplier = std::strtof(tokens[i - 1].c_str(), nullptr);
                    result.hasBumpMultiplier = true;
                }
            }
            else if (
                option == "-boost" ||
                option == "-blendu" ||
                option == "-blendv" ||
                option == "-cc" ||
                option == "-clamp" ||
                option == "-texres" ||
                option == "-type")
            {
                i += 2;
            }
            else
            {
                ++i;
                if (i < tokens.size() && IsFloatToken(tokens[i]))
                    ++i;
            }
        }

        return result;
    }

    std::filesystem::path ResolveObjTexturePath(const std::filesystem::path& materialFilePath, const std::string& texturePath)
    {
        std::filesystem::path path(texturePath);
        if (path.is_absolute())
            return path.lexically_normal();
        return (materialFilePath.parent_path() / path).lexically_normal();
    }

    struct ObjImageRgba8
    {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;

        [[nodiscard]] bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
    };

    bool LoadObjImageRgba8(const ObjTextureReference& texture, ObjImageRgba8& output)
    {
        if (texture.empty())
            return false;

        std::ifstream file(texture.path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            caustica::warning("OBJ texture '%s' could not be opened for channel packing.", texture.path.string().c_str());
            return false;
        }

        const std::streamsize fileSize = file.tellg();
        if (fileSize <= 0 || fileSize > std::numeric_limits<int>::max())
        {
            caustica::warning("OBJ texture '%s' has unsupported size for channel packing.", texture.path.string().c_str());
            return false;
        }

        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> encoded(static_cast<size_t>(fileSize));
        if (!file.read(reinterpret_cast<char*>(encoded.data()), fileSize))
        {
            caustica::warning("OBJ texture '%s' could not be read for channel packing.", texture.path.string().c_str());
            return false;
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
        if (!pixels || width <= 0 || height <= 0)
        {
            caustica::warning("OBJ texture '%s' could not be decoded for channel packing.", texture.path.string().c_str());
            if (pixels)
                stbi_image_free(pixels);
            return false;
        }

        output.width = width;
        output.height = height;
        output.rgba.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        stbi_image_free(pixels);
        return true;
    }

    uint8_t SampleObjTextureChannel(
        const ObjImageRgba8& image,
        ObjTextureChannel channel,
        uint32_t x,
        uint32_t y,
        uint32_t targetWidth,
        uint32_t targetHeight)
    {
        if (!image.valid())
            return 255;

        const uint32_t sourceX = targetWidth > 1
            ? x * static_cast<uint32_t>(image.width - 1) / (targetWidth - 1)
            : 0;
        const uint32_t sourceY = targetHeight > 1
            ? y * static_cast<uint32_t>(image.height - 1) / (targetHeight - 1)
            : 0;
        const size_t offset = (static_cast<size_t>(sourceY) * static_cast<size_t>(image.width) + sourceX) * 4;
        const uint8_t r = image.rgba[offset + 0];
        const uint8_t g = image.rgba[offset + 1];
        const uint8_t b = image.rgba[offset + 2];
        const uint8_t a = image.rgba[offset + 3];

        switch (channel)
        {
        case ObjTextureChannel::Green: return g;
        case ObjTextureChannel::Blue: return b;
        case ObjTextureChannel::Alpha: return a;
        case ObjTextureChannel::Luminance:
            return static_cast<uint8_t>(dm::clamp(0.2126f * float(r) + 0.7152f * float(g) + 0.0722f * float(b), 0.0f, 255.0f));
        case ObjTextureChannel::Red:
        default:
            return r;
        }
    }

    std::string SanitizeObjGeneratedTextureComponent(std::string text)
    {
        if (text.empty())
            text = "default";

        for (char& c : text)
        {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (!std::isalnum(uc) && c != '_' && c != '-' && c != '.')
                c = '_';
        }
        return text;
    }

    std::string MakeGeneratedObjTextureName(
        const std::filesystem::path& objFilePath,
        const std::string& materialName,
        const char* suffix)
    {
        const std::string fileName = objFilePath.stem().string()
            + "."
            + SanitizeObjGeneratedTextureComponent(materialName)
            + "."
            + suffix
            + ".png";
        return (objFilePath.parent_path() / fileName).generic_string();
    }

    void AppendObjPngBytes(void* context, void* data, int size)
    {
        if (!context || !data || size <= 0)
            return;

        auto& bytes = *static_cast<std::vector<uint8_t>*>(context);
        const uint8_t* begin = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), begin, begin + size);
    }

    std::shared_ptr<LoadedTexture> LoadGeneratedObjTexture(
        TextureLoader& textureCache,
        const std::string& generatedName,
        const std::vector<uint8_t>& rgba,
        uint32_t width,
        uint32_t height,
        bool sRGB)
    {
        if (rgba.empty() || width == 0 || height == 0)
            return nullptr;

        std::vector<uint8_t> encodedPng;
        const int encoded = stbi_write_png_to_func(
            AppendObjPngBytes,
            &encodedPng,
            static_cast<int>(width),
            static_cast<int>(height),
            4,
            rgba.data(),
            static_cast<int>(width * 4));

        if (!encoded || encodedPng.empty())
        {
            caustica::warning("Failed to encode generated OBJ material texture '%s'.", generatedName.c_str());
            return nullptr;
        }

        void* pngData = malloc(encodedPng.size());
        if (!pngData)
            return nullptr;

        std::memcpy(pngData, encodedPng.data(), encodedPng.size());
        auto blob = std::make_shared<caustica::Blob>(pngData, encodedPng.size());
        return textureCache.LoadTextureFromMemoryDeferred(blob, generatedName, "image/png", sRGB);
    }

    int ResolveObjIndex(int index, size_t count)
    {
        if (index > 0)
            return index - 1;
        if (index < 0)
            return static_cast<int>(count) + index;
        return -1;
    }

    bool ParseObjIndex(const std::string& text, int& out)
    {
        if (text.empty())
        {
            out = 0;
            return true;
        }

        char* end = nullptr;
        long value = std::strtol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end != '\0')
            return false;

        out = static_cast<int>(value);
        return true;
    }

    struct ObjFaceVertex
    {
        int position = -1;
        int texcoord = -1;
        int normal = -1;

        bool operator==(const ObjFaceVertex& other) const = default;
    };

    struct ObjFaceVertexHash
    {
        size_t operator()(const ObjFaceVertex& key) const noexcept
        {
            size_t h = std::hash<int>{}(key.position);
            h ^= std::hash<int>{}(key.texcoord + 0x9e3779b9 + (int)(h << 6) + (int)(h >> 2));
            h ^= std::hash<int>{}(key.normal + 0x7f4a7c15 + (int)(h << 6) + (int)(h >> 2));
            return h;
        }
    };

    bool ParseObjFaceVertex(const std::string& token, size_t positionCount, size_t texcoordCount, size_t normalCount, ObjFaceVertex& out)
    {
        std::array<std::string, 3> parts;
        size_t partIndex = 0;
        size_t partStart = 0;
        for (size_t i = 0; i <= token.size(); ++i)
        {
            if (i == token.size() || token[i] == '/')
            {
                if (partIndex >= parts.size())
                    return false;
                parts[partIndex++] = token.substr(partStart, i - partStart);
                partStart = i + 1;
            }
        }

        int position = 0;
        if (!ParseObjIndex(parts[0], position))
            return false;

        out.position = ResolveObjIndex(position, positionCount);
        if (out.position < 0 || out.position >= static_cast<int>(positionCount))
            return false;

        if (partIndex > 1 && !parts[1].empty())
        {
            int texcoord = 0;
            if (!ParseObjIndex(parts[1], texcoord))
                return false;
            out.texcoord = ResolveObjIndex(texcoord, texcoordCount);
            if (out.texcoord < 0 || out.texcoord >= static_cast<int>(texcoordCount))
                return false;
        }

        if (partIndex > 2 && !parts[2].empty())
        {
            int normal = 0;
            if (!ParseObjIndex(parts[2], normal))
                return false;
            out.normal = ResolveObjIndex(normal, normalCount);
            if (out.normal < 0 || out.normal >= static_cast<int>(normalCount))
                return false;
        }

        return true;
    }

    dm::float3 NormalizeOrFallback(dm::float3 value, dm::float3 fallback)
    {
        const float len = dm::length(value);
        return len > 1e-20f ? value / len : fallback;
    }

    dm::float3 BuildFallbackTangent(dm::float3 normal)
    {
        const dm::float3 axis = std::abs(normal.z) < 0.999f
            ? dm::float3(0.0f, 0.0f, 1.0f)
            : dm::float3(0.0f, 1.0f, 0.0f);
        return NormalizeOrFallback(dm::cross(axis, normal), dm::float3(1.0f, 0.0f, 0.0f));
    }

    struct ObjMaterialInfo
    {
        std::string name = "default";
        dm::float3 baseColor = dm::float3(0.8f);
        dm::float3 specularColor = dm::float3(0.0f);
        dm::float3 emissiveColor = dm::float3(0.0f);
        float roughness = 0.5f;
        float metalness = 0.0f;
        float opacity = 1.0f;
        float transmissionFactor = 0.0f;
        float normalTextureScale = 1.0f;
        float ior = 1.5f;
        bool hasBaseColor = false;
        bool hasSpecularColor = false;
        bool hasEmissiveColor = false;
        bool hasRoughness = false;
        bool hasMetalness = false;
        bool hasTransmission = false;
        bool useSpecularGlossModel = false;
        ObjTextureReference baseTexture;
        ObjTextureReference packedMetalRoughTexture;
        ObjTextureReference roughnessTexture;
        ObjTextureReference metalnessTexture;
        ObjTextureReference specularTexture;
        ObjTextureReference glossinessTexture;
        ObjTextureReference normalTexture;
        ObjTextureReference emissiveTexture;
        ObjTextureReference occlusionTexture;
        ObjTextureReference opacityTexture;
        ObjTextureReference transmissionTexture;
    };

    std::unordered_map<std::string, ObjMaterialInfo> LoadObjMaterialLibrary(const std::filesystem::path& filePath)
    {
        std::unordered_map<std::string, ObjMaterialInfo> materials;
        std::ifstream file(filePath);
        if (!file)
        {
            caustica::warning("OBJ material library '%s' could not be opened.", filePath.string().c_str());
            return materials;
        }

        ObjMaterialInfo* current = nullptr;
        std::string line;
        while (std::getline(file, line))
        {
            line = TrimString(line);
            if (line.empty() || line[0] == '#')
                continue;

            const auto tokens = SplitWhitespace(line);
            if (tokens.empty())
                continue;

            const std::string keyword = ToLowerString(tokens[0]);

            if (keyword == "newmtl" && tokens.size() >= 2)
            {
                ObjMaterialInfo material;
                material.name = StripMatchingQuotes(JoinTokens(tokens, 1));
                auto [it, inserted] = materials.insert_or_assign(material.name, material);
                current = &it->second;
            }
            else if (current && (keyword == "kd" || keyword == "ke" || keyword == "ks") && tokens.size() >= 4)
            {
                dm::float3 color(
                    std::strtof(tokens[1].c_str(), nullptr),
                    std::strtof(tokens[2].c_str(), nullptr),
                    std::strtof(tokens[3].c_str(), nullptr));
                if (keyword == "kd")
                {
                    current->baseColor = color;
                    current->hasBaseColor = true;
                }
                else if (keyword == "ks")
                {
                    current->specularColor = color;
                    current->hasSpecularColor = true;
                }
                else
                {
                    current->emissiveColor = color;
                    current->hasEmissiveColor = true;
                }
            }
            else if (current && keyword == "ns" && tokens.size() >= 2)
            {
                const float ns = std::max(0.0f, std::strtof(tokens[1].c_str(), nullptr));
                current->roughness = dm::clamp(std::sqrt(2.0f / (ns + 2.0f)), 0.02f, 1.0f);
                current->hasRoughness = true;
            }
            else if (current && keyword == "pr" && tokens.size() >= 2)
            {
                current->roughness = dm::clamp(std::strtof(tokens[1].c_str(), nullptr), 0.02f, 1.0f);
                current->hasRoughness = true;
                current->useSpecularGlossModel = false;
            }
            else if (current && keyword == "pm" && tokens.size() >= 2)
            {
                current->metalness = dm::clamp(std::strtof(tokens[1].c_str(), nullptr), 0.0f, 1.0f);
                current->hasMetalness = true;
                current->useSpecularGlossModel = false;
            }
            else if (current && keyword == "ni" && tokens.size() >= 2)
            {
                current->ior = std::max(1.0f, std::strtof(tokens[1].c_str(), nullptr));
            }
            else if (current && (keyword == "d" || keyword == "tr") && tokens.size() >= 2)
            {
                const float value = dm::clamp(std::strtof(tokens[1].c_str(), nullptr), 0.0f, 1.0f);
                current->opacity = keyword == "tr" ? 1.0f - value : value;
            }
            else if (current && keyword == "tf" && tokens.size() >= 4)
            {
                const dm::float3 color(
                    std::strtof(tokens[1].c_str(), nullptr),
                    std::strtof(tokens[2].c_str(), nullptr),
                    std::strtof(tokens[3].c_str(), nullptr));
                current->transmissionFactor = dm::clamp(dm::maxComponent(color), 0.0f, 1.0f);
                current->hasTransmission = true;
            }
            else if (current && (
                keyword == "map_kd" ||
                keyword == "map_basecolor" ||
                keyword == "map_ka" ||
                keyword == "map_ao" ||
                keyword == "map_occlusion" ||
                keyword == "map_ks" ||
                keyword == "map_ns" ||
                keyword == "map_pr" ||
                keyword == "map_roughness" ||
                keyword == "map_pm" ||
                keyword == "map_metallic" ||
                keyword == "map_metalness" ||
                keyword == "map_orm" ||
                keyword == "map_mr" ||
                keyword == "map_metallicroughness" ||
                keyword == "map_occlusionroughnessmetallic" ||
                keyword == "map_bump" ||
                keyword == "bump" ||
                keyword == "norm" ||
                keyword == "map_normal" ||
                keyword == "map_ke" ||
                keyword == "map_emissive" ||
                keyword == "map_d" ||
                keyword == "map_opacity" ||
                keyword == "map_tf"))
            {
                ObjTextureReference texture = ExtractObjTextureReference(tokens);
                if (texture.path.empty())
                    continue;

                texture.path = ResolveObjTexturePath(filePath, texture.path.string());

                if (keyword == "map_kd" || keyword == "map_basecolor")
                    current->baseTexture = texture;
                else if (keyword == "map_ka" || keyword == "map_ao" || keyword == "map_occlusion")
                    current->occlusionTexture = texture;
                else if (keyword == "map_ke" || keyword == "map_emissive")
                {
                    current->emissiveTexture = texture;
                    if (!current->hasEmissiveColor)
                    {
                        current->emissiveColor = dm::float3(1.0f);
                        current->hasEmissiveColor = true;
                    }
                }
                else if (keyword == "map_d" || keyword == "map_opacity")
                    current->opacityTexture = texture;
                else if (keyword == "map_tf")
                    current->transmissionTexture = texture;
                else if (keyword == "map_bump" || keyword == "bump" || keyword == "norm" || keyword == "map_normal")
                {
                    current->normalTexture = texture;
                    if (texture.hasBumpMultiplier)
                        current->normalTextureScale = texture.bumpMultiplier;
                }
                else if (keyword == "map_pm" || keyword == "map_metallic" || keyword == "map_metalness")
                {
                    current->metalnessTexture = texture;
                    current->useSpecularGlossModel = false;
                }
                else if (keyword == "map_pr" || keyword == "map_roughness")
                {
                    current->roughnessTexture = texture;
                    current->useSpecularGlossModel = false;
                }
                else if (
                    keyword == "map_orm" ||
                    keyword == "map_mr" ||
                    keyword == "map_metallicroughness" ||
                    keyword == "map_occlusionroughnessmetallic")
                {
                    current->packedMetalRoughTexture = texture;
                    current->useSpecularGlossModel = false;
                }
                else if (keyword == "map_ns")
                {
                    current->glossinessTexture = texture;
                    if (current->roughnessTexture.empty() && current->metalnessTexture.empty() && current->packedMetalRoughTexture.empty())
                        current->useSpecularGlossModel = true;
                }
                else if (keyword == "map_ks")
                {
                    current->specularTexture = texture;
                    if (current->roughnessTexture.empty() && current->metalnessTexture.empty() && current->packedMetalRoughTexture.empty())
                        current->useSpecularGlossModel = true;
                }
            }
        }

        return materials;
    }

    bool SelectObjPackedTextureSize(
        const ObjImageRgba8& a,
        const ObjImageRgba8& b,
        const ObjImageRgba8& c,
        uint32_t& width,
        uint32_t& height)
    {
        const ObjImageRgba8* images[] = { &a, &b, &c };
        for (const ObjImageRgba8* image : images)
        {
            if (image->valid())
            {
                width = static_cast<uint32_t>(image->width);
                height = static_cast<uint32_t>(image->height);
                return true;
            }
        }

        return false;
    }

    std::shared_ptr<LoadedTexture> BuildObjMetalRoughTexture(
        const ObjMaterialInfo& material,
        const std::filesystem::path& objFilePath,
        TextureLoader& textureCache)
    {
        if (material.occlusionTexture.empty() && material.roughnessTexture.empty() && material.metalnessTexture.empty())
            return nullptr;

        ObjImageRgba8 occlusionImage;
        ObjImageRgba8 roughnessImage;
        ObjImageRgba8 metalnessImage;

        LoadObjImageRgba8(material.occlusionTexture, occlusionImage);
        LoadObjImageRgba8(material.roughnessTexture, roughnessImage);
        LoadObjImageRgba8(material.metalnessTexture, metalnessImage);

        uint32_t width = 0;
        uint32_t height = 0;
        if (!SelectObjPackedTextureSize(occlusionImage, roughnessImage, metalnessImage, width, height))
            return nullptr;

        std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                rgba[offset + 0] = occlusionImage.valid()
                    ? SampleObjTextureChannel(occlusionImage, material.occlusionTexture.channel, x, y, width, height)
                    : 255;
                rgba[offset + 1] = roughnessImage.valid()
                    ? SampleObjTextureChannel(roughnessImage, material.roughnessTexture.channel, x, y, width, height)
                    : 255;
                rgba[offset + 2] = metalnessImage.valid()
                    ? SampleObjTextureChannel(metalnessImage, material.metalnessTexture.channel, x, y, width, height)
                    : 255;
                rgba[offset + 3] = 255;
            }
        }

        return LoadGeneratedObjTexture(
            textureCache,
            MakeGeneratedObjTextureName(objFilePath, material.name, "orm"),
            rgba,
            width,
            height,
            false);
    }

    std::shared_ptr<LoadedTexture> BuildObjSpecGlossTexture(
        const ObjMaterialInfo& material,
        const std::filesystem::path& objFilePath,
        TextureLoader& textureCache)
    {
        if (material.specularTexture.empty() && material.glossinessTexture.empty())
            return nullptr;

        ObjImageRgba8 specularImage;
        ObjImageRgba8 glossinessImage;

        LoadObjImageRgba8(material.specularTexture, specularImage);
        LoadObjImageRgba8(material.glossinessTexture, glossinessImage);

        uint32_t width = 0;
        uint32_t height = 0;
        if (!SelectObjPackedTextureSize(specularImage, glossinessImage, ObjImageRgba8{}, width, height))
            return nullptr;

        std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                if (specularImage.valid())
                {
                    const uint32_t sourceX = width > 1
                        ? x * static_cast<uint32_t>(specularImage.width - 1) / (width - 1)
                        : 0;
                    const uint32_t sourceY = height > 1
                        ? y * static_cast<uint32_t>(specularImage.height - 1) / (height - 1)
                        : 0;
                    const size_t sourceOffset = (static_cast<size_t>(sourceY) * static_cast<size_t>(specularImage.width) + sourceX) * 4;
                    rgba[offset + 0] = specularImage.rgba[sourceOffset + 0];
                    rgba[offset + 1] = specularImage.rgba[sourceOffset + 1];
                    rgba[offset + 2] = specularImage.rgba[sourceOffset + 2];
                }
                else
                {
                    rgba[offset + 0] = 255;
                    rgba[offset + 1] = 255;
                    rgba[offset + 2] = 255;
                }

                rgba[offset + 3] = glossinessImage.valid()
                    ? SampleObjTextureChannel(glossinessImage, material.glossinessTexture.channel, x, y, width, height)
                    : 255;
            }
        }

        return LoadGeneratedObjTexture(
            textureCache,
            MakeGeneratedObjTextureName(objFilePath, material.name, "specgloss"),
            rgba,
            width,
            height,
            true);
    }
}

bool ObjImporter::Load(const std::filesystem::path& filePath, TextureLoader& textureCache, SceneLoadingStats&, ThreadPool*, SceneImportResult& result, const std::filesystem::path&) const
{
    std::ifstream file(filePath);
    if (!file)
    {
        caustica::error("OBJ file could not be opened: '%s'", filePath.string().c_str());
        return false;
    }

    std::vector<dm::float3> positions;
    std::vector<dm::float2> texcoords;
    std::vector<dm::float3> normals;
    std::unordered_map<std::string, ObjMaterialInfo> materials;
    materials["default"] = ObjMaterialInfo{};

    struct ObjGroup
    {
        std::string materialName = "default";
        std::vector<uint32_t> indices;
        dm::box3 bounds = dm::box3::empty();
    };

    std::vector<ObjGroup> groups;
    std::unordered_map<std::string, size_t> groupByMaterial;
    auto getGroup = [&](const std::string& materialName) -> ObjGroup&
    {
        auto found = groupByMaterial.find(materialName);
        if (found != groupByMaterial.end())
            return groups[found->second];

        ObjGroup group;
        group.materialName = materialName;
        groupByMaterial[materialName] = groups.size();
        groups.push_back(group);
        return groups.back();
    };

    ObjGroup* currentGroup = &getGroup("default");

    auto mesh = std::static_pointer_cast<MeshInfo>(m_SceneTypeFactory->CreateMesh());
    mesh->name = filePath.stem().string();
    mesh->type = MeshType::Triangles;
    mesh->buffers = std::make_shared<BufferGroup>();
    mesh->objectSpaceBounds = dm::box3::empty();

    std::unordered_map<ObjFaceVertex, uint32_t, ObjFaceVertexHash> vertexMap;
    std::vector<int> vertexNormalRefs;

    auto addVertex = [&](const ObjFaceVertex& key) -> uint32_t
    {
        auto found = vertexMap.find(key);
        if (found != vertexMap.end())
            return found->second;

        const uint32_t index = static_cast<uint32_t>(mesh->buffers->positionData.size());
        vertexMap[key] = index;

        const dm::float3 position = positions[key.position];
        mesh->buffers->positionData.push_back(position);
        mesh->buffers->texcoord1Data.push_back(key.texcoord >= 0 ? texcoords[key.texcoord] : dm::float2(0.0f));
        mesh->DeformationSourcePositionIndices.push_back(static_cast<uint32_t>(key.position));
        vertexNormalRefs.push_back(key.normal);
        mesh->objectSpaceBounds |= position;

        return index;
    };

    std::string currentMaterial = "default";
    std::string line;
    uint32_t lineNumber = 0;
    while (std::getline(file, line))
    {
        ++lineNumber;
        line = TrimString(line);
        if (line.empty() || line[0] == '#')
            continue;

        const auto tokens = SplitWhitespace(line);
        if (tokens.empty())
            continue;

        const std::string keyword = ToLowerString(tokens[0]);

        if (keyword == "v" && tokens.size() >= 4)
        {
            positions.push_back(dm::float3(
                std::strtof(tokens[1].c_str(), nullptr),
                std::strtof(tokens[2].c_str(), nullptr),
                std::strtof(tokens[3].c_str(), nullptr)));
        }
        else if (keyword == "vt" && tokens.size() >= 3)
        {
            texcoords.push_back(dm::float2(
                std::strtof(tokens[1].c_str(), nullptr),
                1.0f - std::strtof(tokens[2].c_str(), nullptr)));
        }
        else if (keyword == "vn" && tokens.size() >= 4)
        {
            normals.push_back(NormalizeOrFallback(dm::float3(
                std::strtof(tokens[1].c_str(), nullptr),
                std::strtof(tokens[2].c_str(), nullptr),
                std::strtof(tokens[3].c_str(), nullptr)), dm::float3(0.0f, 1.0f, 0.0f)));
        }
        else if (keyword == "mtllib" && tokens.size() >= 2)
        {
            const std::filesystem::path materialLibrary = StripMatchingQuotes(JoinTokens(tokens, 1));
            auto loaded = LoadObjMaterialLibrary(filePath.parent_path() / materialLibrary);
            for (auto& [name, material] : loaded)
                materials.insert_or_assign(name, material);
        }
        else if (keyword == "usemtl" && tokens.size() >= 2)
        {
            currentMaterial = StripMatchingQuotes(JoinTokens(tokens, 1));
            if (!materials.contains(currentMaterial))
            {
                ObjMaterialInfo material;
                material.name = currentMaterial;
                materials[currentMaterial] = material;
            }
            currentGroup = &getGroup(currentMaterial);
        }
        else if (keyword == "f" && tokens.size() >= 4)
        {
            std::vector<uint32_t> polygon;
            polygon.reserve(tokens.size() - 1);

            bool validFace = true;
            for (size_t i = 1; i < tokens.size(); ++i)
            {
                ObjFaceVertex key;
                if (!ParseObjFaceVertex(tokens[i], positions.size(), texcoords.size(), normals.size(), key))
                {
                    caustica::warning("Skipping malformed OBJ face at '%s':%u", filePath.string().c_str(), lineNumber);
                    validFace = false;
                    break;
                }
                polygon.push_back(addVertex(key));
            }

            if (!validFace || polygon.size() < 3)
                continue;

            for (size_t i = 1; i + 1 < polygon.size(); ++i)
            {
                const uint32_t tri[3] = { polygon[0], polygon[i], polygon[i + 1] };
                currentGroup->indices.insert(currentGroup->indices.end(), std::begin(tri), std::end(tri));
                currentGroup->bounds |= mesh->buffers->positionData[tri[0]];
                currentGroup->bounds |= mesh->buffers->positionData[tri[1]];
                currentGroup->bounds |= mesh->buffers->positionData[tri[2]];
            }
        }
    }

    if (mesh->buffers->positionData.empty())
    {
        caustica::error("OBJ file '%s' has no usable vertices.", filePath.string().c_str());
        return false;
    }

    std::vector<dm::float3> accumulatedNormals(mesh->buffers->positionData.size(), dm::float3(0.0f));
    for (const ObjGroup& group : groups)
    {
        for (size_t i = 0; i + 2 < group.indices.size(); i += 3)
        {
            const uint32_t i0 = group.indices[i + 0];
            const uint32_t i1 = group.indices[i + 1];
            const uint32_t i2 = group.indices[i + 2];
            const dm::float3& p0 = mesh->buffers->positionData[i0];
            const dm::float3& p1 = mesh->buffers->positionData[i1];
            const dm::float3& p2 = mesh->buffers->positionData[i2];
            const dm::float3 faceNormal = NormalizeOrFallback(dm::cross(p1 - p0, p2 - p0), dm::float3(0.0f, 1.0f, 0.0f));
            accumulatedNormals[i0] += faceNormal;
            accumulatedNormals[i1] += faceNormal;
            accumulatedNormals[i2] += faceNormal;
        }
    }

    std::vector<dm::float3> resolvedNormals;
    resolvedNormals.reserve(mesh->buffers->positionData.size());
    mesh->buffers->normalData.reserve(mesh->buffers->positionData.size());
    for (size_t i = 0; i < mesh->buffers->positionData.size(); ++i)
    {
        const int normalIndex = vertexNormalRefs[i];
        const dm::float3 normal = normalIndex >= 0
            ? normals[normalIndex]
            : NormalizeOrFallback(accumulatedNormals[i], dm::float3(0.0f, 1.0f, 0.0f));
        resolvedNormals.push_back(normal);
        mesh->buffers->normalData.push_back(dm::vectorToSnorm8(normal));
    }

    std::vector<dm::float3> accumulatedTangents(mesh->buffers->positionData.size(), dm::float3(0.0f));
    std::vector<dm::float3> accumulatedBitangents(mesh->buffers->positionData.size(), dm::float3(0.0f));
    for (const ObjGroup& group : groups)
    {
        for (size_t i = 0; i + 2 < group.indices.size(); i += 3)
        {
            const uint32_t i0 = group.indices[i + 0];
            const uint32_t i1 = group.indices[i + 1];
            const uint32_t i2 = group.indices[i + 2];
            const dm::float3& p0 = mesh->buffers->positionData[i0];
            const dm::float3& p1 = mesh->buffers->positionData[i1];
            const dm::float3& p2 = mesh->buffers->positionData[i2];
            const dm::float2& uv0 = mesh->buffers->texcoord1Data[i0];
            const dm::float2& uv1 = mesh->buffers->texcoord1Data[i1];
            const dm::float2& uv2 = mesh->buffers->texcoord1Data[i2];

            const dm::float3 edge1 = p1 - p0;
            const dm::float3 edge2 = p2 - p0;
            const dm::float2 deltaUv1 = uv1 - uv0;
            const dm::float2 deltaUv2 = uv2 - uv0;
            const float determinant = deltaUv1.x * deltaUv2.y - deltaUv2.x * deltaUv1.y;
            if (std::abs(determinant) <= 1e-20f)
                continue;

            const float invDeterminant = 1.0f / determinant;
            const dm::float3 tangent = (edge1 * deltaUv2.y - edge2 * deltaUv1.y) * invDeterminant;
            const dm::float3 bitangent = (edge2 * deltaUv1.x - edge1 * deltaUv2.x) * invDeterminant;

            accumulatedTangents[i0] += tangent;
            accumulatedTangents[i1] += tangent;
            accumulatedTangents[i2] += tangent;
            accumulatedBitangents[i0] += bitangent;
            accumulatedBitangents[i1] += bitangent;
            accumulatedBitangents[i2] += bitangent;
        }
    }

    mesh->buffers->tangentData.reserve(mesh->buffers->positionData.size());
    for (size_t i = 0; i < mesh->buffers->positionData.size(); ++i)
    {
        const dm::float3 normal = resolvedNormals[i];
        dm::float3 tangent = accumulatedTangents[i] - normal * dm::dot(normal, accumulatedTangents[i]);
        tangent = NormalizeOrFallback(tangent, BuildFallbackTangent(normal));
        const float handedness = dm::dot(dm::cross(normal, tangent), accumulatedBitangents[i]) < 0.0f ? -1.0f : 1.0f;
        mesh->buffers->tangentData.push_back(dm::vectorToSnorm8(dm::float4(tangent, handedness)));
    }

    auto loadObjTexture = [&](const std::filesystem::path& texturePath, bool sRGB) -> std::shared_ptr<LoadedTexture>
    {
        if (texturePath.empty())
            return nullptr;

        if (!std::filesystem::exists(texturePath))
        {
            caustica::warning("OBJ texture '%s' referenced by '%s' was not found.",
                texturePath.string().c_str(), filePath.string().c_str());
            return nullptr;
        }

        return textureCache.LoadTextureFromFileDeferred(texturePath, sRGB);
    };

    auto loadObjTextureReference = [&](const ObjTextureReference& texture, bool sRGB) -> std::shared_ptr<LoadedTexture>
    {
        return loadObjTexture(texture.path, sRGB);
    };

    for (const ObjGroup& group : groups)
    {
        if (group.indices.empty())
            continue;

        const ObjMaterialInfo& objMaterial = materials.contains(group.materialName)
            ? materials[group.materialName]
            : materials["default"];

        auto material = std::dynamic_pointer_cast<Material>(m_SceneTypeFactory->CreateMaterial());
        material->name = objMaterial.name;
        material->modelFileName = filePath.string();
        const bool hasPbrRoughnessTexture = !objMaterial.roughnessTexture.empty() || !objMaterial.packedMetalRoughTexture.empty();
        const bool hasPbrMetalnessTexture = !objMaterial.metalnessTexture.empty() || !objMaterial.packedMetalRoughTexture.empty();
        const bool hasSpecGlossTexture = !objMaterial.specularTexture.empty() || !objMaterial.glossinessTexture.empty();

        material->baseOrDiffuseColor = !objMaterial.baseTexture.empty() && !objMaterial.hasBaseColor
            ? dm::float3(1.0f)
            : objMaterial.baseColor;
        material->specularColor = hasSpecGlossTexture && objMaterial.useSpecularGlossModel && !objMaterial.hasSpecularColor
            ? dm::float3(1.0f)
            : objMaterial.specularColor;
        material->emissiveColor = objMaterial.emissiveColor;
        if (objMaterial.useSpecularGlossModel)
        {
            material->roughness = !objMaterial.glossinessTexture.empty() && !objMaterial.hasRoughness
                ? 0.0f
                : objMaterial.roughness;
            material->metalness = objMaterial.metalness;
        }
        else
        {
            material->roughness = hasPbrRoughnessTexture && !objMaterial.hasRoughness
                ? 1.0f
                : objMaterial.roughness;
            material->metalness = hasPbrMetalnessTexture && !objMaterial.hasMetalness
                ? 1.0f
                : objMaterial.metalness;
        }
        material->opacity = objMaterial.opacity;
        material->normalTextureScale = objMaterial.normalTextureScale;
        material->useSpecularGlossModel = objMaterial.useSpecularGlossModel;
        material->metalnessInRedChannel = false;
        material->transmissionFactor = !objMaterial.transmissionTexture.empty() && !objMaterial.hasTransmission
            ? 1.0f
            : objMaterial.transmissionFactor;
        material->baseOrDiffuseTexture = loadObjTextureReference(objMaterial.baseTexture, true);
        if (objMaterial.useSpecularGlossModel)
        {
            material->metalRoughOrSpecularTexture = BuildObjSpecGlossTexture(objMaterial, filePath, textureCache);
            if (!material->metalRoughOrSpecularTexture)
                material->metalRoughOrSpecularTexture = loadObjTextureReference(objMaterial.specularTexture, true);
        }
        else
        {
            material->metalRoughOrSpecularTexture = BuildObjMetalRoughTexture(objMaterial, filePath, textureCache);
            if (!material->metalRoughOrSpecularTexture)
                material->metalRoughOrSpecularTexture = loadObjTextureReference(objMaterial.packedMetalRoughTexture, false);
        }
        material->normalTexture = loadObjTextureReference(objMaterial.normalTexture, false);
        material->emissiveTexture = loadObjTextureReference(objMaterial.emissiveTexture, true);
        material->occlusionTexture = loadObjTextureReference(objMaterial.occlusionTexture, false);
        material->opacityTexture = loadObjTextureReference(objMaterial.opacityTexture, false);
        material->transmissionTexture = loadObjTextureReference(objMaterial.transmissionTexture, false);
        if (material->transmissionFactor > 0.0f || material->transmissionTexture)
            material->domain = objMaterial.opacity < 0.999f ? MaterialDomain::TransmissiveAlphaBlended : MaterialDomain::Transmissive;
        else
            material->domain = objMaterial.opacity < 0.999f ? MaterialDomain::AlphaBlended : MaterialDomain::Opaque;

        auto geometry = std::static_pointer_cast<MeshGeometry>(m_SceneTypeFactory->CreateMeshGeometry());
        geometry->material = material;
        geometry->indexOffsetInMesh = static_cast<uint32_t>(mesh->buffers->indexData.size());
        geometry->vertexOffsetInMesh = 0;
        geometry->numIndices = static_cast<uint32_t>(group.indices.size());
        geometry->numVertices = static_cast<uint32_t>(mesh->buffers->positionData.size());
        geometry->objectSpaceBounds = group.bounds;

        mesh->buffers->indexData.insert(mesh->buffers->indexData.end(), group.indices.begin(), group.indices.end());
        mesh->geometries.push_back(geometry);
    }

    if (mesh->geometries.empty())
    {
        caustica::error("OBJ file '%s' has no usable triangle faces.", filePath.string().c_str());
        return false;
    }

    mesh->totalVertices = static_cast<uint32_t>(mesh->buffers->positionData.size());
    mesh->totalIndices = static_cast<uint32_t>(mesh->buffers->indexData.size());

    auto importedRoot = std::make_shared<SceneGraphNode>();
    importedRoot->SetName(filePath.stem().string());
    importedRoot->SetLeaf(m_SceneTypeFactory->CreateMeshInstance(mesh));

    result.rootNode = importedRoot;
    auto importGraph = std::make_shared<SceneGraph>();
    importGraph->SetRootNode(importedRoot);
    result.entityWorld = std::make_shared<scene::SceneEntityWorld>();
    scene::RebuildWorldFromLegacyScene(*result.entityWorld, importGraph);
    result.rootEntity = result.entityWorld->root();
    return true;
}

ObjImporter::ObjImporter(std::shared_ptr<SceneTypeFactory> sceneTypeFactory)
    : m_SceneTypeFactory(std::move(sceneTypeFactory))
{
}

