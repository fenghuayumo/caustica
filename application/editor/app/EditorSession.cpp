#include "EditorSession.h"

namespace caustica::editor
{

EditorSession::EditorSession()
    : sceneEditor(cmdLine, editorUiData, sessionDiagnostics)
{
}

void installEditorLogFilter(EditorSession& /*session*/)
{
    Callback defaultCallback = getCallback();
    setCallback([defaultCallback](Severity severity, const char* message) {
        if (severity == Severity::Error)
        {
            std::string msg(message);
            if (msg.find("Don't know the size") != std::string::npos)
                severity = Severity::Warning;
            if (msg.find("dlss_gEntry.cpp") != std::string::npos)
            {
                if (msg.find("Unable to find DRS context") != std::string::npos
                    || msg.find("NGX indicates DLSS-G is not available") != std::string::npos)
                    severity = Severity::Warning;
            }
            if (msg.find("Missing NGX context") != std::string::npos
                || msg.find("Unable to find NGX ") != std::string::npos
                || msg.find("NvAPI_D3D_Sleep") != std::string::npos)
                severity = Severity::Warning;
        }

        if (defaultCallback)
            defaultCallback(severity, message);
    });
}

} // namespace caustica::editor
