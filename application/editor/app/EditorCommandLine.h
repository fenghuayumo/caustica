#pragma once

#include <string>

namespace caustica::editor
{

// Editor / demo-only flags. Engine CommandLineOptions stays host-agnostic.
struct EditorCommandLine
{
    std::string propShowTags;
    std::string propCameraAttach;
    std::string pythonScript;
    std::string pythonExpr;

    // Parses editor flags from the same argv as CommandLineOptions.
    // Unknown options are ignored so engine flags can coexist.
    bool parse(int argc, char const* const* argv);
};

} // namespace caustica::editor
