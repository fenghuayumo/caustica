#pragma once

#include <memory>
#include <filesystem>

#include <core/command_line.h>

namespace caustica
{
class ShaderFactory;
class IFileSystem;
} // namespace caustica

namespace caustica {
class ImGui_Renderer;
}

namespace caustica::editor
{

class EditorUIData;

// =============================================================================
// ImGuiManager — ImGui lifecycle helper: fonts, context config, input setup.
//
// Extracted from EditorUI constructor.  Handles ImGui initialization concerns
// so EditorUI can focus on panel definitions.
// =============================================================================
class ImGuiManager
{
public:
    ImGuiManager(EditorUIData& uiData, const CommandLineOptions& cmdLine,
                 bool nvapiSERSupported);
    ~ImGuiManager();

    // --- Font loading ---
    // Loads the default DroidSans-Mono font from assets/fonts/.
    void loadDefaultFont(caustica::ImGui_Renderer& renderer, const std::filesystem::path& assetsPath);

    // --- Extension detection ---
    // Configures DX/NVAPI hit object extensions based on device capabilities.
    // Called once during first frame.
    void configureExtensions(int graphicsAPI);

    // --- Command-line init ---
    // Applies command-line overrides to UI data. Called once.
    void applyCommandLineDefaults();

private:
    EditorUIData&             m_uiData;
    const CommandLineOptions& m_cmdLine;
    bool                      m_nvapiSERSupported;
    bool                      m_extensionsConfigured = false;
    bool                      m_cmdLineApplied = false;
};

} // namespace caustica::editor
