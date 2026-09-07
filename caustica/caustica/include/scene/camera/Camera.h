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

        void setMoveSpeed(float value) { m_MoveSpeed = value; }
        void setRotateSpeed(float value) { m_RotateSpeed = value; }

        [[nodiscard]] const math::affine3& getWorldToViewMatrix() const { return m_MatWorldToView; }
        [[nodiscard]] const math::affine3& getTranslatedWorldToViewMatrix() const { return m_MatTranslatedWorldToView; }
        [[nodiscard]] const math::float3& getPosition() const { return m_CameraPos; }
        [[nodiscard]] const math::float3& getDir() const { return m_CameraDir; }
        [[nodiscard]] const math::float3& getUp() const { return m_CameraUp; }

    protected:
        // This can be useful for derived classes while not necessarily public, i.e., in a third person
        // camera class, public clients cannot direct the gaze point.
        void baseLookAt(math::float3 cameraPos, math::float3 cameraTarget, math::float3 cameraUp = math::float3{ 0.f, 1.f, 0.f });
        void updateWorldToView();

        math::affine3 m_MatWorldToView = math::affine3::identity();
        math::affine3 m_MatTranslatedWorldToView = math::affine3::identity();

        math::float3 m_CameraPos   = 0.f;   // in worldspace
        math::float3 m_CameraDir   = math::float3(1.f, 0.f, 0.f); // normalized
        math::float3 m_CameraUp    = math::float3(0.f, 1.f, 0.f); // normalized
        math::float3 m_CameraRight = math::float3(0.f, 0.f, 1.f); // normalized

        float m_MoveSpeed = 1.f;      // movement speed in units/second
        float m_RotateSpeed = .005f;  // mouse sensitivity in radians/pixel
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

        math::float2 m_MousePos = 0.f;
        math::float2 m_MousePosPrev = 0.f;
        math::float2 m_MouseMotionAccumulator = 0.f;
        math::float3 m_CameraMovePrev = 0.f;
        math::float3 m_CameraMoveDamp = 0.f;
        bool m_IsDragging = false;
        bool m_IsPanning = false;

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

        const std::unordered_map<int, int> m_KeyboardMap = {
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

        const std::unordered_map<int, int> m_MouseButtonMap = {
            { GLFW_MOUSE_BUTTON_LEFT, MouseButtons::Left },
            { GLFW_MOUSE_BUTTON_MIDDLE, MouseButtons::Middle },
            { GLFW_MOUSE_BUTTON_RIGHT, MouseButtons::Right },
        };

        std::array<bool, KeyboardControls::KeyboardControlCount> m_KeyboardState = { false };
        std::array<bool, MouseButtons::MouseButtonCount> m_MouseButtonState = { false };
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

        math::float3 getTargetPosition() const { return m_TargetPos; }
        void setTargetPosition(math::float3 position) { m_TargetPos = position; }

        float getDistance() const { return m_Distance; }
        void setDistance(float distance) { m_Distance = distance; }
        
        float getRotationYaw() const { return m_Yaw; }
        float getRotationPitch() const { return m_Pitch; }
        void setRotation(float yaw, float pitch);

        float getMaxDistance() const { return m_MaxDistance; }
        void setMaxDistance(float value) { m_MaxDistance = value; }

        void setView(const ViewInfo& view);

        void lookAt(math::float3 cameraPos, math::float3 cameraTarget);
        void lookTo(math::float3 cameraPos, math::float3 cameraDir,
            std::optional<float> targetDistance = std::optional<float>());
        
    private:
        void animateOrbit(float deltaT, math::float2 mouseMove);
        void animateTranslation(const math::float3x3& viewMatrix);

        // View parameters to derive translation amounts
        math::float4x4 m_ProjectionMatrix = math::float4x4::identity();
        math::float4x4 m_InverseProjectionMatrix = math::float4x4::identity();
        math::float2 m_ViewportSize = math::float2::zero();

        math::float2 m_MousePos = 0.f;
        math::float2 m_MousePosPrev = 0.f;
        
        enum class MouseState {
            Idle,
            Orbiting,
            Panning
        };
        
        MouseState m_MouseState = MouseState::Idle;

        math::float3 m_TargetPos = 0.f;
        float m_Distance = 30.f;
        
        float m_MinDistance = 0.f;
        float m_MaxDistance = std::numeric_limits<float>::max();
        
        float m_Yaw = 0.f;
        float m_Pitch = 0.f;
        
        float m_DeltaYaw = 0.f;
        float m_DeltaPitch = 0.f;
        float m_DeltaDistance = 0.f;

        typedef enum
        {
            HorizontalPan,

            KeyboardControlCount,
        } KeyboardControls;

        const std::unordered_map<int, int> m_KeyboardMap = {
            { GLFW_KEY_LEFT_ALT, KeyboardControls::HorizontalPan },
        };

        std::array<bool, KeyboardControls::KeyboardControlCount> m_KeyboardState = { false };
    };
}
