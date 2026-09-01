#include "EditorCommandLine.h"

#include <core/log.h>
#include <cxxopts.hpp>

bool caustica::editor::EditorCommandLine::parse(int argc, char const* const* argv)
{
    using namespace cxxopts;

    try
    {
        int parseArgc = argc;
        Options options("caustica-editor");
        options.allow_unrecognised_options();
        options.add_options()
            ("propShowTags", "Filter props by comma-separated tags", value(propShowTags))
            ("propCameraAttach", "Attach camera to named prop", value(propCameraAttach))
            ("pythonScript", "Path to Python script to run after scene load", value(pythonScript))
            ("pythonExpr", "Inline Python expression to run after scene load", value(pythonExpr));

        options.parse(parseArgc, argv);
        return true;
    }
    catch (const exceptions::exception& e)
    {
        caustica::error("%s", e.what());
        return false;
    }
}
