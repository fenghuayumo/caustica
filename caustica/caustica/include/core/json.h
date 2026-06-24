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
    bool LoadFromFile(caustica::IFileSystem& fs, const std::filesystem::path& jsonFileName, Json::Value& documentRoot);

    template<typename T> T Read(const Json::Value& node, const T& defaultValue);
    template<typename T> void Write(Json::Value& node, const T& value);

    template<> std::string Read<std::string>(const Json::Value& node, const std::string& defaultValue);
    template<> int Read<int>(const Json::Value& node, const int& defaultValue);
    template<> dm::int2 Read<dm::int2>(const Json::Value& node, const dm::int2& defaultValue);
    template<> dm::int3 Read<dm::int3>(const Json::Value& node, const dm::int3& defaultValue);
    template<> dm::int4 Read<dm::int4>(const Json::Value& node, const dm::int4& defaultValue);
    template<> dm::uint Read<dm::uint>(const Json::Value& node, const dm::uint& defaultValue);
    template<> dm::uint2 Read<dm::uint2>(const Json::Value& node, const dm::uint2& defaultValue);
    template<> dm::uint3 Read<dm::uint3>(const Json::Value& node, const dm::uint3& defaultValue);
    template<> dm::uint4 Read<dm::uint4>(const Json::Value& node, const dm::uint4& defaultValue);
    template<> bool Read<bool>(const Json::Value& node, const bool& defaultValue);
    template<> float Read<float>(const Json::Value& node, const float& defaultValue);
    template<> dm::float2 Read<dm::float2>(const Json::Value& node, const dm::float2& defaultValue);
    template<> dm::float3 Read<dm::float3>(const Json::Value& node, const dm::float3& defaultValue);
    template<> dm::float4 Read<dm::float4>(const Json::Value& node, const dm::float4& defaultValue);
    template<> double Read<double>(const Json::Value& node, const double& defaultValue);
    template<> dm::double2 Read<dm::double2>(const Json::Value& node, const dm::double2& defaultValue);
    template<> dm::double3 Read<dm::double3>(const Json::Value& node, const dm::double3& defaultValue);
    template<> dm::double4 Read<dm::double4>(const Json::Value& node, const dm::double4& defaultValue);

    template<> void Write<std::string>(Json::Value& node, const std::string& value);
    template<> void Write<int>(Json::Value& node, const int& value);
    template<> void Write<dm::int2>(Json::Value& node, const dm::int2& value);
    template<> void Write<dm::int3>(Json::Value& node, const dm::int3& value);
    template<> void Write<dm::int4>(Json::Value& node, const dm::int4& value);
    template<> void Write<dm::uint>(Json::Value& node, const dm::uint& value);
    template<> void Write<dm::uint2>(Json::Value& node, const dm::uint2& value);
    template<> void Write<dm::uint3>(Json::Value& node, const dm::uint3& value);
    template<> void Write<dm::uint4>(Json::Value& node, const dm::uint4& value);
    template<> void Write<bool>(Json::Value& node, const bool& value);
    template<> void Write<float>(Json::Value& node, const float& value);
    template<> void Write<dm::float2>(Json::Value& node, const dm::float2& value);
    template<> void Write<dm::float3>(Json::Value& node, const dm::float3& value);
    template<> void Write<dm::float4>(Json::Value& node, const dm::float4& value);
    template<> void Write<double>(Json::Value& node, const double& value);
    template<> void Write<dm::double2>(Json::Value& node, const dm::double2& value);
    template<> void Write<dm::double3>(Json::Value& node, const dm::double3& value);
    template<> void Write<dm::double4>(Json::Value& node, const dm::double4& value);

// File-based JSON I/O (uses std::filesystem, not VFS)
bool SaveToFile(const std::filesystem::path& filePath, const Json::Value& rootNode);
bool LoadFromFile(const std::filesystem::path& filePath, Json::Value& outRootNode);

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
    dest = caustica::json::Read<T>(node, dest);
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

    dest = std::optional<T>(caustica::json::Read<T>(node, T()));
}

// Overloaded operator for writing data into Json nodes.
// Use like this: myNode["name"] << variable;
template<typename T> void operator << (Json::Value& node, const T& src)
{
    caustica::json::Write<T>(node, src);
}

// Specialization of the writing operator for literal strings.
void operator << (Json::Value& node, const char* src);
