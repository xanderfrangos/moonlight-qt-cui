#include "inputmodetracker.h"

#include <QCursor>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQuickWindow>

// Mouse movement smaller than this (in logical pixels) since the last gamepad
// input is treated as jitter or a synthetic event and does not leave gamepad mode.
#define MOUSE_MOVE_THRESHOLD 10

InputModeTracker* InputModeTracker::get()
{
    static InputModeTracker* s_Instance = nullptr;
    if (s_Instance == nullptr) {
        s_Instance = new InputModeTracker(QCoreApplication::instance());
    }
    return s_Instance;
}

InputModeTracker::InputModeTracker(QObject* parent)
    : QObject(parent),
      m_GamepadActive(false),
      m_CursorOverridden(false)
{
    QCoreApplication::instance()->installEventFilter(this);
}

bool InputModeTracker::isGamepadActive() const
{
    return m_GamepadActive;
}

void InputModeTracker::notifyGamepadInput()
{
    setGamepadActive(true);
}

void InputModeTracker::setGamepadActive(bool gamepadActive)
{
    if (m_GamepadActive == gamepadActive) {
        return;
    }

    m_GamepadActive = gamepadActive;

    if (gamepadActive) {
        m_CursorPosAtGamepadInput = QCursor::pos();

        if (!m_CursorOverridden) {
            QGuiApplication::setOverrideCursor(Qt::BlankCursor);
            m_CursorOverridden = true;
        }

        clearHover();
    }
    else if (m_CursorOverridden) {
        QGuiApplication::restoreOverrideCursor();
        m_CursorOverridden = false;
    }

    emit gamepadActiveChanged();
}

void InputModeTracker::clearHover()
{
    // A Leave event makes Qt Quick drop the hovered state of every item in the
    // window and forget the last cursor position, so hover is not re-evaluated
    // as items move underneath the stationary (now hidden) cursor.
    const auto windows = QGuiApplication::allWindows();
    for (QWindow* window : windows) {
        if (qobject_cast<QQuickWindow*>(window) != nullptr) {
            QEvent leaveEvent(QEvent::Leave);
            QCoreApplication::sendEvent(window, &leaveEvent);
        }
    }
}

bool InputModeTracker::eventFilter(QObject* watched, QEvent* event)
{
    // Only look at events delivered to windows. Qt Quick re-sends pointer
    // events to individual items, and those must not be double counted.
    if (!m_GamepadActive || !watched->isWindowType()) {
        return false;
    }

    switch (event->type()) {
    case QEvent::MouseMove:
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QPoint globalPos = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
#else
        QPoint globalPos = static_cast<QMouseEvent*>(event)->globalPos();
#endif
        if ((globalPos - m_CursorPosAtGamepadInput).manhattanLength() > MOUSE_MOVE_THRESHOLD) {
            // The user is really using the mouse again
            setGamepadActive(false);
            return false;
        }

        // Swallow spurious moves (like those generated when the cursor shape
        // changes) so they can't re-establish hover under the hidden cursor.
        return true;
    }

    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TabletPress:
        setGamepadActive(false);
        return false;

    default:
        return false;
    }
}
