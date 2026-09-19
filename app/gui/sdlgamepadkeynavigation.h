#pragma once

#include <QTimer>
#include <QEvent>
#include <QVariantList>

#include "SDL_compat.h"

#include "settings/streamingpreferences.h"

class SdlGamepadKeyNavigation : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList controllers READ controllers NOTIFY controllersChanged)

public:
    SdlGamepadKeyNavigation(StreamingPreferences* prefs);

    ~SdlGamepadKeyNavigation();

    Q_INVOKABLE void enable();

    Q_INVOKABLE void disable();

    Q_INVOKABLE void notifyWindowFocus(bool hasFocus);

    Q_INVOKABLE void setUiNavMode(bool settingsMode);

    Q_INVOKABLE int getConnectedGamepads();

    Q_INVOKABLE void setControllerEnabled(const QString& id, bool enabled);

    Q_INVOKABLE void moveController(const QString& id, int direction);

    QVariantList controllers() const;

signals:
    void controllersChanged();

private:
    enum NavDirection
    {
        ND_NONE,
        ND_UP,
        ND_DOWN,
        ND_LEFT,
        ND_RIGHT
    };

    void sendKey(QEvent::Type type, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool autoRepeat = false);

    void sendDirection(NavDirection direction, bool autoRepeat);

    void startHeldDirection(NavDirection direction, bool fromDpad);

    void updateHeldDirection();

    NavDirection getStickDirection();

    bool isDpadDirectionHeld(NavDirection direction);

    void updateTimerState();

    void addGamepad(SDL_GameController* controller);

    QStringList connectedControllerIdsInOrder() const;

    struct UiGamepad
    {
        SDL_GameController* controller;
        QString id;
        QString name;
        QString metadata;
    };

private slots:
    void onPollingTimerFired();

private:
    StreamingPreferences* m_Prefs;
    QTimer* m_PollingTimer;
    QList<UiGamepad> m_Gamepads;
    bool m_Enabled;
    bool m_UiNavMode;
    bool m_FirstPoll;
    bool m_HasFocus;

    // State for hold-to-repeat of D-pad and analog stick navigation
    NavDirection m_HeldDirection;
    bool m_HeldDirectionFromDpad;
    Uint32 m_HeldDirectionStartTime;
    Uint32 m_LastRepeatTime;
    int m_RepeatCount;
};
