#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace caustica::editor
{

class IEditorUndoCommand
{
public:
    virtual ~IEditorUndoCommand() = default;
    virtual void undo() = 0;
    virtual void redo() = 0;
    [[nodiscard]] virtual const char* label() const = 0;
};

class EditorUndoStack
{
public:
    static constexpr std::size_t kDefaultMaxDepth = 64;

    void push(std::unique_ptr<IEditorUndoCommand> command);
    bool undo();
    bool redo();
    void clear();

    [[nodiscard]] bool canUndo() const { return !m_undo.empty(); }
    [[nodiscard]] bool canRedo() const { return !m_redo.empty(); }
    [[nodiscard]] const char* undoLabel() const;
    [[nodiscard]] const char* redoLabel() const;
    [[nodiscard]] std::size_t maxDepth() const { return m_maxDepth; }
    void setMaxDepth(std::size_t depth);

private:
    std::vector<std::unique_ptr<IEditorUndoCommand>> m_undo;
    std::vector<std::unique_ptr<IEditorUndoCommand>> m_redo;
    std::size_t m_maxDepth = kDefaultMaxDepth;
};

} // namespace caustica::editor
