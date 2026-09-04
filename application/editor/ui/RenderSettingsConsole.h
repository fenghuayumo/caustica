#pragma once

#include <core/console/ConsoleObjects.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace caustica::console
{
class Interpreter;
}

namespace caustica
{
class App;
}

namespace caustica::editor
{

struct EditorUIData;

// Binds the process-wide console command dictionary to the active editor render
// state. Commands are registered once; the active state is detached on shutdown.
class RenderSettingsConsoleBinding
{
public:
    explicit RenderSettingsConsoleBinding(EditorUIData& ui);
    // App provider enables commands that reach the renderer (e.g. `vis`).
    RenderSettingsConsoleBinding(
        EditorUIData& ui,
        std::function<App*()> appProvider);
    ~RenderSettingsConsoleBinding();

    RenderSettingsConsoleBinding(const RenderSettingsConsoleBinding&) = delete;
    RenderSettingsConsoleBinding& operator=(const RenderSettingsConsoleBinding&) = delete;

    [[nodiscard]] std::shared_ptr<caustica::console::Interpreter> interpreter() const
    {
        return m_interpreter;
    }

    bool execute(
        std::string_view commandLine,
        std::string* output = nullptr,
        caustica::console::VariableState::SetBy origin =
            caustica::console::VariableState::CONSOLE) const;

private:
    EditorUIData* m_ui = nullptr;
    std::function<App*()> m_appProvider;
    std::shared_ptr<caustica::console::Interpreter> m_interpreter;
};

} // namespace caustica::editor
