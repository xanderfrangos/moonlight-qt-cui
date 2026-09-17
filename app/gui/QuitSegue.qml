import QtQuick 2.0
import QtQuick.Controls 2.2

import ComputerManager 1.0
import Session 1.0
import SystemProperties 1.0
import TvTheme 1.0

Item {
    property string appName
    property var quitRunningAppFn
    property Session nextSession : null
    property string nextAppName : ""

    // Box art shown blurred behind the page in TV mode, and for the next game
    property url boxArt
    property url nextBoxArt
    readonly property url tvBackdropSource: boxArt

    property string stageText : qsTr("Quitting %1...").arg(appName)

    function quitAppCompleted(error)
    {
        // Display a failed dialog if we got an error
        if (error !== undefined) {
            errorDialog.text = error
            errorDialog.open()
            console.error(error)
        }

        // If we're supposed to launch another game after this, do so now
        if (error === undefined && nextSession !== null) {
            var component = Qt.createComponent("StreamSegue.qml")
            var segue = component.createObject(stackView, {"appName": nextAppName, "session": nextSession, "boxArt": nextBoxArt})
            stackView.replace(segue)
        }
        else {
            // Exit this view
            stackView.pop()
        }
    }

    StackView.onActivated: {
        // Hide the toolbar before we start loading
        toolBar.visible = false

        // Connect the quit completion signal
        ComputerManager.quitAppCompleted.connect(quitAppCompleted)

        // Start the quit operation if requested
        if (quitRunningAppFn) {
            quitRunningAppFn()
        }
    }

    StackView.onDeactivating: {
        // Show the toolbar again
        toolBar.visible = true

        // Disconnect the signal
        ComputerManager.quitAppCompleted.disconnect(quitAppCompleted)
    }

    Row {
        anchors.centerIn: parent
        spacing: 5
        opacity: SystemProperties.tvMode ? 0.0 : 1.0

        BusyIndicator {
            id: stageSpinner
            running: visible
        }

        Label {
            id: stageLabel
            height: stageSpinner.height
            text: stageText
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter

            wrapMode: Text.Wrap
        }
    }


    // TV mode: the game's name and progress over its blurred box art. The
    // row above stays in place (but transparent) so the existing visibility
    // logic keeps driving what is shown.
    Column {
        visible: SystemProperties.tvMode
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 1000)
        spacing: TvTheme.spacingMedium

        Label {
            width: parent.width
            visible: stageLabel.visible
            text: appName
            font.pointSize: 36
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }

        Label {
            width: parent.width
            visible: stageLabel.visible
            text: stageText
            font.pointSize: 18
            color: TvTheme.textSecondary
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        Item {
            width: parent.width
            height: TvTheme.spacingMedium
        }

        ProgressBar {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width, 480)
            indeterminate: true
            // Opacity rather than visibility, so the text doesn't move
            opacity: 1.0
        }
    }

    ErrorMessageDialog {
        id: errorDialog
    }
}
