#include <platform/file_dialog.h>
#include <core/log.h>
#include <core/string_utils.h>

#include <filesystem>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#include <cstdio>
#include <climits>
#else
#include <Windows.h>
#include <ShlObj.h>
#define PATH_MAX MAX_PATH
#endif

namespace fs = std::filesystem;

namespace caustica
{

bool FileDialog(bool bOpen, const char* pFilters, std::string& fileName, const char* initialDir)
{
#ifdef _WIN32
    OPENFILENAMEA ofn;
    CHAR chars[PATH_MAX] = "";
    ZeroMemory(&ofn, sizeof(ofn));

    if (!fileName.empty() && fileName.size() < ARRAYSIZE(chars))
        strncpy_s(chars, fileName.c_str(), _TRUNCATE);

    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = GetForegroundWindow();
    ofn.lpstrFilter = pFilters;
    ofn.lpstrFile = chars;
    ofn.nMaxFile = ARRAYSIZE(chars);
    ofn.lpstrInitialDir = (initialDir && initialDir[0] != '\0') ? initialDir : nullptr;
    ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (bOpen)
        ofn.Flags |= OFN_FILEMUSTEXIST;
    ofn.lpstrDefExt = "";

    const BOOL ok = bOpen ? GetOpenFileNameA(&ofn) : GetSaveFileNameA(&ofn);
    // Common file dialogs often break freopen'd CONOUT$ bindings; restore before
    // any subsequent info()/error() so Open Scene logs show up in the F1 console.
    refreshNativeConsoleStdio();
    if (ok)
    {
        fileName = chars;
        return true;
    }
    return false;
#else
    (void)initialDir;
    char chars[PATH_MAX] = { 0 };
    std::string app = "zenity --file-selection";
    if (!bOpen)
        app += " --save --confirm-overwrite";

    FILE* f = popen(app.c_str(), "r");
    if (!f)
    {
        error("Error executing zenity.");
        return false;
    }
    const bool gotname = (nullptr != fgets(chars, PATH_MAX, f));
    pclose(f);

    if (gotname && chars[0] != '\0')
    {
        fileName = chars;
        string_utils::trim(fileName);
        return true;
    }
    return false;
#endif
}

#ifdef _WIN32
static INT CALLBACK BrowseCallbackProc(HWND hwnd, UINT uMsg, LPARAM lp, LPARAM pData)
{
    if (uMsg == BFFM_INITIALIZED && pData)
        SendMessage(hwnd, BFFM_SETSELECTION, TRUE, pData);
    return 0;
}
#endif

bool FolderDialog(const char* pTitle, const char* pDefaultFolder, std::string& outFolderName)
{
#ifdef _WIN32
    BROWSEINFO browseInfo{};
    browseInfo.hwndOwner = GetForegroundWindow();
    browseInfo.lpszTitle = pTitle;
    browseInfo.ulFlags = BIF_USENEWUI | BIF_NONEWFOLDERBUTTON;
    if (pDefaultFolder)
    {
        browseInfo.lpfn = BrowseCallbackProc;
        browseInfo.lParam = reinterpret_cast<LPARAM>(pDefaultFolder);
    }
    if (PIDLIST_ABSOLUTE pIdList = SHBrowseForFolder(&browseInfo))
    {
        char path[MAX_PATH];
        if (SHGetPathFromIDList(pIdList, path) && fs::is_directory(path))
        {
            outFolderName = path;
            return true;
        }
    }
    return false;
#else
    char chars[PATH_MAX] = { 0 };

    std::stringstream ss;
    ss << "zenity --file-selection --directory";
    if (pTitle)
        ss << " --title \"" << pTitle << "\"";
    if (pDefaultFolder)
        ss << " --filename=\"" << pDefaultFolder << "\"";

    FILE* f = popen(ss.str().c_str(), "r");
    if (!f)
    {
        error("Error executing zenity.");
        return false;
    }
    const bool gotname = (nullptr != fgets(chars, PATH_MAX, f));
    pclose(f);

    if (gotname && chars[0] != '\0')
    {
        std::string path = chars;
        string_utils::trim(path);
        if (fs::is_directory(path))
        {
            outFolderName = path;
            return true;
        }
    }
    return false;
#endif
}

} // namespace caustica
