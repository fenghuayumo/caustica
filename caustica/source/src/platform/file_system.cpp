/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "platform/file_system.h"

#include <fstream>
#include <sstream>
#include <iostream>

namespace caustica
{

void FileSystem::init()
{
    // Platform-specific init can go here (e.g. setting up asset root)
}

bool FileSystem::fileExists(const std::filesystem::path& path) const
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec);
}

bool FileSystem::folderExists(const std::filesystem::path& path) const
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_directory(path, ec);
}

bool FileSystem::createDirectory(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::create_directories(path, ec);
}

void FileSystem::absolutePathToFileSystem(const std::string& relative, std::string& outAbsolute) const
{
    std::filesystem::path base(m_AssetRoot);
    std::filesystem::path rel(relative);
    outAbsolute = (base / rel).string();
}

std::string FileSystem::readTextFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::in);
    if (!file.is_open())
        return {};

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool FileSystem::writeTextFile(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file.is_open())
        return false;

    file << content;
    return true;
}

std::vector<uint8_t> FileSystem::readBinaryFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return {};

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

bool FileSystem::writeBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& data)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;

    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return true;
}

std::vector<std::filesystem::path> FileSystem::listFiles(const std::filesystem::path& directory,
                                                          const std::string& extension) const
{
    std::vector<std::filesystem::path> result;
    std::error_code ec;

    if (!std::filesystem::exists(directory, ec))
        return result;

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        if (entry.is_regular_file(ec))
        {
            if (extension.empty() || entry.path().extension() == extension)
                result.push_back(entry.path());
        }
    }
    return result;
}

std::vector<std::filesystem::path> FileSystem::listDirectories(const std::filesystem::path& directory) const
{
    std::vector<std::filesystem::path> result;
    std::error_code ec;

    if (!std::filesystem::exists(directory, ec))
        return result;

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        if (entry.is_directory(ec))
            result.push_back(entry.path());
    }
    return result;
}

// File-watch stubs — can be extended with platform-specific implementations
bool FileSystem::watchFile(const std::filesystem::path& /*path*/, FileChangeCallback /*callback*/)
{
    return false; // Not yet implemented
}

void FileSystem::stopWatching(const std::filesystem::path& /*path*/)
{
    // Not yet implemented
}

void FileSystem::pollWatches()
{
    // Not yet implemented
}

} // namespace caustica
