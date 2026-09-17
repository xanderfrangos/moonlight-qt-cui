#pragma once

#include <QObject>
#include <QPoint>

// Tracks whether the user is currently driving the UI with a gamepad or with
// a pointing device. While a gamepad is in use, the mouse cursor is hidden and
// Qt Quick hover state is cleared so that only the focused control appears
// selected. Any real mouse movement, click, scroll, or touch switches back.
class InputModeTracker : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool gamepadActive READ isGamepadActive NOTIFY gamepadActiveChanged)

public:
    static InputModeTracker* get();

    bool isGamepadActive() const;

    // Called by SdlGamepadKeyNavigation whenever it translates gamepad input
    void notifyGamepadInput();

signals:
    void gamepadActiveChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit InputModeTracker(QObject* parent = nullptr);

    void setGamepadActive(bool gamepadActive);

    void clearHover();

    bool m_GamepadActive;
    bool m_CursorOverridden;
    QPoint m_CursorPosAtGamepadInput;
};
