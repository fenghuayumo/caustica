#include <cassert>
#include <algorithm>
#include <scene/camera/Camera.h>
#include <scene/View.h>

using namespace caustica::math;
using namespace caustica;

void BaseCamera::updateWorldToView()
{
    m_matTranslatedWorldToView = affine3::from_cols(m_cameraRight, m_cameraUp, m_cameraDir, 0.f);
    m_matWorldToView = translation(-m_cameraPos) * m_matTranslatedWorldToView;
}

void BaseCamera::baseLookAt(float3 cameraPos, float3 cameraTarget, float3 cameraUp)
{
    this->m_cameraPos = cameraPos;
    this->m_cameraDir = normalize(cameraTarget - cameraPos);
    this->m_cameraUp = normalize(cameraUp);
    this->m_cameraRight = normalize(cross(this->m_cameraDir, this->m_cameraUp));
    this->m_cameraUp = normalize(cross(this->m_cameraRight, this->m_cameraDir));

    updateWorldToView();
}

void FirstPersonCamera::keyboardUpdate(int key, int scancode, int action, int mods)
{
    if (m_keyboardMap.find(key) == m_keyboardMap.end())
    {
        return;
    }

    auto cameraKey = m_keyboardMap.at(key);
    if (action == GLFW_PRESS || action == GLFW_REPEAT)
    {
        m_keyboardState[cameraKey] = true;
    }
    else {
        m_keyboardState[cameraKey] = false;
    }
}

void FirstPersonCamera::mousePosUpdate(double xpos, double ypos)
{
    m_mousePos = { float(xpos), float(ypos) };
}

void FirstPersonCamera::mouseButtonUpdate(int button, int action, int mods)
{
    if (m_mouseButtonMap.find(button) == m_mouseButtonMap.end())
    {
        return;
    }

    auto cameraButton = m_mouseButtonMap.at(button);
    if (action == GLFW_PRESS)
    {
        m_mouseButtonState[cameraButton] = true;
    }
    else {
        m_mouseButtonState[cameraButton] = false;
    }
}

void FirstPersonCamera::clearFlyKeyboardState()
{
    m_keyboardState[KeyboardControls::MoveUp] = false;
    m_keyboardState[KeyboardControls::MoveDown] = false;
    m_keyboardState[KeyboardControls::MoveLeft] = false;
    m_keyboardState[KeyboardControls::MoveRight] = false;
    m_keyboardState[KeyboardControls::MoveForward] = false;
    m_keyboardState[KeyboardControls::MoveBackward] = false;
    m_keyboardState[KeyboardControls::RollLeft] = false;
    m_keyboardState[KeyboardControls::RollRight] = false;
}

void FirstPersonCamera::mouseScrollUpdate(double /*xoffset*/, double yoffset)
{
    if (yoffset == 0.0)
        return;

    // Dolly along look direction; step scales with move speed.
    const float step = m_moveSpeed * 0.35f * static_cast<float>(yoffset);
    updateCamera(m_cameraDir * step, affine3::identity());
}

void FirstPersonCamera::lookAt(float3 cameraPos, float3 cameraTarget, float3 cameraUp)
{
    // make the base method public.
    baseLookAt(cameraPos, cameraTarget, cameraUp);
    m_mouseMotionAccumulator = 0.f;
    m_cameraMoveDamp = 0.f;
    m_cameraMovePrev = 0.f;
}

void FirstPersonCamera::lookTo(math::float3 cameraPos, math::float3 cameraDir, math::float3 cameraUp)
{
    baseLookAt(cameraPos, cameraPos + cameraDir, cameraUp);
    m_mouseMotionAccumulator = 0.f;
    m_cameraMoveDamp = 0.f;
    m_cameraMovePrev = 0.f;
}

std::pair<bool, float3> FirstPersonCamera::animateTranslation(float deltaT)
{
    bool cameraDirty = false;
    float moveStep = deltaT * m_moveSpeed;
    float3 cameraMoveVec = 0.f;

    if (m_keyboardState[KeyboardControls::SpeedUp])
        moveStep *= 3.f;

    if (m_keyboardState[KeyboardControls::SlowDown])
        moveStep *= .1f;

    if (m_keyboardState[KeyboardControls::MoveForward])
    {
        cameraDirty = true;
        cameraMoveVec += m_cameraDir * moveStep;
    }

    if (m_keyboardState[KeyboardControls::MoveBackward])
    {
        cameraDirty = true;
        cameraMoveVec += -m_cameraDir * moveStep;
    }

    if (m_keyboardState[KeyboardControls::MoveLeft])
    {
        cameraDirty = true;
        cameraMoveVec += -m_cameraRight * moveStep;
    }

    if (m_keyboardState[KeyboardControls::MoveRight])
    {
        cameraDirty = true;
        cameraMoveVec += m_cameraRight * moveStep;
    }

    if (m_keyboardState[KeyboardControls::MoveUp])
    {
        cameraDirty = true;
        cameraMoveVec += m_cameraUp * moveStep;
    }

    if (m_keyboardState[KeyboardControls::MoveDown])
    {
        cameraDirty = true;
        cameraMoveVec += -m_cameraUp * moveStep;
    }

    // Arrow keys: screen-aligned pan (always available; no conflict with WASD fly).
    if (m_keyboardState[KeyboardControls::YawLeft])
    {
        cameraDirty = true;
        cameraMoveVec += -m_cameraRight * moveStep;
    }
    if (m_keyboardState[KeyboardControls::YawRight])
    {
        cameraDirty = true;
        cameraMoveVec += m_cameraRight * moveStep;
    }
    if (m_keyboardState[KeyboardControls::PitchUp])
    {
        cameraDirty = true;
        cameraMoveVec += m_cameraUp * moveStep;
    }
    if (m_keyboardState[KeyboardControls::PitchDown])
    {
        cameraDirty = true;
        cameraMoveVec += -m_cameraUp * moveStep;
    }

    return std::make_pair(cameraDirty, cameraMoveVec);
}

void FirstPersonCamera::updateCamera(math::float3 cameraMoveVec, math::affine3 cameraRotation)
{
    m_cameraPos += cameraMoveVec;
    m_cameraDir = normalize(cameraRotation.transformVector(m_cameraDir));
    m_cameraUp = normalize(cameraRotation.transformVector(m_cameraUp));
    m_cameraRight = normalize(cross(m_cameraDir, m_cameraUp));

    updateWorldToView();
}

std::pair<bool, affine3> FirstPersonCamera::animateRoll(affine3 initialRotation)
{
    bool cameraDirty = false;
    affine3 cameraRotation = initialRotation;
    if (m_keyboardState[KeyboardControls::RollLeft] ||
        m_keyboardState[KeyboardControls::RollRight])
    {
        float roll = float(m_keyboardState[KeyboardControls::RollLeft]) * -m_rotateSpeed * 2.0f +
            float(m_keyboardState[KeyboardControls::RollRight]) * m_rotateSpeed * 2.0f;

        cameraRotation = rotation(m_cameraDir, roll) * cameraRotation;
        cameraDirty = true;
    }
    return std::make_pair(cameraDirty, cameraRotation);
}

void FirstPersonCamera::animate(float deltaT)
{
    // Track mouse delta.
    // Use m_isDragging / m_isPanning to avoid jumps on the first frame of a press.
    const bool lookHeld = m_mouseButtonState[MouseButtons::Left]
        || m_mouseButtonState[MouseButtons::Right];
    const bool panHeld = m_mouseButtonState[MouseButtons::Middle];

    float2 mouseMove = 0.f;
    if ((lookHeld && m_isDragging) || (panHeld && m_isPanning))
        mouseMove = m_mousePos - m_mousePosPrev;

    m_isDragging = lookHeld;
    m_isPanning = panHeld;
    m_mousePosPrev = m_mousePos;

    bool cameraDirty = false;
    affine3 cameraRotation = affine3::identity();
    float3 cameraMoveVec = 0.f;

    // Middle-mouse drag: screen-space pan (truck / pedestal).
    if (panHeld && (mouseMove.x != 0.f || mouseMove.y != 0.f))
    {
        cameraMoveVec += (-mouseMove.x * m_cameraRight + mouseMove.y * m_cameraUp) * m_panSpeed;
        cameraDirty = true;
    }

    // Look (yaw / pitch). The editor only arms Left while Alt is held so
    // transform-gizmo drags do not tumble the camera. RMB is fly, not look.
    if (lookHeld && !panHeld && (mouseMove.x != 0.f || mouseMove.y != 0.f))
    {
        float yaw = m_rotateSpeed * mouseMove.x;
        float pitch = m_rotateSpeed * mouseMove.y;

        cameraRotation = rotation(float3(0.f, 1.f, 0.f), -yaw);
        cameraRotation = rotation(m_cameraRight, -pitch) * cameraRotation;

        cameraDirty = true;
    }

    // handle keyboard roll next
    auto rollResult = animateRoll(cameraRotation);
    cameraDirty |= rollResult.first;
    cameraRotation = rollResult.second;

    // handle translation (WASD / QE / arrows)
    auto translateResult = animateTranslation(deltaT);
    cameraDirty |= translateResult.first;
    cameraMoveVec += translateResult.second;

    if (cameraDirty)
    {
        updateCamera(cameraMoveVec, cameraRotation);
    }
}

void FirstPersonCamera::animateSmooth(float deltaT)
{
    const float c_DampeningRate = 7.5f;
    float dampenWeight = exp(-c_DampeningRate * deltaT);

    // Track mouse delta.
    // Use m_isDragging to avoid random camera rotations when clicking inside an inactive window.
    if (m_mouseButtonState[MouseButtons::Left])
    {
        if (m_isDragging)
        {
            // Use an accumulator to keep the camera animating after mouse button has been released.
            m_mouseMotionAccumulator += m_mousePos - m_mousePosPrev;
        }

        m_isDragging = true;
    }
    else
    {
        m_isDragging = false;
    }
    m_mousePosPrev = m_mousePos;

    float2 mouseMove = m_mouseMotionAccumulator * (1.f - dampenWeight);
    m_mouseMotionAccumulator *= dampenWeight;

    affine3 cameraRotation = affine3::identity();

    // handle mouse rotation first
    // this will affect the movement vectors in the world matrix, which we use below
    if (mouseMove.x || mouseMove.y)
    {
        float yaw = m_rotateSpeed * mouseMove.x;
        float pitch = m_rotateSpeed * mouseMove.y;

        cameraRotation = rotation(float3(0.f, 1.f, 0.f), -yaw);
        cameraRotation = rotation(m_cameraRight, -pitch) * cameraRotation;
    }

    // handle keyboard roll next
    auto rollResult = animateRoll(cameraRotation);
    cameraRotation = rollResult.second;

    // handle translation
    auto translateResult = animateTranslation(deltaT);
    const float3& cameraMoveVec = translateResult.second;

    m_cameraMoveDamp = lerp(cameraMoveVec, m_cameraMovePrev, dampenWeight);
    m_cameraMovePrev = m_cameraMoveDamp;

    updateCamera(m_cameraMoveDamp, cameraRotation);
}

void ThirdPersonCamera::keyboardUpdate(int key, int scancode, int action, int mods)
{
    if (m_keyboardMap.find(key) == m_keyboardMap.end())
    {
        return;
    }

    auto cameraKey = m_keyboardMap.at(key);
    if (action == GLFW_PRESS || action == GLFW_REPEAT)
    {
        m_keyboardState[cameraKey] = true;
    }
    else {
        m_keyboardState[cameraKey] = false;
    }
}

void ThirdPersonCamera::mousePosUpdate(double xpos, double ypos)
{
    m_mousePos = float2(float(xpos), float(ypos));
}

void ThirdPersonCamera::mouseButtonUpdate(int button, int action, int mods)
{
    const bool pressed = (action == GLFW_PRESS);

    switch(m_mouseState)
    {
    case MouseState::Idle:
        if (pressed)
        {
            if (button == GLFW_MOUSE_BUTTON_LEFT)
                m_mouseState = MouseState::Orbiting;
            else if (button == GLFW_MOUSE_BUTTON_MIDDLE)
                m_mouseState = MouseState::Panning;
            m_mousePosPrev = m_mousePos;
        }
        break;

    case MouseState::Orbiting:
        if (!pressed && button == GLFW_MOUSE_BUTTON_LEFT)
            m_mouseState = MouseState::Idle;
        break;

    case MouseState::Panning:
        if (!pressed && button == GLFW_MOUSE_BUTTON_MIDDLE)
            m_mouseState = MouseState::Idle;
        break;
    }
}

void ThirdPersonCamera::mouseScrollUpdate(double xoffset, double yoffset)
{
    const float scrollFactor = 1.15f;
    m_distance = clamp(m_distance * (yoffset < 0 ? scrollFactor : 1.0f / scrollFactor), m_minDistance,  m_maxDistance);
}

void ThirdPersonCamera::joystickUpdate(int axis, float value)
{
    switch (axis)
    {
    case GLFW_GAMEPAD_AXIS_RIGHT_X: m_deltaYaw = value; break;
    case GLFW_GAMEPAD_AXIS_RIGHT_Y: m_deltaPitch = value; break;
    default: break;
    }
}

void ThirdPersonCamera::joystickButtonUpdate(int button, bool pressed)
{
    switch (button)
    {
    case GLFW_GAMEPAD_BUTTON_B: if (pressed) m_deltaDistance -= 1; break;
    case GLFW_GAMEPAD_BUTTON_A: if (pressed) m_deltaDistance += 1; break;
    default: break;
    }
}

void ThirdPersonCamera::setRotation(float yaw, float pitch)
{
    m_yaw = yaw;
    m_pitch = pitch;
}

void ThirdPersonCamera::setView(const caustica::ViewInfo& view)
{
    m_projectionMatrix = view.getProjectionMatrix(false);
    m_inverseProjectionMatrix = view.getInverseProjectionMatrix(false);
    auto viewport = view.getViewport();
    m_viewportSize = float2(viewport.width(), viewport.height());
}

void ThirdPersonCamera::animateOrbit(float deltaT, float2 mouseMove)
{
    m_yaw -= m_rotateSpeed * mouseMove.x;
    m_pitch += m_rotateSpeed * mouseMove.y;

    const float ORBIT_SENSITIVITY = 1.5f;
    const float ZOOM_SENSITIVITY = 40.f;
    m_distance += ZOOM_SENSITIVITY * deltaT * m_deltaDistance;
    m_yaw += ORBIT_SENSITIVITY * deltaT * m_deltaYaw;
    m_pitch += ORBIT_SENSITIVITY * deltaT * m_deltaPitch;

    m_distance = clamp(m_distance, m_minDistance, m_maxDistance);
    
    m_pitch = clamp(m_pitch, PI_f * -0.5f, PI_f * 0.5f);
    
    m_deltaDistance = 0;
    m_deltaYaw = 0;
    m_deltaPitch = 0;
}

void ThirdPersonCamera::animateTranslation(const math::float3x3& viewMatrix)
{
    // If the view parameters have never been set, we can't translate
    if (m_viewportSize.x <= 0.f || m_viewportSize.y <= 0.f)
        return;

    if (all(m_mousePos == m_mousePosPrev))
        return;

    float4 oldClipPos = float4(0.f, 0.f, m_distance, 1.f) * m_projectionMatrix;
    oldClipPos /= oldClipPos.w;
    oldClipPos.x = 2.f * (m_mousePosPrev.x) / m_viewportSize.x - 1.f;
    oldClipPos.y = 1.f - 2.f * (m_mousePosPrev.y) / m_viewportSize.y;
    float4 newClipPos = oldClipPos;
    newClipPos.x = 2.f * (m_mousePos.x) / m_viewportSize.x - 1.f;
    newClipPos.y = 1.f - 2.f * (m_mousePos.y) / m_viewportSize.y;

    float4 oldViewPos = oldClipPos * m_inverseProjectionMatrix;
    oldViewPos /= oldViewPos.w;
    float4 newViewPos = newClipPos * m_inverseProjectionMatrix;
    newViewPos /= newViewPos.w;

    float2 viewMotion = oldViewPos.xy() - newViewPos.xy();

    m_targetPos -= viewMotion.x * viewMatrix.row0;

    if (m_keyboardState[KeyboardControls::HorizontalPan])
    {
        float3 horizontalForward = float3(viewMatrix.row2.x, 0.f, viewMatrix.row2.z);
        float horizontalLength = length(horizontalForward);
        if (horizontalLength == 0.f)
            horizontalForward = float3(viewMatrix.row1.x, 0.f, viewMatrix.row1.z);
        horizontalForward = normalize(horizontalForward);
        m_targetPos += viewMotion.y * horizontalForward * 1.5f;
    }
    else
        m_targetPos += viewMotion.y * viewMatrix.row1;
}

void ThirdPersonCamera::animate(float deltaT)
{
    quat orbit = rotationQuat(float3(m_pitch, m_yaw, 0));
    const auto targetRotation = orbit.toMatrix();

    switch(m_mouseState)
    {
    case MouseState::Orbiting:
        {
            float2 mouseMove = m_mousePos - m_mousePosPrev;
            animateOrbit(deltaT, mouseMove);
            break;
        }
    case MouseState::Panning:
        animateTranslation(targetRotation);
        break;
    case MouseState::Idle:
        break;
    }

    const float3 vectorToCamera = -m_distance * targetRotation.row2;
    const float3 camPos = m_targetPos + vectorToCamera;

    m_cameraPos = camPos;
    m_cameraRight = -targetRotation.row0;
    m_cameraUp = targetRotation.row1;
    m_cameraDir = targetRotation.row2;
    updateWorldToView();
    
    m_mousePosPrev = m_mousePos;
}

void ThirdPersonCamera::lookAt(math::float3 cameraPos, math::float3 cameraTarget)
{
    math::float3 cameraDir = cameraTarget - cameraPos;

    float azimuth, elevation, dirLength;
    math::cartesianToSpherical(cameraDir, azimuth, elevation, dirLength);

    setTargetPosition(cameraTarget);
    setDistance(dirLength);
    azimuth = -(azimuth + math::PI_f * 0.5f);
    setRotation(azimuth, elevation);
}

void ThirdPersonCamera::lookTo(math::float3 cameraPos, math::float3 cameraDir,
    std::optional<float> targetDistance)
{
    float azimuth, elevation, dirLength;
    math::cartesianToSpherical(-cameraDir, azimuth, elevation, dirLength);
    cameraDir /= dirLength;

    float const distance = targetDistance.value_or(getDistance());
    setTargetPosition(cameraPos + cameraDir * distance);
    setDistance(distance);
    azimuth = -(azimuth + math::PI_f * 0.5f);
    setRotation(azimuth, elevation);
}
