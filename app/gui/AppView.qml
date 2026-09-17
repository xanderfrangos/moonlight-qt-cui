import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.2

import StreamingPreferences 1.0
import AppModel 1.0
import ComputerManager 1.0
import SdlGamepadKeyNavigation 1.0
import InputModeTracker 1.0
import SystemProperties 1.0
import TvTheme 1.0

CenteredGridView {
    property int computerIndex
    property AppModel appModel : createModel()
    property bool activated
    property bool showHiddenGames
    property bool showGames

    id: appGrid
    focus: true
    activeFocusOnTab: true
    // Leave room for the focused card to grow at the edges in TV mode. Each
    // card sits at the left of its cell, so shift the grid right by half of
    // the unused cell width to center the cards.
    minMargin: SystemProperties.tvMode ? 40 : 10
    leftInset: SystemProperties.tvMode ? (rowsFilled ? minMargin : 0) + (cellWidth - tvCardWidth) / 2 : 0
    topMargin: SystemProperties.tvMode ? 30 : 20
    bottomMargin: 5
    // TV mode uses bigger box art with more room around each card, since
    // the focused card grows and shows the game's name underneath
    readonly property int tvCardWidth: 260
    cellWidth: SystemProperties.tvMode ? tvCardWidth + 30 : 230
    cellHeight: SystemProperties.tvMode ? 425 : 297

    // Shown blurred behind the page in TV mode
    readonly property url tvBackdropSource: currentItem ? currentItem.backdropArt : ""

    function computerLost()
    {
        // Go back to the PC view on PC loss
        stackView.pop()
    }

    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        // We do this here instead of onActivated to avoid losing the user's
        // selection when backing out of a different page of the app.
        currentIndex = -1
    }

    StackView.onActivated: {
        appModel.computerLost.connect(computerLost)
        activated = true

        // Highlight the first item if a gamepad is connected
        if (currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            currentIndex = 0
        }

        if (!showGames && !showHiddenGames) {
            // Check if there's a direct launch app
            var directLaunchAppIndex = model.getDirectLaunchAppIndex();
            if (directLaunchAppIndex >= 0) {
                // Start the direct launch app if nothing else is running
                currentIndex = directLaunchAppIndex
                currentItem.launchOrResumeSelectedApp(false)

                // Set showGames so we will not loop when the stream ends
                showGames = true
            }
        }
    }

    StackView.onDeactivating: {
        appModel.computerLost.disconnect(computerLost)
        activated = false
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import StreamingPreferences 1.0
import AppModel 1.0; AppModel {}', parent, '')
        model.initialize(ComputerManager, computerIndex, showHiddenGames)
        return model
    }

    model: appModel

    delegate: NavigableItemDelegate {
        width: SystemProperties.tvMode ? appGrid.tvCardWidth : 220
        height: SystemProperties.tvMode ? 390 : 287
        grid: appGrid
        tvCardStyle: true

        property alias appContextMenu: appContextMenuLoader.item
        property alias appNameText: appNameTextLoader.item

        readonly property int artWidth: SystemProperties.tvMode ? 240 : 200
        readonly property int artHeight: SystemProperties.tvMode ? 320 : 267

        // Placeholder art is a plain gray box, which makes a poor backdrop
        readonly property url backdropArt: appIcon.isPlaceholder ? "" : model.boxart

        // Dim the app if it's hidden
        cardOpacity: model.hidden ? 0.4 : 1.0

        // TV mode: a soft shadow under the box art, built from two layers
        Rectangle {
            visible: tvCard
            x: appIcon.x - 4
            y: appIcon.y + 8
            width: appIcon.width + 8
            height: appIcon.height + 6
            radius: TvTheme.cardRadius + 4
            color: "black"
            opacity: 0.2
        }
        Rectangle {
            visible: tvCard
            x: appIcon.x
            y: appIcon.y + 4
            width: appIcon.width
            height: appIcon.height
            radius: TvTheme.cardRadius
            color: "black"
            opacity: 0.35
        }

        // TV mode: a glow around the focused game's box art
        Item {
            visible: tvCard
            anchors.fill: appIcon
            opacity: highlighted ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: TvTheme.animationNormal } }

            Rectangle {
                anchors.fill: parent
                anchors.margins: -9
                radius: 10
                color: "transparent"
                border.width: 6
                border.color: Material.accent
                opacity: 0.3
            }

            // The border overlaps the art by a pixel, so no gap shows between
            // them when the card is scaled. The small radius keeps the square
            // corners of the art from poking out.
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                radius: 4
                color: "transparent"
                border.width: 4
                border.color: Material.accent
            }
        }

        Image {
            property bool isPlaceholder: false

            id: appIcon
            anchors.horizontalCenter: parent.horizontalCenter
            y: 10
            source: model.boxart

            onSourceSizeChanged: {
                // Nearly all of Nvidia's official box art does not match the dimensions of placeholder
                // images, however the one known exception is Overcooked. Therefore, we only execute
                // the image size checks if this is not an app collector game. We know the officially
                // supported games all have box art, so this check is not required.
                if (!model.isAppCollectorGame &&
                    ((sourceSize.width === 130 && sourceSize.height === 180) || // GFE 2.0 placeholder image
                     (sourceSize.width === 628 && sourceSize.height === 888) || // GFE 3.0 placeholder image
                     (sourceSize.width === 200 && sourceSize.height === 266)))  // Our no_app_image.png
                {
                    isPlaceholder = true
                }
                else
                {
                    isPlaceholder = false
                }

                width = artWidth
                height = artHeight
            }

            // Display a tooltip with the full name if it's truncated
            ToolTip.text: model.name
            ToolTip.delay: 1000
            ToolTip.timeout: 5000
            // TV mode shows the name under the focused card instead
            ToolTip.visible: !SystemProperties.tvMode && (parent.hovered || parent.highlighted) && (!appNameText || appNameText.truncated)
        }

        Loader {
            active: model.running
            asynchronous: true
            anchors.fill: appIcon

            sourceComponent: Item {
                RoundButton {
                    // Don't steal focus from the toolbar buttons
                    focusPolicy: Qt.NoFocus

                    anchors.horizontalCenterOffset: appIcon.isPlaceholder ? -47 : 0
                    anchors.verticalCenterOffset: appIcon.isPlaceholder ? -75 : -60
                    anchors.centerIn: parent
                    implicitWidth: 85
                    implicitHeight: 85

                    icon.source: "qrc:/res/play_arrow_FILL1_wght700_GRAD200_opsz48.svg"
                    icon.width: 75
                    icon.height: 75

                    onClicked: {
                        launchOrResumeSelectedApp(true)
                    }

                    ToolTip.text: qsTr("Resume Game")
                    ToolTip.delay: 1000
                    ToolTip.timeout: 3000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered

                    Material.background: SystemProperties.tvMode ? "#E01F2330" : "#D0808080"
                }

                RoundButton {
                    // Don't steal focus from the toolbar buttons
                    focusPolicy: Qt.NoFocus

                    anchors.horizontalCenterOffset: appIcon.isPlaceholder ? 47 : 0
                    anchors.verticalCenterOffset: appIcon.isPlaceholder ? -75 : 60
                    anchors.centerIn: parent
                    implicitWidth: 85
                    implicitHeight: 85

                    icon.source: "qrc:/res/stop_FILL1_wght700_GRAD200_opsz48.svg"
                    icon.width: 75
                    icon.height: 75

                    onClicked: {
                        doQuitGame()
                    }

                    ToolTip.text: qsTr("Quit Game")
                    ToolTip.delay: 1000
                    ToolTip.timeout: 3000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered

                    Material.background: SystemProperties.tvMode ? "#E01F2330" : "#D0808080"
                }
            }
        }

        // TV mode: marks games that are running on the host
        Rectangle {
            visible: tvCard && model.running
            anchors.left: appIcon.left
            anchors.top: appIcon.top
            anchors.margins: 10
            width: runningLabel.implicitWidth + 24
            height: runningLabel.implicitHeight + 8
            radius: height / 2
            color: Material.accent

            Label {
                id: runningLabel
                anchors.centerIn: parent
                text: qsTr("Running")
                font.pointSize: 11
                font.bold: true
            }
        }

        // TV mode: the focused game's full name, under its box art
        Label {
            visible: tvCard
            anchors.top: appIcon.bottom
            anchors.topMargin: 16
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width
            text: model.name
            font.pointSize: 16
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            opacity: highlighted ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: TvTheme.animationNormal } }
        }

        Loader {
            id: appNameTextLoader
            active: appIcon.isPlaceholder

            // This loader is not asynchronous to avoid noticeable differences
            // in the time in which the text loads for each game.

            width: appIcon.width
            height: model.running ? 175 : appIcon.height

            anchors.left: appIcon.left
            anchors.right: appIcon.right
            anchors.bottom: appIcon.bottom

            sourceComponent: Label {
                id: appNameText
                text: model.name
                font.pointSize: 22
                leftPadding: 20
                rightPadding: 20
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                elide: Text.ElideRight
            }
        }

        function launchOrResumeSelectedApp(quitExistingApp)
        {
            var runningId = appModel.getRunningAppId()
            if (runningId !== 0 && runningId !== model.appid) {
                if (quitExistingApp) {
                    quitAppDialog.appName = appModel.getRunningAppName()
                    quitAppDialog.segueToStream = true
                    quitAppDialog.nextAppName = model.name
                    quitAppDialog.nextAppIndex = index
                    quitAppDialog.open()
                }

                return
            }

            var component = Qt.createComponent("StreamSegue.qml")
            var segue = component.createObject(stackView, {
                                                   "appName": model.name,
                                                   "session": appModel.createSessionForApp(index),
                                                   "isResume": runningId === model.appid
                                               })
            stackView.push(segue)
        }

        onClicked: {
            // Only allow clicking on the box art for non-running games.
            // For running games, buttons will appear to resume or quit which
            // will handle starting the game and clicks on the box art will
            // be ignored.
            if (!model.running) {
                launchOrResumeSelectedApp(true)
            }
        }

        onPressAndHold: {
            // popup() ensures the menu appears under the mouse cursor
            if (appContextMenu.popup) {
                appContextMenu.popup()
            }
            else {
                // Qt 5.9 doesn't have popup()
                appContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton;
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onReturnPressed: {
            // Open the app context menu if activated via the gamepad or keyboard
            // for running games. If the game isn't running, the above onClicked
            // method will handle the launch.
            if (model.running) {
                // This will be keyboard/gamepad driven so use
                // open() instead of popup()
                appContextMenu.open()
            }
        }

        Keys.onEnterPressed: {
            // Open the app context menu if activated via the gamepad or keyboard
            // for running games. If the game isn't running, the above onClicked
            // method will handle the launch.
            if (model.running) {
                // This will be keyboard/gamepad driven so use
                // open() instead of popup()
                appContextMenu.open()
            }
        }

        Keys.onMenuPressed: {
            // This will be keyboard/gamepad driven so use open() instead of popup()
            appContextMenu.open()
        }

        function doQuitGame() {
            quitAppDialog.appName = appModel.getRunningAppName()
            quitAppDialog.segueToStream = false
            quitAppDialog.open()
        }

        Loader {
            id: appContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: appContextMenu
                initiator: appContextMenuLoader.parent
                NavigableMenuItem {
                    text: model.running ? qsTr("Resume Game") : qsTr("Launch Game")
                    onTriggered: launchOrResumeSelectedApp(true)
                }
                NavigableMenuItem {
                    text: qsTr("Quit Game")
                    onTriggered: doQuitGame()
                    visible: model.running
                }
                NavigableMenuItem {
                    checkable: true
                    checked: model.directLaunch
                    text: qsTr("Direct Launch")
                    onTriggered: appModel.setAppDirectLaunch(model.index, !model.directLaunch)
                    enabled: !model.hidden

                    ToolTip.text: qsTr("Launch this app immediately when the host is selected, bypassing the app selection grid.")
                    ToolTip.delay: 1000
                    ToolTip.timeout: 3000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                }
                NavigableMenuItem {
                    checkable: true
                    checked: model.hidden
                    text: qsTr("Hide Game")
                    onTriggered: appModel.setAppHidden(model.index, !model.hidden)
                    enabled: model.hidden || (!model.running && !model.directLaunch)

                    ToolTip.text: qsTr("Hide this game from the app grid. To access hidden games, right-click on the host and choose %1.").arg(qsTr("View All Apps"))
                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                }
            }
        }
    }

    Row {
        anchors.centerIn: parent
        spacing: 5
        visible: appGrid.count === 0

        Label {
            text: qsTr("This computer doesn't seem to have any applications or some applications are hidden")
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    NavigableMessageDialog {
        id: quitAppDialog
        property string appName : ""
        property bool segueToStream : false
        property string nextAppName: ""
        property int nextAppIndex: 0
        text:qsTr("Are you sure you want to quit %1? Any unsaved progress will be lost.").arg(appName)
        standardButtons: Dialog.Yes | Dialog.No

        function quitApp() {
            var component = Qt.createComponent("QuitSegue.qml")
            var params = {"appName": appName, "quitRunningAppFn": function() { appModel.quitRunningApp() }}
            if (segueToStream) {
                // Store the session and app name if we're going to stream after
                // successfully quitting the old app.
                params.nextAppName = nextAppName
                params.nextSession = appModel.createSessionForApp(nextAppIndex)
            }
            else {
                params.nextAppName = null
                params.nextSession = null
            }

            stackView.push(component.createObject(stackView, params))
        }

        onAccepted: quitApp()
    }

    ScrollBar.vertical: ScrollBar {}
}
