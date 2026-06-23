#pragma once

#include "window.h"

struct GLFWwindow;

namespace caustica
{

// GLFW-based window implementation.
class GlfwWindow : public Window
{
public:
    GlfwWindow() = default;
    ~GlfwWindow() override;

    // Register this class as the default window factory.
    static void makeDefault();

    // --- Window interface ---
    bool getExit() const override;
    void setExit(bool exit) override;

    uint32_t getWidth() const override;
    uint32_t getHeight() const override;
    float    getDPIScale() const override;
    float    getDPIScaleX() const override { return m_DPIScaleX; }
    float    getDPIScaleY() const override { return m_DPIScaleY; }
    float    getScreenRatio() const override;
    std::string getTitle() const override;
    void*    getNativeHandle() override;

    std::array<uint32_t, 2> getFramebufferSize() const override;

    void setWindowTitle(const std::string& title) override;
    void toggleVSync() override;
    void setVSync(bool set) override;
    void setBorderless(bool borderless) override;
    void maximise() override;
    void hideMouse(bool hide) override;
    void setMousePosition(float x, float y) override;
    void setIcon(const WindowDesc& desc) override;

    void onUpdate() override;
    void processInput() override;
    void updateCursorImgui() override;

    void setEventCallback(const EventCallbackFn& callback) override;

    // --- Window events (with DPI tracking from GpuDevice) ---
    void onFocusChanged(bool focused) override;
    void onIconifyChanged(bool iconified) override;
    void onMove(int x, int y) override;

    // --- Factory ---
    static Window* createGlfwWindow(const WindowDesc& desc);

    // Access the raw GLFW window
    GLFWwindow* glfwWindow() const { return m_Window; }

    // Render-during-move callback (set by GpuDevice/Application)
    using RenderDuringMoveFn = std::function<void()>;
    void setRenderDuringMoveCallback(RenderDuringMoveFn fn) { m_OnRenderDuringMove = std::move(fn); }

private:
    bool initialise(const WindowDesc& desc);

    // Static GLFW callbacks
    static void glfwErrorCallback(int error, const char* description);
    static void glfwKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void glfwCharCallback(GLFWwindow* window, unsigned int codepoint);
    static void glfwCursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void glfwMouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void glfwScrollCallback(GLFWwindow* window, double xoffset, double yoffset);
    static void glfwWindowCloseCallback(GLFWwindow* window);
    static void glfwWindowSizeCallback(GLFWwindow* window, int width, int height);
    static void glfwWindowFocusCallback(GLFWwindow* window, int focused);
    static void glfwWindowIconifyCallback(GLFWwindow* window, int iconified);
    static void glfwWindowPosCallback(GLFWwindow* window, int xpos, int ypos);
    static void glfwDropCallback(GLFWwindow* window, int count, const char** paths);

    GLFWwindow* m_Window = nullptr;
    EventCallbackFn m_EventCallback;
    RenderDuringMoveFn m_OnRenderDuringMove;
    bool m_ExitRequested = false;
    std::string m_Title;
};

} // namespace caustica
