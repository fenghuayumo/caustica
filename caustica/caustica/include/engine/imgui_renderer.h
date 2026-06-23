#pragma once

#include <engine/DeviceManager.h>
#include <engine/imgui_nvrhi.h>

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

        void CreateScaledFont(float displayScale);
        void ReleaseScaledFont();
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
        // in the first call to ImGui_Renderer::Animate(...). After that, use GetScaledFont()
        // to test if the font is valid.
        bool HasFontData() const { return m_data != nullptr; }

        // Returns the ImFont object that can be used with ImGUI.
        // Note that the returned pointer is transient and will change when screen DPI changes,
        // or when new fonts are loaded. Do not cache the returned value between frames.
        // The returned pointer may be NULL if the font has failed to load, which is OK for ImGUI's PushFont(...)
        ImFont* GetScaledFont() { return m_imFont; }
    };

    // base class to build IRenderPass-based UIs using ImGui through NVRHI
    class ImGui_Renderer : public IRenderPass
    {
    protected:

        std::unique_ptr<ImGui_NVRHI> imgui_nvrhi;

        std::vector<std::shared_ptr<RegisteredFont>> m_fonts;

        std::shared_ptr<RegisteredFont> m_defaultFont;

        bool m_supportExplicitDisplayScaling;
        bool m_imguiFrameOpened = false;

    public:
        ImGui_Renderer(DeviceManager *devManager);
        ~ImGui_Renderer();
        bool Init(std::shared_ptr<caustica::ShaderFactory> shaderFactory);

        // Loads a TTF font from file and registers it with the ImGui_Renderer.
        // To use the font with ImGUI at runtime, call RegisteredFont::GetScaledFont().
        std::shared_ptr<RegisteredFont> CreateFontFromFile(caustica::IFileSystem& fs,
            std::filesystem::path const& fontFile, float fontSize);

        // Registers a TTF font stored in memory with the ImGui_Renderer.
        // To use the font with ImGUI at runtime, call RegisteredFont::GetScaledFont().
        std::shared_ptr<RegisteredFont> CreateFontFromMemory(void const* pData, size_t size, float fontSize);
        
        // Identical to CreateFontFromMemory except that the data is compressed
        // using 'binary_to_compressed_c.cpp' in imgui.
        std::shared_ptr<RegisteredFont> CreateFontFromMemoryCompressed(void const* pData, size_t size, float fontSize);

        // Returns the default font.
        std::shared_ptr<RegisteredFont> GetDefaultFont() { return m_defaultFont; }

        virtual bool KeyboardUpdate(int key, int scancode, int action, int mods) override;
        virtual bool KeyboardCharInput(unsigned int unicode, int mods) override;
        virtual bool MousePosUpdate(double xpos, double ypos) override;
        virtual bool MouseScrollUpdate(double xoffset, double yoffset) override;
        virtual bool MouseButtonUpdate(int button, int action, int mods) override;
        virtual void Animate(float elapsedTimeSeconds) override;
        virtual void Render(nvrhi::IFramebuffer* framebuffer) override;
        virtual void BackBufferResizing() override;
        virtual void DisplayScaleChanged(float scaleX, float scaleY) override;
        virtual bool ShouldAnimateUnfocused() override { return true; }
        virtual bool SupportsDepthBuffer() override { return false; }

    protected:
        // creates the UI in ImGui, updates internal UI state
        virtual void buildUI(void) = 0;

        void BeginFullScreenWindow();
        void DrawScreenCenteredText(const char* text);
        void EndFullScreenWindow();
    private:
        std::shared_ptr<RegisteredFont> CreateFontFromMemoryInternal(void const* pData, size_t size,
            bool compressed, float fontSize);
    };
}
