#include <engine/Input.h>

#include <engine/App.h>
#include <engine/AppResources.h>
#include <events/application_event.h>
#include <events/key_event.h>
#include <events/mouse_event.h>
#include <render/core/CameraController.h>
#include <scene/camera/Camera.h>

namespace caustica
{

void applyEventToInput(InputState& input, const Event& event)
{
    switch (event.getEventType())
    {
    case EventType::KeyPressed:
    {
        const auto& e = static_cast<const KeyPressedEvent&>(event);
        input.modifiers = e.getModifiers();
        if (!e.isRepeat())
            input.keys.press(e.getKeyCode());
        break;
    }
    case EventType::KeyReleased:
    {
        const auto& e = static_cast<const KeyReleasedEvent&>(event);
        input.modifiers = e.getModifiers();
        input.keys.release(e.getKeyCode());
        break;
    }
    case EventType::KeyTyped:
    {
        const auto& e = static_cast<const KeyTypedEvent&>(event);
        input.text.push_back(e.getCodepoint());
        break;
    }
    case EventType::MouseMoved:
    {
        const auto& e = static_cast<const MouseMovedEvent&>(event);
        input.cursor.deltaX += e.getX() - input.cursor.x;
        input.cursor.deltaY += e.getY() - input.cursor.y;
        input.cursor.x = e.getX();
        input.cursor.y = e.getY();
        break;
    }
    case EventType::MouseScrolled:
    {
        const auto& e = static_cast<const MouseScrolledEvent&>(event);
        input.scrollX += e.getXOffset();
        input.scrollY += e.getYOffset();
        break;
    }
    case EventType::MouseButtonPressed:
    {
        const auto& e = static_cast<const MouseButtonPressedEvent&>(event);
        input.modifiers = e.getModifiers();
        input.mouse.press(e.getButton());
        break;
    }
    case EventType::MouseButtonReleased:
    {
        const auto& e = static_cast<const MouseButtonReleasedEvent&>(event);
        input.modifiers = e.getModifiers();
        input.mouse.release(e.getButton());
        break;
    }
    default:
        break;
    }
}

void applyCameraWindowInput(App& app)
{
    auto* input = app.tryResource<InputState>();
    auto* cam = cameraController(app);
    if (!input || !cam)
        return;

    const CameraInputGate* gate = app.tryResource<CameraInputGate>();
    const CameraInputConfig* config = app.tryResource<CameraInputConfig>();
    if (gate && !gate->enabled)
        return;

    const bool flyOk = !gate || gate->fly;
    const bool lookOk = !gate || gate->look;
    const bool panOk = !gate || gate->pan;
    const bool scrollOk = !gate || gate->scroll;
    const bool keyboardOk = !gate || gate->keyboard;

    const bool flyRequiresRmb = config && config->flyRequiresRightMouse;
    const bool lookAltLeft = config && config->lookWithAltLeft;
    const bool lookRmb = !config || config->lookWithRightMouse;
    const bool panMmb = !config || config->panWithMiddleMouse;

    FirstPersonCamera& camera = cam->camera();
    const int mods = static_cast<int>(input->modifiers);
    constexpr int kPress = 1;
    constexpr int kRelease = 0;

    camera.mousePosUpdate(input->cursor.x, input->cursor.y);

    const bool rmb = input->mouse.pressed(Mouse::Right);
    const bool allowFlyKeys = flyOk && keyboardOk && (!flyRequiresRmb || rmb);

    static constexpr KeyCode kFlyKeys[] = {
        Key::W, Key::A, Key::S, Key::D, Key::Q, Key::E, Key::Z, Key::C,
        Key::Left, Key::Right, Key::Up, Key::Down,
        Key::LeftShift, Key::RightShift, Key::LeftControl, Key::RightControl,
    };

    auto feedKey = [&](KeyCode code) {
        if (!allowFlyKeys && input->keys.justPressed(code))
            return;
        if (!allowFlyKeys && !input->keys.justReleased(code))
            return;
        if (input->keys.justPressed(code))
            camera.keyboardUpdate(static_cast<int>(code), 0, kPress, mods);
        if (input->keys.justReleased(code))
            camera.keyboardUpdate(static_cast<int>(code), 0, kRelease, mods);
    };

    if (keyboardOk)
    {
        if (flyRequiresRmb && allowFlyKeys && input->mouse.justPressed(Mouse::Right))
        {
            for (KeyCode code : kFlyKeys)
            {
                if (input->keys.pressed(code) && !input->keys.justPressed(code))
                    camera.keyboardUpdate(static_cast<int>(code), 0, kPress, mods);
            }
        }

        for (KeyCode code : kFlyKeys)
            feedKey(code);
    }

    if (flyRequiresRmb && input->mouse.justReleased(Mouse::Right))
        camera.clearFlyKeyboardState();

    auto feedButton = [&](MouseCode button, bool allowPress) {
        if (allowPress && input->mouse.justPressed(button))
            camera.mouseButtonUpdate(static_cast<int>(button), kPress, mods);
        if (input->mouse.justReleased(button))
            camera.mouseButtonUpdate(static_cast<int>(button), kRelease, mods);
    };

    if (lookAltLeft)
        feedButton(Mouse::Left, lookOk && input->alt());
    if (lookRmb)
        feedButton(Mouse::Right, lookOk);
    if (panMmb)
        feedButton(Mouse::Middle, panOk);

    if (scrollOk && (input->scrollX != 0.0 || input->scrollY != 0.0))
        camera.mouseScrollUpdate(input->scrollX, input->scrollY);
}

} // namespace caustica
