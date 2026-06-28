#pragma once

#include <string>

namespace caustica
{

bool FileDialog(bool open, const char* filters, std::string& fileName);
bool FolderDialog(const char* title, const char* defaultFolder, std::string& outFolderName);

} // namespace caustica
