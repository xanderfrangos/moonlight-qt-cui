import QtQuick 2.9
import QtQuick.Controls 2.3
import QtQuick.Controls.Material 2.2
import QtQuick.Layouts 1.3

import SdlGamepadKeyNavigation 1.0
import InputModeTracker 1.0
import StreamingPreferences 1.0
import TvTheme 1.0

Item {
    id: controllerPage
    objectName: qsTr("Controllers")
    property string pendingFocusId: ""
    property int pendingFocusControl: 0

    function rememberFocus(id, control) {
        pendingFocusId = id
        pendingFocusControl = control
    }

    function restorePendingFocus() {
        if (!pendingFocusId) {
            return
        }
        var controllers = SdlGamepadKeyNavigation.controllers
        for (var i = 0; i < controllers.length; i++) {
            if (controllers[i].id === pendingFocusId) {
                controllerList.currentIndex = i
                controllerList.positionViewAtIndex(i, ListView.Contain)
                Qt.callLater(function() {
                    if (!controllerList.currentItem) return
                    if (pendingFocusControl === 1 && controllerList.currentItem.upControl.enabled) controllerList.currentItem.upControl.forceActiveFocus(Qt.TabFocusReason)
                    else if (pendingFocusControl === 2 && controllerList.currentItem.downControl.enabled) controllerList.currentItem.downControl.forceActiveFocus(Qt.TabFocusReason)
                    else controllerList.currentItem.firstControl.forceActiveFocus(Qt.TabFocusReason)
                })
                break
            }
        }
    }

    Connections {
        target: SdlGamepadKeyNavigation
        function onControllersChanged() {
            controllerPage.restorePendingFocus()
        }
    }

    StackView.onActivated: {
        SdlGamepadKeyNavigation.setUiNavMode(true)
        if (controllerList.count > 0) {
            controllerList.currentIndex = 0
            Qt.callLater(function() {
                if (controllerList.currentItem) {
                    controllerList.currentItem.firstControl.forceActiveFocus(Qt.TabFocusReason)
                }
            })
        }
    }

    StackView.onDeactivating: SdlGamepadKeyNavigation.setUiNavMode(false)

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 32
        anchors.rightMargin: 32
        anchors.topMargin: 16
        anchors.bottomMargin: 16
        spacing: TvTheme.spacingMedium

        Label {
            Layout.fillWidth: true
            text: (StreamingPreferences.multiController ?
                       qsTr("Enabled controllers are passed to the host in this player order.") :
                       qsTr("Enabled controllers are combined as Player 1 because Force gamepad #1 always connected is enabled.")) +
                  " " + qsTr("Press a button on a controller to light up its row.")
            color: TvTheme.textSecondary
            font.pointSize: 14
            wrapMode: Text.WordWrap
        }

        Label {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: controllerList.count === 0
            text: qsTr("No controllers detected")
            color: TvTheme.textSecondary
            font.pointSize: 22
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        ListView {
            id: controllerList
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: count > 0
            clip: true
            spacing: TvTheme.spacingMedium
            model: SdlGamepadKeyNavigation.controllers

            ScrollBar.vertical: ScrollBar { }

            delegate: Rectangle {
                id: controllerRow
                width: controllerList.width - 16
                height: 116
                radius: TvTheme.cardRadius
                color: TvTheme.surface
                border.width: rowFocus ? 2 : 0
                border.color: Material.accent

                property alias firstControl: enabledButton
                property alias upControl: upButton
                property alias downControl: downButton
                readonly property bool rowFocus: identifyButton.activeFocus || enabledButton.activeFocus ||
                                                 upButton.activeFocus || downButton.activeFocus

                // Lights up whenever a button is pressed on this controller, so
                // the user can match the rows to the controllers in their hands
                Rectangle {
                    id: activityLight
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 6
                    height: parent.height - 40
                    radius: width / 2
                    color: Material.accent
                    opacity: 0.12

                    SequentialAnimation {
                        id: activityAnimation
                        PropertyAction { target: activityLight; property: "opacity"; value: 1.0 }
                        PauseAnimation { duration: 150 }
                        NumberAnimation {
                            target: activityLight
                            property: "opacity"
                            to: 0.12
                            duration: 600
                            easing.type: Easing.InQuad
                        }
                    }
                }

                Connections {
                    target: SdlGamepadKeyNavigation
                    function onControllerInput(id) {
                        if (id === modelData.id) {
                            activityAnimation.restart()
                        }
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 24
                    anchors.rightMargin: 24
                    spacing: TvTheme.spacingMedium

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        Label {
                            Layout.fillWidth: true
                            text: modelData.name
                            font.pointSize: 18
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: modelData.metadata.length > 0
                            text: modelData.metadata
                            color: TvTheme.textSecondary
                            font.pointSize: 13
                            elide: Text.ElideRight
                        }

                        Label {
                            text: modelData.enabled ? qsTr("Player %1").arg(modelData.playerNumber) : qsTr("Not passed to host")
                            color: TvTheme.textSecondary
                            font.pointSize: 12
                        }
                    }

                    Button {
                        id: identifyButton
                        text: qsTr("Identify")
                        enabled: modelData.canIdentify
                        onClicked: SdlGamepadKeyNavigation.identifyController(modelData.id)
                        onActiveFocusChanged: if (activeFocus) controllerList.currentIndex = index

                        ToolTip.delay: 1000
                        ToolTip.timeout: 3000
                        ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                        ToolTip.text: modelData.canIdentify ? qsTr("Rumble this controller") :
                                                              qsTr("This controller can't rumble")
                    }

                    Button {
                        id: enabledButton
                        text: modelData.enabled ? qsTr("Enabled") : qsTr("Disabled")
                        highlighted: modelData.enabled
                        onClicked: {
                            controllerPage.rememberFocus(modelData.id, 0)
                            SdlGamepadKeyNavigation.setControllerEnabled(modelData.id, !modelData.enabled)
                        }
                        onActiveFocusChanged: if (activeFocus) controllerList.currentIndex = index
                    }

                    Button {
                        id: upButton
                        text: qsTr("Move Up")
                        enabled: modelData.canMoveUp
                        onClicked: {
                            controllerPage.rememberFocus(modelData.id, 1)
                            SdlGamepadKeyNavigation.moveController(modelData.id, -1)
                        }
                        onActiveFocusChanged: if (activeFocus) controllerList.currentIndex = index
                    }

                    Button {
                        id: downButton
                        text: qsTr("Move Down")
                        enabled: modelData.canMoveDown
                        onClicked: {
                            controllerPage.rememberFocus(modelData.id, 2)
                            SdlGamepadKeyNavigation.moveController(modelData.id, 1)
                        }
                        onActiveFocusChanged: if (activeFocus) controllerList.currentIndex = index
                    }
                }
            }
        }
    }
}
