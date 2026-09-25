#pragma once

#include "settings/streamingpreferences.h"
#include "backend/computermanager.h"

#include "SDL_compat.h"
#include "dualsensetriggers.h"


struct GamepadState {
    SDL_GameController* controller;
    bool hapticsAttached;
    SDL_JoystickID jsId;
    short index;

#if !SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_Haptic* haptic;
    int hapticMethod;
    int hapticEffectId;
#endif

    SDL_TimerID mouseEmulationTimer;
    uint32_t lastStartDownTime;

    bool clickpadButtonEmulationEnabled;
    bool emulatedClickpadButtonDown;

#if SDL_VERSION_ATLEAST(2, 0, 14)
    uint8_t gyroReportPeriodMs;
    float lastGyroEventData[SDL_arraysize(SDL_ControllerSensorEvent::data)];
    uint32_t lastGyroEventTime;

    uint8_t accelReportPeriodMs;
    float lastAccelEventData[SDL_arraysize(SDL_ControllerSensorEvent::data)];
    uint32_t lastAccelEventTime;
#endif

    // Last stick direction reported to the gamepad menu, for edge detection
    int menuNavDir;

    // Buttons that were held when the gamepad menu closed. They stay hidden
    // from the host until they're physically released, so the presses that
    // worked the menu don't leak into the game underneath it.
    int suppressedButtons;

    // A button that can open the gamepad menu, held back from the host until
    // we know whether it's opening the menu or meant for the game. The menu
    // opens, or the button is passed on, once heldBackDeadline passes.
    int heldBackButtons;
    uint32_t heldBackDeadline;

    // Buttons still reported as down for a moment after a quick tap of a
    // held back button, since a press and release sent together is too
    // short for some games to notice. Released at tapDeadline.
    int tapButtons;
    uint32_t tapDeadline;

    int buttons;
    short lsX, lsY;
    short rsX, rsY;
    unsigned char lt, rt;
};


// activeGamepadMask is a short, so we're bounded by the number of mask bits
#define MAX_GAMEPADS 16

#define MAX_FINGERS 2

// Pushed by SdlInputHandler's timers for the Session event loop, which calls
// SdlInputHandler::handleGamepadMenuTriggerTimers() on the main thread. This
// shares the SDL_USEREVENT code space with the codes in session.cpp.
#define SDL_CODE_GAMEPAD_MENU_TRIGGER_TIMER 106

#define GAMEPAD_HAPTIC_METHOD_NONE 0
#define GAMEPAD_HAPTIC_METHOD_LEFTRIGHT 1
#define GAMEPAD_HAPTIC_METHOD_SIMPLERUMBLE 2

#define GAMEPAD_HAPTIC_SIMPLE_HIFREQ_MOTOR_WEIGHT 0.33
#define GAMEPAD_HAPTIC_SIMPLE_LOWFREQ_MOTOR_WEIGHT 0.8

class SdlInputHandler
{
public:
    explicit SdlInputHandler(StreamingPreferences& prefs, int streamWidth, int streamHeight);

    ~SdlInputHandler();

    void setWindow(SDL_Window* window);

    void handleKeyEvent(SDL_KeyboardEvent* event);

    void handleMouseButtonEvent(SDL_MouseButtonEvent* event);

    void handleMouseMotionEvent(SDL_MouseMotionEvent* event);

    void handleMouseWheelEvent(SDL_MouseWheelEvent* event);

    void handleControllerAxisEvent(SDL_ControllerAxisEvent* event);

    void handleControllerButtonEvent(SDL_ControllerButtonEvent* event);

    void initializeControllers();

    void handleControllerDeviceEvent(SDL_ControllerDeviceEvent* event);

#if SDL_VERSION_ATLEAST(2, 0, 14)
    void handleControllerSensorEvent(SDL_ControllerSensorEvent* event);

    void handleControllerTouchpadEvent(SDL_ControllerTouchpadEvent* event);
#endif

#if SDL_VERSION_ATLEAST(2, 24, 0)
    void handleJoystickBatteryEvent(SDL_JoyBatteryEvent* event);
#endif

    void handleJoystickArrivalEvent(SDL_JoyDeviceEvent* event);

    void sendText(QString& string);

    void rumble(uint16_t controllerNumber, uint16_t lowFreqMotor, uint16_t highFreqMotor);

    void rumbleTriggers(uint16_t controllerNumber, uint16_t leftTrigger, uint16_t rightTrigger);

    void setMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz);

    void setControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b);

    void setAdaptiveTriggers(uint16_t controllerNumber, DualSenseOutputReport *report);



    void handleTouchFingerEvent(SDL_TouchFingerEvent* event);

    int getAttachedGamepadMask();

    void sendAllGamepadStates();

    void notifyGamepadMenuClosed();

    // Opens the gamepad menu or passes held back buttons on to the host once
    // their deadlines pass
    void handleGamepadMenuTriggerTimers();

    // How the gamepad labels its face buttons, as a ControllerButtonStyle::Style
    int getButtonStyle(SDL_JoystickID jsId);

    bool isMouseEmulationActive(SDL_JoystickID jsId);

    void toggleMouseEmulation(SDL_JoystickID jsId);

    void sendGuideButtonPress(short gamepadIndex);
    int getAttachedPlayStationGamepadMask();

    void raiseAllKeys();

    void notifyMouseLeave();

    void notifyFocusLost();

    void notifyFocusGained();

    bool isCaptureActive();

    bool isSystemKeyCaptureActive();

    void setCaptureActive(bool active);

    bool isMouseInVideoRegion(int mouseX, int mouseY, int windowWidth = -1, int windowHeight = -1);

    void updateKeyboardGrabState();

    void updatePointerRegionLock();

    static
    QString getUnmappedGamepads();

private:
    enum KeyCombo {
        KeyComboQuit,
        KeyComboUngrabInput,
        KeyComboToggleFullScreen,
        KeyComboToggleStatsOverlay,
        KeyComboToggleMouseMode,
        KeyComboToggleCursorHide,
        KeyComboToggleMinimize,
        KeyComboPasteText,
        KeyComboTogglePointerRegionLock,
        KeyComboQuitAndExit,
        KeyComboToggleKeyboardGrab,
        KeyComboMax
    };

    GamepadState*
    findStateForGamepad(SDL_JoystickID id);

    bool isControllerEnabled(const QString& id) const;

    int preferredControllerIndex(const QString& id) const;
    uint8_t moonlightControllerType(SDL_GameController* controller);

    void sendControllerArrival(GamepadState* state);

    void sendGamepadState(GamepadState* state);

    // The buttons a gamepad reports to the host, leaving out any that are
    // hidden because they worked the gamepad menu or might be opening it
    static int hostButtons(const GamepadState* state);

    // Buttons that open the gamepad menu under the current trigger setting,
    // either pressed together or held on their own
    int gamepadMenuTriggerButtons() const;

    // Returns true if the button event was consumed by the gamepad menu trigger
    bool handleGamepadMenuTriggerButton(GamepadState* state, int buttonFlag, bool pressed);

    void openGamepadMenu(GamepadState* state);

    void setMouseEmulation(GamepadState* state, bool enabled);

    void sendGamepadBatteryState(GamepadState* state, SDL_JoystickPowerLevel level);

    void handleAbsoluteFingerEvent(SDL_TouchFingerEvent* event);

    void emulateAbsoluteFingerEvent(SDL_TouchFingerEvent* event);

    void disableTouchFeedback();

    void handleRelativeFingerEvent(SDL_TouchFingerEvent* event);

    void performSpecialKeyCombo(KeyCombo combo);

    static
    Uint32 longPressTimerCallback(Uint32 interval, void* param);

    static
    Uint32 mouseEmulationTimerCallback(Uint32 interval, void* param);

    static
    Uint32 releaseGuideButtonTimerCallback(Uint32 interval, void* param);

    static
    Uint32 gamepadMenuTriggerTimerCallback(Uint32 interval, void* param);

    static
    Uint32 releaseLeftButtonTimerCallback(Uint32 interval, void* param);

    static
    Uint32 releaseRightButtonTimerCallback(Uint32 interval, void* param);

    static
    Uint32 dragTimerCallback(Uint32 interval, void* param);

    SDL_Window* m_Window;
    bool m_MultiController;
    bool m_GamepadMouse;
    bool m_SwapMouseButtons;
    bool m_ReverseScrollDirection;
    bool m_SwapFaceButtons;
    StreamingPreferences::GamepadMenuTrigger m_GamepadMenuTrigger;
    QStringList m_ControllerOrder;
    QStringList m_DisabledControllers;

    bool m_NeedsManualCaptureOnLeave;
    bool m_MouseWasInVideoRegion;
    bool m_PendingMouseButtonsAllUpOnVideoRegionLeave;
    bool m_PointerRegionLockActive;
    bool m_PointerRegionLockToggledByUser;

    int m_GamepadMask;
    GamepadState m_GamepadState[MAX_GAMEPADS];
    QSet<short> m_KeysDown;
    bool m_FakeMouseCaptureActive;
    bool m_KeyboardCaptureActive;
    QString m_OldIgnoreDevices;
    QString m_OldIgnoreDevicesExcept;
    QStringList m_IgnoreDeviceGuids;
    StreamingPreferences::CaptureSysKeysMode m_CaptureSystemKeysMode;
    int m_MouseCursorCapturedVisibilityState;

    struct {
        KeyCombo keyCombo;
        SDL_Keycode keyCode;
        SDL_Scancode scanCode;
        bool enabled;
    } m_SpecialKeyCombos[KeyComboMax];

    SDL_TouchFingerEvent m_LastTouchDownEvent;
    SDL_TouchFingerEvent m_LastTouchUpEvent;
    SDL_TimerID m_LongPressTimer;
    int m_StreamWidth;
    int m_StreamHeight;
    bool m_AbsoluteMouseMode;
    bool m_AbsoluteTouchMode;
    bool m_DisabledTouchFeedback;

    SDL_TimerID m_GuideButtonTimer;
    short m_GuideButtonGamepadIndex;

    SDL_TouchFingerEvent m_TouchDownEvent[MAX_FINGERS];
    SDL_TimerID m_LeftButtonReleaseTimer;
    SDL_TimerID m_RightButtonReleaseTimer;
    SDL_TimerID m_DragTimer;
    char m_DragButton;
    int m_NumFingersDown;

    static const int k_ButtonMap[];
};
