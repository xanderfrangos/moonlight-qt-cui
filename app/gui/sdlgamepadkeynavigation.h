#pragma once

#include <QTimer>
#include <QEvent>
#include <QVariantList>
#include <QColor>

#include "SDL_compat.h"

#include "settings/streamingpreferences.h"

class SdlGamepadKeyNavigation : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList controllers READ controllers NOTIFY controllersChanged)
    // How the controller last used to navigate labels its face buttons, as a
    // ControllerButtonStyle::Style
    Q_PROPERTY(int buttonStyle READ buttonStyle NOTIFY buttonStyleChanged)

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

    // Rumbles a controller so the user can tell which one it is
    Q_INVOKABLE void identifyController(const QString& id);

    // The label and color of the face button at a ControllerButtonStyle::FacePosition
    Q_INVOKABLE QString faceButtonGlyph(int style, int position) const;
    Q_INVOKABLE QColor faceButtonColor(int style, int position) const;

    QVariantList controllers() const;

    int buttonStyle() const;

signals:
    void controllersChanged();

    void buttonStyleChanged();

    // A button was pressed on the controller with this ID
    void controllerInput(const QString& id);

private:
    struct UiGamepad
    {
        SDL_GameController* controller;
        QString id;
        QString name;
        QString metadata;
    };

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

    const UiGamepad* findGamepad(SDL_JoystickID instanceId) const;

    const UiGamepad* findGamepad(const QString& id) const;

    void setButtonStyle(int buttonStyle);

    void rumbleForIdentify(const QString& id);

    QStringList connectedControllerIdsInOrder() const;

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
    int m_ButtonStyle;
    // Whether m_ButtonStyle came from a button press, rather than just being
    // the first controller that happened to connect
    bool m_ButtonStyleFromInput;

    // State for hold-to-repeat of D-pad and analog stick navigation
    NavDirection m_HeldDirection;
    bool m_HeldDirectionFromDpad;
    Uint32 m_HeldDirectionStartTime;
    Uint32 m_LastRepeatTime;
    int m_RepeatCount;
};
