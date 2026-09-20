import QtQuick 2.9
import QtQuick.Window 2.2

import TvTheme 1.0

// A dim, heavily blurred image that fills the window behind TV mode pages.
// The blurring is done by BlurredImageProvider, so no graphical effects
// module is needed. Changing the source crossfades to the new image.
//
// The provider returns the whole background, not just the art: the window
// gradient, the art faded back over it, and the gradient that darkens it are
// all folded together there. Drawing those as three translucent layers rounded
// the result to 8 bits three times over, and dithering one layer does not
// survive being scaled down by the opacity of the next. Folded together they
// round once, where the dither can do its job.
Item {
    id: backdrop

    property url source

    // How far the art is faded back so pages stay readable on top of it
    readonly property real imageOpacity: 0.45

    // How much the backdrop is darkened, top and bottom, so text and cards
    // stay readable toward the bottom of the screen
    readonly property color dimTop: Qt.rgba(TvTheme.background.r, TvTheme.background.g, TvTheme.background.b, 0.35)
    readonly property color dimBottom: Qt.rgba(TvTheme.background.r, TvTheme.background.g, TvTheme.background.b, 0.9)

    // The image that is showing, and the one still covered by it
    property Item currentImage: null
    property Item fadingImage: null

    function blurredUrl(url) {
        if (url == "") {
            return ""
        }

        // Base64url encode the URL so it can be embedded in the image URL
        var art = Qt.btoa(url.toString()).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "")

        // Base64url never produces a dot, so the rest of the background follows one
        var opacity = Math.round(backdrop.imageOpacity * 255).toString(16)
        return "image://blurred/" + art +
               "." + (opacity.length < 2 ? "0" + opacity : opacity) +
               "." + TvTheme.argbHex(TvTheme.backgroundTop) +
               "." + TvTheme.argbHex(TvTheme.backgroundBottom) +
               "." + TvTheme.argbHex(backdrop.dimTop) +
               "." + TvTheme.argbHex(backdrop.dimBottom)
    }

    function showSource() {
        var blurredSource = blurredUrl(source)
        if (currentImage !== null && currentImage.source == blurredSource) {
            return
        }

        if (source == "") {
            // Nothing to show, so fade back to the plain window gradient
            if (currentImage !== null) {
                currentImage.shown = false
                currentImage = null
            }
            return
        }

        var nextImage = currentImage === imageA ? imageB : imageA

        // The outgoing image stays opaque underneath until the incoming one has
        // covered it. Fading both at once would dip through to the bare window
        // gradient every time focus moves along a game list.
        if (fadingImage !== null && fadingImage !== nextImage) {
            fadingImage.shown = false
        }
        fadingImage = currentImage

        nextImage.source = blurredSource
        nextImage.z = 1
        if (currentImage !== null) {
            currentImage.z = 0
        }
        nextImage.shown = true
        currentImage = nextImage

        hideTimer.restart()
    }

    // Wait until focus stops moving, so holding a direction on the gamepad
    // doesn't load the art for every game along the way
    onSourceChanged: settleTimer.restart()

    Timer {
        id: settleTimer
        interval: 150
        onTriggered: backdrop.showSource()
    }

    // Hands the screen over from the outgoing image once the incoming one has
    // had time to cover it
    Timer {
        id: hideTimer
        interval: TvTheme.animationNormal * 2
        onTriggered: {
            if (backdrop.fadingImage !== null) {
                backdrop.fadingImage.shown = false
                backdrop.fadingImage = null
            }
        }
    }

    Image {
        id: imageA
        property bool shown: false
        anchors.fill: parent
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        smooth: true
        // Ask for the size this is actually drawn at. The provider crops and
        // dithers to fit it; left to itself it would hand back a thumbnail for
        // the GPU to stretch, and stretching a thumbnail is most of where the
        // banding came from.
        sourceSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)
        opacity: shown && status === Image.Ready ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: TvTheme.animationNormal * 2 } }
        // Cached art can already be ready by the time it is shown, so start the
        // handover from here rather than assuming it takes the whole animation
        onStatusChanged: if (status === Image.Ready && shown) hideTimer.restart()
    }

    Image {
        id: imageB
        property bool shown: false
        anchors.fill: parent
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        smooth: true
        sourceSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)
        opacity: shown && status === Image.Ready ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: TvTheme.animationNormal * 2 } }
        onStatusChanged: if (status === Image.Ready && shown) hideTimer.restart()
    }
}
