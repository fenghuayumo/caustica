#include <core/file_utils.h>
#include <core/log.h>

#include <fstream>
#include <regex>
#include <sstream>

namespace caustica
{

bool EnsureDirectoryExists(const std::filesystem::path& dir)
{
    if (std::filesystem::exists(dir))
    {
        if (!std::filesystem::is_directory(dir))
        {
            caustica::error("Attempting ensure directory exists, but path '%s' is a file", dir.string().c_str());
            return false;
        }
        return true;
    }

    std::filesystem::path pathSoFar;
    for (const auto& part : std::filesystem::absolute(dir))
    {
        pathSoFar /= part;
        if (std::filesystem::exists(pathSoFar))
        {
            if (!std::filesystem::is_directory(pathSoFar))
            {
                caustica::error("Attempting ensure directory exists, but path '%s' is a file", pathSoFar.string().c_str());
                return false;
            }
        }
        else
        {
            if (!std::filesystem::create_directory(pathSoFar))
            {
                caustica::error("Attempting ensure directory exists, but unable to create or access '%s' directory", pathSoFar.string().c_str());
                return false;
            }
        }
    }
    return true;
}

std::vector<std::filesystem::path> EnumerateFilesWithWildcard(
    const std::filesystem::path& folder, const std::string& wildcard)
{
    // Convert wildcard to regex: ? -> ., * -> .*
    std::string regexPattern = "^";
    for (char c : wildcard)
    {
        if (c == '*')      regexPattern += ".*";
        else if (c == '?') regexPattern += '.';
        else if (c == '.') regexPattern += "\\.";
        else               regexPattern += c;
    }
    regexPattern += "$";
    std::regex pattern(regexPattern, std::regex::icase);

    std::vector<std::filesystem::path> result;
    if (!std::filesystem::exists(folder) || !std::filesystem::is_directory(folder))
        return result;

    for (const auto& entry : std::filesystem::directory_iterator(folder))
    {
        if (entry.is_regular_file())
        {
            if (std::regex_match(entry.path().filename().string(), pattern))
                result.push_back(entry.path());
        }
    }
    return result;
}

std::string StringLoadFromFile(const std::filesystem::path& filePath)
{
    std::ifstream file(filePath);
    if (!file)
        return "";

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::optional<std::filesystem::file_time_type> GetLatestModifiedTimeDirectoryRecursive(
    const std::filesystem::path& directory)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    if (!fs::exists(directory, ec) || !fs::is_directory(directory, ec))
        return std::nullopt;

    std::optional<fs::file_time_type> latest_time;

    fs::recursive_directory_iterator dir_it(directory, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end_it;

    while (dir_it != end_it && !ec)
    {
        const auto& entry = *dir_it;

        if (entry.is_regular_file(ec))
        {
            auto ftime = entry.last_write_time(ec);
            if (!ec)
            {
                if (!latest_time || ftime > *latest_time)
                    latest_time = ftime;
            }
        }

        dir_it.increment(ec);
    }

    if (ec)
    {
        caustica::warning("Invalid directory or access error for '%s', error: %s",
            directory.string().c_str(), ec.message().c_str());
        return std::nullopt;
    }

    return latest_time;
}

std::optional<std::filesystem::file_time_type> GetFileModifiedTime(
    const std::filesystem::path& file)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    if (!fs::exists(file, ec) || !fs::is_regular_file(file, ec))
        return std::nullopt;

    auto ftime = fs::last_write_time(file, ec);
    if (ec)
    {
        caustica::warning("Failed to get last write time for file '%s', error: %s",
            file.string().c_str(), ec.message().c_str());
        return std::nullopt;
    }

    return ftime;
}

} // namespace caustica
