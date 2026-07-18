#pragma once

#include <backend/GpuDevice.h>
#include <backend/renderContext.h>
#include <imgui/imgui_nvrhi.h>

#include <filesystem>
#include <memory>
#include <optional>

namespace caustica
{
    class IBlob;
    class IFileSystem;
}

namespace caustica
{
    class ShaderFactory;
}

namespace caustica
{
    class RegisteredFont
    {
    protected:
        friend class ImGui_Renderer;

        std::shared_ptr<caustica::IBlob> m_data;
        bool const m_isDefault;
        bool const m_isCompressed;
        float const m_sizeAtDefaultScale;
        ImFont* m_imFont = nullptr;

        void createScaledFont(float displayScale);
        void releaseScaledFont();
    public:
        // Creates an invalid font that will not add any ImGUI fonts
        RegisteredFont()
            : m_isDefault(false)
            , m_isCompressed(false)
            , m_sizeAtDefaultScale(0.f)
        { }

        // Creates a default font with the given size
        RegisteredFont(float size)
            : m_isDefault(true)
            , m_isCompressed(false)
            , m_sizeAtDefaultScale(size)
        { }

        // Creates a custom font
        RegisteredFont(std::shared_ptr<caustica::IBlob> data, bool isCompressed, float size)
            : m_data(data)
            , m_isDefault(false)
            , m_isCompressed(isCompressed)
            , m_sizeAtDefaultScale(size)
        { }

        // Returns true if the custom font data has been successfully loaded.
        // This doesn't necessarily mean that the font data is valid: the actual font object is only created
        // in the first call to ImGui_Renderer::animate(...). After that, use getScaledFont()
        // to test if the font is valid.
        bool hasFontData() const { return m_data != nullptr; }

        // Returns the ImFont object that can be used with ImGUI.
        // Note that the returned pointer is transient and will change when screen DPI changes,
        // or when new fonts are loaded. Do not cache the returned value between frames.
        // The returned pointer may be NULL if the font has failed to load, which is OK for ImGUI's PushFont(...)
        ImFont* getScaledFont() { return m_imFont; }
    };

    // Base class for ImGui UIs rendered through NVRHI.
    class ImGui_Renderer : public renderContext
    {
    protected:

        std::unique_ptr<ImGui_NVRHI> imgui_nvrhi;

        std::vector<std::shared_ptr<RegisteredFont>> m_fonts;

        std::shared_ptr<RegisteredFont> m_defaultFont;

        bool m_supportExplicitDisplayScaling;
        bool m_imguiFrameOpened = false;
        bool m_pendingDisplayScaleChanged = false;
        float m_pendingDisplayScaleX = 1.0f;
        float m_pendingDisplayScaleY = 1.0f;

        void prepareImGuiFrame(float elapsedTimeSeconds);

    public:
        ImGui_Renderer(GpuDevice *devManager);
        ~ImGui_Renderer();
        bool init(std::shared_ptr<caustica::ShaderFactory> shaderFactory);

        // Loads a TTF font from file and registers it with the ImGui_Renderer.
        // To use the font with ImGUI at runtime, call RegisteredFont::getScaledFont().
        std::shared_ptr<RegisteredFont> createFontFromFile(caustica::IFileSystem& fs,
            std::filesystem::path const& fontFile, float fontSize);

        // Registers a TTF font stored in memory with the ImGui_Renderer.
        // To use the font with ImGUI at runtime, call RegisteredFont::getScaledFont().
        std::shared_ptr<RegisteredFont> createFontFromMemory(void const* pData, size_t size, float fontSize);
        
        // Identical to createFontFromMemory except that the data is compressed
        // using 'binary_to_compressed_c.cpp' in imgui.
        std::shared_ptr<RegisteredFont> createFontFromMemoryCompressed(void const* pData, size_t size, float fontSize);

        // Returns the default font.
        std::shared_ptr<RegisteredFont> getDefaultFont() { return m_defaultFont; }

        // Promote a loaded font to the UI default (Fonts[0] / PushFont target).
        // Removes the placeholder Proggy font if it is still installed.
        void setDefaultFont(std::shared_ptr<RegisteredFont> font);

        virtual void animate(float elapsedTimeSeconds);
        virtual void render(nvrhi::IFramebuffer* framebuffer);
        virtual void backBufferResizing();
        virtual void backBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) {}
        virtual void displayScaleChanged(float scaleX, float scaleY);
        virtual bool shouldAnimateUnfocused() { return true; }
        virtual bool supportsDepthBuffer() { return false; }

    protected:
        // creates the UI in ImGui, updates internal UI state
        virtual void buildUI(void) = 0;

        void beginFullScreenWindow();
        void drawScreenCenteredText(const char* text);
        void endFullScreenWindow();
    private:
        std::shared_ptr<RegisteredFont> createFontFromMemoryInternal(void const* pData, size_t size,
            bool compressed, float fontSize);
    };

    // Forward GLFW-style input to ImGui (call from application event handlers).
    void imGuiForwardKeyboard(int glfwKey, int action, int scancode);
    void imGuiForwardInputCharacter(unsigned int codepoint);
}
