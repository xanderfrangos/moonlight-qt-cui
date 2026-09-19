#include "controlleridentity.h"

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
