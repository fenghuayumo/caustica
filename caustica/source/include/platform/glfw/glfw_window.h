/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

#include "platform/window.h"

struct GLFWwindow;

namespace caustica
{

// GLFW-based window implementation.
// Wraps the existing GLFW usage from DeviceManager into the Window abstraction.
class GlfwWindow : public Window
{
public:
    GlfwWindow() = default;
    ~GlfwWindow() override;

    // Register this class as the default window factory.
    // Called by platform init (e.g. WindowsOS::init).
    static void makeDefault();

    // --- Window interface ---
    bool getExit() const override;
    void setExit(bool exit) override;

    uint32_t getWidth() const override;
    uint32_t getHeight() const override;
    float    getDPIScale() const override;
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

    // --- Factory ---
    static Window* createGlfwWindow(const WindowDesc& desc);

private:
    bool initialise(const WindowDesc& desc);

    // Static GLFW callbacks → forward to GlfwWindow instance via glfwGetWindowUserPointer
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
    static void glfwDropCallback(GLFWwindow* window, int count, const char** paths);

    GLFWwindow* m_Window = nullptr;
    EventCallbackFn m_EventCallback;
    bool m_ExitRequested = false;
    std::string m_Title;

    // DPI
    float m_DPIScaleX = 1.0f;
    float m_DPIScaleY = 1.0f;
};

} // namespace caustica
