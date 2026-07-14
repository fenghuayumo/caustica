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
    template<> dm::int2 read<dm::int2>(const Json::Value& node, const dm::int2& defaultValue);
    template<> dm::int3 read<dm::int3>(const Json::Value& node, const dm::int3& defaultValue);
    template<> dm::int4 read<dm::int4>(const Json::Value& node, const dm::int4& defaultValue);
    template<> dm::uint read<dm::uint>(const Json::Value& node, const dm::uint& defaultValue);
    template<> dm::uint2 read<dm::uint2>(const Json::Value& node, const dm::uint2& defaultValue);
    template<> dm::uint3 read<dm::uint3>(const Json::Value& node, const dm::uint3& defaultValue);
    template<> dm::uint4 read<dm::uint4>(const Json::Value& node, const dm::uint4& defaultValue);
    template<> bool read<bool>(const Json::Value& node, const bool& defaultValue);
    template<> float read<float>(const Json::Value& node, const float& defaultValue);
    template<> dm::float2 read<dm::float2>(const Json::Value& node, const dm::float2& defaultValue);
    template<> dm::float3 read<dm::float3>(const Json::Value& node, const dm::float3& defaultValue);
    template<> dm::float4 read<dm::float4>(const Json::Value& node, const dm::float4& defaultValue);
    template<> double read<double>(const Json::Value& node, const double& defaultValue);
    template<> dm::double2 read<dm::double2>(const Json::Value& node, const dm::double2& defaultValue);
    template<> dm::double3 read<dm::double3>(const Json::Value& node, const dm::double3& defaultValue);
    template<> dm::double4 read<dm::double4>(const Json::Value& node, const dm::double4& defaultValue);

    template<> void write<std::string>(Json::Value& node, const std::string& value);
    template<> void write<int>(Json::Value& node, const int& value);
    template<> void write<dm::int2>(Json::Value& node, const dm::int2& value);
    template<> void write<dm::int3>(Json::Value& node, const dm::int3& value);
    template<> void write<dm::int4>(Json::Value& node, const dm::int4& value);
    template<> void write<dm::uint>(Json::Value& node, const dm::uint& value);
    template<> void write<dm::uint2>(Json::Value& node, const dm::uint2& value);
    template<> void write<dm::uint3>(Json::Value& node, const dm::uint3& value);
    template<> void write<dm::uint4>(Json::Value& node, const dm::uint4& value);
    template<> void write<bool>(Json::Value& node, const bool& value);
    template<> void write<float>(Json::Value& node, const float& value);
    template<> void write<dm::float2>(Json::Value& node, const dm::float2& value);
    template<> void write<dm::float3>(Json::Value& node, const dm::float3& value);
    template<> void write<dm::float4>(Json::Value& node, const dm::float4& value);
    template<> void write<double>(Json::Value& node, const double& value);
    template<> void write<dm::double2>(Json::Value& node, const dm::double2& value);
    template<> void write<dm::double3>(Json::Value& node, const dm::double3& value);
    template<> void write<dm::double4>(Json::Value& node, const dm::double4& value);

// File-based JSON I/O (uses std::filesystem, not VFS)
bool SaveToFile(const std::filesystem::path& filePath, const Json::Value& rootNode);
bool loadFromFile(const std::filesystem::path& filePath, Json::Value& outRootNode);

// String-based JSON I/O
std::string ToString(const Json::Value& rootNode);
bool FromString(const std::string& jsonData, Json::Value& outRootNode);

// Extract string array from JSON array node
std::vector<std::string> ReadStringArray(const Json::Value& arr);

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
