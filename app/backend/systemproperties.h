#pragma once

#include <QObject>
#include <QRect>

#include "SDL_compat.h"

class SystemProperties : public QObject
{
    Q_OBJECT

    friend class SystemPropertyQueryThread;

public:
    SystemProperties();
    ~SystemProperties();

    // Static properties queried synchronously during the constructor
    Q_PROPERTY(bool isRunningWayland MEMBER isRunningWayland CONSTANT)
    Q_PROPERTY(bool isRunningXWayland MEMBER isRunningXWayland CONSTANT)
    Q_PROPERTY(bool isWow64 MEMBER isWow64 CONSTANT)
    Q_PROPERTY(bool isDarwin MEMBER isDarwin CONSTANT)
    Q_PROPERTY(bool supportsVideoDithering MEMBER supportsVideoDithering CONSTANT)
    Q_PROPERTY(bool supportsVideoDebanding MEMBER supportsVideoDebanding CONSTANT)
    Q_PROPERTY(bool supportsAmdLowLatencyDecode MEMBER supportsAmdLowLatencyDecode CONSTANT)
    Q_PROPERTY(QString friendlyNativeArchName MEMBER friendlyNativeArchName CONSTANT)
    Q_PROPERTY(bool hasDesktopEnvironment MEMBER hasDesktopEnvironment CONSTANT)
    Q_PROPERTY(bool hasBrowser MEMBER hasBrowser CONSTANT)
    Q_PROPERTY(bool hasDiscordIntegration MEMBER hasDiscordIntegration CONSTANT)
    Q_PROPERTY(bool usesMaterial3Theme MEMBER usesMaterial3Theme CONSTANT)
    Q_PROPERTY(QString versionString MEMBER versionString CONSTANT)
    Q_PROPERTY(bool supportsUiScale MEMBER supportsUiScale CONSTANT)
    Q_PROPERTY(int activeUiScale MEMBER activeUiScale CONSTANT)
    Q_PROPERTY(bool hoverEffectsDisabled MEMBER hoverEffectsDisabled CONSTANT)
    Q_PROPERTY(bool tvMode MEMBER tvMode CONSTANT)
    Q_PROPERTY(bool tvModeOverridden MEMBER tvModeOverridden CONSTANT)

    // Properties queried asynchronously (startAsyncLoad() must be called!)
    Q_PROPERTY(bool hasHardwareAcceleration MEMBER hasHardwareAcceleration NOTIFY hasHardwareAccelerationChanged)
    Q_PROPERTY(bool rendererAlwaysFullScreen MEMBER rendererAlwaysFullScreen NOTIFY rendererAlwaysFullScreenChanged)
    Q_PROPERTY(QString unmappedGamepads MEMBER unmappedGamepads NOTIFY unmappedGamepadsChanged)
    Q_PROPERTY(QSize maximumResolution MEMBER maximumResolution NOTIFY maximumResolutionChanged)
    Q_PROPERTY(bool supportsHdr MEMBER supportsHdr NOTIFY supportsHdrChanged)

    // Either startAsyncLoad()+waitForAsyncLoad() or refreshDisplays() must be invoked first
    Q_INVOKABLE QRect getNativeResolution(int displayIndex);
    Q_INVOKABLE QRect getSafeAreaResolution(int displayIndex);
    Q_INVOKABLE int getRefreshRate(int displayIndex);

    Q_INVOKABLE void startAsyncLoad();
    Q_INVOKABLE void waitForAsyncLoad();
    Q_INVOKABLE void refreshDisplays();

    // Saves preferences, launches a new instance of Moonlight, and quits this one
    Q_INVOKABLE void restartApplication();

    // Called by main() with the TV mode state for this launch, and whether it
    // was forced by a command line option rather than the saved preference
    static void setTvModeState(bool enabled, bool overridden);

signals:
    void unmappedGamepadsChanged();
    void hasHardwareAccelerationChanged();
    void rendererAlwaysFullScreenChanged();
    void maximumResolutionChanged();
    void supportsHdrChanged();

private slots:
    void updateDecoderProperties(bool hasHardwareAcceleration, bool rendererAlwaysFullScreen, QSize maximumResolution, bool supportsHdr);

private:
    QThread* systemPropertyQueryThread = nullptr;
    SDL_Window* testWindow = nullptr;

    // Properties set by the constructor
    bool isRunningWayland;
    bool isRunningXWayland;
    bool isWow64;
    QString friendlyNativeArchName;
    bool hasDesktopEnvironment;
    bool hasBrowser;
    bool hasDiscordIntegration;
    QString versionString;
    bool usesMaterial3Theme;
    bool isDarwin;
    bool supportsVideoDithering;
    bool supportsVideoDebanding;
    bool supportsAmdLowLatencyDecode;
    bool supportsUiScale;
    int activeUiScale;
    bool hoverEffectsDisabled;
    bool tvMode;
    bool tvModeOverridden;

    // Properties only set if startAsyncLoad() is called
    bool hasHardwareAcceleration;
    bool rendererAlwaysFullScreen;
    QSize maximumResolution;
    bool supportsHdr;
    QString unmappedGamepads;

    // Properties set by refreshDisplays()
    QList<QRect> monitorNativeResolutions;
    QList<QRect> monitorSafeAreaResolutions;
    QList<int> monitorRefreshRates;
};

