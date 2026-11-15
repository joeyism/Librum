#pragma once

namespace application::utility
{

class TouchSelectionSession
{
public:
    void longPress()
    {
        m_hasSelection = true;
        m_dragging = false;
        m_menuVisible = true;
    }

    void dragStarted()
    {
        if(!m_hasSelection)
            return;
        m_dragging = true;
        m_menuVisible = false;
    }

    void dragStopped()
    {
        if(!m_hasSelection)
            return;
        m_dragging = false;
        m_menuVisible = true;
    }

    void released()
    {
        if(!m_hasSelection)
            return;
        m_dragging = false;
        m_menuVisible = true;
    }

    void tap()
    {
        m_hasSelection = false;
        m_dragging = false;
        m_menuVisible = false;
    }

    bool hasSelection() const
    {
        return m_hasSelection;
    }

    bool menuVisible() const
    {
        return m_menuVisible;
    }

private:
    bool m_hasSelection = false;
    bool m_dragging = false;
    bool m_menuVisible = false;
};

}  // namespace application::utility
