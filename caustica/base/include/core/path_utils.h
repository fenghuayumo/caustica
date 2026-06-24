#pragma once

#include <filesystem>

namespace caustica
{

class IFileSystem;

// Returns the directory containing the current executable.
std::filesystem::path GetDirectoryWithExecutable();

// Searches upward from 'startPath' for a directory 'dirname'.
std::filesystem::path FindDirectory(IFileSystem& fs,
    const std::filesystem::path& startPath,
    const std::filesystem::path& dirname,
    int maxDepth = 5);

// Searches upward from 'startPath' for a file with 'relativeFilePath'.
std::filesystem::path FindDirectoryWithFile(IFileSystem& fs,
    const std::filesystem::path& startPath,
    const std::filesystem::path& relativeFilePath,
    int maxDepth = 5);

} // namespace caustica
