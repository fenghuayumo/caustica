#pragma once

#include <math/math.h>
#include <json/json.h>
#include <filesystem>
#include <optional>
#include <string>

namespace Json
{
    class Value;
}

namespace caustica
{
    class IFileSystem;
}

namespace caustica::json
{
    bool loadFromFile(caustica::IFileSystem& fs, const std::filesystem::path& jsonFileName, Json::Value& documentRoot);

    template<typename T> T read(const Json::Value& node, const T& defaultValue);
    template<typename T> void write(Json::Value& node, const T& value);

    template<> std::string read<std::string>(const Json::Value& node, const std::string& defaultValue);
    template<> int read<int>(const Json::Value& node, const int& defaultValue);
    template<> math::int2 read<math::int2>(const Json::Value& node, const math::int2& defaultValue);
    template<> math::int3 read<math::int3>(const Json::Value& node, const math::int3& defaultValue);
    template<> math::int4 read<math::int4>(const Json::Value& node, const math::int4& defaultValue);
    template<> math::uint read<math::uint>(const Json::Value& node, const math::uint& defaultValue);
    template<> math::uint2 read<math::uint2>(const Json::Value& node, const math::uint2& defaultValue);
    template<> math::uint3 read<math::uint3>(const Json::Value& node, const math::uint3& defaultValue);
    template<> math::uint4 read<math::uint4>(const Json::Value& node, const math::uint4& defaultValue);
    template<> bool read<bool>(const Json::Value& node, const bool& defaultValue);
    template<> float read<float>(const Json::Value& node, const float& defaultValue);
    template<> math::float2 read<math::float2>(const Json::Value& node, const math::float2& defaultValue);
    template<> math::float3 read<math::float3>(const Json::Value& node, const math::float3& defaultValue);
    template<> math::float4 read<math::float4>(const Json::Value& node, const math::float4& defaultValue);
    template<> double read<double>(const Json::Value& node, const double& defaultValue);
    template<> math::double2 read<math::double2>(const Json::Value& node, const math::double2& defaultValue);
    template<> math::double3 read<math::double3>(const Json::Value& node, const math::double3& defaultValue);
    template<> math::double4 read<math::double4>(const Json::Value& node, const math::double4& defaultValue);

    template<> void write<std::string>(Json::Value& node, const std::string& value);
    template<> void write<int>(Json::Value& node, const int& value);
    template<> void write<math::int2>(Json::Value& node, const math::int2& value);
    template<> void write<math::int3>(Json::Value& node, const math::int3& value);
    template<> void write<math::int4>(Json::Value& node, const math::int4& value);
    template<> void write<math::uint>(Json::Value& node, const math::uint& value);
    template<> void write<math::uint2>(Json::Value& node, const math::uint2& value);
    template<> void write<math::uint3>(Json::Value& node, const math::uint3& value);
    template<> void write<math::uint4>(Json::Value& node, const math::uint4& value);
    template<> void write<bool>(Json::Value& node, const bool& value);
    template<> void write<float>(Json::Value& node, const float& value);
    template<> void write<math::float2>(Json::Value& node, const math::float2& value);
    template<> void write<math::float3>(Json::Value& node, const math::float3& value);
    template<> void write<math::float4>(Json::Value& node, const math::float4& value);
    template<> void write<double>(Json::Value& node, const double& value);
    template<> void write<math::double2>(Json::Value& node, const math::double2& value);
    template<> void write<math::double3>(Json::Value& node, const math::double3& value);
    template<> void write<math::double4>(Json::Value& node, const math::double4& value);

// File-based JSON I/O (uses std::filesystem, not VFS)
bool saveToFile(const std::filesystem::path& filePath, const Json::Value& rootNode);
bool loadFromFile(const std::filesystem::path& filePath, Json::Value& outRootNode);

// String-based JSON I/O
std::string toString(const Json::Value& rootNode);
bool fromString(const std::string& jsonData, Json::Value& outRootNode);

// Extract string array from JSON array node
std::vector<std::string> readStringArray(const Json::Value& arr);

}

// Overloaded operator for reading data from Json nodes.
// When the node doesn't have data in the right format, the destination value is unchanged.
// Use like this: myNode["name"] >> variable;
template<typename T> void operator >> (const Json::Value& node, T& dest)
{
    dest = caustica::json::read<T>(node, dest);
}

// Overloaded operator for reading data from Json nodes.
// When the node is null, the destination value is set to an empty optional.
template<typename T> void operator >> (const Json::Value& node, std::optional<T>& dest)
{
    if (node.isNull())
    {
        dest = std::optional<T>();
        return;
    }

    dest = std::optional<T>(caustica::json::read<T>(node, T()));
}

// Overloaded operator for writing data into Json nodes.
// Use like this: myNode["name"] << variable;
template<typename T> void operator << (Json::Value& node, const T& src)
{
    caustica::json::write<T>(node, src);
}

// Specialization of the writing operator for literal strings.
void operator << (Json::Value& node, const char* src);
