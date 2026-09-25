#pragma once

#include <QObject>
#include <QRect>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>
#include <QVector>

class StreamingPreferences : public QObject
{
    Q_OBJECT

public:
    static StreamingPreferences* get(QQmlEngine *qmlEngine = nullptr);

    Q_INVOKABLE static int
    getDefaultBitrate(int width, int height, int fps, bool yuv444);

    Q_INVOKABLE void save();

    void reload();

    // These preferences must be applied before the QGuiApplication is created,
    // so they can be read directly from storage without a preferences instance.
    static int loadUiScale();
    static bool loadTvMode();

    enum AudioConfig
    {
        AC_STEREO,
        AC_51_SURROUND,
        AC_71_SURROUND
    };
    Q_ENUM(AudioConfig)

    enum VideoCodecConfig
    {
        VCC_AUTO,
        VCC_FORCE_H264,
        VCC_FORCE_HEVC,
        VCC_FORCE_HEVC_HDR_DEPRECATED, // Kept for backwards compatibility
        VCC_FORCE_AV1
    };
    Q_ENUM(VideoCodecConfig)

    enum VideoDecoderSelection
    {
        VDS_AUTO,
        VDS_FORCE_HARDWARE,
        VDS_FORCE_SOFTWARE
    };
    Q_ENUM(VideoDecoderSelection)

    // Persisted IDs also identify the VRR controller's timing profile. Keep
    // the numeric values stable when changing the user-facing names.
    enum VrrLatencyMode
    {
        VLM_SMOOTH = 0,
        VLM_BALANCED_TARGET = 1,
        VLM_LOW_LATENCY = 2,

        // Source compatibility for code using the former profile names.
        VLM_SMOOTHEST = VLM_SMOOTH,
        VLM_BALANCED = VLM_BALANCED_TARGET,
        VLM_LOWEST_LATENCY = VLM_LOW_LATENCY
    };
    Q_ENUM(VrrLatencyMode)

    // Mac only (for now)
    enum RendererSelection
    {
        RS_PROBE_ONLY = -1, // Only valid for probing decoder properties
        RS_AUTO,
        RS_VULKAN,
        RS_METAL,
        RS_AVSBDL
    };
    Q_ENUM(RendererSelection)

    enum WindowMode
    {
        WM_FULLSCREEN,
        WM_FULLSCREEN_DESKTOP,
        WM_WINDOWED
    };
    Q_ENUM(WindowMode)

    enum UIDisplayMode
    {
        UI_WINDOWED,
        UI_MAXIMIZED,
        UI_FULLSCREEN
    };
    Q_ENUM(UIDisplayMode)

    // New entries must go at the end of the enum
    // to avoid renumbering existing entries (which
    // would affect existing user preferences).
    enum Language
    {
        LANG_AUTO,
        LANG_EN,
        LANG_FR,
        LANG_ZH_CN,
        LANG_DE,
        LANG_NB_NO,
        LANG_RU,
        LANG_ES,
        LANG_JA,
        LANG_VI,
        LANG_TH,
        LANG_KO,
        LANG_HU,
        LANG_NL,
        LANG_SV,
        LANG_TR,
        LANG_UK,
        LANG_ZH_TW,
        LANG_PT,
        LANG_PT_BR,
        LANG_EL,
        LANG_IT,
        LANG_HI,
        LANG_PL,
        LANG_CS,
        LANG_HE,
        LANG_CKB,
        LANG_LT,
        LANG_ET,
        LANG_BG,
        LANG_EO,
        LANG_TA,
    };
    Q_ENUM(Language);

    // Dithering kernel, ordered from cheapest to highest quality. Persisted
    // IDs must stay stable when changing the user-facing names.
    enum DitheringMode
    {
        DM_OFF = 0,
        DM_ORDERED = 1,
        DM_BLUE_NOISE = 2,
        DM_ERROR_DIFFUSION = 3,
        DM_ERROR_DIFFUSION_HQ = 4,
    };
    Q_ENUM(DitheringMode)

    // Debanding strength. Persisted IDs must stay stable when changing the
    // user-facing names.
    enum DebandMode
    {
        DB_OFF = 0,
        DB_GRAIN_ONLY = 1,
        DB_LIGHT = 2,
        DB_MEDIUM = 3,
        DB_STRONG = 4,
    };
    Q_ENUM(DebandMode)

    enum CaptureSysKeysMode
    {
        CSK_OFF,
        CSK_FULLSCREEN,
        CSK_ALWAYS,
    };
    Q_ENUM(CaptureSysKeysMode);

    // What opens the in-stream gamepad menu. Start+Select+L1+R1 always works
    // as well. Persisted IDs must stay stable.
    enum GamepadMenuTrigger
    {
        GMT_COMBO = 0,
        GMT_START_SELECT = 1,
        GMT_HOLD_SELECT = 2,
        GMT_HOLD_START = 3,
    };
    Q_ENUM(GamepadMenuTrigger);

    // Persisted IDs must stay stable when changing the user-facing names.
    enum PerformanceOverlayMode
    {
        POM_TEXT_ONLY = 0,
        POM_TEXT_AND_GRAPHS = 1,
        POM_GRAPHS_ONLY = 2,
    };
    Q_ENUM(PerformanceOverlayMode)

    // Bit positions in performanceGraphsToggled. These are persisted, so they
    // must stay stable when graphs are renamed or reordered on screen.
    enum PerformanceGraph
    {
        PG_INCOMING_FRAMETIME = 0,
        PG_BANDWIDTH = 1,
        PG_NETWORK_LATENCY = 2,
        PG_NETWORK_JITTER = 3,
        PG_NETWORK_DROPS = 4,
        PG_RENDERING_FRAMETIME = 5,
        PG_HOST_PROCESSING_LATENCY = 6,
        PG_REASSEMBLY = 7,
        PG_QUEUE_DEPTH = 8,
        PG_JITTER_DROPS = 9,
        PG_DECODING_FRAMETIME = 10,
        PG_DECODING_TIME = 11,
        PG_RENDERING_TIME = 12,
    };
    Q_ENUM(PerformanceGraph)

    enum PerformanceGraphType
    {
        PGT_NETWORK,
        PGT_CLIENT
    };
    Q_ENUM(PerformanceGraphType)

    // Any other value of performanceGraphSize is a fixed scale in percent.
    enum PerformanceGraphSize
    {
        PGS_AUTO = 0,
    };
    Q_ENUM(PerformanceGraphSize)

    // Persisted IDs must stay stable when changing the user-facing names.
    enum PerformanceGraphHeight
    {
        PGH_COMPACT = 0,
        PGH_NORMAL = 1,
        PGH_TALL = 2,
    };
    Q_ENUM(PerformanceGraphHeight)

    // Which side of the stream the graphs sit on. The text stats take the
    // other side. Persisted IDs must stay stable.
    enum PerformanceGraphPosition
    {
        PGP_RIGHT = 0,
        PGP_LEFT = 1,
    };
    Q_ENUM(PerformanceGraphPosition)

    // The one list of performance graphs, in the order the settings page
    // shows them. The stream lays them out in its own order, but takes their
    // names from here so the two can't drift apart.
    struct PerformanceGraphInfo {
        PerformanceGraph id;
        // Untranslated. Use performanceGraphName() for display.
        const char* name;
        PerformanceGraphType type;
        bool defaultVisible;
    };
    static const QVector<PerformanceGraphInfo>& performanceGraphs();
    static QString performanceGraphName(int id);

    // Each entry is a map with "bit" and "text" for QML
    Q_INVOKABLE static QVariantList getPerformanceGraphs(PerformanceGraphType type);

    Q_PROPERTY(int width MEMBER width NOTIFY displayModeChanged)
    Q_PROPERTY(int height MEMBER height NOTIFY displayModeChanged)
    Q_PROPERTY(int fps MEMBER fps NOTIFY displayModeChanged)
    Q_PROPERTY(int bitrateKbps MEMBER bitrateKbps NOTIFY bitrateChanged)
    Q_PROPERTY(bool unlockBitrate MEMBER unlockBitrate NOTIFY unlockBitrateChanged)
    Q_PROPERTY(bool autoAdjustBitrate MEMBER autoAdjustBitrate NOTIFY autoAdjustBitrateChanged)
    Q_PROPERTY(bool enableVsync MEMBER enableVsync NOTIFY enableVsyncChanged)
    Q_PROPERTY(bool enableVrr MEMBER enableVrr NOTIFY enableVrrChanged)
    Q_PROPERTY(int vrrLatencyMode MEMBER vrrLatencyMode NOTIFY vrrLatencyModeChanged)
    Q_PROPERTY(bool smoothVrrFrameTiming MEMBER smoothVrrFrameTiming NOTIFY smoothVrrFrameTimingChanged)
    Q_PROPERTY(bool traceVrrFrames MEMBER traceVrrFrames NOTIFY traceVrrFramesChanged)
    Q_PROPERTY(bool exportingDiagnostics MEMBER m_ExportingDiagnostics NOTIFY diagnosticsChanged)
    Q_PROPERTY(QString diagnosticsStatus MEMBER m_DiagnosticsStatus NOTIFY diagnosticsChanged)
    Q_PROPERTY(bool gameOptimizations MEMBER gameOptimizations NOTIFY gameOptimizationsChanged)
    Q_PROPERTY(bool playAudioOnHost MEMBER playAudioOnHost NOTIFY playAudioOnHostChanged)
    Q_PROPERTY(bool multiController MEMBER multiController NOTIFY multiControllerChanged)
    Q_PROPERTY(bool enableMdns MEMBER enableMdns NOTIFY enableMdnsChanged)
    Q_PROPERTY(bool quitAppAfter MEMBER quitAppAfter NOTIFY quitAppAfterChanged)
    Q_PROPERTY(bool absoluteMouseMode MEMBER absoluteMouseMode NOTIFY absoluteMouseModeChanged)
    Q_PROPERTY(bool absoluteTouchMode MEMBER absoluteTouchMode NOTIFY absoluteTouchModeChanged)
    Q_PROPERTY(bool framePacing MEMBER framePacing NOTIFY framePacingChanged)
    Q_PROPERTY(bool connectionWarnings MEMBER connectionWarnings NOTIFY connectionWarningsChanged)
    Q_PROPERTY(bool configurationWarnings MEMBER configurationWarnings NOTIFY configurationWarningsChanged)
    Q_PROPERTY(bool richPresence MEMBER richPresence NOTIFY richPresenceChanged)
    Q_PROPERTY(bool gamepadMouse MEMBER gamepadMouse NOTIFY gamepadMouseChanged)
    Q_PROPERTY(bool detectNetworkBlocking MEMBER detectNetworkBlocking NOTIFY detectNetworkBlockingChanged)
    Q_PROPERTY(bool showPerformanceOverlay MEMBER showPerformanceOverlay NOTIFY showPerformanceOverlayChanged)
    Q_PROPERTY(int performanceOverlayMode MEMBER performanceOverlayMode NOTIFY performanceOverlayModeChanged)
    Q_PROPERTY(int performanceGraphsToggled MEMBER performanceGraphsToggled NOTIFY performanceGraphsToggledChanged)
    Q_PROPERTY(int performanceGraphsDefault READ getPerformanceGraphsDefault CONSTANT)
    Q_PROPERTY(int performanceGraphSize MEMBER performanceGraphSize NOTIFY performanceGraphSizeChanged)
    Q_PROPERTY(int performanceGraphHeight MEMBER performanceGraphHeight NOTIFY performanceGraphHeightChanged)
    Q_PROPERTY(int performanceGraphOpacity MEMBER performanceGraphOpacity NOTIFY performanceGraphOpacityChanged)
    Q_PROPERTY(int performanceGraphHistory MEMBER performanceGraphHistory NOTIFY performanceGraphHistoryChanged)
    Q_PROPERTY(int performanceGraphPosition MEMBER performanceGraphPosition NOTIFY performanceGraphPositionChanged)
    Q_PROPERTY(AudioConfig audioConfig MEMBER audioConfig NOTIFY audioConfigChanged)
    Q_PROPERTY(VideoCodecConfig videoCodecConfig MEMBER videoCodecConfig NOTIFY videoCodecConfigChanged)
    Q_PROPERTY(bool enableHdr MEMBER enableHdr NOTIFY enableHdrChanged)
    Q_PROPERTY(bool enableYUV444 MEMBER enableYUV444 NOTIFY enableYUV444Changed)
    Q_PROPERTY(int ditheringMode MEMBER ditheringMode NOTIFY ditheringModeChanged)
    Q_PROPERTY(bool temporalDithering MEMBER temporalDithering NOTIFY temporalDitheringChanged)
    Q_PROPERTY(int debandMode MEMBER debandMode NOTIFY debandModeChanged)
    Q_PROPERTY(bool fsr1Upscaling MEMBER fsr1Upscaling NOTIFY fsr1UpscalingChanged)
    Q_PROPERTY(double fsr1RcasSharpness MEMBER fsr1RcasSharpness NOTIFY fsr1RcasSharpnessChanged)
    Q_PROPERTY(bool ls1Upscaling MEMBER ls1Upscaling NOTIFY ls1UpscalingChanged)
    Q_PROPERTY(int ls1Sharpness MEMBER ls1Sharpness NOTIFY ls1SharpnessChanged)
    Q_PROPERTY(QString ls1DllPath MEMBER ls1DllPath NOTIFY ls1DllPathChanged)
    Q_PROPERTY(VideoDecoderSelection videoDecoderSelection MEMBER videoDecoderSelection NOTIFY videoDecoderSelectionChanged)
    Q_PROPERTY(RendererSelection rendererSelection MEMBER rendererSelection NOTIFY rendererSelectionChanged)
    Q_PROPERTY(WindowMode windowMode MEMBER windowMode NOTIFY windowModeChanged)
    Q_PROPERTY(WindowMode recommendedFullScreenMode MEMBER recommendedFullScreenMode CONSTANT)
    Q_PROPERTY(UIDisplayMode uiDisplayMode MEMBER uiDisplayMode NOTIFY uiDisplayModeChanged)
    Q_PROPERTY(int uiScale MEMBER uiScale NOTIFY uiScaleChanged)
    Q_PROPERTY(bool tvMode MEMBER tvMode NOTIFY tvModeChanged)
    Q_PROPERTY(bool swapMouseButtons MEMBER swapMouseButtons NOTIFY mouseButtonsChanged)
    Q_PROPERTY(bool muteOnFocusLoss MEMBER muteOnFocusLoss NOTIFY muteOnFocusLossChanged)
    Q_PROPERTY(bool backgroundGamepad MEMBER backgroundGamepad NOTIFY backgroundGamepadChanged)
    Q_PROPERTY(bool reverseScrollDirection MEMBER reverseScrollDirection NOTIFY reverseScrollDirectionChanged)
    Q_PROPERTY(bool swapFaceButtons MEMBER swapFaceButtons NOTIFY swapFaceButtonsChanged)
    Q_PROPERTY(GamepadMenuTrigger gamepadMenuTrigger MEMBER gamepadMenuTrigger NOTIFY gamepadMenuTriggerChanged)
    Q_PROPERTY(bool keepAwake MEMBER keepAwake NOTIFY keepAwakeChanged)
    Q_PROPERTY(CaptureSysKeysMode captureSysKeysMode MEMBER captureSysKeysMode NOTIFY captureSysKeysModeChanged)
    Q_PROPERTY(Language language MEMBER language NOTIFY languageChanged);

    Q_INVOKABLE bool retranslate();
    Q_INVOKABLE void openDiagnosticsFolder();
    Q_INVOKABLE void exportLatestDiagnostics();
    void setDiagnosticsStatus(const QString& message);

    // Rate choices are advisory; toggling VRR never rewrites the saved FPS
    // preference.
    Q_INVOKABLE QVariantList getFpsChoices(const QVariantList& refreshRates) const;

    // Directly accessible members for preferences
    int width;
    int height;
    int fps;
    int bitrateKbps;
    bool unlockBitrate;
    bool autoAdjustBitrate;
    bool enableVsync;
    bool enableVrr;
    int vrrLatencyMode;
    // Re-present the last frame inside a host gap longer than the panel's
    // adaptive-refresh floor, so the panel never engages its own
    // low-framerate compensation.
    bool smoothVrrFrameTiming;
    bool traceVrrFrames;
    bool gameOptimizations;
    bool playAudioOnHost;
    bool multiController;
    // Stable controller IDs in preferred host player order. Controllers not
    // present in this list are appended when first discovered.
    QStringList controllerOrder;
    // Stable controller IDs that remain available to the Moonlight UI but are
    // not passed through to the streaming host.
    QStringList disabledControllers;
    bool enableMdns;
    bool quitAppAfter;
    bool absoluteMouseMode;
    bool absoluteTouchMode;
    bool framePacing;
    bool connectionWarnings;
    bool configurationWarnings;
    bool richPresence;
    bool gamepadMouse;
    bool detectNetworkBlocking;
    bool showPerformanceOverlay;
    int performanceOverlayMode;
    // Graphs the user has flipped from their default visibility, rather than
    // the graphs shown, so a graph added later gets its own default for users
    // who already saved their selection.
    int performanceGraphsToggled;

    // Graphs shown unless the user hides them. The rest are opt-in.
    static int getPerformanceGraphsDefault();
    int visiblePerformanceGraphs() const
    {
        return getPerformanceGraphsDefault() ^ performanceGraphsToggled;
    }
    int performanceGraphSize;
    int performanceGraphHeight;
    // Background opacity in percent
    int performanceGraphOpacity;
    // Seconds of history plotted
    int performanceGraphHistory;
    int performanceGraphPosition;
    bool swapMouseButtons;
    bool muteOnFocusLoss;
    bool backgroundGamepad;
    bool reverseScrollDirection;
    bool swapFaceButtons;
    GamepadMenuTrigger gamepadMenuTrigger;
    bool keepAwake;
    int packetSize;
    AudioConfig audioConfig;
    VideoCodecConfig videoCodecConfig;
    bool enableHdr;
    bool enableYUV444;
    // Dithering kernel used to reduce 10-bit video to the output bit depth in
    // the renderer instead of letting it be quantized without dithering.
    // Renderers that cannot honor the exact kernel approximate it.
    int ditheringMode;
    // Vary the dither pattern per frame so it stops sitting still in screen
    // space. Off by default because it can alias on some LCD panels.
    bool temporalDithering;
    // Reconstruct banded gradients in the decoded frame before quantization.
    // Unlike dithering this can fix banding that arrived in the stream, at
    // the cost of some fine detail.
    int debandMode;
    // Linux Vulkan FSR1 upscaling. Takes effect on the next stream.
    bool fsr1Upscaling;
    // RCAS sharpness slider (0-100), with the shader's 0.75-stop default at 62.5.
    double fsr1RcasSharpness;
    // Linux Vulkan LS1 uses the user's Lossless Scaling DLL at runtime.
    bool ls1Upscaling;
    int ls1Sharpness;
    QString ls1DllPath;
    VideoDecoderSelection videoDecoderSelection;
    WindowMode windowMode;
    WindowMode recommendedFullScreenMode;
    UIDisplayMode uiDisplayMode;
    // GUI scale in percent. Takes effect on the next launch.
    int uiScale;
    // Controller and TV friendly GUI. Takes effect on the next launch and
    // can be overridden with --tv-mode/--no-tv-mode.
    bool tvMode;
    Language language;
    CaptureSysKeysMode captureSysKeysMode;
    RendererSelection rendererSelection;

signals:
    void displayModeChanged();
    void bitrateChanged();
    void unlockBitrateChanged();
    void autoAdjustBitrateChanged();
    void enableVsyncChanged();
    void enableVrrChanged();
    void vrrLatencyModeChanged();
    void smoothVrrFrameTimingChanged();
    void traceVrrFramesChanged();
    void diagnosticsChanged();
    void gameOptimizationsChanged();
    void playAudioOnHostChanged();
    void multiControllerChanged();
    void unsupportedFpsChanged();
    void enableMdnsChanged();
    void quitAppAfterChanged();
    void absoluteMouseModeChanged();
    void absoluteTouchModeChanged();
    void audioConfigChanged();
    void videoCodecConfigChanged();
    void enableHdrChanged();
    void enableYUV444Changed();
    void ditheringModeChanged();
    void temporalDitheringChanged();
    void debandModeChanged();
    void fsr1UpscalingChanged();
    void fsr1RcasSharpnessChanged();
    void ls1UpscalingChanged();
    void ls1SharpnessChanged();
    void ls1DllPathChanged();
    void videoDecoderSelectionChanged();
    void uiDisplayModeChanged();
    void uiScaleChanged();
    void tvModeChanged();
    void windowModeChanged();
    void framePacingChanged();
    void connectionWarningsChanged();
    void configurationWarningsChanged();
    void richPresenceChanged();
    void gamepadMouseChanged();
    void detectNetworkBlockingChanged();
    void showPerformanceOverlayChanged();
    void performanceOverlayModeChanged();
    void performanceGraphsToggledChanged();
    void performanceGraphSizeChanged();
    void performanceGraphHeightChanged();
    void performanceGraphOpacityChanged();
    void performanceGraphHistoryChanged();
    void performanceGraphPositionChanged();
    void mouseButtonsChanged();
    void muteOnFocusLossChanged();
    void backgroundGamepadChanged();
    void reverseScrollDirectionChanged();
    void swapFaceButtonsChanged();
    void gamepadMenuTriggerChanged();
    void captureSysKeysModeChanged();
    void keepAwakeChanged();
    void languageChanged();
    void rendererSelectionChanged();

private:
    explicit StreamingPreferences(QQmlEngine *qmlEngine);

    QString getSuffixFromLanguage(Language lang);

    QQmlEngine* m_QmlEngine;
    bool m_ExportingDiagnostics = false;
    QString m_DiagnosticsStatus;
};
