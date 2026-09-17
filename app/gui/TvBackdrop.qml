import QtQuick 2.9

import TvTheme 1.0

// A dim, heavily blurred image that fills the window behind TV mode pages.
// The blurring is done by BlurredImageProvider, so no graphical effects
// module is needed. Changing the source crossfades to the new image.
Item {
    id: backdrop

    property url source

    // The image that is showing, or fading in
    property Item currentImage: null

    function blurredUrl(url) {
        if (url == "") {
            return ""
        }

        // Base64url encode the URL so it can be embedded in the image URL
        return "image://blurred/" + Qt.btoa(url.toString()).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "")
    }

    function showSource() {
        var blurredSource = blurredUrl(source)
        if (currentImage !== null && currentImage.source == blurredSource) {
            currentImage.shown = source != ""
            return
        }

        var nextImage = currentImage === imageA ? imageB : imageA
        if (currentImage !== null) {
            currentImage.shown = false
        }

        nextImage.source = blurredSource
        nextImage.shown = source != ""
        currentImage = nextImage
    }

    // Wait until focus stops moving, so holding a direction on the gamepad
    // doesn't load the art for every game along the way
    onSourceChanged: settleTimer.restart()

    Timer {
        id: settleTimer
        interval: 150
        onTriggered: backdrop.showSource()
    }

    Image {
        id: imageA
        property bool shown: false
        anchors.fill: parent
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        smooth: true
        opacity: shown && status === Image.Ready ? 0.45 : 0.0
        Behavior on opacity { NumberAnimation { duration: TvTheme.animationNormal * 2 } }
    }

    Image {
        id: imageB
        property bool shown: false
        anchors.fill: parent
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        smooth: true
        opacity: shown && status === Image.Ready ? 0.45 : 0.0
        Behavior on opacity { NumberAnimation { duration: TvTheme.animationNormal * 2 } }
    }

    // Darken the image toward the bottom so text and cards stay readable
    Rectangle {
        anchors.fill: parent
        opacity: backdrop.currentImage !== null && backdrop.currentImage.shown ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: TvTheme.animationNormal * 2 } }
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(TvTheme.background.r, TvTheme.background.g, TvTheme.background.b, 0.35) }
            GradientStop { position: 1.0; color: Qt.rgba(TvTheme.background.r, TvTheme.background.g, TvTheme.background.b, 0.9) }
        }
    }
}
