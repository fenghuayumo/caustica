#include <core/json.h>
#include <core/vfs/VFS.h>
#include <core/log.h>
#include <json/json-forwards.h>
#include <fstream>
#include <sstream>

using namespace caustica::math;
using namespace caustica;

namespace caustica::json
{
    bool loadFromFile(IFileSystem& fs, const std::filesystem::path& jsonFileName, Json::Value& documentRoot)
    {
        std::shared_ptr<IBlob> data = fs.readFile(jsonFileName);
        if (!data)
        {
            caustica::error("Couldn't read file %s", jsonFileName.generic_string().c_str());
            return false;
        }

        Json::CharReaderBuilder builder;
        builder["collectComments"] = false;
        Json::CharReader* reader = builder.newCharReader();

        const char* dataPtr = static_cast<const char*>(data->data());
        std::string errors;
        bool success = reader->parse(dataPtr, dataPtr + data->size(), &documentRoot, &errors);

        if (!success)
        {
            caustica::error("Couldn't parse JSON file %s:\n%s", jsonFileName.generic_string().c_str(), errors.c_str());
        }

        delete reader;

        return success;
    }

    template<>
    std::string read<std::string>(const Json::Value& node, const std::string& defaultValue)
    {
        if (node.isString())
            return node.asString();

        return defaultValue;
    }

    template<>
    int read<int>(const Json::Value& node, const int& defaultValue)
    {
        if (node.isNumeric())
            return node.asInt();

        return defaultValue;
    }

    template<>
    int2 read<int2>(const Json::Value& node, const int2& defaultValue)
    {
        if (node.isArray() && node.size() == 2)
            return int2(node[0].asInt(), node[1].asInt());

        if (node.isNumeric())
            return node.asInt();

        return defaultValue;
    }

    template<>
    int3 read<int3>(const Json::Value& node, const int3& defaultValue)
    {
        if (node.isArray() && node.size() == 3)
            return int3(node[0].asInt(), node[1].asInt(), node[2].asInt());

        if (node.isNumeric())
            return node.asInt();

        return defaultValue;
    }

    template<>
    int4 read<int4>(const Json::Value& node, const int4& defaultValue)
    {
        if (node.isArray() && node.size() == 4)
            return int4(node[0].asInt(), node[1].asInt(), node[2].asInt(), node[3].asInt());

        if (node.isNumeric())
            return node.asInt();

        return defaultValue;
    }

    template<>
    uint read<uint>(const Json::Value& node, const uint& defaultValue)
    {
        if (node.isNumeric())
            return node.asUInt();

        return defaultValue;
    }

    template<>
    uint2 read<uint2>(const Json::Value& node, const uint2& defaultValue)
    {
        if (node.isArray() && node.size() == 2)
            return uint2(node[0].asUInt(), node[1].asUInt());

        if (node.isNumeric())
            return node.asUInt();

        return defaultValue;
    }

    template<>
    uint3 read<uint3>(const Json::Value& node, const uint3& defaultValue)
    {
        if (node.isArray() && node.size() == 3)
            return uint3(node[0].asUInt(), node[1].asUInt(), node[2].asUInt());

        if (node.isNumeric())
            return node.asUInt();

        return defaultValue;
    }

    template<>
    uint4 read<uint4>(const Json::Value& node, const uint4& defaultValue)
    {
        if (node.isArray() && node.size() == 4)
            return uint4(node[0].asUInt(), node[1].asUInt(), node[2].asUInt(), node[3].asUInt());

        if (node.isNumeric())
            return node.asUInt();

        return defaultValue;
    }

    template<>
    bool read<bool>(const Json::Value& node, const bool& defaultValue)
    {
        if (node.isBool())
            return node.asBool();

        if (node.isNumeric())
            return node.asFloat() != 0.f;

        return defaultValue;
    }

    template<>
    float read<float>(const Json::Value& node, const float& defaultValue)
    {
        if (node.isNumeric())
            return node.asFloat();

        return defaultValue;
    }

    template<>
    float2 read<float2>(const Json::Value& node, const float2& defaultValue)
    {
        if (node.isArray() && node.size() == 2)
            return float2(node[0].asFloat(), node[1].asFloat());

        if (node.isNumeric())
            return node.asFloat();

        return defaultValue;
    }

    template<>
    float3 read<float3>(const Json::Value& node, const float3& defaultValue)
    {
        if (node.isArray() && node.size() == 3)
            return float3(node[0].asFloat(), node[1].asFloat(), node[2].asFloat());

        if (node.isNumeric())
            return node.asFloat();

        return defaultValue;
    }

    template<>
    float4 read<float4>(const Json::Value& node, const float4& defaultValue)
    {
        if (node.isArray() && node.size() == 4)
            return float4(node[0].asFloat(), node[1].asFloat(), node[2].asFloat(), node[3].asFloat());

        if (node.isNumeric())
            return node.asFloat();

        return defaultValue;
    }

    template<>
    double read<double>(const Json::Value& node, const double& defaultValue)
    {
        if (node.isNumeric())
            return node.asDouble();

        return defaultValue;
    }

    template<>
    double2 read<double2>(const Json::Value& node, const double2& defaultValue)
    {
        if (node.isArray() && node.size() == 2)
            return double2(node[0].asDouble(), node[1].asDouble());

        if (node.isNumeric())
            return node.asDouble();

        return defaultValue;
    }

    template<>
    double3 read<double3>(const Json::Value& node, const double3& defaultValue)
    {
        if (node.isArray() && node.size() == 3)
            return double3(node[0].asDouble(), node[1].asDouble(), node[2].asDouble());

        if (node.isNumeric())
            return node.asDouble();

        return defaultValue;
    }

    template<>
    double4 read<double4>(const Json::Value& node, const double4& defaultValue)
    {
        if (node.isArray() && node.size() == 4)
            return double4(node[0].asDouble(), node[1].asDouble(), node[2].asDouble(), node[3].asDouble());

        if (node.isNumeric())
            return node.asDouble();

        return defaultValue;
    }

    template <>
    void write<std::string>(Json::Value& node, const std::string& value)
    {
        node = value;
    }

    template <>
    void write<int>(Json::Value& node, const int& value)
    {
        node = value;
    }

    template <>
    void write<int2>(Json::Value& node, const dm::int2& value)
    {
        node.append(value.x);
        node.append(value.y);
    }

    template <>
    void write<int3>(Json::Value& node, const dm::int3& value)
    {
        node.append(value.x);
        node.append(value.y);
        node.append(value.z);
    }

    template <>
    void write<int4>(Json::Value& node, const dm::int4& value)
    {
        node.append(value.x);
        node.append(value.y);
        node.append(value.z);
        node.append(value.w);
    }

    template <>
    void write<uint>(Json::Value& node, const uint& value)
    {
        node = value;
    }

    template <>
    void write<uint2>(Json::Value& node, const dm::uint2& value)
    {
        node.append(value.x);
        node.append(value.y);
    }

    template <>
    void write<uint3>(Json::Value& node, const dm::uint3& value)
    {
        node.append(value.x);
        node.append(value.y);
        node.append(value.z);
    }

    template <>
    void write<uint4>(Json::Value& node, const dm::uint4& value)
    {
        node.append(value.x);
        node.append(value.y);
        node.append(value.z);
        node.append(value.w);
    }

    template <>
    void write<bool>(Json::Value& node, const bool& value)
    {
        node = value;
    }

    template <>
    void write<float>(Json::Value& node, const float& value)
    {
        node = double(value);
    }

    template <>
    void write<float2>(Json::Value& node, const dm::float2& value)
    {
        node.append(double(value.x));
        node.append(double(value.y));
    }

    template <>
    void write<float3>(Json::Value& node, const dm::float3& value)
    {
        node.append(double(value.x));
        node.append(double(value.y));
        node.append(double(value.z));
    }

    template <>
    void write<float4>(Json::Value& node, const dm::float4& value)
    {
        node.append(double(value.x));
        node.append(double(value.y));
        node.append(double(value.z));
        node.append(double(value.w));
    }

    template <>
    void write<double>(Json::Value& node, const double& value)
    {
        node = double(value);
    }

    template <>
    void write<double2>(Json::Value& node, const dm::double2& value)
    {
        node.append(double(value.x));
        node.append(double(value.y));
    }

    template <>
    void write<double3>(Json::Value& node, const dm::double3& value)
    {
        node.append(double(value.x));
        node.append(double(value.y));
        node.append(double(value.z));
    }

    template <>
    void write<double4>(Json::Value& node, const dm::double4& value)
    {
        node.append(double(value.x));
        node.append(double(value.y));
        node.append(double(value.z));
        node.append(double(value.w));
    }
}

void operator<<(Json::Value& node, const char* src)
{
    node = src;
}

// --- File/string-based JSON I/O utilities (moved from SampleCommon) ---

namespace caustica::json
{

bool SaveToFile(const std::filesystem::path& filePath, const Json::Value& rootNode)
{
    std::ofstream outFile(filePath, std::ios::trunc);
    if (!outFile.is_open())
    {
        caustica::error("Error saving json to file '%s'", filePath.string().c_str());
        return false;
    }
    Json::StreamWriterBuilder builder;
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    writer->write(rootNode, &outFile);
    outFile.close();
    return true;
}

bool loadFromFile(const std::filesystem::path& filePath, Json::Value& outRootNode)
{
    std::ifstream inFile;
    inFile.open(filePath);
    if (!inFile.is_open())
    {
        caustica::warning("Error loading json file '%s'", filePath.string().c_str());
        return false;
    }
    try { inFile >> outRootNode; }
    catch (const Json::RuntimeError& e)
    { caustica::warning("Json::RuntimeError: %s", e.what()); return false; }
    catch (const std::exception& e)
    { caustica::warning("std::exception: %s", e.what()); return false; }
    return true;
}

std::string ToString(const Json::Value& rootNode)
{
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, rootNode);
}

bool FromString(const std::string& jsonData, Json::Value& outRootNode)
{
    Json::CharReaderBuilder readerBuilder;
    std::string errs;
    std::istringstream iss(jsonData);
    return Json::parseFromStream(readerBuilder, iss, &outRootNode, &errs);
}

std::vector<std::string> ReadStringArray(const Json::Value& arr)
{
    std::vector<std::string> result;
    if (arr.isArray())
    {
        result.reserve(arr.size());
        for (const auto& v : arr)
            result.push_back(v.asString());
    }
    return result;
}

} // namespace caustica::json
