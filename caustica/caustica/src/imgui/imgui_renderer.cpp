#include <imgui/imgui_renderer.h>
#include <core/vfs/VFS.h>

#include <algorithm>

using namespace caustica;

ImGui_Renderer::ImGui_Renderer(GpuDevice *devManager)
    : renderContext(devManager)
    , m_supportExplicitDisplayScaling(devManager->supportsExplicitDisplayScaling())
{
    ImGui::CreateContext();

    m_defaultFont = std::make_shared<RegisteredFont>(13.f);
    m_fonts.push_back(m_defaultFont);
}

ImGui_Renderer::~ImGui_Renderer()
{
    ImGui::DestroyContext();
}

// Key conversion function - copied from imgui_impl_glfw.cpp
static ImGuiKey ImGui_ImplGlfw_KeyToImGuiKey(int keycode)
{
    switch (keycode)
    {
        case GLFW_KEY_TAB: return ImGuiKey_Tab;
        case GLFW_KEY_LEFT: return ImGuiKey_LeftArrow;
        case GLFW_KEY_RIGHT: return ImGuiKey_RightArrow;
        case GLFW_KEY_UP: return ImGuiKey_UpArrow;
        case GLFW_KEY_DOWN: return ImGuiKey_DownArrow;
        case GLFW_KEY_PAGE_UP: return ImGuiKey_PageUp;
        case GLFW_KEY_PAGE_DOWN: return ImGuiKey_PageDown;
        case GLFW_KEY_HOME: return ImGuiKey_Home;
        case GLFW_KEY_END: return ImGuiKey_End;
        case GLFW_KEY_INSERT: return ImGuiKey_Insert;
        case GLFW_KEY_DELETE: return ImGuiKey_Delete;
        case GLFW_KEY_BACKSPACE: return ImGuiKey_Backspace;
        case GLFW_KEY_SPACE: return ImGuiKey_Space;
        case GLFW_KEY_ENTER: return ImGuiKey_Enter;
        case GLFW_KEY_ESCAPE: return ImGuiKey_Escape;
        case GLFW_KEY_APOSTROPHE: return ImGuiKey_Apostrophe;
        case GLFW_KEY_COMMA: return ImGuiKey_Comma;
        case GLFW_KEY_MINUS: return ImGuiKey_Minus;
        case GLFW_KEY_PERIOD: return ImGuiKey_Period;
        case GLFW_KEY_SLASH: return ImGuiKey_Slash;
        case GLFW_KEY_SEMICOLON: return ImGuiKey_Semicolon;
        case GLFW_KEY_EQUAL: return ImGuiKey_Equal;
        case GLFW_KEY_LEFT_BRACKET: return ImGuiKey_LeftBracket;
        case GLFW_KEY_BACKSLASH: return ImGuiKey_Backslash;
        case GLFW_KEY_WORLD_1: return ImGuiKey_Oem102;
        case GLFW_KEY_WORLD_2: return ImGuiKey_Oem102;
        case GLFW_KEY_RIGHT_BRACKET: return ImGuiKey_RightBracket;
        case GLFW_KEY_GRAVE_ACCENT: return ImGuiKey_GraveAccent;
        case GLFW_KEY_CAPS_LOCK: return ImGuiKey_CapsLock;
        case GLFW_KEY_SCROLL_LOCK: return ImGuiKey_ScrollLock;
        case GLFW_KEY_NUM_LOCK: return ImGuiKey_NumLock;
        case GLFW_KEY_PRINT_SCREEN: return ImGuiKey_PrintScreen;
        case GLFW_KEY_PAUSE: return ImGuiKey_Pause;
        case GLFW_KEY_KP_0: return ImGuiKey_Keypad0;
        case GLFW_KEY_KP_1: return ImGuiKey_Keypad1;
        case GLFW_KEY_KP_2: return ImGuiKey_Keypad2;
        case GLFW_KEY_KP_3: return ImGuiKey_Keypad3;
        case GLFW_KEY_KP_4: return ImGuiKey_Keypad4;
        case GLFW_KEY_KP_5: return ImGuiKey_Keypad5;
        case GLFW_KEY_KP_6: return ImGuiKey_Keypad6;
        case GLFW_KEY_KP_7: return ImGuiKey_Keypad7;
        case GLFW_KEY_KP_8: return ImGuiKey_Keypad8;
        case GLFW_KEY_KP_9: return ImGuiKey_Keypad9;
        case GLFW_KEY_KP_DECIMAL: return ImGuiKey_KeypadDecimal;
        case GLFW_KEY_KP_DIVIDE: return ImGuiKey_KeypadDivide;
        case GLFW_KEY_KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
        case GLFW_KEY_KP_SUBTRACT: return ImGuiKey_KeypadSubtract;
        case GLFW_KEY_KP_ADD: return ImGuiKey_KeypadAdd;
        case GLFW_KEY_KP_ENTER: return ImGuiKey_KeypadEnter;
        case GLFW_KEY_KP_EQUAL: return ImGuiKey_KeypadEqual;
        case GLFW_KEY_LEFT_SHIFT: return ImGuiKey_LeftShift;
        case GLFW_KEY_LEFT_CONTROL: return ImGuiKey_LeftCtrl;
        case GLFW_KEY_LEFT_ALT: return ImGuiKey_LeftAlt;
        case GLFW_KEY_LEFT_SUPER: return ImGuiKey_LeftSuper;
        case GLFW_KEY_RIGHT_SHIFT: return ImGuiKey_RightShift;
        case GLFW_KEY_RIGHT_CONTROL: return ImGuiKey_RightCtrl;
        case GLFW_KEY_RIGHT_ALT: return ImGuiKey_RightAlt;
        case GLFW_KEY_RIGHT_SUPER: return ImGuiKey_RightSuper;
        case GLFW_KEY_MENU: return ImGuiKey_Menu;
        case GLFW_KEY_0: return ImGuiKey_0;
        case GLFW_KEY_1: return ImGuiKey_1;
        case GLFW_KEY_2: return ImGuiKey_2;
        case GLFW_KEY_3: return ImGuiKey_3;
        case GLFW_KEY_4: return ImGuiKey_4;
        case GLFW_KEY_5: return ImGuiKey_5;
        case GLFW_KEY_6: return ImGuiKey_6;
        case GLFW_KEY_7: return ImGuiKey_7;
        case GLFW_KEY_8: return ImGuiKey_8;
        case GLFW_KEY_9: return ImGuiKey_9;
        case GLFW_KEY_A: return ImGuiKey_A;
        case GLFW_KEY_B: return ImGuiKey_B;
        case GLFW_KEY_C: return ImGuiKey_C;
        case GLFW_KEY_D: return ImGuiKey_D;
        case GLFW_KEY_E: return ImGuiKey_E;
        case GLFW_KEY_F: return ImGuiKey_F;
        case GLFW_KEY_G: return ImGuiKey_G;
        case GLFW_KEY_H: return ImGuiKey_H;
        case GLFW_KEY_I: return ImGuiKey_I;
        case GLFW_KEY_J: return ImGuiKey_J;
        case GLFW_KEY_K: return ImGuiKey_K;
        case GLFW_KEY_L: return ImGuiKey_L;
        case GLFW_KEY_M: return ImGuiKey_M;
        case GLFW_KEY_N: return ImGuiKey_N;
        case GLFW_KEY_O: return ImGuiKey_O;
        case GLFW_KEY_P: return ImGuiKey_P;
        case GLFW_KEY_Q: return ImGuiKey_Q;
        case GLFW_KEY_R: return ImGuiKey_R;
        case GLFW_KEY_S: return ImGuiKey_S;
        case GLFW_KEY_T: return ImGuiKey_T;
        case GLFW_KEY_U: return ImGuiKey_U;
        case GLFW_KEY_V: return ImGuiKey_V;
        case GLFW_KEY_W: return ImGuiKey_W;
        case GLFW_KEY_X: return ImGuiKey_X;
        case GLFW_KEY_Y: return ImGuiKey_Y;
        case GLFW_KEY_Z: return ImGuiKey_Z;
        case GLFW_KEY_F1: return ImGuiKey_F1;
        case GLFW_KEY_F2: return ImGuiKey_F2;
        case GLFW_KEY_F3: return ImGuiKey_F3;
        case GLFW_KEY_F4: return ImGuiKey_F4;
        case GLFW_KEY_F5: return ImGuiKey_F5;
        case GLFW_KEY_F6: return ImGuiKey_F6;
        case GLFW_KEY_F7: return ImGuiKey_F7;
        case GLFW_KEY_F8: return ImGuiKey_F8;
        case GLFW_KEY_F9: return ImGuiKey_F9;
        case GLFW_KEY_F10: return ImGuiKey_F10;
        case GLFW_KEY_F11: return ImGuiKey_F11;
        case GLFW_KEY_F12: return ImGuiKey_F12;
        case GLFW_KEY_F13: return ImGuiKey_F13;
        case GLFW_KEY_F14: return ImGuiKey_F14;
        case GLFW_KEY_F15: return ImGuiKey_F15;
        case GLFW_KEY_F16: return ImGuiKey_F16;
        case GLFW_KEY_F17: return ImGuiKey_F17;
        case GLFW_KEY_F18: return ImGuiKey_F18;
        case GLFW_KEY_F19: return ImGuiKey_F19;
        case GLFW_KEY_F20: return ImGuiKey_F20;
        case GLFW_KEY_F21: return ImGuiKey_F21;
        case GLFW_KEY_F22: return ImGuiKey_F22;
        case GLFW_KEY_F23: return ImGuiKey_F23;
        case GLFW_KEY_F24: return ImGuiKey_F24;
        default: return ImGuiKey_None;
    }
}

// Also copied from imgui_impl_glfw.cpp
// X11 does not include current pressed/released modifier key in 'mods' flags submitted by GLFW
// See https://github.com/ocornut/imgui/issues/6034 and https://github.com/glfw/glfw/issues/1630
static void ImGui_ImplGlfw_UpdateKeyModifiers(ImGuiIO& io, GLFWwindow* window)
{
    io.AddKeyEvent(ImGuiMod_Ctrl,  (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) || (glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS));
    io.AddKeyEvent(ImGuiMod_Shift, (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT)   == GLFW_PRESS) || (glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT)   == GLFW_PRESS));
    io.AddKeyEvent(ImGuiMod_Alt,   (glfwGetKey(window, GLFW_KEY_LEFT_ALT)     == GLFW_PRESS) || (glfwGetKey(window, GLFW_KEY_RIGHT_ALT)     == GLFW_PRESS));
    io.AddKeyEvent(ImGuiMod_Super, (glfwGetKey(window, GLFW_KEY_LEFT_SUPER)   == GLFW_PRESS) || (glfwGetKey(window, GLFW_KEY_RIGHT_SUPER)   == GLFW_PRESS));
}

bool ImGui_Renderer::init(std::shared_ptr<ShaderFactory> shaderFactory)
{
    m_imguiRhi = std::make_unique<ImGui_RHI>();
    return m_imguiRhi->init(getDevice(), shaderFactory);
}

std::shared_ptr<RegisteredFont> ImGui_Renderer::createFontFromFile(IFileSystem& fs,
    const std::filesystem::path& fontFile, float fontSize)
{
	auto fontData = fs.readFile(fontFile);
	if (!fontData)
		return std::make_shared<RegisteredFont>();

	auto font = std::make_shared<RegisteredFont>(fontData, false, fontSize);
    m_fonts.push_back(font);

    return font;
}

void ImGui_Renderer::setDefaultFont(std::shared_ptr<RegisteredFont> font)
{
    if (!font || !font->hasFontData())
        return;

    if (m_defaultFont && m_defaultFont != font)
    {
        m_fonts.erase(
            std::remove(m_fonts.begin(), m_fonts.end(), m_defaultFont),
            m_fonts.end());
    }

    m_defaultFont = std::move(font);

    auto it = std::find(m_fonts.begin(), m_fonts.end(), m_defaultFont);
    if (it == m_fonts.end())
    {
        m_fonts.insert(m_fonts.begin(), m_defaultFont);
    }
    else if (it != m_fonts.begin())
    {
        auto held = *it;
        m_fonts.erase(it);
        m_fonts.insert(m_fonts.begin(), std::move(held));
    }
}

std::shared_ptr<RegisteredFont> ImGui_Renderer::createFontFromMemoryInternal(void const* pData, size_t size,
    bool compressed, float fontSize)
{
    if (!pData || !size)
		return std::make_shared<RegisteredFont>();

    // Copy the font data into a blob to make the RegisteredFont object own it
    void* dataCopy = malloc(size);
    memcpy(dataCopy, pData, size);
    std::shared_ptr<caustica::Blob> blob = std::make_shared<caustica::Blob>(dataCopy, size);
    
    auto font = std::make_shared<RegisteredFont>(blob, compressed, fontSize);
    m_fonts.push_back(font);

    return font;
}

std::shared_ptr<RegisteredFont> ImGui_Renderer::createFontFromMemory(void const* pData, size_t size, float fontSize)
{
    return createFontFromMemoryInternal(pData, size, false, fontSize);
}

std::shared_ptr<RegisteredFont> ImGui_Renderer::createFontFromMemoryCompressed(void const* pData, size_t size,
    float fontSize)
{
    return createFontFromMemoryInternal(pData, size, true, fontSize);
}






void ImGui_Renderer::animate(float elapsedTimeSeconds)
{
    if (!m_imguiRhi)
        return;

    // ImGui CPU frame (NewFrame/buildUI/Render) must run on the update thread:
    // buildUI touches App resources (tryResource) which are not thread-safe with
    // the dedicated render thread. GPU submit uses a captured draw snapshot.
    prepareImGuiFrame(elapsedTimeSeconds);
    buildUI();
    ImGui::Render();
    m_imguiRhi->captureDrawData();
    m_imguiFrameOpened = false;
}

void ImGui_Renderer::prepareImGuiFrame(float elapsedTimeSeconds)
{
    if (m_pendingDisplayScaleChanged)
    {
        auto& ioPending = ImGui::GetIO();
        ioPending.Fonts->Clear();
        ioPending.Fonts->TexRef = ImTextureRef();
        for (auto& font : m_fonts)
            font->releaseScaledFont();
        // Keep live FontSizeBase/FontScale*; full style assign is unsafe on ImGui 1.92.
        ImGui::GetStyle().ScaleAllSizes(m_pendingDisplayScaleX);
        m_pendingDisplayScaleChanged = false;
    }

    float scaleX, scaleY;
    getGpuDevice()->getDPIScaleInfo(scaleX, scaleY);
    for (auto& font : m_fonts)
    {
        if (!font->getScaledFont())
            font->createScaledFont(m_supportExplicitDisplayScaling ? scaleX : 1.f);
    }

    m_imguiRhi->updateFontTexture();

    int w, h;
    getGpuDevice()->getWindowDimensions(w, h);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(w), float(h));
    if (!m_supportExplicitDisplayScaling)
    {
        io.DisplayFramebufferScale.x = scaleX;
        io.DisplayFramebufferScale.y = scaleY;
    }

    io.DeltaTime = (elapsedTimeSeconds > 0.0f) ? elapsedTimeSeconds : (1.0f / 60.0f);
    io.MouseDrawCursor = false;

    GLFWwindow* glfwWindow = getGpuDevice()->getWindow();
    if (glfwWindow)
    {
        ImGui_ImplGlfw_UpdateKeyModifiers(io, glfwWindow);

        double mouseX, mouseY;
        glfwGetCursorPos(glfwWindow, &mouseX, &mouseY);
        io.AddMousePosEvent((float)mouseX, (float)mouseY);

        for (int i = 0; i < ImGuiMouseButton_COUNT; i++)
            io.AddMouseButtonEvent(i, glfwGetMouseButton(glfwWindow, i) == GLFW_PRESS);
    }

    ImGui::NewFrame();
    m_imguiFrameOpened = true;
}

void ImGui_Renderer::render(caustica::rhi::Framebuffer* framebuffer)
{
    if (!m_imguiRhi)
        return;

    // Draw data was captured on the update thread after ImGui::Render().
    m_imguiRhi->render(framebuffer);
}

void ImGui_Renderer::backBufferResizing()
{
    if(m_imguiRhi) m_imguiRhi->backbufferResizing();
}

void ImGui_Renderer::displayScaleChanged(float scaleX, float scaleY)
{
    // Apps that don't implement explicit scaling won't expect the fonts to be resized etc.
    if (!m_supportExplicitDisplayScaling)
        return;

    // Defer atlas rebuild to the render thread. Doing EndFrame/Fonts->Clear here races
    // with buildUI() when a dedicated render thread is in flight.
    m_pendingDisplayScaleX = scaleX;
    m_pendingDisplayScaleY = scaleY;
    m_pendingDisplayScaleChanged = true;
}

void ImGui_Renderer::beginFullScreenWindow()
{
    ImGuiIO const& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(
        io.DisplaySize.x / io.DisplayFramebufferScale.x,
        io.DisplaySize.y / io.DisplayFramebufferScale.y),
        ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::Begin(" ", 0, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
}

void ImGui_Renderer::drawScreenCenteredText(const char* text)
{
    ImGuiIO const& io = ImGui::GetIO();
    ImVec2 textSize = ImGui::CalcTextSize(text);
    ImGui::SetCursorPosX((io.DisplaySize.x / io.DisplayFramebufferScale.x - textSize.x) * 0.5f);
    ImGui::SetCursorPosY((io.DisplaySize.y / io.DisplayFramebufferScale.y - textSize.y) * 0.5f);
    ImGui::TextUnformatted(text);
}

void ImGui_Renderer::endFullScreenWindow()
{
    ImGui::End();
    ImGui::PopStyleVar();
}

void RegisteredFont::createScaledFont(float displayScale)
{
    ImFontConfig fontConfig;
    fontConfig.SizePixels = m_sizeAtDefaultScale * displayScale;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;
    // Slight brighten helps thin UI fonts stay readable on dark themes.
    fontConfig.RasterizerMultiply = 1.05f;
    fontConfig.MergeMode = m_mergeMode;
    if (!m_glyphRanges.empty())
        fontConfig.GlyphRanges = m_glyphRanges.data();

    m_imFont = nullptr;

    if (m_data)
    {
        fontConfig.FontDataOwnedByAtlas = false;
        if (m_isCompressed)
        {
            m_imFont = ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(
                (void*)(m_data->data()), (int)(m_data->size()), 0.f, &fontConfig);
        }
        else
        {
            m_imFont = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
                (void*)(m_data->data()), (int)(m_data->size()), 0.f, &fontConfig);
        }
    }
    else if (m_isDefault)
    {
        m_imFont = ImGui::GetIO().Fonts->AddFontDefault(&fontConfig);
    }

    if (m_imFont)
    {
        ImGui::GetIO().Fonts->TexRef = ImTextureRef();
    }
}

void RegisteredFont::releaseScaledFont()
{
    m_imFont = nullptr;
}

void caustica::imGuiForwardKeyboard(int glfwKey, int action, int /*scancode*/)
{
    if (action == GLFW_REPEAT)
        return;

    ImGuiIO& io = ImGui::GetIO();
    const ImGuiKey imguiKey = ImGui_ImplGlfw_KeyToImGuiKey(glfwKey);
    if (imguiKey == ImGuiKey_None)
        return;

    io.AddKeyEvent(imguiKey, action != GLFW_RELEASE);
}

void caustica::imGuiForwardInputCharacter(unsigned int codepoint)
{
    ImGui::GetIO().AddInputCharacter(codepoint);
}
