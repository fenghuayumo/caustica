#include <scene/loader/ColladaLoader.h>

#include <core/log.h>
#include <math/math.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace caustica::math;

namespace caustica
{
namespace
{
    std::string ToLower(std::string text)
    {
        for (char& c : text)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    bool IEquals(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    }

    std::string Trim(std::string text)
    {
        const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
        const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
        return begin < end ? std::string(begin, end) : std::string();
    }

    std::string_view TrimView(std::string_view text)
    {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.remove_suffix(1);
        return text;
    }

    std::string LocalName(std::string_view name)
    {
        const size_t colon = name.rfind(':');
        if (colon != std::string_view::npos)
            name.remove_prefix(colon + 1);
        return ToLower(std::string(name));
    }

    int HexValue(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return 0;
    }

    std::string DecodeXml(std::string_view text)
    {
        if (text.find('&') == std::string_view::npos)
            return std::string(text);

        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != '&')
            {
                out.push_back(text[i]);
                continue;
            }
            const size_t semi = text.find(';', i + 1);
            if (semi == std::string_view::npos || semi - i > 8)
            {
                out.push_back(text[i]);
                continue;
            }
            const std::string_view entity = text.substr(i + 1, semi - i - 1);
            if (entity == "amp")
                out.push_back('&');
            else if (entity == "lt")
                out.push_back('<');
            else if (entity == "gt")
                out.push_back('>');
            else if (entity == "quot")
                out.push_back('"');
            else if (entity == "apos")
                out.push_back('\'');
            else if (!entity.empty() && entity[0] == '#' && entity.size() > 1)
            {
                int code = 0;
                if (entity[1] == 'x' || entity[1] == 'X')
                {
                    for (size_t h = 2; h < entity.size(); ++h)
                        code = code * 16 + HexValue(entity[h]);
                }
                else
                {
                    for (size_t h = 1; h < entity.size(); ++h)
                    {
                        if (entity[h] < '0' || entity[h] > '9')
                            break;
                        code = code * 10 + (entity[h] - '0');
                    }
                }
                if (code > 0 && code < 128)
                    out.push_back(char(code));
            }
            else
            {
                out.append(text.substr(i, semi - i + 1));
            }
            i = semi;
        }
        return out;
    }

    std::string PercentDecode(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '%' && i + 2 < text.size()
                && std::isxdigit(static_cast<unsigned char>(text[i + 1]))
                && std::isxdigit(static_cast<unsigned char>(text[i + 2])))
            {
                out.push_back(char(HexValue(text[i + 1]) * 16 + HexValue(text[i + 2])));
                i += 2;
            }
            else
            {
                out.push_back(text[i]);
            }
        }
        return out;
    }

    struct XmlNode
    {
        std::string name;
        std::vector<std::pair<std::string, std::string>> attrs;
        std::string text;
        std::vector<XmlNode> children;
    };

    const std::string* FindAttr(const XmlNode& node, const char* key)
    {
        for (const auto& [name, value] : node.attrs)
        {
            if (name == key)
                return &value;
        }
        return nullptr;
    }

    std::string Attr(const XmlNode& node, const char* key)
    {
        if (const std::string* value = FindAttr(node, key))
            return *value;
        return {};
    }

    const XmlNode* FindChild(const XmlNode& node, const char* name)
    {
        for (const XmlNode& child : node.children)
        {
            if (child.name == name)
                return &child;
        }
        return nullptr;
    }

    template <typename Fn>
    void ForEachChild(const XmlNode& node, const char* name, Fn&& fn)
    {
        for (const XmlNode& child : node.children)
        {
            if (child.name == name)
                fn(child);
        }
    }

    void AdvanceToTag(const std::string& xml, size_t& i)
    {
        for (;;)
        {
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i])))
                ++i;
            if (i + 4 <= xml.size() && xml.compare(i, 4, "<!--") == 0)
            {
                const size_t end = xml.find("-->", i + 4);
                i = end == std::string::npos ? xml.size() : end + 3;
                continue;
            }
            if (i + 2 <= xml.size() && xml.compare(i, 2, "<?") == 0)
            {
                const size_t end = xml.find("?>", i + 2);
                i = end == std::string::npos ? xml.size() : end + 2;
                continue;
            }
            if (i + 2 <= xml.size() && xml.compare(i, 2, "<!") == 0 && xml.compare(i, 9, "<![CDATA[") != 0)
            {
                const size_t end = xml.find('>', i + 2);
                i = end == std::string::npos ? xml.size() : end + 1;
                continue;
            }
            break;
        }
    }

    bool ParseElement(const std::string& xml, size_t& i, XmlNode& out)
    {
        AdvanceToTag(xml, i);
        if (i >= xml.size() || xml[i] != '<')
            return false;
        if (i + 1 < xml.size() && (xml[i + 1] == '/' || xml[i + 1] == '!' || xml[i + 1] == '?'))
            return false;

        ++i;
        const size_t nameStart = i;
        while (i < xml.size() && !std::isspace(static_cast<unsigned char>(xml[i])) && xml[i] != '>' && xml[i] != '/')
            ++i;
        out.name = LocalName(std::string_view(xml.data() + nameStart, i - nameStart));

        bool selfClosing = false;
        while (i < xml.size() && xml[i] != '>')
        {
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i])))
                ++i;
            if (i >= xml.size() || xml[i] == '>')
                break;
            if (xml[i] == '/')
            {
                selfClosing = true;
                ++i;
                continue;
            }

            const size_t keyStart = i;
            while (i < xml.size() && !std::isspace(static_cast<unsigned char>(xml[i])) && xml[i] != '=' && xml[i] != '>' && xml[i] != '/')
                ++i;
            std::string key = LocalName(std::string_view(xml.data() + keyStart, i - keyStart));
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i])))
                ++i;
            if (i >= xml.size() || xml[i] != '=')
                continue;
            ++i;
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i])))
                ++i;
            if (i >= xml.size())
                break;
            const char quote = xml[i];
            if (quote != '"' && quote != '\'')
                continue;
            ++i;
            const size_t valueStart = i;
            while (i < xml.size() && xml[i] != quote)
                ++i;
            std::string value = DecodeXml(std::string_view(xml.data() + valueStart, i - valueStart));
            if (i < xml.size() && xml[i] == quote)
                ++i;
            if (!key.empty())
                out.attrs.emplace_back(std::move(key), std::move(value));
        }
        if (i < xml.size() && xml[i] == '>')
            ++i;
        if (selfClosing)
            return true;

        std::string text;
        while (i < xml.size())
        {
            if (i + 9 <= xml.size() && xml.compare(i, 9, "<![CDATA[") == 0)
            {
                const size_t end = xml.find("]]>", i + 9);
                if (end == std::string::npos)
                {
                    text.append(xml, i + 9, std::string::npos);
                    i = xml.size();
                    break;
                }
                text.append(xml, i + 9, end - (i + 9));
                i = end + 3;
                continue;
            }
            if (i + 4 <= xml.size() && xml.compare(i, 4, "<!--") == 0)
            {
                const size_t end = xml.find("-->", i + 4);
                i = end == std::string::npos ? xml.size() : end + 3;
                continue;
            }
            if (i + 2 <= xml.size() && xml.compare(i, 2, "</") == 0)
            {
                const size_t end = xml.find('>', i + 2);
                i = end == std::string::npos ? xml.size() : end + 1;
                break;
            }
            if (xml[i] == '<')
            {
                XmlNode child;
                if (!ParseElement(xml, i, child))
                {
                    ++i;
                    continue;
                }
                out.children.push_back(std::move(child));
                continue;
            }

            const size_t start = i;
            while (i < xml.size() && xml[i] != '<')
                ++i;
            text.append(xml, start, i - start);
        }
        out.text = text.find('&') == std::string::npos ? std::move(text) : DecodeXml(text);
        return !out.name.empty();
    }

    bool ParseFloatToken(const char*& p, const char* end, float& out)
    {
        while (p < end && std::isspace(static_cast<unsigned char>(*p)))
            ++p;
        if (p >= end)
            return false;

        const char* start = p;
        int sign = 1;
        if (*p == '+')
            ++p;
        else if (*p == '-')
        {
            sign = -1;
            ++p;
        }

        bool anyDigit = false;
        double value = 0.0;
        while (p < end && std::isdigit(static_cast<unsigned char>(*p)))
        {
            anyDigit = true;
            value = value * 10.0 + double(*p - '0');
            ++p;
        }
        if (p < end && *p == '.')
        {
            ++p;
            double place = 0.1;
            while (p < end && std::isdigit(static_cast<unsigned char>(*p)))
            {
                anyDigit = true;
                value += double(*p - '0') * place;
                place *= 0.1;
                ++p;
            }
        }
        if (!anyDigit)
        {
            p = start;
            return false;
        }
        if (p < end && (*p == 'e' || *p == 'E'))
        {
            const char* exponentMark = p;
            ++p;
            int exponentSign = 1;
            if (p < end && (*p == '+' || *p == '-'))
            {
                if (*p == '-')
                    exponentSign = -1;
                ++p;
            }
            if (p >= end || !std::isdigit(static_cast<unsigned char>(*p)))
            {
                p = exponentMark;
            }
            else
            {
                int exponent = 0;
                while (p < end && std::isdigit(static_cast<unsigned char>(*p)))
                {
                    exponent = exponent * 10 + (*p - '0');
                    ++p;
                }
                value *= std::pow(10.0, double(exponentSign * exponent));
            }
        }
        out = float(sign < 0 ? -value : value);
        return true;
    }

    bool ParseIntToken(const char*& p, const char* end, int& out)
    {
        while (p < end && std::isspace(static_cast<unsigned char>(*p)))
            ++p;
        if (p >= end)
            return false;
        int sign = 1;
        if (*p == '+')
            ++p;
        else if (*p == '-')
        {
            sign = -1;
            ++p;
        }
        if (p >= end || !std::isdigit(static_cast<unsigned char>(*p)))
            return false;
        long long value = 0;
        while (p < end && std::isdigit(static_cast<unsigned char>(*p)))
        {
            value = value * 10 + (*p - '0');
            ++p;
        }
        out = int(value * sign);
        return true;
    }

    void ParseFloats(std::string_view text, std::vector<float>& out)
    {
        const char* p = text.data();
        const char* end = p + text.size();
        float value = 0.f;
        while (ParseFloatToken(p, end, value))
            out.push_back(value);
    }

    void ParseInts(std::string_view text, std::vector<int>& out)
    {
        const char* p = text.data();
        const char* end = p + text.size();
        int value = 0;
        while (ParseIntToken(p, end, value))
            out.push_back(value);
    }

    int ParseInt(const std::string& text, int fallback)
    {
        const char* p = text.data();
        const char* end = p + text.size();
        int value = 0;
        return ParseIntToken(p, end, value) ? value : fallback;
    }

    size_t ParseSize(const std::string& text)
    {
        const int value = ParseInt(text, 0);
        return value > 0 ? size_t(value) : 0;
    }

    float ParseFloat(const std::string& text, float fallback)
    {
        const char* p = text.data();
        const char* end = p + text.size();
        float value = 0.f;
        return ParseFloatToken(p, end, value) ? value : fallback;
    }

    std::string StripRef(std::string value)
    {
        value = Trim(std::move(value));
        const size_t hash = value.rfind('#');
        if (hash != std::string::npos)
            value = value.substr(hash + 1);
        return value;
    }

    std::string ReadFileText(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return {};
        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    struct Source
    {
        std::vector<float> values;
        size_t count = 0;
        size_t offset = 0;
        int stride = 1;

        [[nodiscard]] bool contains(int index) const
        {
            return index >= 0 && size_t(index) < count;
        }

        [[nodiscard]] float at(int index, int component) const
        {
            if (!contains(index) || component < 0)
                return 0.f;
            const size_t i = offset + size_t(index) * size_t(stride) + size_t(component);
            return i < values.size() ? values[i] : 0.f;
        }
    };

    struct RawInput
    {
        std::string semantic;
        std::string source;
        int offset = 0;
        int set = 0;
    };

    enum class RawPrimType
    {
        Triangles,
        Polylist,
        Polygons,
        Tristrips,
        Trifans
    };

    struct RawPrim
    {
        RawPrimType type = RawPrimType::Triangles;
        std::string material;
        std::vector<RawInput> inputs;
        std::vector<int> indices;
        std::vector<int> vcount;
        std::vector<std::vector<int>> groups;
    };

    struct VertexStream
    {
        std::string semantic;
        std::string source;
    };

    struct Geometry
    {
        std::string id;
        std::vector<RawPrim> prims;
    };

    struct EffectParam
    {
        std::string samplerSource;
        std::string imageId;
        bool hasColor = false;
        float4 color = float4(0.f, 0.f, 0.f, 1.f);
        bool hasFloat = false;
        float number = 0.f;
    };

    struct ColorSample
    {
        bool authored = false;
        float4 color = float4(0.f, 0.f, 0.f, 1.f);
        std::string textureToken;
    };

    enum class UpAxis
    {
        Y,
        Z,
        X
    };

    float4 ParseColor(std::string_view text, float4 fallback)
    {
        float values[4] = { fallback.x, fallback.y, fallback.z, fallback.w };
        const char* p = text.data();
        const char* end = p + text.size();
        int count = 0;
        while (count < 4 && ParseFloatToken(p, end, values[count]))
            ++count;
        if (count == 3)
            values[3] = 1.f;
        if (count < 3)
            return fallback;
        return float4(values[0], values[1], values[2], values[3]);
    }

    float3 Sample3(const Source& source, int index)
    {
        return float3(source.at(index, 0), source.at(index, 1), source.stride >= 3 ? source.at(index, 2) : 0.f);
    }

    float2 Sample2(const Source& source, int index)
    {
        return float2(source.at(index, 0), source.stride >= 2 ? source.at(index, 1) : 0.f);
    }

    bool Finite3(const float3x3& m)
    {
        for (float value : m.m_data)
        {
            if (!std::isfinite(value))
                return false;
        }
        return true;
    }

    float3 SafeNormalize(float3 n, float3 fallback = float3(0.f, 0.f, 1.f))
    {
        const float len = length(n);
        return len > 1e-20f ? n / len : fallback;
    }

    float3 ApplyUp(float3 v, UpAxis axis)
    {
        switch (axis)
        {
        case UpAxis::Z:
            return v;
        case UpAxis::Y:
            return float3(v.x, -v.z, v.y);
        case UpAxis::X:
            return float3(-v.y, -v.z, v.x);
        }
        return v;
    }

    float3 TransformPoint(const float4x4& m, float3 p)
    {
        const float4 r = m * float4(p, 1.f);
        if (std::abs(r.w) > 1e-8f && std::abs(r.w - 1.f) > 1e-5f)
            return float3(r.x / r.w, r.y / r.w, r.z / r.w);
        return float3(r.x, r.y, r.z);
    }

    float4x4 Translation(float x, float y, float z)
    {
        return float4x4(
            1.f, 0.f, 0.f, x,
            0.f, 1.f, 0.f, y,
            0.f, 0.f, 1.f, z,
            0.f, 0.f, 0.f, 1.f);
    }

    float4x4 ScaleMatrix(float x, float y, float z)
    {
        return float4x4(
            x, 0.f, 0.f, 0.f,
            0.f, y, 0.f, 0.f,
            0.f, 0.f, z, 0.f,
            0.f, 0.f, 0.f, 1.f);
    }

    float4x4 AxisRotation(float3 axis, float degrees)
    {
        const float len = length(axis);
        if (len < 1e-8f)
            return float4x4::identity();
        axis = axis / len;
        const float rad = degrees * 0.01745329251994329577f;
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        const float t = 1.f - c;
        const float x = axis.x;
        const float y = axis.y;
        const float z = axis.z;
        return float4x4(
            t * x * x + c, t * x * y - s * z, t * x * z + s * y, 0.f,
            t * x * y + s * z, t * y * y + c, t * y * z - s * x, 0.f,
            t * x * z - s * y, t * y * z + s * x, t * z * z + c, 0.f,
            0.f, 0.f, 0.f, 1.f);
    }

    float4x4 MatrixFromText(std::string_view text)
    {
        float m[16] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1
        };
        const char* p = text.data();
        const char* end = p + text.size();
        int count = 0;
        while (count < 16 && ParseFloatToken(p, end, m[count]))
            ++count;
        if (count < 16)
            return float4x4::identity();
        // COLLADA writes matrices row-major. Translation is the fourth column.
        return float4x4(
            m[0], m[1], m[2], m[3],
            m[4], m[5], m[6], m[7],
            m[8], m[9], m[10], m[11],
            m[12], m[13], m[14], m[15]);
    }

    float3x3 NormalMatrix(const float4x4& m)
    {
        const float3x3 linear(m);
        const float3x3 inv = inverse(linear);
        if (!Finite3(inv))
            return linear;
        return transpose(inv);
    }

    // Phong shininess is the specular exponent. 0 -> roughness 1.
    float PhongRoughness(float shininess)
    {
        const float exponent = std::max(shininess, 0.f);
        const float roughness = std::sqrt(2.f / (exponent + 2.f));
        return std::clamp(roughness, 0.f, 1.f);
    }

    float Luminance(float3 c)
    {
        return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
    }

    void ResolveShading(ColladaMaterialInfo& material)
    {
        const bool phong = material.shading == "phong" || material.shading == "blinn";
        const float roughness = phong ? PhongRoughness(material.shininess) : 1.f;
        const float specMax = std::max(material.specular.x, std::max(material.specular.y, material.specular.z));
        const float diffuseLum = Luminance(material.diffuse);
        // Phong specular is a highlight color. A dark diffuse with a strong highlight
        // is the usual DCC authoring for metal. Bright diffuse stays the albedo:
        // treating that specular as F0 would zero the diffuse (Franka shells use
        // specular 1 with a white or gray diffuse and shininess 0).
        const bool metal = phong && specMax >= 0.5f && diffuseLum <= 0.2f;
        if (material.shading == "constant")
        {
            material.baseColor = Luminance(material.emission) > diffuseLum ? material.emission : material.diffuse;
            material.metalness = 0.f;
            material.roughness = 1.f;
            return;
        }
        if (metal)
        {
            material.baseColor = material.specular;
            material.metalness = std::clamp(specMax, 0.f, 1.f);
            material.roughness = roughness;
            return;
        }
        material.baseColor = material.diffuse;
        material.metalness = 0.f;
        material.roughness = roughness;
    }

    std::filesystem::path ResolveImagePath(const std::filesystem::path& daePath, std::string reference)
    {
        reference = PercentDecode(Trim(std::move(reference)));
        if (reference.rfind("file://", 0) == 0)
        {
            reference = reference.substr(7);
            if (reference.size() >= 3 && reference[0] == '/'
                && std::isalpha(static_cast<unsigned char>(reference[1])) && reference[2] == ':')
            {
                reference.erase(reference.begin());
            }
        }
        std::filesystem::path path(reference);
        if (path.empty())
            return {};
        if (path.is_absolute())
            return path.lexically_normal();
        return (daePath.parent_path() / path).lexically_normal();
    }

    void ParseSource(const XmlNode& node, std::unordered_map<std::string, Source>& sources)
    {
        const std::string id = Attr(node, "id");
        if (id.empty())
            return;

        std::unordered_map<std::string, std::vector<float>> arrays;
        ForEachChild(node, "float_array", [&](const XmlNode& arrayNode)
        {
            std::string arrayId = Attr(arrayNode, "id");
            if (arrayId.empty())
                arrayId = id + "#array";
            std::vector<float> values;
            const size_t count = ParseSize(Attr(arrayNode, "count"));
            if (count > 0)
                values.reserve(count);
            ParseFloats(arrayNode.text, values);
            arrays.emplace(std::move(arrayId), std::move(values));
        });

        Source source;
        const XmlNode* technique = FindChild(node, "technique_common");
        const XmlNode* accessor = technique ? FindChild(*technique, "accessor") : nullptr;
        if (accessor)
        {
            const std::string arrayId = StripRef(Attr(*accessor, "source"));
            if (auto found = arrays.find(arrayId); found != arrays.end())
                source.values = std::move(found->second);
            else if (!arrays.empty())
                source.values = std::move(arrays.begin()->second);

            source.count = ParseSize(Attr(*accessor, "count"));
            source.offset = ParseSize(Attr(*accessor, "offset"));
            source.stride = ParseInt(Attr(*accessor, "stride"), 0);
            int params = 0;
            for (const XmlNode& child : accessor->children)
            {
                if (child.name == "param")
                    ++params;
            }
            if (source.stride <= 0)
                source.stride = params > 0 ? params : 1;
        }
        else if (!arrays.empty())
        {
            source.values = std::move(arrays.begin()->second);
            source.stride = 3;
        }
        if (source.stride <= 0)
            source.stride = 1;
        if (source.count == 0 && source.offset < source.values.size())
            source.count = (source.values.size() - source.offset) / size_t(source.stride);
        sources.emplace(id, std::move(source));
    }

    RawInput ParseInput(const XmlNode& node)
    {
        RawInput input;
        input.semantic = ToLower(Attr(node, "semantic"));
        input.source = StripRef(Attr(node, "source"));
        input.offset = ParseInt(Attr(node, "offset"), 0);
        input.set = ParseInt(Attr(node, "set"), 0);
        return input;
    }

    RawPrim ParsePrimitive(const XmlNode& node, RawPrimType type)
    {
        RawPrim prim;
        prim.type = type;
        prim.material = Attr(node, "material");
        for (const XmlNode& child : node.children)
        {
            if (child.name == "input")
                prim.inputs.push_back(ParseInput(child));
            else if (child.name == "vcount")
                ParseInts(child.text, prim.vcount);
            else if (child.name == "p")
            {
                if (type == RawPrimType::Triangles || type == RawPrimType::Polylist)
                    ParseInts(child.text, prim.indices);
                else
                {
                    std::vector<int> group;
                    ParseInts(child.text, group);
                    if (!group.empty())
                        prim.groups.push_back(std::move(group));
                }
            }
            else if (child.name == "ph")
            {
                for (const XmlNode& holeChild : child.children)
                {
                    if (holeChild.name != "p")
                        continue;
                    std::vector<int> group;
                    ParseInts(holeChild.text, group);
                    if (!group.empty())
                        prim.groups.push_back(std::move(group));
                    break;
                }
            }
        }
        return prim;
    }

    ColorSample ReadColorSample(
        const XmlNode& channel,
        const std::unordered_map<std::string, EffectParam>& params)
    {
        ColorSample sample;
        for (const XmlNode& child : channel.children)
        {
            if (child.name == "color")
            {
                sample.authored = true;
                sample.color = ParseColor(child.text, float4(0.f, 0.f, 0.f, 1.f));
            }
            else if (child.name == "float")
            {
                sample.authored = true;
                const float value = ParseFloat(Trim(child.text), 0.f);
                sample.color = float4(value, value, value, 1.f);
            }
            else if (child.name == "texture")
            {
                sample.textureToken = Attr(child, "texture");
            }
            else if (child.name == "param")
            {
                const std::string ref = Attr(child, "ref");
                if (auto found = params.find(ref); found != params.end())
                {
                    if (found->second.hasColor)
                    {
                        sample.authored = true;
                        sample.color = found->second.color;
                    }
                    else if (found->second.hasFloat)
                    {
                        sample.authored = true;
                        const float value = found->second.number;
                        sample.color = float4(value, value, value, 1.f);
                    }
                    if (!found->second.imageId.empty() && sample.textureToken.empty())
                        sample.textureToken = found->second.imageId;
                }
            }
        }
        return sample;
    }

    float ReadFloatChannel(
        const XmlNode& channel,
        const std::unordered_map<std::string, EffectParam>& params,
        float fallback)
    {
        for (const XmlNode& child : channel.children)
        {
            if (child.name == "float")
                return ParseFloat(Trim(child.text), fallback);
            if (child.name == "param")
            {
                if (auto found = params.find(Attr(child, "ref")); found != params.end() && found->second.hasFloat)
                    return found->second.number;
            }
        }
        return fallback;
    }

    bool FindDoubleSided(const XmlNode& node)
    {
        if (node.name == "double_sided")
        {
            const std::string text = ToLower(Trim(node.text));
            return text == "1" || text == "true";
        }
        for (const XmlNode& child : node.children)
        {
            if (FindDoubleSided(child))
                return true;
        }
        return false;
    }

    std::string ResolveTextureToken(
        const std::string& token,
        const std::unordered_map<std::string, EffectParam>& params,
        const std::unordered_map<std::string, std::string>& images)
    {
        if (token.empty())
            return {};
        if (auto image = images.find(token); image != images.end())
            return image->second;

        std::string current = token;
        for (int depth = 0; depth < 4; ++depth)
        {
            auto found = params.find(current);
            if (found == params.end())
                break;
            if (!found->second.imageId.empty())
            {
                if (auto image = images.find(found->second.imageId); image != images.end())
                    return image->second;
                return found->second.imageId;
            }
            if (found->second.samplerSource.empty())
                break;
            current = found->second.samplerSource;
        }
        if (token.find('/') != std::string::npos || token.find('\\') != std::string::npos || token.find('.') != std::string::npos)
            return token;
        return {};
    }

    std::filesystem::path TexturePath(
        const std::string& token,
        const std::unordered_map<std::string, EffectParam>& params,
        const std::unordered_map<std::string, std::string>& images,
        const std::filesystem::path& daePath)
    {
        const std::string reference = ResolveTextureToken(token, params, images);
        if (reference.empty())
            return {};
        return ResolveImagePath(daePath, reference);
    }

    void ParseEffectParams(const XmlNode& profile, std::unordered_map<std::string, EffectParam>& params)
    {
        ForEachChild(profile, "newparam", [&](const XmlNode& paramNode)
        {
            const std::string sid = Attr(paramNode, "sid");
            if (sid.empty())
                return;
            EffectParam param;
            if (const XmlNode* sampler = FindChild(paramNode, "sampler2d"))
            {
                if (const XmlNode* source = FindChild(*sampler, "source"))
                    param.samplerSource = Trim(source->text);
            }
            if (const XmlNode* surface = FindChild(paramNode, "surface"))
            {
                if (const XmlNode* init = FindChild(*surface, "init_from"))
                    param.imageId = Trim(init->text);
            }
            if (const XmlNode* color = FindChild(paramNode, "color"))
            {
                param.hasColor = true;
                param.color = ParseColor(color->text, float4(0.f, 0.f, 0.f, 1.f));
            }
            if (const XmlNode* number = FindChild(paramNode, "float"))
            {
                param.hasFloat = true;
                param.number = ParseFloat(Trim(number->text), 0.f);
            }
            params[sid] = std::move(param);
        });
    }

    ColladaMaterialInfo ParseEffect(
        const XmlNode& effect,
        const std::unordered_map<std::string, std::string>& images,
        const std::filesystem::path& daePath)
    {
        ColladaMaterialInfo material;
        material.diffuse = float3(0.8f);
        material.baseColor = material.diffuse;

        const XmlNode* profile = FindChild(effect, "profile_common");
        if (!profile)
            return material;

        std::unordered_map<std::string, EffectParam> params;
        ParseEffectParams(*profile, params);
        material.doubleSided = FindDoubleSided(effect);

        const XmlNode* technique = FindChild(*profile, "technique");
        const XmlNode* shader = nullptr;
        if (technique)
        {
            for (const XmlNode& child : technique->children)
            {
                if (child.name == "phong" || child.name == "blinn" || child.name == "lambert" || child.name == "constant")
                {
                    shader = &child;
                    material.shading = child.name;
                    break;
                }
            }
        }
        if (!shader)
        {
            ResolveShading(material);
            return material;
        }

        float4 diffuse = float4(0.8f, 0.8f, 0.8f, 1.f);
        bool authoredDiffuse = false;
        float4 transparent(1.f, 1.f, 1.f, 1.f);
        std::string opaqueMode = "A_ONE";
        bool hasTransparency = false;
        float transparency = 1.f;

        for (const XmlNode& channel : shader->children)
        {
            if (channel.name == "diffuse")
            {
                const ColorSample sample = ReadColorSample(channel, params);
                authoredDiffuse = sample.authored;
                if (sample.authored)
                    diffuse = sample.color;
                material.diffuseTexture = TexturePath(sample.textureToken, params, images, daePath);
            }
            else if (channel.name == "specular")
            {
                const ColorSample sample = ReadColorSample(channel, params);
                if (sample.authored)
                    material.specular = float3(sample.color.x, sample.color.y, sample.color.z);
            }
            else if (channel.name == "emission" || channel.name == "emissive")
            {
                const ColorSample sample = ReadColorSample(channel, params);
                if (sample.authored)
                    material.emission = float3(sample.color.x, sample.color.y, sample.color.z);
                material.emissiveTexture = TexturePath(sample.textureToken, params, images, daePath);
            }
            else if (channel.name == "shininess")
            {
                material.shininess = std::max(0.f, ReadFloatChannel(channel, params, 0.f));
            }
            else if (channel.name == "transparent")
            {
                if (const std::string* opaque = FindAttr(channel, "opaque"))
                    opaqueMode = *opaque;
                const ColorSample sample = ReadColorSample(channel, params);
                if (sample.authored)
                    transparent = sample.color;
            }
            else if (channel.name == "transparency")
            {
                hasTransparency = true;
                transparency = ReadFloatChannel(channel, params, 1.f);
            }
            else if (channel.name == "index_of_refraction")
            {
                material.indexOfRefraction = std::max(0.f, ReadFloatChannel(channel, params, 1.f));
            }
            else if (channel.name == "bump" || channel.name == "normal")
            {
                const ColorSample sample = ReadColorSample(channel, params);
                material.normalTexture = TexturePath(sample.textureToken, params, images, daePath);
            }
        }

        if (!material.diffuseTexture.empty() && !authoredDiffuse)
            diffuse = float4(1.f, 1.f, 1.f, diffuse.w);
        material.diffuse = float3(diffuse.x, diffuse.y, diffuse.z);

        float opacity = 1.f;
        if (IEquals(opaqueMode, "RGB_ZERO"))
            opacity = 1.f - Luminance(float3(transparent.x, transparent.y, transparent.z));
        else
            opacity = transparent.w;
        if (hasTransparency)
            opacity *= transparency;
        opacity *= diffuse.w;
        material.opacity = std::clamp(opacity, 0.f, 1.f);
        ResolveShading(material);
        return material;
    }

    struct CornerRef
    {
        int position = -1;
        int normal = -1;
        int texcoord = -1;

        bool operator==(const CornerRef& other) const
        {
            return position == other.position && normal == other.normal && texcoord == other.texcoord;
        }
    };

    struct CornerHash
    {
        size_t operator()(const CornerRef& key) const
        {
            size_t h = std::hash<int>{}(key.position);
            h ^= std::hash<int>{}(key.normal) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(key.texcoord) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct Stream
    {
        std::string semantic;
        const Source* source = nullptr;
        int offset = 0;
        int set = 0;
    };

    struct PrimBuild
    {
        ColladaPrimitive primitive;
        std::unordered_map<CornerRef, uint32_t, CornerHash> weld;
        const Source* positions = nullptr;
        const Source* normals = nullptr;
        const Source* texcoords = nullptr;
        int positionOffset = 0;
        int normalOffset = -1;
        int texcoordOffset = -1;
        int stride = 1;
        bool hasNormals = false;
        const float4x4* transform = nullptr;
        float3x3 normalMatrix = float3x3::identity();
        float unitMeter = 1.f;
        UpAxis upAxis = UpAxis::Z;
        int nextUniqueNormal = 0;
    };

    int IndexAt(const std::vector<int>& indices, size_t corner, int offset, int stride)
    {
        const size_t i = corner * size_t(stride) + size_t(offset);
        if (i >= indices.size())
            return -1;
        return indices[i];
    }

    uint32_t EmitCorner(PrimBuild& build, const std::vector<int>& indices, size_t corner)
    {
        if (!build.positions)
            return ~0u;
        const int positionIndex = IndexAt(indices, corner, build.positionOffset, build.stride);
        if (!build.positions->contains(positionIndex))
            return ~0u;

        CornerRef key;
        key.position = positionIndex;
        if (build.hasNormals && build.normals)
        {
            key.normal = IndexAt(indices, corner, build.normalOffset, build.stride);
            if (!build.normals->contains(key.normal))
                key.normal = -1;
        }
        else
        {
            key.normal = build.nextUniqueNormal++;
        }
        if (build.texcoords && build.texcoordOffset >= 0)
        {
            key.texcoord = IndexAt(indices, corner, build.texcoordOffset, build.stride);
            if (!build.texcoords->contains(key.texcoord))
                key.texcoord = -1;
        }

        if (auto found = build.weld.find(key); found != build.weld.end())
            return found->second;

        float3 position = Sample3(*build.positions, positionIndex);
        if (build.transform)
            position = TransformPoint(*build.transform, position);
        position = position * build.unitMeter;
        position = ApplyUp(position, build.upAxis);

        float3 normal(0.f, 0.f, 1.f);
        if (key.normal >= 0 && build.normals && build.hasNormals)
        {
            normal = Sample3(*build.normals, key.normal);
            normal = build.normalMatrix * normal;
            normal = SafeNormalize(ApplyUp(normal, build.upAxis));
        }

        float2 uv(0.f);
        if (key.texcoord >= 0 && build.texcoords)
            uv = Sample2(*build.texcoords, key.texcoord);

        const uint32_t index = uint32_t(build.primitive.positions.size());
        build.primitive.positions.push_back(position);
        build.primitive.normals.push_back(normal);
        build.primitive.texcoords.push_back(uv);
        build.primitive.bounds |= position;
        build.weld.emplace(key, index);
        return index;
    }

    void EmitTriangle(PrimBuild& build, const std::vector<int>& indices, size_t c0, size_t c1, size_t c2)
    {
        const uint32_t i0 = EmitCorner(build, indices, c0);
        const uint32_t i1 = EmitCorner(build, indices, c1);
        const uint32_t i2 = EmitCorner(build, indices, c2);
        if (i0 == ~0u || i1 == ~0u || i2 == ~0u)
            return;
        if (!build.hasNormals)
        {
            const float3 face = SafeNormalize(cross(
                build.primitive.positions[i1] - build.primitive.positions[i0],
                build.primitive.positions[i2] - build.primitive.positions[i0]));
            build.primitive.normals[i0] = face;
            build.primitive.normals[i1] = face;
            build.primitive.normals[i2] = face;
        }
        build.primitive.indices.push_back(i0);
        build.primitive.indices.push_back(i1);
        build.primitive.indices.push_back(i2);
    }

    void EmitPolygon(PrimBuild& build, const std::vector<int>& indices)
    {
        if (build.stride <= 0)
            return;
        const size_t corners = indices.size() / size_t(build.stride);
        if (corners < 3)
            return;
        for (size_t i = 1; i + 1 < corners; ++i)
            EmitTriangle(build, indices, 0, i, i + 1);
    }

    void EmitTriangleList(PrimBuild& build, const std::vector<int>& indices)
    {
        if (build.stride <= 0)
            return;
        const size_t corners = indices.size() / size_t(build.stride);
        for (size_t i = 0; i + 2 < corners; i += 3)
            EmitTriangle(build, indices, i, i + 1, i + 2);
    }

    void EmitStrip(PrimBuild& build, const std::vector<int>& indices)
    {
        if (build.stride <= 0)
            return;
        const size_t corners = indices.size() / size_t(build.stride);
        for (size_t i = 0; i + 2 < corners; ++i)
        {
            if ((i & 1u) == 0)
                EmitTriangle(build, indices, i, i + 1, i + 2);
            else
                EmitTriangle(build, indices, i + 1, i, i + 2);
        }
    }

    void EmitFan(PrimBuild& build, const std::vector<int>& indices)
    {
        EmitPolygon(build, indices);
    }

    void EmitPolylist(PrimBuild& build, const RawPrim& prim)
    {
        if (build.stride <= 0)
            return;
        size_t cursor = 0;
        const size_t cornerCount = prim.indices.size() / size_t(build.stride);
        for (int count : prim.vcount)
        {
            if (count < 3 || cursor >= cornerCount)
            {
                cursor += size_t(std::max(count, 0));
                continue;
            }
            const size_t available = std::min(size_t(count), cornerCount - cursor);
            if (available >= 3)
            {
                for (size_t i = 1; i + 1 < available; ++i)
                    EmitTriangle(build, prim.indices, cursor, cursor + i, cursor + i + 1);
            }
            cursor += size_t(count);
        }
    }

    int StreamStride(const std::vector<Stream>& streams)
    {
        int stride = 1;
        for (const Stream& stream : streams)
            stride = std::max(stride, stream.offset + 1);
        return stride;
    }

    void AddPrimitive(
        const RawPrim& prim,
        const std::unordered_map<std::string, Source>& sources,
        const std::unordered_map<std::string, std::vector<VertexStream>>& vertices,
        const std::unordered_map<std::string, std::string>& bind,
        const float4x4& transform,
        float unitMeter,
        UpAxis upAxis,
        ColladaMeshData& out)
    {
        std::vector<Stream> streams;
        for (const RawInput& input : prim.inputs)
        {
            if (input.semantic == "vertex")
            {
                auto found = vertices.find(input.source);
                if (found == vertices.end())
                    continue;
                for (const VertexStream& vertexStream : found->second)
                {
                    Stream stream;
                    stream.semantic = vertexStream.semantic;
                    stream.offset = input.offset;
                    stream.set = 0;
                    if (auto source = sources.find(vertexStream.source); source != sources.end())
                        stream.source = &source->second;
                    streams.push_back(stream);
                }
            }
            else
            {
                Stream stream;
                stream.semantic = input.semantic;
                stream.offset = input.offset;
                stream.set = input.set;
                if (auto source = sources.find(input.source); source != sources.end())
                    stream.source = &source->second;
                streams.push_back(stream);
            }
        }

        PrimBuild build;
        build.stride = StreamStride(streams);
        build.transform = &transform;
        build.normalMatrix = NormalMatrix(transform);
        build.unitMeter = unitMeter;
        build.upAxis = upAxis;
        int texcoordSet = 0;
        bool haveTexcoord = false;
        for (const Stream& stream : streams)
        {
            if (stream.semantic == "position" && !build.positions)
            {
                build.positions = stream.source;
                build.positionOffset = stream.offset;
            }
            else if (stream.semantic == "normal" && !build.normals)
            {
                build.normals = stream.source;
                build.normalOffset = stream.offset;
                build.hasNormals = stream.source != nullptr;
            }
            else if (stream.semantic == "texcoord" && stream.source)
            {
                if (!haveTexcoord || stream.set < texcoordSet)
                {
                    build.texcoords = stream.source;
                    build.texcoordOffset = stream.offset;
                    texcoordSet = stream.set;
                    haveTexcoord = true;
                }
            }
        }
        if (!build.positions)
            return;

        std::string materialId = prim.material;
        if (auto bound = bind.find(materialId); bound != bind.end())
            materialId = bound->second;
        build.primitive.materialId = materialId;

        if (prim.type == RawPrimType::Triangles)
            EmitTriangleList(build, prim.indices);
        else if (prim.type == RawPrimType::Polylist)
            EmitPolylist(build, prim);
        else if (prim.type == RawPrimType::Tristrips)
        {
            for (const std::vector<int>& group : prim.groups)
                EmitStrip(build, group);
        }
        else if (prim.type == RawPrimType::Trifans)
        {
            for (const std::vector<int>& group : prim.groups)
                EmitFan(build, group);
        }
        else
        {
            for (const std::vector<int>& group : prim.groups)
                EmitPolygon(build, group);
        }

        if (build.primitive.indices.size() < 3)
            return;
        out.primitives.push_back(std::move(build.primitive));
    }

    void AddGeometry(
        const Geometry& geometry,
        const std::unordered_map<std::string, Source>& sources,
        const std::unordered_map<std::string, std::vector<VertexStream>>& vertices,
        const float4x4& transform,
        const std::unordered_map<std::string, std::string>& bind,
        float unitMeter,
        UpAxis upAxis,
        ColladaMeshData& out)
    {
        for (const RawPrim& prim : geometry.prims)
            AddPrimitive(prim, sources, vertices, bind, transform, unitMeter, upAxis, out);
    }

    float4x4 NodeLocalTransform(const XmlNode& node)
    {
        float4x4 local = float4x4::identity();
        for (const XmlNode& child : node.children)
        {
            if (child.name == "matrix")
            {
                local = local * MatrixFromText(child.text);
            }
            else if (child.name == "translate")
            {
                float v[3] = { 0.f, 0.f, 0.f };
                const char* p = child.text.data();
                const char* end = p + child.text.size();
                int count = 0;
                while (count < 3 && ParseFloatToken(p, end, v[count]))
                    ++count;
                local = local * Translation(v[0], v[1], v[2]);
            }
            else if (child.name == "scale")
            {
                float v[3] = { 1.f, 1.f, 1.f };
                const char* p = child.text.data();
                const char* end = p + child.text.size();
                int count = 0;
                while (count < 3 && ParseFloatToken(p, end, v[count]))
                    ++count;
                local = local * ScaleMatrix(v[0], v[1], v[2]);
            }
            else if (child.name == "rotate")
            {
                float v[4] = { 0.f, 0.f, 1.f, 0.f };
                const char* p = child.text.data();
                const char* end = p + child.text.size();
                int count = 0;
                while (count < 4 && ParseFloatToken(p, end, v[count]))
                    ++count;
                local = local * AxisRotation(float3(v[0], v[1], v[2]), v[3]);
            }
        }
        return local;
    }

    std::unordered_map<std::string, std::string> BindMaterials(const XmlNode& instance)
    {
        std::unordered_map<std::string, std::string> bind;
        const XmlNode* bindNode = FindChild(instance, "bind_material");
        if (!bindNode)
            return bind;
        const XmlNode* technique = FindChild(*bindNode, "technique_common");
        const XmlNode* scope = technique ? technique : bindNode;
        ForEachChild(*scope, "instance_material", [&](const XmlNode& materialNode)
        {
            const std::string symbol = Attr(materialNode, "symbol");
            const std::string target = StripRef(Attr(materialNode, "target"));
            if (!symbol.empty() && !target.empty())
                bind[symbol] = target;
        });
        return bind;
    }

    void IndexNodes(const XmlNode& node, std::unordered_map<std::string, const XmlNode*>& nodes)
    {
        if (node.name == "node")
        {
            const std::string id = Attr(node, "id");
            if (!id.empty())
                nodes.emplace(id, &node);
        }
        for (const XmlNode& child : node.children)
            IndexNodes(child, nodes);
    }

    void WalkNode(
        const XmlNode& node,
        const float4x4& parent,
        const std::unordered_map<std::string, Geometry>& geometries,
        const std::unordered_map<std::string, Source>& sources,
        const std::unordered_map<std::string, std::vector<VertexStream>>& vertices,
        const std::unordered_map<std::string, const XmlNode*>& nodes,
        std::unordered_set<std::string>& stack,
        float unitMeter,
        UpAxis upAxis,
        ColladaMeshData& out,
        bool& instantiated)
    {
        const float4x4 world = parent * NodeLocalTransform(node);
        const std::string id = Attr(node, "id");
        if (!id.empty() && !stack.insert(id).second)
            return;

        ForEachChild(node, "instance_geometry", [&](const XmlNode& instance)
        {
            instantiated = true;
            const std::string geometryId = StripRef(Attr(instance, "url"));
            auto geometry = geometries.find(geometryId);
            if (geometry == geometries.end())
            {
                caustica::warning("COLLADA instance_geometry '%s' was not found.", geometryId.c_str());
                return;
            }
            AddGeometry(geometry->second, sources, vertices, world, BindMaterials(instance), unitMeter, upAxis, out);
        });

        ForEachChild(node, "instance_node", [&](const XmlNode& instance)
        {
            const std::string target = StripRef(Attr(instance, "url"));
            if (target.empty() || stack.count(target))
                return;
            auto found = nodes.find(target);
            if (found == nodes.end())
                return;
            WalkNode(*found->second, world, geometries, sources, vertices, nodes, stack, unitMeter, upAxis, out, instantiated);
        });

        for (const XmlNode& child : node.children)
        {
            if (child.name == "node")
                WalkNode(child, world, geometries, sources, vertices, nodes, stack, unitMeter, upAxis, out, instantiated);
        }

        if (!id.empty())
            stack.erase(id);
    }
} // namespace

bool loadColladaFile(const std::filesystem::path& filePath, ColladaMeshData& outMesh)
{
    outMesh = ColladaMeshData{};
    std::string xml = ReadFileText(filePath);
    if (xml.empty())
    {
        caustica::error("COLLADA file could not be opened or is empty: '%s'", filePath.string().c_str());
        return false;
    }

    XmlNode root;
    size_t cursor = 0;
    if (xml.size() >= 3
        && static_cast<unsigned char>(xml[0]) == 0xEF
        && static_cast<unsigned char>(xml[1]) == 0xBB
        && static_cast<unsigned char>(xml[2]) == 0xBF)
    {
        cursor = 3;
    }
    if (!ParseElement(xml, cursor, root) || root.name != "collada")
    {
        caustica::error("COLLADA file '%s' has no <COLLADA> root.", filePath.string().c_str());
        return false;
    }

    float unitMeter = 1.f;
    UpAxis upAxis = UpAxis::Y;
    if (const XmlNode* asset = FindChild(root, "asset"))
    {
        if (const XmlNode* unit = FindChild(*asset, "unit"))
        {
            if (const std::string* meter = FindAttr(*unit, "meter"))
            {
                const float parsed = ParseFloat(*meter, 1.f);
                if (parsed > 0.f)
                    unitMeter = parsed;
            }
        }
        if (const XmlNode* axis = FindChild(*asset, "up_axis"))
        {
            const std::string text = ToLower(Trim(axis->text));
            if (text == "z_up")
                upAxis = UpAxis::Z;
            else if (text == "x_up")
                upAxis = UpAxis::X;
            else
                upAxis = UpAxis::Y;
        }
    }

    std::unordered_map<std::string, std::string> images;
    if (const XmlNode* library = FindChild(root, "library_images"))
    {
        ForEachChild(*library, "image", [&](const XmlNode& image)
        {
            const std::string id = Attr(image, "id");
            std::string reference;
            if (const XmlNode* init = FindChild(image, "init_from"))
                reference = Trim(init->text);
            if (!id.empty() && !reference.empty())
                images.emplace(id, std::move(reference));
        });
    }

    std::unordered_map<std::string, ColladaMaterialInfo> effects;
    if (const XmlNode* library = FindChild(root, "library_effects"))
    {
        ForEachChild(*library, "effect", [&](const XmlNode& effect)
        {
            const std::string id = Attr(effect, "id");
            if (id.empty())
                return;
            effects.emplace(id, ParseEffect(effect, images, filePath));
        });
    }

    if (const XmlNode* library = FindChild(root, "library_materials"))
    {
        ForEachChild(*library, "material", [&](const XmlNode& materialNode)
        {
            const std::string id = Attr(materialNode, "id");
            if (id.empty())
                return;
            ColladaMaterialInfo material;
            const std::string effectId = StripRef([&]()
            {
                if (const XmlNode* instance = FindChild(materialNode, "instance_effect"))
                    return Attr(*instance, "url");
                return std::string();
            }());
            if (auto effect = effects.find(effectId); effect != effects.end())
                material = effect->second;
            material.id = id;
            material.name = Attr(materialNode, "name");
            if (material.name.empty())
                material.name = id;
            outMesh.materials.emplace(id, std::move(material));
        });
    }

    std::unordered_map<std::string, Source> sources;
    std::unordered_map<std::string, std::vector<VertexStream>> vertices;
    std::unordered_map<std::string, Geometry> geometries;
    if (const XmlNode* library = FindChild(root, "library_geometries"))
    {
        ForEachChild(*library, "geometry", [&](const XmlNode& geometryNode)
        {
            const XmlNode* mesh = FindChild(geometryNode, "mesh");
            if (!mesh)
                return;
            for (const XmlNode& child : mesh->children)
            {
                if (child.name == "source")
                    ParseSource(child, sources);
                else if (child.name == "vertices")
                {
                    const std::string id = Attr(child, "id");
                    if (id.empty())
                        continue;
                    std::vector<VertexStream> streams;
                    ForEachChild(child, "input", [&](const XmlNode& input)
                    {
                        VertexStream stream;
                        stream.semantic = ToLower(Attr(input, "semantic"));
                        stream.source = StripRef(Attr(input, "source"));
                        if (!stream.semantic.empty() && !stream.source.empty())
                            streams.push_back(std::move(stream));
                    });
                    vertices.emplace(id, std::move(streams));
                }
            }

            Geometry geometry;
            geometry.id = Attr(geometryNode, "id");
            for (const XmlNode& child : mesh->children)
            {
                if (child.name == "triangles")
                    geometry.prims.push_back(ParsePrimitive(child, RawPrimType::Triangles));
                else if (child.name == "polylist")
                    geometry.prims.push_back(ParsePrimitive(child, RawPrimType::Polylist));
                else if (child.name == "polygons")
                    geometry.prims.push_back(ParsePrimitive(child, RawPrimType::Polygons));
                else if (child.name == "tristrips")
                    geometry.prims.push_back(ParsePrimitive(child, RawPrimType::Tristrips));
                else if (child.name == "trifans")
                    geometry.prims.push_back(ParsePrimitive(child, RawPrimType::Trifans));
            }
            if (!geometry.id.empty() && !geometry.prims.empty())
                geometries.emplace(geometry.id, std::move(geometry));
        });
    }

    std::unordered_map<std::string, const XmlNode*> nodes;
    IndexNodes(root, nodes);

    const XmlNode* visualScene = nullptr;
    std::string sceneId;
    if (const XmlNode* scene = FindChild(root, "scene"))
    {
        if (const XmlNode* instance = FindChild(*scene, "instance_visual_scene"))
            sceneId = StripRef(Attr(*instance, "url"));
    }
    if (const XmlNode* library = FindChild(root, "library_visual_scenes"))
    {
        ForEachChild(*library, "visual_scene", [&](const XmlNode& scene)
        {
            if (!visualScene)
                visualScene = &scene;
            if (!sceneId.empty() && Attr(scene, "id") == sceneId)
                visualScene = &scene;
        });
    }

    bool instantiated = false;
    if (visualScene)
    {
        std::unordered_set<std::string> stack;
        for (const XmlNode& child : visualScene->children)
        {
            if (child.name == "node")
            {
                WalkNode(child, float4x4::identity(), geometries, sources, vertices, nodes, stack,
                    unitMeter, upAxis, outMesh, instantiated);
            }
        }
    }
    if (!instantiated)
    {
        for (const auto& [id, geometry] : geometries)
        {
            (void)id;
            AddGeometry(geometry, sources, vertices, float4x4::identity(), {}, unitMeter, upAxis, outMesh);
        }
    }

    if (outMesh.primitives.empty())
    {
        caustica::error("COLLADA file '%s' produced no triangles.", filePath.string().c_str());
        outMesh = ColladaMeshData{};
        return false;
    }

    size_t triangles = 0;
    for (const ColladaPrimitive& prim : outMesh.primitives)
        triangles += prim.indices.size() / 3;
    caustica::info("COLLADA '%s': %zu materials, %zu primitives, %zu triangles.",
        filePath.string().c_str(), outMesh.materials.size(), outMesh.primitives.size(), triangles);
    return true;
}
} // namespace caustica
