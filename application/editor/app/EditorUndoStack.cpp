#include "EditorUndoStack.h"

namespace caustica::editor
{

void EditorUndoStack::push(std::unique_ptr<IEditorUndoCommand> command)
{
    if (!command)
        return;

    m_undo.push_back(std::move(command));
    m_redo.clear();

    while (m_undo.size() > m_maxDepth)
        m_undo.erase(m_undo.begin());
}

bool EditorUndoStack::undo()
{
    if (m_undo.empty())
        return false;

    std::unique_ptr<IEditorUndoCommand> command = std::move(m_undo.back());
    m_undo.pop_back();
    command->undo();
    m_redo.push_back(std::move(command));
    return true;
}

bool EditorUndoStack::redo()
{
    if (m_redo.empty())
        return false;

    std::unique_ptr<IEditorUndoCommand> command = std::move(m_redo.back());
    m_redo.pop_back();
    command->redo();
    m_undo.push_back(std::move(command));
    return true;
}

void EditorUndoStack::clear()
{
    m_undo.clear();
    m_redo.clear();
}

const char* EditorUndoStack::undoLabel() const
{
    return m_undo.empty() ? nullptr : m_undo.back()->label();
}

const char* EditorUndoStack::redoLabel() const
{
    return m_redo.empty() ? nullptr : m_redo.back()->label();
}

void EditorUndoStack::setMaxDepth(std::size_t depth)
{
    m_maxDepth = depth == 0 ? 1 : depth;
    while (m_undo.size() > m_maxDepth)
        m_undo.erase(m_undo.begin());
}

} // namespace caustica::editor
