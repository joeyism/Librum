#include <gtest/gtest.h>
#include "touch_selection_session.hpp"

using application::utility::TouchSelectionSession;

namespace tests::application
{

TEST(ATouchSelectionSession, LongPressShowsMenuAndSelects)
{
    TouchSelectionSession session;
    session.longPress();

    EXPECT_TRUE(session.hasSelection());
    EXPECT_TRUE(session.menuVisible());
}

TEST(ATouchSelectionSession, MenuStaysVisibleAfterRelease)
{
    TouchSelectionSession session;
    session.longPress();
    session.released();

    EXPECT_TRUE(session.hasSelection());
    EXPECT_TRUE(session.menuVisible());
}

TEST(ATouchSelectionSession, DragHidesMenuThenShowsItWhenStopped)
{
    TouchSelectionSession session;
    session.longPress();
    session.dragStarted();

    EXPECT_TRUE(session.hasSelection());
    EXPECT_FALSE(session.menuVisible());

    session.dragStopped();

    EXPECT_TRUE(session.hasSelection());
    EXPECT_TRUE(session.menuVisible());
}

TEST(ATouchSelectionSession, MenuStaysVisibleAfterDragThenRelease)
{
    TouchSelectionSession session;
    session.longPress();
    session.dragStarted();
    session.dragStopped();
    session.released();

    EXPECT_TRUE(session.hasSelection());
    EXPECT_TRUE(session.menuVisible());
}

TEST(ATouchSelectionSession, TapClearsSelectionAndMenu)
{
    TouchSelectionSession session;
    session.longPress();
    session.tap();

    EXPECT_FALSE(session.hasSelection());
    EXPECT_FALSE(session.menuVisible());
}

TEST(ATouchSelectionSession, NewLongPressReplacesPreviousSelection)
{
    TouchSelectionSession session;
    session.longPress();
    session.longPress();

    EXPECT_TRUE(session.hasSelection());
    EXPECT_TRUE(session.menuVisible());
}

TEST(ATouchSelectionSession, DragWithoutSelectionDoesNothing)
{
    TouchSelectionSession session;
    session.dragStarted();
    session.dragStopped();
    session.released();

    EXPECT_FALSE(session.hasSelection());
    EXPECT_FALSE(session.menuVisible());
}

}  // namespace tests::application
