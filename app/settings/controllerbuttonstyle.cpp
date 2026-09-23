#include "controllerbuttonstyle.h"

ControllerButtonStyle::Style ControllerButtonStyle::fromController(SDL_GameController* controller)
{
    if (controller == nullptr) {
        return Xbox;
    }

    switch (SDL_GameControllerGetType(controller)) {
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4:
#if SDL_VERSION_ATLEAST(2, 0, 14)
    case SDL_CONTROLLER_TYPE_PS5:
#endif
        return PlayStation;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
        return Nintendo;
    default:
        // Xbox, and everything else that copies its A/B/X/Y layout
        return Xbox;
    }
}

QString ControllerButtonStyle::glyph(Style style, FacePosition position)
{
    switch (style) {
    case PlayStation:
        switch (position) {
        case South: return QStringLiteral("✕");
        case East: return QStringLiteral("○");
        case West: return QStringLiteral("□");
        case North: return QStringLiteral("△");
        }
        break;
    case Nintendo:
        // Nintendo prints A and B, and X and Y, the other way around
        switch (position) {
        case South: return QStringLiteral("B");
        case East: return QStringLiteral("A");
        case West: return QStringLiteral("Y");
        case North: return QStringLiteral("X");
        }
        break;
    case Xbox:
        break;
    }

    switch (position) {
    case South: return QStringLiteral("A");
    case East: return QStringLiteral("B");
    case West: return QStringLiteral("X");
    case North: return QStringLiteral("Y");
    }
    return QString();
}

QColor ControllerButtonStyle::color(Style style, FacePosition position)
{
    switch (style) {
    case PlayStation:
        switch (position) {
        case South: return QColor(0x7C, 0xB2, 0xE8);
        case East: return QColor(0xF0, 0x6A, 0x6A);
        case West: return QColor(0xE6, 0x8F, 0xC6);
        case North: return QColor(0x4F, 0xD1, 0xB0);
        }
        break;
    case Nintendo:
        // Nintendo's face buttons are all one color
        return QColor(0xE6, 0xE8, 0xEE);
    case Xbox:
        break;
    }

    switch (position) {
    case South: return QColor(0x6C, 0xC2, 0x4A);
    case East: return QColor(0xE5, 0x53, 0x4B);
    case West: return QColor(0x4C, 0x9A, 0xE8);
    case North: return QColor(0xF2, 0xC1, 0x4E);
    }
    return Qt::white;
}
