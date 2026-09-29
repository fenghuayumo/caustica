#pragma once

#include <unordered_map>
#include <array>
#include <optional>

#include <math/math.h>

#define GLFW_INCLUDE_NONE // Do not include any OpenGL headers
#include <GLFW/glfw3.h>

namespace caustica
{
    class ViewInfo;
}

namespace caustica
{

    // A camera with position and orientation. Methods for moving it come from derived classes.
    class BaseCamera
    {
    public:
        virtual void keyboardUpdate(int key, int scancode, int action, int mods) { }
        virtual void mousePosUpdate(double xpos, double ypos) { }
        virtual void mouseButtonUpdate(int button, int action, int mods) { }
        virtual void mouseScrollUpdate(double xoffset, double yoffset) { }
        virtual void joystickButtonUpdate(int button, bool pressed) { }
        virtual void joystickUpdate(int axis, float value) { }
        virtual void animate(float deltaT) { }
        virtual ~BaseCamera() = default;

        void setMoveSpeed(float value) { m_moveSpeed = value; }
        void setRotateSpeed(float value) { m_rotateSpeed = value; }

        [[nodiscard]] const math::affine3& getWorldToViewMatrix() const { return m_matWorldToView; }
        [[nodiscard]] const math::affine3& getTranslatedWorldToViewMatrix() const { return m_matTranslatedWorldToView; }
        [[nodiscard]] const math::float3& getPosition() const { return m_cameraPos; }
        [[nodiscard]] const math::float3& getDir() const { return m_cameraDir; }
        [[nodiscard]] const math::float3& getUp() const { return m_cameraUp; }

    protected:
        // This can be useful for derived classes while not necessarily public, i.e., in a third person
        // camera class, public clients cannot direct the gaze point.
        void baseLookAt(math::float3 cameraPos, math::float3 cameraTarget, math::float3 cameraUp = math::float3{ 0.f, 1.f, 0.f });
        void updateWorldToView();

        math::affine3 m_matWorldToView = math::affine3::identity();
        math::affine3 m_matTranslatedWorldToView = math::affine3::identity();

        math::float3 m_cameraPos   = 0.f;   // in worldspace
        math::float3 m_cameraDir   = math::float3(1.f, 0.f, 0.f); // normalized
        math::float3 m_cameraUp    = math::float3(0.f, 1.f, 0.f); // normalized
        math::float3 m_cameraRight = math::float3(0.f, 0.f, 1.f); // normalized

        float m_moveSpeed = 1.f;      // movement speed in units/second
        float m_rotateSpeed = .005f;  // mouse sensitivity in radians/pixel
    };

    class FirstPersonCamera : public BaseCamera
    {
    public:
        void keyboardUpdate(int key, int scancode, int action, int mods) override;
        void mousePosUpdate(double xpos, double ypos) override;
        void mouseButtonUpdate(int button, int action, int mods) override;
        void mouseScrollUpdate(double xoffset, double yoffset) override;
        void animate(float deltaT) override;
        void animateSmooth(float deltaT);

        void lookAt(math::float3 cameraPos, math::float3 cameraTarget, math::float3 cameraUp = math::float3{ 0.f, 1.f, 0.f });
        void lookTo(math::float3 cameraPos, math::float3 cameraDir, math::float3 cameraUp = math::float3{ 0.f, 1.f, 0.f });

        // Clears WASD/QE/roll keys so releasing RMB fly-mode cannot leave stuck motion.
        void clearFlyKeyboardState();

    private:
        std::pair<bool, math::affine3> animateRoll(math::affine3 initialRotation);
        std::pair<bool, math::float3> animateTranslation(float deltaT);
        void updateCamera(math::float3 cameraMoveVec, math::affine3 cameraRotation);

        math::float2 m_mousePos = 0.f;
        math::float2 m_mousePosPrev = 0.f;
        math::float2 m_mouseMotionAccumulator = 0.f;
        math::float3 m_cameraMovePrev = 0.f;
        math::float3 m_cameraMoveDamp = 0.f;
        bool m_isDragging = false;
        bool m_isPanning = false;

        typedef enum
        {
            MoveUp,
            MoveDown,
            MoveLeft,
            MoveRight,
            MoveForward,
            MoveBackward,

            YawRight,
            YawLeft,
            PitchUp,
            PitchDown,
            RollLeft,
            RollRight,

            SpeedUp,
            SlowDown,

            KeyboardControlCount,
        } KeyboardControls;

        typedef enum
        {
            Left,
            Middle,
            Right,

            MouseButtonCount,
            MouseButtonFirst = Left,
        } MouseButtons;

        const std::unordered_map<int, int> m_keyboardMap = {
            { GLFW_KEY_Q, KeyboardControls::MoveDown },
            { GLFW_KEY_E, KeyboardControls::MoveUp },
            { GLFW_KEY_A, KeyboardControls::MoveLeft },
            { GLFW_KEY_D, KeyboardControls::MoveRight },
            { GLFW_KEY_W, KeyboardControls::MoveForward },
            { GLFW_KEY_S, KeyboardControls::MoveBackward },
            { GLFW_KEY_LEFT, KeyboardControls::YawLeft },
            { GLFW_KEY_RIGHT, KeyboardControls::YawRight },
            { GLFW_KEY_UP, KeyboardControls::PitchUp },
            { GLFW_KEY_DOWN, KeyboardControls::PitchDown },
            { GLFW_KEY_Z, KeyboardControls::RollLeft },
            { GLFW_KEY_C, KeyboardControls::RollRight },
            { GLFW_KEY_LEFT_SHIFT, KeyboardControls::SpeedUp },
            { GLFW_KEY_RIGHT_SHIFT, KeyboardControls::SpeedUp },
            { GLFW_KEY_LEFT_CONTROL, KeyboardControls::SlowDown },
            { GLFW_KEY_RIGHT_CONTROL, KeyboardControls::SlowDown },
        };

        const std::unordered_map<int, int> m_mouseButtonMap = {
            { GLFW_MOUSE_BUTTON_LEFT, MouseButtons::Left },
            { GLFW_MOUSE_BUTTON_MIDDLE, MouseButtons::Middle },
            { GLFW_MOUSE_BUTTON_RIGHT, MouseButtons::Right },
        };

        std::array<bool, KeyboardControls::KeyboardControlCount> m_keyboardState = { false };
        std::array<bool, MouseButtons::MouseButtonCount> m_mouseButtonState = { false };
    };

    class ThirdPersonCamera : public BaseCamera
    {
    public:
        void keyboardUpdate(int key, int scancode, int action, int mods) override;
        void mousePosUpdate(double xpos, double ypos) override;
        void mouseButtonUpdate(int button, int action, int mods) override;
        void mouseScrollUpdate(double xoffset, double yoffset) override;
        void joystickButtonUpdate(int button, bool pressed) override;
        void joystickUpdate(int axis, float value) override;
        void animate(float deltaT) override;

        math::float3 getTargetPosition() const { return m_targetPos; }
        void setTargetPosition(math::float3 position) { m_targetPos = position; }

        float getDistance() const { return m_distance; }
        void setDistance(float distance) { m_distance = distance; }
        
        float getRotationYaw() const { return m_yaw; }
        float getRotationPitch() const { return m_pitch; }
        void setRotation(float yaw, float pitch);

        float getMaxDistance() const { return m_maxDistance; }
        void setMaxDistance(float value) { m_maxDistance = value; }

        void setView(const ViewInfo& view);

        void lookAt(math::float3 cameraPos, math::float3 cameraTarget);
        void lookTo(math::float3 cameraPos, math::float3 cameraDir,
            std::optional<float> targetDistance = std::optional<float>());
        
    private:
        void animateOrbit(float deltaT, math::float2 mouseMove);
        void animateTranslation(const math::float3x3& viewMatrix);

        // View parameters to derive translation amounts
        math::float4x4 m_projectionMatrix = math::float4x4::identity();
        math::float4x4 m_inverseProjectionMatrix = math::float4x4::identity();
        math::float2 m_viewportSize = math::float2::zero();

        math::float2 m_mousePos = 0.f;
        math::float2 m_mousePosPrev = 0.f;
        
        enum class MouseState {
            Idle,
            Orbiting,
            Panning
        };
        
        MouseState m_mouseState = MouseState::Idle;

        math::float3 m_targetPos = 0.f;
        float m_distance = 30.f;
        
        float m_minDistance = 0.f;
        float m_maxDistance = std::numeric_limits<float>::max();
        
        float m_yaw = 0.f;
        float m_pitch = 0.f;
        
        float m_deltaYaw = 0.f;
        float m_deltaPitch = 0.f;
        float m_deltaDistance = 0.f;

        typedef enum
        {
            HorizontalPan,

            KeyboardControlCount,
        } KeyboardControls;

        const std::unordered_map<int, int> m_keyboardMap = {
            { GLFW_KEY_LEFT_ALT, KeyboardControls::HorizontalPan },
        };

        std::array<bool, KeyboardControls::KeyboardControlCount> m_keyboardState = { false };
    };
}
