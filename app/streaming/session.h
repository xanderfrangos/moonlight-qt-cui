#pragma once

#include <QSemaphore>
#include <QQuickWindow>

#include <Limelight.h>
#include <opus_multistream.h>
#include "settings/streamingpreferences.h"
#include "input/input.h"
#include "video/decoder.h"
#include "audio/renderers/renderer.h"
#include "audio/audiostats.h"
#include "video/overlaymanager.h"
#include "video/clientpacingwarning.h"
#include "diagnostics/diagnosticcapture.h"
#include "displayprobe.h"
#include "video/overlaypainter.h"
#ifdef HAVE_KWIN_SCANOUT_GUARD
#include "streaming/kwinscanoutguard.h"
#endif

class SupportedVideoFormatList : public QList<int>
{
public:
    operator int() const
    {
        int value = 0;

        for (const int v : *this) {
            value |= v;
        }

        return value;
    }

    void
    removeByMask(int mask)
    {
        int i = 0;
        while (i < this->length()) {
            if (this->value(i) & mask) {
                this->removeAt(i);
            }
            else {
                i++;
            }
        }
    }

    void
    deprioritizeByMask(int mask)
    {
        QList<int> deprioritizedList;

        int i = 0;
        while (i < this->length()) {
            if (this->value(i) & mask) {
                deprioritizedList.append(this->takeAt(i));
            }
            else {
                i++;
            }
        }

        this->append(std::move(deprioritizedList));
    }

    int maskByServerCodecModes(int serverCodecModes)
    {
        int mask = 0;

        const QMap<int, int> mapping = {
            {SCM_H264, VIDEO_FORMAT_H264},
            {SCM_H264_HIGH8_444, VIDEO_FORMAT_H264_HIGH8_444},
            {SCM_HEVC, VIDEO_FORMAT_H265},
            {SCM_HEVC_MAIN10, VIDEO_FORMAT_H265_MAIN10},
            {SCM_HEVC_REXT8_444, VIDEO_FORMAT_H265_REXT8_444},
            {SCM_HEVC_REXT10_444, VIDEO_FORMAT_H265_REXT10_444},
            {SCM_AV1_MAIN8, VIDEO_FORMAT_AV1_MAIN8},
            {SCM_AV1_MAIN10, VIDEO_FORMAT_AV1_MAIN10},
            {SCM_AV1_HIGH8_444, VIDEO_FORMAT_AV1_HIGH8_444},
            {SCM_AV1_HIGH10_444, VIDEO_FORMAT_AV1_HIGH10_444},
            {SCM_PYROWAVE, VIDEO_FORMAT_PYROWAVE},
            {SCM_PYROWAVE_444, VIDEO_FORMAT_PYROWAVE_444},
            {SCM_PYROWAVE_HDR10, VIDEO_FORMAT_PYROWAVE_HDR10},
            {SCM_PYROWAVE_HDR10_444, VIDEO_FORMAT_PYROWAVE_HDR10_444},
        };

        for (QMap<int, int>::const_iterator it = mapping.cbegin(); it != mapping.cend(); ++it) {
            if (serverCodecModes & it.key()) {
                mask |= it.value();
                serverCodecModes &= ~it.key();
            }
        }

        // Make sure nobody forgets to update this for new SCM values
        SDL_assert(serverCodecModes == 0);

        int val = *this;
        return val & mask;
    }
};

class Session : public QObject
{
    Q_OBJECT

    friend class SdlInputHandler;
    friend class DeferredSessionCleanupTask;
    friend class AsyncConnectionStartThread;

public:
    explicit Session(NvComputer* computer, NvApp& app, StreamingPreferences *preferences = nullptr);
    virtual ~Session();

    Q_INVOKABLE bool initialize(QQuickWindow* qtWindow);
    Q_INVOKABLE void start();
    Q_INVOKABLE void interrupt();
    Q_PROPERTY(QStringList launchWarnings MEMBER m_LaunchWarnings NOTIFY launchWarningsChanged);

    // Whether the stream ended to start again with settings changed from the
    // gamepad menu that the host only reads when a stream starts
    Q_INVOKABLE bool isRestartRequested() const
    {
        return m_RestartRequested;
    }

    // A session that resumes this one's app with the saved settings, for a
    // restart. The caller owns it.
    Q_INVOKABLE Session* createRestartSession();

    static
    void getDecoderInfo(SDL_Window* window,
                        bool& isHardwareAccelerated, bool& isFullScreenOnly,
                        bool& isHdrSupported, QSize& maxResolution);
    static bool supportsIntraRefresh(SDL_Window* window,
                                     StreamingPreferences::VideoDecoderSelection decoderSelection,
                                     StreamingPreferences::VideoCodecConfig codecConfig);

    static Session* get()
    {
        return s_ActiveSession;
    }

    Overlay::OverlayManager& getOverlayManager()
    {
        return m_OverlayManager;
    }

    AudioStats& getAudioStats()
    {
        return m_AudioStats;
    }

    // Shows or hides the configured arrangement of stats overlays.
    void toggleStatsOverlay();
    // Which stats graphs to draw and how, from the user's preferences
    Overlay::StatsGraphConfig getStatsGraphConfig() const;
    // Size of the stream window in the pixel units renderers draw in
    static void getWindowPixelSize(SDL_Window* window, int& width, int& height);
    bool clientPacingWarningsEnabled() const { return m_Preferences->connectionWarnings && !m_Preferences->hideVrrBufferWarnings; }
    ClientPacingWarning& clientPacingWarning() { return m_ClientPacingWarning; }
    bool hevcPacingAlternative() const { return m_HevcPacingAlternative; }

    int streamColorRange() const
    {
        return m_StreamConfig.colorRange;
    }

    QString vrrCalibrationContext() const;

    void flushWindowEvents();

    void setShouldExit(bool quitHostApp = false);

signals:
    void stageStarting(QString stage);

    void stageFailed(QString stage, int errorCode, QString failingPorts);

    void connectionStarted();

    void displayLaunchError(QString text);

    void quitStarting();

    void sessionFinished(int portTestResult);

    // Emitted after sessionFinished() when the session is ready to be destroyed
    void readyForDeletion();

    void launchWarningsChanged();

private:
    void setStatsOverlayEnabled(bool enabled);
    void exec();

    bool startConnectionAsync();

    bool validateLaunch(SDL_Window* testWindow);

    void emitLaunchWarning(QString text);

    bool populateDecoderProperties(SDL_Window* window);

    struct PacingSettings {
        bool effectiveVsync = false;
        bool enableFramePacing = false;
        bool enableVrr = false;
        TimestampPacingOptions timestampPacing;
        int refreshRate = 0;
    };

    // How the preferences' V-Sync, VRR Pacing Mode and timestamp pacing work
    // out on the window's display: the stream's start takes them, and the
    // gamepad menu takes them again when one changes. log reports why VRR
    // isn't used.
    PacingSettings resolvePacingSettings(SDL_Window* window, bool log) const;

    // Whether VRR Pacing Mode could run on the window's display at the
    // stream's frame rate, if it were on
    bool canUseVrr(SDL_Window* window, bool effectiveVsync, bool log) const;

    void snapshotPresentationSettings(SDL_Window* window);

    // Replaces the stream's resolution and frame rate with the target
    // display's current mode where the Native options are selected
    void applyNativeStreamMode();

    IAudioRenderer* createAudioRenderer(const POPUS_MULTISTREAM_CONFIGURATION opusConfig);

    bool initializeAudioRenderer();

    bool testAudio(int audioConfiguration);

    int getAudioRendererCapabilities(int audioConfiguration);

    void getWindowDimensions(int& x, int& y,
                             int& width, int& height);

    void toggleFullscreen();

    void notifyMouseEmulationMode(bool enabled);

    void refreshStatusOverlay();

    enum GamepadMenuPage {
        GamepadMenuPageMain,
        GamepadMenuPageAdvanced,
        // Settings that can change during the stream. These pages sit to one
        // side and leave the stream undimmed, so changes can be judged.
        GamepadMenuPageSettings,
        GamepadMenuPageVsync,
        // The values to choose from for one setting
        GamepadMenuPageChoices,
    };

    enum GamepadMenuItem {
        GamepadMenuDisconnect,
        GamepadMenuEndSession,
        GamepadMenuToggleStats,
        GamepadMenuToggleMouse,
        GamepadMenuPressGuide,
        GamepadMenuAdvanced,
        GamepadMenuChangeSettings,
        GamepadMenuVsyncSettings,
        GamepadMenuBitrate,
        GamepadMenuDownscalingFilter,
        GamepadMenuFsr1,
        GamepadMenuFsr1Sharpness,
        GamepadMenuLs1,
        GamepadMenuLs1Sharpness,
        GamepadMenuDithering,
        GamepadMenuDitherGrain,
        GamepadMenuDeband,
        GamepadMenuVsync,
        GamepadMenuVrr,
        GamepadMenuTimestampPacing,
        // A value on a choices page
        GamepadMenuChoice,
    };

    struct GamepadMenuRow {
        GamepadMenuItem item;
        Overlay::Painter::MenuItem display;
        // What a GamepadMenuChoice row picks
        int value = 0;
    };

    // An open page of the gamepad menu and what's selected on it
    struct GamepadMenuLevel {
        GamepadMenuPage page;
        // The setting a choices page picks a value for
        GamepadMenuItem setting;
        // Selected row
        int index;
        // First row shown, on a page with more rows than fit
        int firstVisible;
    };

    // The rows a page of the gamepad menu shows, in order. Settings that
    // would make no difference to this stream are left out.
    QVector<GamepadMenuRow> gamepadMenuRows(const GamepadMenuLevel& level) const;

    QVector<GamepadMenuRow> gamepadMenuChoices(GamepadMenuItem setting) const;

    QString gamepadMenuItemLabel(GamepadMenuItem item) const;

    bool isGamepadMenuOpen() const
    {
        return m_GamepadMenuOpen;
    }

    // jsId is the gamepad that opened the menu, whose button labels it shows
    void openGamepadMenu(short gamepadIndex, SDL_JoystickID jsId);

    void closeGamepadMenu();

    // Returns to the previous page, or closes the menu from the first one
    void gamepadMenuBack();

    void pushGamepadMenuPage(GamepadMenuPage page, GamepadMenuItem setting = GamepadMenuChoice);

    void moveGamepadMenuSelection(int delta);

    void activateGamepadMenuSelection();

    // Changes a setting from the settings pages of the gamepad menu
    void activateGamepadMenuSetting(GamepadMenuItem item, int value);

    // Saves a setting changed in m_Preferences and applies it to the stream:
    // the renderer is rebuilt with it, or the stream restarts if the host
    // needs to know (the bitrate, and whether VRR Pacing Mode runs).
    void applyStreamSetting(StreamingPreferences::StreamSetting setting);

    // Ends the stream for the UI to start it again
    void restartStream();

    // Whether the stream is drawn larger or smaller than it's decoded, which
    // decides whether the upscalers or the downscaling filter do anything
    bool isStreamUpscaled() const;
    bool isStreamDownscaled() const;

    // The bitrate the settings page would pick for this stream's mode
    int getDefaultStreamBitrate() const;

    QVector<int> getBitrateChoices() const;

    void updateOptimalWindowDisplayMode();

    enum class DecoderAvailability {
        None,
        Software,
        Hardware
    };

    static
    DecoderAvailability getDecoderAvailability(SDL_Window* window,
                                               StreamingPreferences::VideoDecoderSelection vds,
                                               int videoFormat, int width, int height, int frameRate);

    static
    bool chooseDecoder(StreamingPreferences::VideoDecoderSelection vds,
                       StreamingPreferences::RendererSelection renderer,
                       SDL_Window* window, int videoFormat, int width, int height,
                       int frameRate, bool enableVsync, bool enableFramePacing,
                       bool testOnly, IVideoDecoder*& chosenDecoder,
                       bool enableVrr = false, bool preferVrrRenderer = false,
                       bool fsr1Upscaling = false,
                       double fsr1RcasSharpness = 20.0,
                       bool ls1Upscaling = false,
                       int ls1Sharpness = 0,
                       int downscalingFilter = StreamingPreferences::DF_BILINEAR,
                       int vrrDisplayRefreshHz = 0,
                       bool* effectiveVrr = nullptr, bool smoothVrrFrameTiming = true,
                       bool gamescopeMailbox = false, int vrrLatencyMode = 0,
                       bool gamescopeRepaint = false,
                       VrrTimingOptions vrrTimingOptions = {},
                       bool absorbVrrMicrostutter = false, int ditheringMode = 0,
                       bool temporalDithering = false, int debandMode = 0,
                       int ditherGrainMode = 0,
                       TimestampPacingOptions timestampPacing = {});

    static
    void clStageStarting(int stage);

    static
    void clStageFailed(int stage, int errorCode);

    static
    void clConnectionTerminated(int errorCode);

    static
    void clLogMessage(const char* format, ...);

    static
    void clRumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor);

    static
    void clConnectionStatusUpdate(int connectionStatus);

    static
    void clSetHdrMode(bool enabled);

    static
    void clRumbleTriggers(uint16_t controllerNumber, uint16_t leftTrigger, uint16_t rightTrigger);

    static
    void clSetMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz);

    static
    void clSetControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b);

    static
    void clSetAdaptiveTriggers(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight, uint8_t *left, uint8_t *right);

    static
    int arInit(int audioConfiguration,
               const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
               void* arContext, int arFlags);

    static
    void arCleanup();

    static
    int arDecodeForRenderer(void* context, const unsigned char* data, int length,
                            float* pcm, int frameCount);

    static
    void arDecodeAndPlaySample(char* sampleData, int sampleLength);

    static
    int drSetup(int videoFormat, int width, int height, int frameRate, void*, int);

    static
    void drCleanup();

    static
    int drSubmitDecodeUnit(PDECODE_UNIT du);

    struct PresentationSettings {
        bool effectiveVsync = false;
        bool enableFramePacing = false;
        bool enableVrr = false;
        // Pacing to smoothed host timestamps. Never enabled with VRR.
        TimestampPacingOptions timestampPacing;
        int vrrLatencyMode = 0;
        VrrTimingOptions vrrTimingOptions;
        bool gamescopeMailbox = false;
        bool gamescopeRepaint = false;
        bool smoothVrrFrameTiming = true;
        bool absorbVrrMicrostutter = false;
        int ditheringMode = 0;
        bool temporalDithering = false;
        int ditherGrainMode = 0;
        int debandMode = 0;
        bool fsr1Upscaling = false;
        int downscalingFilter = StreamingPreferences::DF_BILINEAR;
        double fsr1RcasSharpness = 20.0;
        bool ls1Upscaling = false;
        int ls1Sharpness = 0;
        int refreshRate = 0;
        StreamingPreferences::WindowMode effectiveWindowMode = StreamingPreferences::WM_WINDOWED;
        StreamingPreferences::VideoDecoderSelection decoderSelection = StreamingPreferences::VDS_AUTO;
        StreamingPreferences::RendererSelection rendererSelection = StreamingPreferences::RS_AUTO;
    };

    // The PC's own settings, when it has any and the caller didn't pass
    // preferences. Must be declared before m_Preferences, which points to it.
    std::unique_ptr<StreamingPreferences> m_HostPreferences;
    // Preferences the caller passed in (the command line's), if any
    StreamingPreferences* m_CallerPreferences;
    StreamingPreferences* m_Preferences;
    ClientPacingWarning m_ClientPacingWarning;
    PresentationSettings m_PresentationSettings;
    std::unique_ptr<DiagnosticCapture> m_DiagnosticCapture;
    DisplayProbe::StreamSampler m_DisplayProbe;
#ifdef HAVE_KWIN_SCANOUT_GUARD
    std::unique_ptr<KWinScanoutGuard> m_ScanoutGuard;
#endif
    bool m_IsFullScreen;
    bool m_HevcPacingAlternative = false;
    SupportedVideoFormatList m_SupportedVideoFormats; // Sorted in order of descending priority
    STREAM_CONFIGURATION m_StreamConfig;
    DECODER_RENDERER_CALLBACKS m_VideoCallbacks;
    AUDIO_RENDERER_CALLBACKS m_AudioCallbacks;
    NvComputer* m_Computer;
    NvApp m_App;
    SDL_Window* m_Window;
    IVideoDecoder* m_VideoDecoder;
    SDL_mutex* m_DecoderLock;
    bool m_AudioDisabled;
    bool m_AudioMuted;
    Uint32 m_FullScreenFlag;
    QQuickWindow* m_QtWindow;
    bool m_UnexpectedTermination;
    SdlInputHandler* m_InputHandler;
    int m_MouseEmulationRefCount;
    bool m_GamepadMenuOpen;
    // The open pages, the one showing last
    QVector<GamepadMenuLevel> m_GamepadMenuStack;
    short m_GamepadMenuGamepadIndex;
    SDL_JoystickID m_GamepadMenuJsId;
    // The VIDEO_OPTION_* flags of the current renderer
    int m_VideoOptions = 0;
    // Whether the host was told this client runs VRR Pacing Mode, which it
    // reads when the stream starts
    bool m_HostVrrRequested = false;
    bool m_RestartRequested = false;
    // Resume the app even if the PC hasn't reported it running yet, as on a
    // restart soon after it launched
    bool m_ResumeApp = false;
    int m_FlushingWindowEventsRef;
    QStringList m_LaunchWarnings;
    QString m_HostLaunchWarning;
    bool m_ShouldExit;
    bool m_QuitHostAppRequested;

    bool m_AsyncConnectionSuccess;
    int m_PortTestResults;
    Uint32 m_ConnectionStartedAtTicks;

    int m_ActiveVideoFormat;
    int m_ActiveVideoWidth;
    int m_ActiveVideoHeight;
    int m_ActiveVideoFrameRate;

    OpusMSDecoder* m_OpusDecoder;
    IAudioRenderer* m_AudioRenderer;
    OPUS_MULTISTREAM_CONFIGURATION m_ActiveAudioConfig;
    OPUS_MULTISTREAM_CONFIGURATION m_OriginalAudioConfig;
    int m_AudioSampleCount;
    Uint32 m_DropAudioEndTime;
    AudioStats m_AudioStats;

    Overlay::OverlayManager m_OverlayManager;

    static CONNECTION_LISTENER_CALLBACKS k_ConnCallbacks;
    static Session* s_ActiveSession;
    static QSemaphore s_ActiveSessionSemaphore;
};
