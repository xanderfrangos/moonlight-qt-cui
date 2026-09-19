#include "controlleridentity.h"

#include <QStringList>

namespace
{
QString guidString(SDL_Joystick* joystick)
{
    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid, sizeof(guid));
    return QString::fromLatin1(guid);
}

QString safeUtf8(const char* value)
{
    return value != nullptr ? QString::fromUtf8(value) : QString();
}

QString transportName(SDL_GameController* controller)
{
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
    const SDL_JoystickGUID guid = SDL_JoystickGetGUID(joystick);

    // SDL GUIDs use the platform bus type in the first two bytes. On the
    // platforms where these values aren't available, the device path is a
    // useful secondary signal.
    const Uint16 bus = static_cast<Uint16>(guid.data[0]) |
            (static_cast<Uint16>(guid.data[1]) << 8);
#if SDL_VERSION_ATLEAST(2, 24, 0)
    const QString path = safeUtf8(SDL_GameControllerPath(controller)).toLower();
#else
    const QString path;
#endif

    if (bus == 0x0005 || path.contains(QStringLiteral("bluetooth")) ||
            path.contains(QStringLiteral("bthenum"))) {
        return QStringLiteral("Bluetooth");
    }
    if (bus == 0x0003 || path.contains(QStringLiteral("usb")) ||
            path.contains(QStringLiteral("vid_"))) {
        return QStringLiteral("USB");
    }
    if (path.contains(QStringLiteral("xinput")) || guidString(joystick).startsWith(QStringLiteral("78696e707574"))) {
        return QStringLiteral("XInput");
    }

    return QString();
}

QString baseIdentity(SDL_GameController* controller)
{
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
    const QString guid = guidString(joystick);

#if SDL_VERSION_ATLEAST(2, 0, 14)
    const QString serial = safeUtf8(SDL_GameControllerGetSerial(controller));
    if (!serial.isEmpty()) {
        return QStringLiteral("serial:%1:%2").arg(guid, serial);
    }
#endif

#if SDL_VERSION_ATLEAST(2, 24, 0)
    const QString path = safeUtf8(SDL_GameControllerPath(controller));
    if (!path.isEmpty()) {
        return QStringLiteral("path:%1:%2").arg(guid, path);
    }
#endif

    return QStringLiteral("device:%1:%2:%3:%4")
            .arg(guid)
            .arg(SDL_GameControllerGetVendor(controller), 4, 16, QLatin1Char('0'))
            .arg(SDL_GameControllerGetProduct(controller), 4, 16, QLatin1Char('0'))
            .arg(ControllerIdentity::displayName(controller));
}
}

QString ControllerIdentity::fromController(SDL_GameController* controller)
{
    if (controller == nullptr) {
        return QString();
    }

    const SDL_JoystickID instanceId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_JoystickGetDeviceInstanceID(i) == instanceId) {
            return fromDeviceIndex(i);
        }
    }

    return baseIdentity(controller);
}

QString ControllerIdentity::fromDeviceIndex(int deviceIndex)
{
    SDL_GameController* controller = SDL_GameControllerOpen(deviceIndex);
    if (controller == nullptr) {
        return QString();
    }

    const QString baseId = baseIdentity(controller);
    SDL_GameControllerClose(controller);

    int occurrence = 0;
    for (int i = 0; i < deviceIndex; i++) {
        if (!SDL_IsGameController(i)) {
            continue;
        }
        SDL_GameController* earlier = SDL_GameControllerOpen(i);
        if (earlier != nullptr) {
            if (baseIdentity(earlier) == baseId) {
                occurrence++;
            }
            SDL_GameControllerClose(earlier);
        }
    }

    return occurrence == 0 ? baseId : QStringLiteral("%1#%2").arg(baseId).arg(occurrence + 1);
}

QString ControllerIdentity::displayName(SDL_GameController* controller)
{
    const QString name = safeUtf8(SDL_GameControllerName(controller));
    return name.isEmpty() ? QStringLiteral("Controller") : name;
}

QString ControllerIdentity::displayMetadata(SDL_GameController* controller)
{
    if (controller == nullptr) {
        return QString();
    }

    QStringList parts;
    const QString transport = transportName(controller);
    if (!transport.isEmpty()) {
        parts.append(transport);
    }

#if SDL_VERSION_ATLEAST(2, 0, 14)
    const QString serial = safeUtf8(SDL_GameControllerGetSerial(controller)).trimmed();
    if (!serial.isEmpty()) {
        parts.append(serial);
    }
#endif

    const Uint16 vendor = SDL_GameControllerGetVendor(controller);
    const Uint16 product = SDL_GameControllerGetProduct(controller);
    if (vendor != 0) {
        parts.append(QStringLiteral("VID %1").arg(vendor, 4, 16, QLatin1Char('0')).toUpper());
    }
    if (product != 0) {
        parts.append(QStringLiteral("PID %1").arg(product, 4, 16, QLatin1Char('0')).toUpper());
    }

    return parts.join(QStringLiteral(" \u00b7 "));
}
