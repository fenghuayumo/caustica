#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace caustica
{

// Recursively create directories if they don't exist.
// Returns false if any path component exists as a file (not a directory).
bool ensureDirectoryExists(const std::filesystem::path& dir);

// Enumerate files in 'folder' matching 'wildcard' (* and ? patterns, case-insensitive).
std::vector<std::filesystem::path> enumerateFilesWithWildcard(
    const std::filesystem::path& folder,
    const std::string& wildcard);

// read entire contents of a text file into a string.
std::string stringLoadFromFile(const std::filesystem::path& filePath);

// Returns the latest write time of any file recursively under 'directory'.
std::optional<std::filesystem::file_time_type> getLatestModifiedTimeDirectoryRecursive(
    const std::filesystem::path& directory);

// Returns the last write time of a single file.
std::optional<std::filesystem::file_time_type> getFileModifiedTime(
    const std::filesystem::path& file);

} // namespace caustica
