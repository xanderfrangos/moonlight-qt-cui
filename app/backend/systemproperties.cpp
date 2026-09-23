#include "systemproperties.h"
#include "utils.h"

#include <QGuiApplication>
#include <QLibraryInfo>
#include <QDir>
#include <QFile>
#include <QProcess>

#include "settings/streamingpreferences.h"

#include "streaming/session.h"
#include "streaming/streamutils.h"

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

class SystemPropertyQueryThread : public QThread
{
public:
    SystemPropertyQueryThread(SystemProperties* properties)
        : QThread(properties), m_Properties(properties)
    {
        setObjectName("System Properties Async Query Thread");
    }

private:
    void run() override
    {
        bool hasHardwareAcceleration;
        bool rendererAlwaysFullScreen;
        bool supportsHdr;
        QSize maximumResolution;

        Session::getDecoderInfo(m_Properties->testWindow, hasHardwareAcceleration, rendererAlwaysFullScreen, supportsHdr, maximumResolution);

        // Propagate the decoder properties to the SystemProperties singleton and emit any change signals on the main thread
        QMetaObject::invokeMethod(m_Properties, "updateDecoderProperties",
                                  Qt::QueuedConnection,
                                  Q_ARG(bool, hasHardwareAcceleration),
                                  Q_ARG(bool, rendererAlwaysFullScreen),
                                  Q_ARG(QSize, maximumResolution),
                                  Q_ARG(bool, supportsHdr));
    }

private:
    SystemProperties* m_Properties;
};

static bool s_TvMode = false;
static bool s_TvModeOverridden = false;

#if defined(Q_OS_LINUX) && defined(HAVE_LIBVA)
static bool hasAmdDrmRenderNode()
{
    const QDir drmDirectory(QStringLiteral("/sys/class/drm"));
    const QStringList renderNodes = drmDirectory.entryList({QStringLiteral("renderD*")},
                                                           QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& renderNode : renderNodes) {
        QFile vendorFile(drmDirectory.filePath(renderNode + QStringLiteral("/device/vendor")));
        if (vendorFile.open(QIODevice::ReadOnly) && vendorFile.readAll().trimmed().toLower() == "0x1002") {
            return true;
        }
    }
    return false;
}
#endif

void SystemProperties::setTvModeState(bool enabled, bool overridden)
{
    s_TvMode = enabled;
    s_TvModeOverridden = overridden;
}

SystemProperties::SystemProperties()
{
    versionString = QString(VERSION_STR);
    hasDesktopEnvironment = WMUtils::isRunningDesktopEnvironment();
    isRunningWayland = WMUtils::isRunningWayland();
    isRunningXWayland = isRunningWayland && QGuiApplication::platformName() == "xcb";
    usesMaterial3Theme = QLibraryInfo::version() >= QVersionNumber(6, 5, 0);

    // GUI scaling is applied at startup via QT_SCALE_FACTOR (see main.cpp),
    // but not on EGLFS where we don't enable High DPI support.
    supportsUiScale = WMUtils::isRunningWindowManager();
    activeUiScale = 100;
    if (qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        double scaleFactor = qEnvironmentVariable("QT_SCALE_FACTOR").toDouble();
        if (scaleFactor > 0) {
            activeUiScale = qRound(scaleFactor * 100);
        }
    }

    // Mirrors how Qt Quick Controls interprets this variable
    bool hoverEnvValid;
    int hoverEnvValue = qEnvironmentVariableIntValue("QT_QUICK_CONTROLS_HOVER_ENABLED", &hoverEnvValid);
    hoverEffectsDisabled = hoverEnvValid && hoverEnvValue == 0;

    tvMode = s_TvMode;
    tvModeOverridden = s_TvModeOverridden;

#ifdef Q_OS_DARWIN
    isDarwin = true;
#else
    isDarwin = false;
#endif

    // Dithering is implemented by the D3D11VA renderer (its own shaders) and by
    // the libplacebo renderer (libplacebo's built-in dithering). Other
    // renderers ignore the preference, so hide it where neither can run.
#if defined(Q_OS_WIN32) || defined(HAVE_LIBPLACEBO_VULKAN)
    supportsVideoDithering = true;
#else
    supportsVideoDithering = false;
#endif

    // Only libplacebo has debanding. On Windows 10-bit always goes through the
    // D3D11VA renderer, which has no equivalent, so don't offer it there.
#if defined(HAVE_LIBPLACEBO_VULKAN) && !defined(Q_OS_WIN32)
    supportsVideoDebanding = true;
#else
    supportsVideoDebanding = false;
#endif

#if defined(Q_OS_LINUX) && defined(HAVE_LIBVA)
    // Mesa's low-latency decode option is specific to AMD VAAPI hardware.
    supportsAmdLowLatencyDecode = hasAmdDrmRenderNode();
#else
    supportsAmdLowLatencyDecode = false;
#endif

    QString nativeArch = QSysInfo::currentCpuArchitecture();

#ifdef Q_OS_WIN32
    {
        USHORT processArch, machineArch;

        // Use IsWow64Process2() because it doesn't lie on ARM64
        if (IsWow64Process2(GetCurrentProcess(), &processArch, &machineArch)) {
            switch (machineArch) {
            case IMAGE_FILE_MACHINE_I386:
                nativeArch = "i386";
                break;
            case IMAGE_FILE_MACHINE_AMD64:
                nativeArch = "x86_64";
                break;
            case IMAGE_FILE_MACHINE_ARM64:
                nativeArch = "arm64";
                break;
            }
        }

        isWow64 = nativeArch != QSysInfo::buildCpuArchitecture();
    }
#else
    isWow64 = false;
#endif

    if (nativeArch == "i386") {
        friendlyNativeArchName = "x86";
    }
    else if (nativeArch == "x86_64") {
        friendlyNativeArchName = "x64";
    }
    else {
        friendlyNativeArchName = nativeArch.toUpper();
    }

    // Assume we can probably launch a browser if we're in a GUI environment
    hasBrowser = hasDesktopEnvironment;

#ifdef HAVE_DISCORD
    hasDiscordIntegration = true;
#else
    hasDiscordIntegration = false;
#endif

    // These will be queried asynchronously to avoid blocking the UI
    hasHardwareAcceleration = true;
    rendererAlwaysFullScreen = false;
    supportsHdr = true;
    maximumResolution = QSize(0, 0);
}

SystemProperties::~SystemProperties()
{
    waitForAsyncLoad();
}

void SystemProperties::updateDecoderProperties(bool hasHardwareAcceleration, bool rendererAlwaysFullScreen, QSize maximumResolution, bool supportsHdr)
{
    SDL_assert(testWindow);

    if (hasHardwareAcceleration != this->hasHardwareAcceleration) {
        this->hasHardwareAcceleration = hasHardwareAcceleration;
        emit hasHardwareAccelerationChanged();
    }

    if (rendererAlwaysFullScreen != this->rendererAlwaysFullScreen) {
        this->rendererAlwaysFullScreen = rendererAlwaysFullScreen;
        emit rendererAlwaysFullScreenChanged();
    }

    if (maximumResolution != this->maximumResolution) {
        this->maximumResolution = maximumResolution;
        emit maximumResolutionChanged();
    }

    if (supportsHdr != this->supportsHdr) {
        this->supportsHdr = supportsHdr;
        emit supportsHdrChanged();
    }

    SDL_DestroyWindow(testWindow);
    testWindow = nullptr;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

QRect SystemProperties::getNativeResolution(int displayIndex)
{
    // Returns default constructed QRect if out of bounds
    return monitorNativeResolutions.value(displayIndex);
}

QRect SystemProperties::getSafeAreaResolution(int displayIndex)
{
    // Returns default constructed QRect if out of bounds
    return monitorSafeAreaResolutions.value(displayIndex);
}

int SystemProperties::getRefreshRate(int displayIndex)
{
    // Returns 0 if out of bounds
    return monitorRefreshRates.value(displayIndex);
}

void SystemProperties::startAsyncLoad()
{
    if (systemPropertyQueryThread) {
        // Already started/completed
        return;
    }

    // This isn't actually asynchronous (due to the need to synchronize with
    // SdlGamepadKeyNavigation), but we don't query it in the constructor
    // because it's expensive.
    unmappedGamepads = SdlInputHandler::getUnmappedGamepads();
    if (!unmappedGamepads.isEmpty()) {
        emit unmappedGamepadsChanged();
    }

    // We initialize the video subsystem and test window on the main thread
    // because some platforms (macOS) do not support window creation on
    // non-main threads.
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                     SDL_GetError());
        return;
    }

    testWindow = StreamUtils::createTestWindow();
    if (!testWindow) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create window for hardware decode test: %s",
                     SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return;
    }

    // Update display related attributes (max FPS, native resolution, etc).
    //
    // NB: SDL3 will forcefully refresh displays when a window is created,
    // so we place this after the window creation to ensure we don't pay
    // the penalty for mode enumeration twice.
    refreshDisplays();

    systemPropertyQueryThread = new SystemPropertyQueryThread(this);
    systemPropertyQueryThread->start();
}

void SystemProperties::waitForAsyncLoad()
{
    if (systemPropertyQueryThread) {
        systemPropertyQueryThread->wait();
    }
}

void SystemProperties::restartApplication()
{
    // Persist any changes made in the settings page before we exit
    StreamingPreferences::get()->save();

    // Don't pass along environment variables that we set ourselves from
    // preferences at startup, so the new process applies the new values.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QStringList injectedEnvVars = env.value("MOONLIGHT_INJECTED_ENV").split(',');
    const bool injectedAmdDebug = injectedEnvVars.contains(QStringLiteral("AMD_DEBUG"));
    const bool hadOriginalAmdDebug = env.value(QStringLiteral("MOONLIGHT_AMD_DEBUG_ORIGINAL_SET")) == QStringLiteral("1");
    const QString originalAmdDebug = env.value(QStringLiteral("MOONLIGHT_AMD_DEBUG_ORIGINAL"));
    for (const QString& var : injectedEnvVars) {
        if (!var.isEmpty()) {
            env.remove(var);
        }
    }
    env.remove("MOONLIGHT_INJECTED_ENV");
    if (injectedAmdDebug) {
        // Restore the environment as it was before Moonlight added its Mesa
        // request. The new process will then apply the currently saved setting.
        if (hadOriginalAmdDebug) {
            env.insert(QStringLiteral("AMD_DEBUG"), originalAmdDebug);
        }
        else {
            env.remove(QStringLiteral("AMD_DEBUG"));
        }
    }

    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments(QCoreApplication::arguments().mid(1));
    process.setProcessEnvironment(env);

    // Portable mode locates its settings using the working directory
    process.setWorkingDirectory(QDir::currentPath());

    if (!process.startDetached()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to restart Moonlight: %s",
                     qPrintable(process.errorString()));
        return;
    }

    QCoreApplication::quit();
}

void SystemProperties::refreshDisplays()
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_VIDEO) failed: %s",
                     SDL_GetError());
        return;
    }

    monitorNativeResolutions.clear();
    monitorSafeAreaResolutions.clear();
    monitorRefreshRates.clear();

    SDL_DisplayMode bestMode;
    for (int displayIndex = 0; displayIndex < SDL_GetNumVideoDisplays(); displayIndex++) {
        SDL_DisplayMode desktopMode;
        SDL_Rect safeArea;

        if (StreamUtils::getNativeDesktopMode(displayIndex, &desktopMode, &safeArea)) {
            if (desktopMode.w <= 8192 && desktopMode.h <= 8192) {
                // Keep these lists compact because their QML consumers iterate until
                // the first empty entry. Inserting by SDL display index is invalid if
                // an earlier display was skipped (for example, a >8K virtual display).
                monitorNativeResolutions.append(QRect(0, 0, desktopMode.w, desktopMode.h));
                monitorSafeAreaResolutions.append(QRect(0, 0, safeArea.w, safeArea.h));
            }
            else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Skipping resolution over 8K: %dx%d",
                            desktopMode.w, desktopMode.h);
            }

            // Start at desktop mode and work our way up
            bestMode = desktopMode;
            int numDisplayModes = SDL_GetNumDisplayModes(displayIndex);
            for (int i = 0; i < numDisplayModes; i++) {
                SDL_DisplayMode mode;
                if (SDL_GetDisplayMode(displayIndex, i, &mode) == 0) {
                    if (mode.w == desktopMode.w && mode.h == desktopMode.h) {
                        if (mode.refresh_rate > bestMode.refresh_rate) {
                            bestMode = mode;
                        }
                    }
                }
            }

            // Try to normalize values around our our standard refresh rates.
            // Some displays/OSes report values that are slightly off.
            if (bestMode.refresh_rate >= 58 && bestMode.refresh_rate <= 62) {
                monitorRefreshRates.append(60);
            }
            else if (bestMode.refresh_rate >= 28 && bestMode.refresh_rate <= 32) {
                monitorRefreshRates.append(30);
            }
            else {
                monitorRefreshRates.append(bestMode.refresh_rate);
            }
        }
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
