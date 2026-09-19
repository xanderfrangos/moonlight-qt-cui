#pragma once

#include <QString>

#include "SDL_compat.h"

namespace ControllerIdentity
{
QString fromController(SDL_GameController* controller);
QString fromDeviceIndex(int deviceIndex);
QString displayName(SDL_GameController* controller);
}
