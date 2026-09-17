#include "sdlgamepadkeynavigation.h"

#include <QKeyEvent>
#include <QGuiApplication>
#include <QWindow>

#include "settings/mappingmanager.h"
#include "inputmodetracker.h"

// Holding a D-pad direction or the left stick repeats navigation after an
// initial delay, starting slowly and speeding up the longer it is held.
#define NAV_REPEAT_INITIAL_DELAY 400
#define NAV_REPEAT_START_INTERVAL 100
#define NAV_REPEAT_MIN_INTERVAL 50
#define NAV_REPEAT_INTERVAL_STEP 10

// The stick must pass the press threshold to start navigating, but only needs
// to stay beyond the lower release threshold to keep repeating. This avoids
// stuttering when the stick is held near the threshold.
#define STICK_PRESS_THRESHOLD 20000
#define STICK_RELEASE_THRESHOLD 12000

SdlGamepadKeyNavigation::SdlGamepadKeyNavigation(StreamingPreferences* prefs)
    : m_Prefs(prefs),
      m_Enabled(false),
      m_UiNavMode(false),
      m_FirstPoll(false),
      m_HasFocus(false),
      m_HeldDirection(ND_NONE),
      m_HeldDirectionFromDpad(false),
      m_HeldDirectionStartTime(0),
      m_LastRepeatTime(0),
      m_RepeatCount(0)
{
    m_PollingTimer = new QTimer(this);
    connect(m_PollingTimer, &QTimer::timeout, this, &SdlGamepadKeyNavigation::onPollingTimerFired);
}

SdlGamepadKeyNavigation::~SdlGamepadKeyNavigation()
{
    disable();
}

void SdlGamepadKeyNavigation::enable()
{
    if (m_Enabled) {
        return;
    }

    // We have to initialize and uninitialize this in enable()/disable()
    // because we need to get out of the way of the Session class. If it
    // doesn't get to reinitialize the GC subsystem, it won't get initial
    // arrival events. Additionally, there's a race condition between
    // our QML objects being destroyed and SDL being deinitialized that
    // this solves too.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) failed: %s",
                     SDL_GetError());
        return;
    }

    MappingManager mappingManager;
    mappingManager.applyMappings();

    // Drop all pending gamepad add events. SDL will generate these for us
    // on first init of the GC subsystem. We can't depend on them due to
    // overlapping lifetimes of SdlGamepadKeyNavigation instances, so we
    // will attach ourselves.
    //
    // NB: We use SDL_JoystickUpdate() instead of SDL_PumpEvents() because
    // the latter can do a bit more work that we want (like handling video
    // events that we intentionally do not want to process yet).
    SDL_JoystickUpdate();
    SDL_FlushEvent(SDL_CONTROLLERDEVICEADDED);

    // Open all currently attached game controllers
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* gc = SDL_GameControllerOpen(i);
            if (gc != nullptr) {
                m_Gamepads.append(gc);
            }
        }
    }

    m_Enabled = true;

    // Start the polling timer if the window is focused
    updateTimerState();
}

void SdlGamepadKeyNavigation::disable()
{
    if (!m_Enabled) {
        return;
    }

    m_Enabled = false;
    updateTimerState();
    Q_ASSERT(!m_PollingTimer->isActive());

    while (!m_Gamepads.isEmpty()) {
        SDL_GameControllerClose(m_Gamepads[0]);
        m_Gamepads.removeAt(0);
    }

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

void SdlGamepadKeyNavigation::notifyWindowFocus(bool hasFocus)
{
    m_HasFocus = hasFocus;
    updateTimerState();
}

void SdlGamepadKeyNavigation::onPollingTimerFired()
{
    SDL_Event event;

    // Update joystick state without pumping other events (see enable() comment)
    SDL_JoystickUpdate();

    // Discard any pending button events on the first poll to avoid picking up
    // stale input data from the stream session (like the quit combo).
    if (m_FirstPoll) {
        SDL_FlushEvent(SDL_CONTROLLERBUTTONDOWN);
        SDL_FlushEvent(SDL_CONTROLLERBUTTONUP);
        m_FirstPoll = false;
    }

    // Peep events rather than polling to avoid calling SDL_PumpEvents()
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) == 1) {
        switch (event.type) {
        case SDL_QUIT:
            // SDL may send us a quit event since we initialize
            // the video subsystem on startup. If we get one,
            // forward it on for Qt to take care of.
            QCoreApplication::instance()->quit();
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        {
            QEvent::Type type =
                    event.type == SDL_CONTROLLERBUTTONDOWN ?
                        QEvent::Type::KeyPress : QEvent::Type::KeyRelease;

            // Swap face buttons if needed
            if (m_Prefs->swapFaceButtons) {
                switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_B;
                    break;
                case SDL_CONTROLLER_BUTTON_B:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_A;
                    break;
                case SDL_CONTROLLER_BUTTON_X:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_Y;
                    break;
                case SDL_CONTROLLER_BUTTON_Y:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_X;
                    break;
                }
            }

            switch (event.cbutton.button) {
            // D-pad presses navigate immediately. Releases are detected by
            // polling in updateHeldDirection(), which also handles repeat.
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                if (type == QEvent::Type::KeyPress) {
                    startHeldDirection(ND_UP, true);
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                if (type == QEvent::Type::KeyPress) {
                    startHeldDirection(ND_DOWN, true);
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                if (type == QEvent::Type::KeyPress) {
                    startHeldDirection(ND_LEFT, true);
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                if (type == QEvent::Type::KeyPress) {
                    startHeldDirection(ND_RIGHT, true);
                }
                break;
            case SDL_CONTROLLER_BUTTON_A:
                if (m_UiNavMode) {
                    sendKey(type, Qt::Key_Space);
                }
                else {
                    sendKey(type, Qt::Key_Return);
                }
                break;
            case SDL_CONTROLLER_BUTTON_B:
                sendKey(type, Qt::Key_Escape);
                break;
            case SDL_CONTROLLER_BUTTON_X:
                sendKey(type, Qt::Key_Menu);
                break;
            case SDL_CONTROLLER_BUTTON_Y:
            case SDL_CONTROLLER_BUTTON_START:
                // HACK: We use this keycode to inform main.qml
                // to show the settings when Key_Menu is handled
                // by the control in focus.
                sendKey(type, Qt::Key_Hangup);
                break;
            default:
                break;
            }
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            SDL_GameController* gc = SDL_GameControllerOpen(event.cdevice.which);
            if (gc != nullptr) {
                // SDL_CONTROLLERDEVICEADDED can be reported multiple times for the same
                // gamepad in rare cases, because SDL doesn't fixup the device index in
                // the SDL_CONTROLLERDEVICEADDED event if an unopened gamepad disappears
                // before we've processed the add event.
                if (!m_Gamepads.contains(gc)) {
                    m_Gamepads.append(gc);
                }
                else {
                    // We already have this game controller open
                    SDL_GameControllerClose(gc);
                }
            }
            break;
        }
    }

    // Handle D-pad releases, the analog stick, and hold-to-repeat
    updateHeldDirection();
}

void SdlGamepadKeyNavigation::startHeldDirection(NavDirection direction, bool fromDpad)
{
    sendDirection(direction, false);

    m_HeldDirection = direction;
    m_HeldDirectionFromDpad = fromDpad;
    m_HeldDirectionStartTime = m_LastRepeatTime = SDL_GetTicks();
    m_RepeatCount = 0;
}

void SdlGamepadKeyNavigation::updateHeldDirection()
{
    // Stop repeating once the D-pad direction is released
    if (m_HeldDirection != ND_NONE && m_HeldDirectionFromDpad && !isDpadDirectionHeld(m_HeldDirection)) {
        m_HeldDirection = ND_NONE;
    }

    // The D-pad takes precedence over the stick while it is held
    if (m_HeldDirection == ND_NONE || !m_HeldDirectionFromDpad) {
        NavDirection stickDirection = getStickDirection();
        if (stickDirection != m_HeldDirection) {
            if (stickDirection == ND_NONE) {
                m_HeldDirection = ND_NONE;
            }
            else {
                startHeldDirection(stickDirection, false);
            }
            return;
        }
    }

    if (m_HeldDirection == ND_NONE) {
        return;
    }

    Uint32 now = SDL_GetTicks();
    if (now - m_HeldDirectionStartTime < NAV_REPEAT_INITIAL_DELAY) {
        return;
    }

    // Repeat faster the longer the direction is held
    Uint32 interval = (Uint32)qMax(NAV_REPEAT_MIN_INTERVAL,
                                   NAV_REPEAT_START_INTERVAL - (m_RepeatCount * NAV_REPEAT_INTERVAL_STEP));
    if (now - m_LastRepeatTime >= interval) {
        sendDirection(m_HeldDirection, true);
        m_LastRepeatTime = now;
        m_RepeatCount++;
    }
}

SdlGamepadKeyNavigation::NavDirection SdlGamepadKeyNavigation::getStickDirection()
{
    for (auto gc : std::as_const(m_Gamepads)) {
        int leftX = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        int leftY = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);

        // Keep the current stick direction until the stick returns toward center
        if (m_HeldDirection != ND_NONE && !m_HeldDirectionFromDpad) {
            if ((m_HeldDirection == ND_UP && leftY < -STICK_RELEASE_THRESHOLD) ||
                    (m_HeldDirection == ND_DOWN && leftY > STICK_RELEASE_THRESHOLD) ||
                    (m_HeldDirection == ND_LEFT && leftX < -STICK_RELEASE_THRESHOLD) ||
                    (m_HeldDirection == ND_RIGHT && leftX > STICK_RELEASE_THRESHOLD)) {
                return m_HeldDirection;
            }
        }

        // Otherwise use the dominant axis if it is past the press threshold
        if (qAbs(leftY) >= qAbs(leftX)) {
            if (leftY < -STICK_PRESS_THRESHOLD) {
                return ND_UP;
            }
            else if (leftY > STICK_PRESS_THRESHOLD) {
                return ND_DOWN;
            }
        }
        else {
            if (leftX < -STICK_PRESS_THRESHOLD) {
                return ND_LEFT;
            }
            else if (leftX > STICK_PRESS_THRESHOLD) {
                return ND_RIGHT;
            }
        }
    }

    return ND_NONE;
}

bool SdlGamepadKeyNavigation::isDpadDirectionHeld(NavDirection direction)
{
    SDL_GameControllerButton button;
    switch (direction) {
    case ND_UP:
        button = SDL_CONTROLLER_BUTTON_DPAD_UP;
        break;
    case ND_DOWN:
        button = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
        break;
    case ND_LEFT:
        button = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
        break;
    case ND_RIGHT:
        button = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
        break;
    default:
        return false;
    }

    for (auto gc : std::as_const(m_Gamepads)) {
        if (SDL_GameControllerGetButton(gc, button)) {
            return true;
        }
    }

    return false;
}

void SdlGamepadKeyNavigation::sendDirection(NavDirection direction, bool autoRepeat)
{
    Qt::Key key;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;

    switch (direction) {
    case ND_UP:
        if (m_UiNavMode) {
            // Back-tab
            key = Qt::Key_Tab;
            modifiers = Qt::ShiftModifier;
        }
        else {
            key = Qt::Key_Up;
        }
        break;
    case ND_DOWN:
        key = m_UiNavMode ? Qt::Key_Tab : Qt::Key_Down;
        break;
    case ND_LEFT:
        key = Qt::Key_Left;
        break;
    case ND_RIGHT:
        key = Qt::Key_Right;
        break;
    default:
        return;
    }

    sendKey(QEvent::Type::KeyPress, key, modifiers, autoRepeat);
    sendKey(QEvent::Type::KeyRelease, key, modifiers, autoRepeat);
}

void SdlGamepadKeyNavigation::sendKey(QEvent::Type type, Qt::Key key, Qt::KeyboardModifiers modifiers, bool autoRepeat)
{
    QGuiApplication* app = static_cast<QGuiApplication*>(QGuiApplication::instance());

    // All keys sent from here originate from gamepad input
    if (type == QEvent::Type::KeyPress) {
        InputModeTracker::get()->notifyGamepadInput();
    }

    QWindow* focusWindow = app->focusWindow();
    if (focusWindow != nullptr) {
        QKeyEvent keyPressEvent(type, key, modifiers, QString(), autoRepeat);
        app->sendEvent(focusWindow, &keyPressEvent);
    }
}

void SdlGamepadKeyNavigation::updateTimerState()
{
    if (m_PollingTimer->isActive() && (!m_HasFocus || !m_Enabled)) {
        m_PollingTimer->stop();
    }
    else if (!m_PollingTimer->isActive() && m_HasFocus && m_Enabled) {
        // Flush events on the first poll
        m_FirstPoll = true;

        // Don't resume repeating a direction held before we lost focus
        m_HeldDirection = ND_NONE;

        // Poll every 50 ms for a new joystick event
        m_PollingTimer->start(50);
    }
}

void SdlGamepadKeyNavigation::setUiNavMode(bool uiNavMode)
{
    m_UiNavMode = uiNavMode;
}

int SdlGamepadKeyNavigation::getConnectedGamepads()
{
    Q_ASSERT(m_Enabled);

    int count = 0;
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            count++;
        }
    }

    return count;
}
