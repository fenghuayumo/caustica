#include <scene/loader/UrdfImporter.h>

#include <scene/loader/StlLoader.h>
#include <scene/SceneImport.h>
#include <scene/SceneEcs.h>
#include <scene/SceneObjects.h>
#include <scene/SceneTypes.h>
#include <assets/loader/TextureLoader.h>
#include <core/log.h>
#include <math/math.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace caustica::math;

namespace caustica
{
namespace
{
    constexpr float kPi = 3.14159265358979323846f;

    std::string ToLower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    std::string Trim(std::string text)
    {
        const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
        const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
        return begin < end ? std::string(begin, end) : std::string();
    }

    std::string StripXmlComments(std::string text)
    {
        for (;;)
        {
            const size_t begin = text.find("<!--");
            if (begin == std::string::npos)
                break;
            const size_t end = text.find("-->", begin + 4);
            if (end == std::string::npos)
            {
                text.erase(begin);
                break;
            }
            text.erase(begin, end + 3 - begin);
        }
        return text;
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

    std::optional<std::string> getAttribute(const std::string& tag, const std::string& attribute)
    {
        const std::string key = attribute + "=\"";
        size_t pos = 0;
        while ((pos = tag.find(key, pos)) != std::string::npos)
        {
            // Ensure attribute name starts at a delimiter.
            if (pos > 0 && !std::isspace(static_cast<unsigned char>(tag[pos - 1])) && tag[pos - 1] != '<')
            {
                ++pos;
                continue;
            }
            const size_t valueBegin = pos + key.size();
            const size_t valueEnd = tag.find('"', valueBegin);
            if (valueEnd == std::string::npos)
                return std::nullopt;
            return tag.substr(valueBegin, valueEnd - valueBegin);
        }

        const std::string keySingle = attribute + "='";
        pos = 0;
        while ((pos = tag.find(keySingle, pos)) != std::string::npos)
        {
            if (pos > 0 && !std::isspace(static_cast<unsigned char>(tag[pos - 1])) && tag[pos - 1] != '<')
            {
                ++pos;
                continue;
            }
            const size_t valueBegin = pos + keySingle.size();
            const size_t valueEnd = tag.find('\'', valueBegin);
            if (valueEnd == std::string::npos)
                return std::nullopt;
            return tag.substr(valueBegin, valueEnd - valueBegin);
        }
        return std::nullopt;
    }

    bool ParseFloats(const std::string& text, float* out, size_t count)
    {
        std::istringstream stream(text);
        for (size_t i = 0; i < count; ++i)
        {
            if (!(stream >> out[i]))
                return false;
        }
        return true;
    }

    dm::double3 ParseXyz(const std::string& text, dm::double3 fallback = dm::double3(0.0))
    {
        float values[3] = { float(fallback.x), float(fallback.y), float(fallback.z) };
        ParseFloats(text, values, 3);
        return dm::double3(values[0], values[1], values[2]);
    }

    dm::float3 ParseFloat3(const std::string& text, dm::float3 fallback = dm::float3(1.f))
    {
        float values[3] = { fallback.x, fallback.y, fallback.z };
        ParseFloats(text, values, 3);
        return dm::float3(values[0], values[1], values[2]);
    }

    dm::float4 ParseRgba(const std::string& text, dm::float4 fallback = dm::float4(0.8f, 0.8f, 0.8f, 1.f))
    {
        float values[4] = { fallback.x, fallback.y, fallback.z, fallback.w };
        ParseFloats(text, values, 4);
        return dm::float4(values[0], values[1], values[2], values[3]);
    }

    struct UrdfPose
    {
        dm::double3 xyz = dm::double3(0.0);
        dm::double3 rpy = dm::double3(0.0);

        [[nodiscard]] bool isIdentity() const
        {
            return dm::length(xyz) < 1e-12 && dm::length(rpy) < 1e-12;
        }
    };

    UrdfPose ParseOrigin(const std::string& xmlRegion)
    {
        UrdfPose pose;
        const size_t originPos = ToLower(xmlRegion).find("<origin");
        if (originPos == std::string::npos)
            return pose;

        const size_t originEnd = xmlRegion.find('>', originPos);
        if (originEnd == std::string::npos)
            return pose;

        const std::string tag = xmlRegion.substr(originPos, originEnd - originPos + 1);
        if (auto xyz = getAttribute(tag, "xyz"))
            pose.xyz = ParseXyz(*xyz);
        if (auto rpy = getAttribute(tag, "rpy"))
            pose.rpy = ParseXyz(*rpy);
        return pose;
    }

    void ApplyPose(scene::SceneEntityWorld& world, ecs::Entity entity, const UrdfPose& pose)
    {
        const dm::dquat rotation = dm::rotationQuat(pose.rpy);
        world.setLocalTransform(entity, &pose.xyz, &rotation, nullptr);
    }

    struct UrdfVisual
    {
        UrdfPose origin;
        std::string meshFilename;
        dm::float3 meshScale = dm::float3(1.f);
        enum class GeomType { Mesh, Box, Cylinder, Sphere } geomType = GeomType::Mesh;
        dm::float3 boxSize = dm::float3(1.f);
        float cylinderRadius = 0.05f;
        float cylinderLength = 0.1f;
        float sphereRadius = 0.05f;
        dm::float4 rgba = dm::float4(0.8f, 0.8f, 0.8f, 1.f);
        std::string materialName;
    };

    struct UrdfLink
    {
        std::string name;
        std::vector<UrdfVisual> visuals;
    };

    struct UrdfJoint
    {
        std::string name;
        std::string parent;
        std::string child;
        UrdfPose origin;
    };

    size_t FindTagClose(const std::string& xml, size_t openBracket, const std::string& tagName)
    {
        // openBracket points at '<'. Supports self-closing and matching end tags (non-nested).
        const size_t firstClose = xml.find('>', openBracket);
        if (firstClose == std::string::npos)
            return std::string::npos;
        if (firstClose > openBracket && xml[firstClose - 1] == '/')
            return firstClose;

        const std::string endTag = "</" + tagName + ">";
        const std::string endTagUpper = "</" + ToLower(tagName) + ">";
        size_t pos = firstClose + 1;
        while (pos < xml.size())
        {
            const size_t candidate = xml.find("</", pos);
            if (candidate == std::string::npos)
                return std::string::npos;
            const std::string slice = ToLower(xml.substr(candidate, endTag.size()));
            if (slice == ToLower(endTag) || slice == endTagUpper)
                return candidate + endTag.size() - 1;
            pos = candidate + 2;
        }
        return std::string::npos;
    }

    std::vector<std::pair<size_t, size_t>> FindElements(const std::string& xml, const std::string& tagName)
    {
        std::vector<std::pair<size_t, size_t>> ranges;
        const std::string open = "<" + tagName;
        const std::string openLower = ToLower(open);
        const std::string xmlLower = ToLower(xml);
        size_t pos = 0;
        while (pos < xmlLower.size())
        {
            const size_t found = xmlLower.find(openLower, pos);
            if (found == std::string::npos)
                break;
            // Ensure exact tag match (not "link" matching "linkage").
            const size_t after = found + open.size();
            if (after < xml.size())
            {
                const char c = xml[after];
                if (!std::isspace(static_cast<unsigned char>(c)) && c != '>' && c != '/')
                {
                    pos = after;
                    continue;
                }
            }
            const size_t end = FindTagClose(xml, found, tagName);
            if (end == std::string::npos)
                break;
            ranges.emplace_back(found, end + 1);
            pos = end + 1;
        }
        return ranges;
    }

    std::string ElementInner(const std::string& xml, size_t begin, size_t end)
    {
        const size_t firstClose = xml.find('>', begin);
        if (firstClose == std::string::npos || firstClose + 1 >= end)
            return {};
        // Self-closing
        if (xml[firstClose - 1] == '/')
            return {};
        size_t innerEnd = end;
        // Trim trailing end tag
        const size_t lastOpen = xml.rfind("</", end);
        if (lastOpen != std::string::npos && lastOpen > firstClose)
            innerEnd = lastOpen;
        return xml.substr(firstClose + 1, innerEnd - (firstClose + 1));
    }

    std::string ElementOpenTag(const std::string& xml, size_t begin, size_t end)
    {
        const size_t firstClose = xml.find('>', begin);
        if (firstClose == std::string::npos || firstClose >= end)
            return {};
        return xml.substr(begin, firstClose - begin + 1);
    }

    void ParseVisualGeometry(const std::string& visualXml, UrdfVisual& visual)
    {
        const std::string lower = ToLower(visualXml);
        auto parseMesh = [&](size_t pos)
        {
            const size_t end = visualXml.find('>', pos);
            if (end == std::string::npos)
                return;
            const std::string tag = visualXml.substr(pos, end - pos + 1);
            visual.geomType = UrdfVisual::GeomType::Mesh;
            if (auto filename = getAttribute(tag, "filename"))
                visual.meshFilename = *filename;
            if (auto scale = getAttribute(tag, "scale"))
                visual.meshScale = ParseFloat3(*scale, dm::float3(1.f));
        };
        auto parseBox = [&](size_t pos)
        {
            const size_t end = visualXml.find('>', pos);
            if (end == std::string::npos)
                return;
            const std::string tag = visualXml.substr(pos, end - pos + 1);
            visual.geomType = UrdfVisual::GeomType::Box;
            if (auto size = getAttribute(tag, "size"))
                visual.boxSize = ParseFloat3(*size, dm::float3(1.f));
        };
        auto parseCylinder = [&](size_t pos)
        {
            const size_t end = visualXml.find('>', pos);
            if (end == std::string::npos)
                return;
            const std::string tag = visualXml.substr(pos, end - pos + 1);
            visual.geomType = UrdfVisual::GeomType::Cylinder;
            if (auto radius = getAttribute(tag, "radius"))
                visual.cylinderRadius = std::strtof(radius->c_str(), nullptr);
            if (auto length = getAttribute(tag, "length"))
                visual.cylinderLength = std::strtof(length->c_str(), nullptr);
        };
        auto parseSphere = [&](size_t pos)
        {
            const size_t end = visualXml.find('>', pos);
            if (end == std::string::npos)
                return;
            const std::string tag = visualXml.substr(pos, end - pos + 1);
            visual.geomType = UrdfVisual::GeomType::Sphere;
            if (auto radius = getAttribute(tag, "radius"))
                visual.sphereRadius = std::strtof(radius->c_str(), nullptr);
        };

        if (const size_t pos = lower.find("<mesh"); pos != std::string::npos)
            parseMesh(pos);
        else if (const size_t pos = lower.find("<box"); pos != std::string::npos)
            parseBox(pos);
        else if (const size_t pos = lower.find("<cylinder"); pos != std::string::npos)
            parseCylinder(pos);
        else if (const size_t pos = lower.find("<sphere"); pos != std::string::npos)
            parseSphere(pos);
    }

    void ParseVisualMaterial(
        const std::string& visualXml,
        UrdfVisual& visual,
        const std::unordered_map<std::string, dm::float4>& namedMaterials)
    {
        const auto materialRanges = FindElements(visualXml, "material");
        if (materialRanges.empty())
            return;

        const auto [begin, end] = materialRanges.front();
        const std::string openTag = ElementOpenTag(visualXml, begin, end);
        if (auto name = getAttribute(openTag, "name"))
            visual.materialName = *name;

        const std::string inner = ElementInner(visualXml, begin, end);
        const std::string innerLower = ToLower(inner);
        const size_t colorPos = innerLower.find("<color");
        if (colorPos != std::string::npos)
        {
            const size_t colorEnd = inner.find('>', colorPos);
            if (colorEnd != std::string::npos)
            {
                const std::string colorTag = inner.substr(colorPos, colorEnd - colorPos + 1);
                if (auto rgba = getAttribute(colorTag, "rgba"))
                    visual.rgba = ParseRgba(*rgba);
            }
        }
        else if (!visual.materialName.empty())
        {
            auto found = namedMaterials.find(visual.materialName);
            if (found != namedMaterials.end())
                visual.rgba = found->second;
        }
    }

    std::unordered_map<std::string, dm::float4> ParseNamedMaterials(const std::string& robotXml)
    {
        std::unordered_map<std::string, dm::float4> materials;
        // Only top-level materials (direct robot children): scan material tags not nested in link.
        // Simpler approach: parse all materials that contain a color child; link-local ones also ok.
        for (const auto& [begin, end] : FindElements(robotXml, "material"))
        {
            const std::string openTag = ElementOpenTag(robotXml, begin, end);
            auto name = getAttribute(openTag, "name");
            if (!name || name->empty())
                continue;
            const std::string inner = ElementInner(robotXml, begin, end);
            const std::string innerLower = ToLower(inner);
            const size_t colorPos = innerLower.find("<color");
            if (colorPos == std::string::npos)
                continue;
            const size_t colorEnd = inner.find('>', colorPos);
            if (colorEnd == std::string::npos)
                continue;
            const std::string colorTag = inner.substr(colorPos, colorEnd - colorPos + 1);
            if (auto rgba = getAttribute(colorTag, "rgba"))
                materials[*name] = ParseRgba(*rgba);
        }
        return materials;
    }

    std::vector<UrdfLink> ParseLinks(
        const std::string& robotXml,
        const std::unordered_map<std::string, dm::float4>& namedMaterials)
    {
        std::vector<UrdfLink> links;
        for (const auto& [begin, end] : FindElements(robotXml, "link"))
        {
            UrdfLink link;
            const std::string openTag = ElementOpenTag(robotXml, begin, end);
            if (auto name = getAttribute(openTag, "name"))
                link.name = *name;
            if (link.name.empty())
                continue;

            const std::string inner = ElementInner(robotXml, begin, end);
            for (const auto& [vBegin, vEnd] : FindElements(inner, "visual"))
            {
                UrdfVisual visual;
                const std::string visualXml = inner.substr(vBegin, vEnd - vBegin);
                visual.origin = ParseOrigin(visualXml);
                ParseVisualGeometry(visualXml, visual);
                ParseVisualMaterial(visualXml, visual, namedMaterials);
                if (visual.geomType == UrdfVisual::GeomType::Mesh && visual.meshFilename.empty())
                    continue;
                link.visuals.push_back(std::move(visual));
            }
            links.push_back(std::move(link));
        }
        return links;
    }

    std::vector<UrdfJoint> ParseJoints(const std::string& robotXml)
    {
        std::vector<UrdfJoint> joints;
        for (const auto& [begin, end] : FindElements(robotXml, "joint"))
        {
            UrdfJoint joint;
            const std::string openTag = ElementOpenTag(robotXml, begin, end);
            if (auto name = getAttribute(openTag, "name"))
                joint.name = *name;

            const std::string inner = ElementInner(robotXml, begin, end);
            joint.origin = ParseOrigin(inner);

            for (const auto& [pBegin, pEnd] : FindElements(inner, "parent"))
            {
                const std::string tag = ElementOpenTag(inner, pBegin, pEnd);
                if (auto link = getAttribute(tag, "link"))
                    joint.parent = *link;
            }
            for (const auto& [cBegin, cEnd] : FindElements(inner, "child"))
            {
                const std::string tag = ElementOpenTag(inner, cBegin, cEnd);
                if (auto link = getAttribute(tag, "link"))
                    joint.child = *link;
            }

            if (!joint.parent.empty() && !joint.child.empty())
                joints.push_back(std::move(joint));
        }
        return joints;
    }

    std::filesystem::path ResolveUrdfMeshPath(
        const std::filesystem::path& urdfPath,
        const std::string& filename,
        const std::filesystem::path& meshDirHint)
    {
        std::string pathText = filename;
        if (pathText.rfind("package://", 0) == 0)
        {
            pathText = pathText.substr(std::string("package://").size());
            const size_t slash = pathText.find('/');
            if (slash != std::string::npos)
                pathText = pathText.substr(slash + 1);
        }
        else if (pathText.rfind("file://", 0) == 0)
        {
            pathText = pathText.substr(std::string("file://").size());
        }

        std::filesystem::path meshPath(pathText);
        if (meshPath.is_absolute())
            return meshPath.lexically_normal();

        std::vector<std::filesystem::path> candidates;
        candidates.push_back((urdfPath.parent_path() / meshPath).lexically_normal());
        if (!meshDirHint.empty())
            candidates.push_back((urdfPath.parent_path() / meshDirHint / meshPath.filename()).lexically_normal());
        candidates.push_back((urdfPath.parent_path().parent_path() / meshPath).lexically_normal());
        // Common layout: URDF in robots/foo/, meshes in robots/foo/meshes/
        candidates.push_back((urdfPath.parent_path() / "meshes" / meshPath.filename()).lexically_normal());

        for (const auto& candidate : candidates)
        {
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec))
                return candidate;
        }
        return candidates.front();
    }

    std::filesystem::path ParseMeshDirHint(const std::string& robotXml)
    {
        const std::string lower = ToLower(robotXml);
        const size_t compilerPos = lower.find("<compiler");
        if (compilerPos == std::string::npos)
            return {};
        const size_t end = robotXml.find('>', compilerPos);
        if (end == std::string::npos)
            return {};
        const std::string tag = robotXml.substr(compilerPos, end - compilerPos + 1);
        if (auto meshdir = getAttribute(tag, "meshdir"))
            return std::filesystem::path(*meshdir);
        return {};
    }

    void AppendBox(StlMeshData& mesh, const dm::float3& size)
    {
        const dm::float3 h = size * 0.5f;
        const dm::float3 corners[8] = {
            {-h.x, -h.y, -h.z}, { h.x, -h.y, -h.z}, { h.x,  h.y, -h.z}, {-h.x,  h.y, -h.z},
            {-h.x, -h.y,  h.z}, { h.x, -h.y,  h.z}, { h.x,  h.y,  h.z}, {-h.x,  h.y,  h.z},
        };
        const int faces[6][4] = {
            {0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1},
            {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0},
        };
        const dm::float3 normals[6] = {
            {0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0},
        };

        for (int f = 0; f < 6; ++f)
        {
            const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
            for (int i = 0; i < 4; ++i)
            {
                mesh.positions.push_back(corners[faces[f][i]]);
                mesh.normals.push_back(normals[f]);
                mesh.bounds |= corners[faces[f][i]];
            }
            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 1);
            mesh.indices.push_back(base + 2);
            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 2);
            mesh.indices.push_back(base + 3);
        }
    }

    void AppendCylinder(StlMeshData& mesh, float radius, float length, int segments = 24)
    {
        const float half = length * 0.5f;
        const uint32_t baseTop = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.emplace_back(0.f, 0.f, half);
        mesh.normals.emplace_back(0.f, 0.f, 1.f);
        mesh.bounds |= dm::float3(0.f, 0.f, half);

        const uint32_t ringTop = static_cast<uint32_t>(mesh.positions.size());
        for (int i = 0; i < segments; ++i)
        {
            const float a = (float(i) / float(segments)) * 2.f * kPi;
            const dm::float3 p(std::cos(a) * radius, std::sin(a) * radius, half);
            mesh.positions.push_back(p);
            mesh.normals.emplace_back(0.f, 0.f, 1.f);
            mesh.bounds |= p;
        }
        for (int i = 0; i < segments; ++i)
        {
            mesh.indices.push_back(baseTop);
            mesh.indices.push_back(ringTop + i);
            mesh.indices.push_back(ringTop + ((i + 1) % segments));
        }

        const uint32_t baseBottom = static_cast<uint32_t>(mesh.positions.size());
        mesh.positions.emplace_back(0.f, 0.f, -half);
        mesh.normals.emplace_back(0.f, 0.f, -1.f);
        mesh.bounds |= dm::float3(0.f, 0.f, -half);

        const uint32_t ringBottom = static_cast<uint32_t>(mesh.positions.size());
        for (int i = 0; i < segments; ++i)
        {
            const float a = (float(i) / float(segments)) * 2.f * kPi;
            const dm::float3 p(std::cos(a) * radius, std::sin(a) * radius, -half);
            mesh.positions.push_back(p);
            mesh.normals.emplace_back(0.f, 0.f, -1.f);
            mesh.bounds |= p;
        }
        for (int i = 0; i < segments; ++i)
        {
            mesh.indices.push_back(baseBottom);
            mesh.indices.push_back(ringBottom + ((i + 1) % segments));
            mesh.indices.push_back(ringBottom + i);
        }

        const uint32_t sideTop = static_cast<uint32_t>(mesh.positions.size());
        for (int i = 0; i < segments; ++i)
        {
            const float a = (float(i) / float(segments)) * 2.f * kPi;
            const dm::float3 n(std::cos(a), std::sin(a), 0.f);
            const dm::float3 top(n.x * radius, n.y * radius, half);
            const dm::float3 bottom(n.x * radius, n.y * radius, -half);
            mesh.positions.push_back(top);
            mesh.normals.push_back(n);
            mesh.bounds |= top;
            mesh.positions.push_back(bottom);
            mesh.normals.push_back(n);
            mesh.bounds |= bottom;
        }
        for (int i = 0; i < segments; ++i)
        {
            const uint32_t i0 = sideTop + uint32_t(i) * 2;
            const uint32_t i1 = sideTop + uint32_t((i + 1) % segments) * 2;
            mesh.indices.push_back(i0);
            mesh.indices.push_back(i0 + 1);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i0 + 1);
            mesh.indices.push_back(i1 + 1);
        }
    }

    void AppendSphere(StlMeshData& mesh, float radius, int slices = 16, int stacks = 12)
    {
        for (int lat = 0; lat <= stacks; ++lat)
        {
            const float v = float(lat) / float(stacks);
            const float phi = v * kPi;
            for (int lon = 0; lon <= slices; ++lon)
            {
                const float u = float(lon) / float(slices);
                const float theta = u * 2.f * kPi;
                const dm::float3 n(
                    std::sin(phi) * std::cos(theta),
                    std::sin(phi) * std::sin(theta),
                    std::cos(phi));
                const dm::float3 p = n * radius;
                mesh.positions.push_back(p);
                mesh.normals.push_back(n);
                mesh.bounds |= p;
            }
        }

        const int stride = slices + 1;
        for (int lat = 0; lat < stacks; ++lat)
        {
            for (int lon = 0; lon < slices; ++lon)
            {
                const uint32_t i0 = uint32_t(lat * stride + lon);
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + uint32_t(stride);
                const uint32_t i3 = i2 + 1;
                mesh.indices.push_back(i0);
                mesh.indices.push_back(i2);
                mesh.indices.push_back(i1);
                mesh.indices.push_back(i1);
                mesh.indices.push_back(i2);
                mesh.indices.push_back(i3);
            }
        }
    }

    void ApplyScale(StlMeshData& mesh, const dm::float3& scale)
    {
        if (std::abs(scale.x - 1.f) < 1e-8f && std::abs(scale.y - 1.f) < 1e-8f && std::abs(scale.z - 1.f) < 1e-8f)
            return;

        mesh.bounds = dm::box3::empty();
        for (size_t i = 0; i < mesh.positions.size(); ++i)
        {
            mesh.positions[i] = mesh.positions[i] * scale;
            // Inverse-transpose of diagonal scale for normals.
            dm::float3 n = mesh.normals[i] / dm::float3(
                scale.x != 0.f ? scale.x : 1.f,
                scale.y != 0.f ? scale.y : 1.f,
                scale.z != 0.f ? scale.z : 1.f);
            const float len = dm::length(n);
            mesh.normals[i] = len > 1e-20f ? n / len : dm::float3(0.f, 1.f, 0.f);
            mesh.bounds |= mesh.positions[i];
        }
    }

    bool BuildVisualMeshData(
        const UrdfVisual& visual,
        const std::filesystem::path& urdfPath,
        const std::filesystem::path& meshDirHint,
        std::unordered_map<std::string, StlMeshData>& stlCache,
        StlMeshData& outMesh)
    {
        outMesh = StlMeshData{};
        if (visual.geomType == UrdfVisual::GeomType::Box)
        {
            AppendBox(outMesh, visual.boxSize);
            return !outMesh.empty();
        }
        if (visual.geomType == UrdfVisual::GeomType::Cylinder)
        {
            AppendCylinder(outMesh, visual.cylinderRadius, visual.cylinderLength);
            return !outMesh.empty();
        }
        if (visual.geomType == UrdfVisual::GeomType::Sphere)
        {
            AppendSphere(outMesh, visual.sphereRadius);
            return !outMesh.empty();
        }

        const std::filesystem::path resolved = ResolveUrdfMeshPath(urdfPath, visual.meshFilename, meshDirHint);
        std::string ext = resolved.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext != ".stl")
        {
            caustica::warning("URDF visual mesh '%s' uses unsupported format '%s' (only .stl is supported).",
                visual.meshFilename.c_str(), ext.c_str());
            return false;
        }

        const std::string cacheKey = resolved.lexically_normal().string();
        auto found = stlCache.find(cacheKey);
        if (found == stlCache.end())
        {
            StlMeshData loaded;
            if (!loadStlFile(resolved, loaded))
                return false;
            found = stlCache.emplace(cacheKey, std::move(loaded)).first;
        }

        outMesh = found->second;
        ApplyScale(outMesh, visual.meshScale);
        return !outMesh.empty();
    }

    std::shared_ptr<MeshInfo> BuildMeshFromStl(
        SceneTypeFactory& factory,
        const std::string& name,
        const StlMeshData& stl,
        const dm::float4& rgba,
        const std::filesystem::path& sourcePath)
    {
        auto mesh = std::static_pointer_cast<MeshInfo>(factory.createMesh());
        mesh->name = name;
        mesh->type = MeshType::Triangles;
        mesh->buffers = std::make_shared<BufferGroup>();
        mesh->objectSpaceBounds = stl.bounds;

        auto& buffers = *mesh->buffers;
        buffers.positionData = stl.positions;
        buffers.indexData = stl.indices;
        buffers.texcoord1Data.assign(stl.positions.size(), dm::float2(0.f));
        buffers.normalData.resize(stl.positions.size());
        buffers.tangentData.resize(stl.positions.size());

        for (size_t i = 0; i < stl.positions.size(); ++i)
        {
            const dm::float3 n = i < stl.normals.size() ? stl.normals[i] : dm::float3(0.f, 1.f, 0.f);
            buffers.normalData[i] = dm::vectorToSnorm8(n);
            // Stable fallback tangent for untextured STL.
            const dm::float3 axis = std::abs(n.z) < 0.999f ? dm::float3(0.f, 0.f, 1.f) : dm::float3(0.f, 1.f, 0.f);
            dm::float3 tangent = dm::cross(axis, n);
            const float tlen = dm::length(tangent);
            tangent = tlen > 1e-20f ? tangent / tlen : dm::float3(1.f, 0.f, 0.f);
            buffers.tangentData[i] = dm::vectorToSnorm8(dm::float4(tangent, 1.f));
        }

        auto material = std::dynamic_pointer_cast<Material>(factory.createMaterial());
        material->name = name + "_mat";
        material->modelFileName = sourcePath.string();
        material->baseOrDiffuseColor = dm::float3(rgba.x, rgba.y, rgba.z);
        material->opacity = rgba.w;
        material->roughness = 0.45f;
        material->metalness = 0.05f;
        material->domain = rgba.w < 0.999f ? MaterialDomain::AlphaBlended : MaterialDomain::Opaque;

        auto geometry = std::static_pointer_cast<MeshGeometry>(factory.createMeshGeometry());
        geometry->material = material;
        geometry->objectSpaceBounds = stl.bounds;
        geometry->indexOffsetInMesh = 0;
        geometry->vertexOffsetInMesh = 0;
        geometry->numIndices = static_cast<uint32_t>(stl.indices.size());
        geometry->numVertices = static_cast<uint32_t>(stl.positions.size());
        mesh->geometries.push_back(geometry);

        mesh->totalVertices = geometry->numVertices;
        mesh->totalIndices = geometry->numIndices;
        return mesh;
    }
} // namespace

UrdfImporter::UrdfImporter(std::shared_ptr<SceneTypeFactory> sceneTypeFactory)
    : m_SceneTypeFactory(std::move(sceneTypeFactory))
{
}

bool UrdfImporter::load(
    const std::filesystem::path& fileName,
    TextureLoader&,
    SceneLoadingStats&,
    ThreadPool*,
    SceneImportResult& result,
    const std::filesystem::path&) const
{
    if (!m_SceneTypeFactory)
    {
        caustica::error("UrdfImporter: scene type factory is null.");
        return false;
    }

    std::string xml = ReadFileText(fileName);
    if (xml.empty())
    {
        caustica::error("URDF file could not be opened or is empty: '%s'", fileName.string().c_str());
        return false;
    }
    xml = StripXmlComments(std::move(xml));

    const auto robotRanges = FindElements(xml, "robot");
    if (robotRanges.empty())
    {
        caustica::error("URDF file '%s' has no <robot> root element.", fileName.string().c_str());
        return false;
    }

    const auto [robotBegin, robotEnd] = robotRanges.front();
    const std::string robotOpen = ElementOpenTag(xml, robotBegin, robotEnd);
    const std::string robotInner = ElementInner(xml, robotBegin, robotEnd);
    std::string robotName = fileName.stem().string();
    if (auto name = getAttribute(robotOpen, "name"))
        if (!name->empty())
            robotName = *name;

    const auto namedMaterials = ParseNamedMaterials(robotInner);
    const auto links = ParseLinks(robotInner, namedMaterials);
    const auto joints = ParseJoints(robotInner);
    const auto meshDirHint = ParseMeshDirHint(robotInner);

    if (links.empty())
    {
        caustica::error("URDF file '%s' contains no links.", fileName.string().c_str());
        return false;
    }

    result.entityWorld = std::make_shared<scene::SceneEntityWorld>();
    auto& world = *result.entityWorld;
    const ecs::Entity rootEntity = world.createEntity(robotName);
    result.rootEntity = rootEntity;

    std::unordered_map<std::string, ecs::Entity> linkEntities;
    linkEntities.reserve(links.size());
    for (const UrdfLink& link : links)
    {
        const ecs::Entity entity = world.createEntity(link.name, rootEntity);
        linkEntities[link.name] = entity;
    }

    std::unordered_set<std::string> childLinks;
    for (const UrdfJoint& joint : joints)
    {
        auto parentIt = linkEntities.find(joint.parent);
        auto childIt = linkEntities.find(joint.child);
        if (parentIt == linkEntities.end() || childIt == linkEntities.end())
        {
            caustica::warning("URDF joint '%s' references missing link(s) parent='%s' child='%s'.",
                joint.name.c_str(), joint.parent.c_str(), joint.child.c_str());
            continue;
        }
        if (!world.setParent(childIt->second, parentIt->second))
        {
            caustica::warning("URDF joint '%s' failed to parent '%s' under '%s'.",
                joint.name.c_str(), joint.child.c_str(), joint.parent.c_str());
            continue;
        }
        ApplyPose(world, childIt->second, joint.origin);
        childLinks.insert(joint.child);
    }

    // Links that are never joint children stay under the robot root (typically base_link).
    (void)childLinks;

    std::unordered_map<std::string, StlMeshData> stlCache;
    size_t visualCount = 0;
    size_t failedVisuals = 0;

    for (const UrdfLink& link : links)
    {
        auto linkIt = linkEntities.find(link.name);
        if (linkIt == linkEntities.end())
            continue;

        for (size_t visualIndex = 0; visualIndex < link.visuals.size(); ++visualIndex)
        {
            const UrdfVisual& visual = link.visuals[visualIndex];
            StlMeshData meshData;
            if (!BuildVisualMeshData(visual, fileName, meshDirHint, stlCache, meshData))
            {
                ++failedVisuals;
                continue;
            }

            const std::string meshName = link.name + "_visual" + std::to_string(visualIndex);
            auto mesh = BuildMeshFromStl(*m_SceneTypeFactory, meshName, meshData, visual.rgba, fileName);

            // Always attach under a dedicated visual child so URDF visual origins are preserved.
            const ecs::Entity visualEntity = world.createEntity(meshName, linkIt->second);
            ApplyPose(world, visualEntity, visual.origin);
            world.setMeshInstance(visualEntity, mesh);
            ++visualCount;
        }
    }

    world.rebuildPathsFromRoot();

    if (visualCount == 0)
    {
        caustica::error("URDF file '%s' produced no visual meshes (%zu failed).",
            fileName.string().c_str(), failedVisuals);
        result = SceneImportResult{};
        return false;
    }

    if (failedVisuals > 0)
    {
        caustica::info("URDF '%s': loaded %zu links, %zu joints, %zu visuals (%zu mesh files cached, %zu visuals skipped).",
            fileName.string().c_str(), links.size(), joints.size(), visualCount, stlCache.size(), failedVisuals);
    }
    else
    {
        caustica::info("URDF '%s': loaded %zu links, %zu joints, %zu visuals (%zu mesh files cached).",
            fileName.string().c_str(), links.size(), joints.size(), visualCount, stlCache.size());
    }

    return true;
}
} // namespace caustica
