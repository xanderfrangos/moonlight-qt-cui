#pragma once

#include <QColor>
#include <QString>

#include "SDL_compat.h"

// How a controller labels its face buttons, so button prompts can show what is
// printed on the controller in the user's hands. SDL reports face buttons by
// position (SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS is off), so SDL's A is
// always the bottom button, whatever it's labeled.
namespace ControllerButtonStyle
{
// QML sees these as plain integers
enum Style
{
    Xbox = 0,
    PlayStation = 1,
    Nintendo = 2,
};

// Face button positions, matching SDL's A, B, X, and Y
enum FacePosition
{
    South = 0,
    East = 1,
    West = 2,
    North = 3,
};

Style fromController(SDL_GameController* controller);

// The label printed on the button at a position
QString glyph(Style style, FacePosition position);

// The color the label is printed in
QColor color(Style style, FacePosition position);
}
