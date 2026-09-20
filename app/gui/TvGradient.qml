import QtQuick 2.9
import QtQuick.Window 2.2

import TvTheme 1.0

// A vertical gradient that doesn't band.
//
// A QML Gradient rounds to 8 bits with nothing to break the rounding up, and
// TV mode's gradients are shallow enough that this shows: the window gradient
// moves about twenty levels over the whole window height, so each level covers
// sixty or more rows and the step to the next one reads as a line across the
// screen. GradientImageProvider builds the same ramp and dithers it as it
// rounds, which is the only point where that can be fixed.
Item {
    id: root

    property color topColor
    property color bottomColor

    Image {
        // Drawn into an item scaled back down by the device pixel ratio, so the
        // strip lands on whole physical pixels. Any resampling would average
        // the dither away and put the banding straight back.
        width: root.width * Screen.devicePixelRatio
        height: root.height * Screen.devicePixelRatio
        transformOrigin: Item.TopLeft
        scale: 1.0 / Screen.devicePixelRatio

        // Only the height matters; the provider returns a strip to tile across
        sourceSize.height: root.height * Screen.devicePixelRatio
        source: root.height > 0 ? "image://tvgradient/" + TvTheme.argbHex(root.topColor) +
                                  "-" + TvTheme.argbHex(root.bottomColor)
                                : ""
        fillMode: Image.TileHorizontally
        smooth: false
    }
}
