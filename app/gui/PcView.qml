import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtQuick.Controls.Material 2.2

import ComputerModel 1.0

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0
import TvTheme 1.0

CenteredGridView {
    property ComputerModel computerModel : createModel()

    id: pcGrid
    focus: true
    activeFocusOnTab: true
    topMargin: SystemProperties.tvMode ? 30 : 20
    bottomMargin: 5

    // TV mode cards have room around them to grow when focused, and the grid
    // is shifted so the cards are centered in their cells
    readonly property int tvCardWidth: 300
    minMargin: SystemProperties.tvMode ? 40 : 10
    leftInset: SystemProperties.tvMode ? (rowsFilled ? minMargin : 0) + (cellWidth - tvCardWidth) / 2 : 0
    cellWidth: SystemProperties.tvMode ? tvCardWidth + 40 : 310
    cellHeight: SystemProperties.tvMode ? 400 : 330
    objectName: qsTr("Computers")

    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        // We do this here instead of onActivated to avoid losing the user's
        // selection when backing out of a different page of the app.
        currentIndex = -1
    }

    // Note: Any initialization done here that is critical for streaming must
    // also be done in CliStartStreamSegue.qml, since this code does not run
    // for command-line initiated streams.
    StackView.onActivated: {
        // Setup signals on CM
        ComputerManager.computerAddCompleted.connect(addComplete)

        // Highlight the first item if a gamepad is connected
        if (currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            currentIndex = 0
        }
    }

    StackView.onDeactivating: {
        ComputerManager.computerAddCompleted.disconnect(addComplete)
    }

    function pairingComplete(error)
    {
        // Close the PIN dialog
        pairDialog.close()

        // Display a failed dialog if we got an error
        if (error !== undefined) {
            errorDialog.text = error
            errorDialog.helpText = ""
            errorDialog.open()
        }
    }

    function addComplete(success, detectedPortBlocking)
    {
        if (!success) {
            errorDialog.text = qsTr("Unable to connect to the specified PC.")

            if (detectedPortBlocking) {
                errorDialog.text += "\n\n" + qsTr("This PC's Internet connection is blocking Moonlight. Streaming over the Internet may not work while connected to this network.")
            }
            else {
                errorDialog.helpText = qsTr("Click the Help button for possible solutions.")
            }

            errorDialog.open()
        }
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', parent, '')
        model.initialize(ComputerManager)
        model.pairingCompleted.connect(pairingComplete)
        model.connectionTestCompleted.connect(testConnectionDialog.connectionTestComplete)
        return model
    }

    // TV mode: a larger empty state, with a hint about adding a PC
    Column {
        id: tvEmptyState
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 900)
        spacing: TvTheme.spacingMedium
        readonly property bool showing: SystemProperties.tvMode && pcGrid.count === 0
        visible: showing

        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 160
            height: 160

            Image {
                anchors.centerIn: parent
                source: "qrc:/res/desktop_windows-48px.svg"
                sourceSize.width: 160
                sourceSize.height: 160
                opacity: 0.25
            }

            BusyIndicator {
                anchors.centerIn: parent
                anchors.verticalCenterOffset: -14
                width: 64
                height: 64
                visible: StreamingPreferences.enableMdns
                running: StreamingPreferences.enableMdns && tvEmptyState.showing
            }
        }

        Label {
            width: parent.width
            text: StreamingPreferences.enableMdns ? qsTr("Searching for compatible hosts on your local network...")
                                                  : qsTr("Automatic PC discovery is disabled.")
            font.pointSize: 24
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        Label {
            width: parent.width
            text: qsTr("If your PC doesn't appear, make sure it's on the same network, or add it manually using the button in the top right.")
            font.pointSize: 16
            color: TvTheme.textSecondary
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }

    Row {
        anchors.centerIn: parent
        spacing: 5
        visible: !SystemProperties.tvMode && pcGrid.count === 0

        BusyIndicator {
            id: searchSpinner
            visible: StreamingPreferences.enableMdns
            running: visible
        }

        Label {
            height: searchSpinner.height
            elide: Label.ElideRight
            text: StreamingPreferences.enableMdns ? qsTr("Searching for compatible hosts on your local network...")
                                                  : qsTr("Automatic PC discovery is disabled. Add your PC manually.")
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    model: computerModel

    delegate: NavigableItemDelegate {
        width: SystemProperties.tvMode ? pcGrid.tvCardWidth : 300
        height: SystemProperties.tvMode ? 360 : 320
        grid: pcGrid
        tvCardStyle: true

        property alias pcContextMenu : pcContextMenuLoader.item

        popupOpen: pcContextMenu !== null && pcContextMenu.visible

        // Matches what onClicked does below
        tvHintSelect: !model.online ? qsTr("Options") :
                      !model.serverSupported ? qsTr("Select") :
                      model.paired ? qsTr("Open") : qsTr("Pair")

        // TV mode: the card surface
        Rectangle {
            visible: tvCard
            anchors.fill: parent
            radius: TvTheme.cardRadius
            color: tvSelected ? TvTheme.surfaceRaised : TvTheme.surface
            Behavior on color { ColorAnimation { duration: TvTheme.animationNormal } }
        }

        // TV mode: a glow around the focused card
        Item {
            visible: tvCard
            anchors.fill: parent
            opacity: tvSelected ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: TvTheme.animationNormal } }

            Rectangle {
                anchors.fill: parent
                anchors.margins: -9
                radius: TvTheme.cardRadius + 9
                color: "transparent"
                border.width: 6
                border.color: Material.accent
                opacity: 0.3
            }

            // Overlaps the card by a pixel so no gap shows when it is scaled
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                radius: TvTheme.cardRadius + 3
                color: "transparent"
                border.width: 4
                border.color: Material.accent
            }
        }

        Image {
            id: pcIcon
            anchors.horizontalCenter: parent.horizontalCenter
            y: SystemProperties.tvMode ? 24 : 0
            source: "qrc:/res/desktop_windows-48px.svg"
            sourceSize {
                width: SystemProperties.tvMode ? 170 : 200
                height: SystemProperties.tvMode ? 170 : 200
            }
        }

        Image {
            // TODO: Tooltip
            id: stateIcon
            anchors.horizontalCenter: pcIcon.horizontalCenter
            anchors.verticalCenter: pcIcon.verticalCenter
            anchors.verticalCenterOffset: !model.online ? -18 : -16
            visible: !model.statusUnknown && (!model.online || !model.paired)
            source: !model.online ? "qrc:/res/warning_FILL1_wght300_GRAD200_opsz24.svg" : "qrc:/res/baseline-lock-24px.svg"
            sourceSize {
                width: !model.online ? 75 : 70
                height: !model.online ? 75 : 70
            }
        }

        BusyIndicator {
            id: statusUnknownSpinner
            anchors.horizontalCenter: pcIcon.horizontalCenter
            anchors.verticalCenter: pcIcon.verticalCenter
            anchors.verticalCenterOffset: -15
            width: 75
            height: 75
            visible: model.statusUnknown
            running: visible
        }

        Label {
            id: pcNameText
            text: model.name

            width: parent.width
            anchors.top: pcIcon.bottom
            anchors.bottom: SystemProperties.tvMode ? statusChip.top : parent.bottom
            leftPadding: SystemProperties.tvMode ? 16 : 0
            rightPadding: SystemProperties.tvMode ? 16 : 0
            font.pointSize: SystemProperties.tvMode ? 26 : 36
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: SystemProperties.tvMode ? Text.AlignVCenter : Text.AlignTop
            // TV mode keeps names to one line to leave room for the status
            wrapMode: SystemProperties.tvMode ? Text.NoWrap : Text.Wrap
            elide: Text.ElideRight
        }

        // TV mode: the PC's status in words, with a colored dot
        Rectangle {
            id: statusChip

            readonly property color dotColor: {
                if (model.statusUnknown || !model.online) {
                    return "#8A90A0"
                }
                else if (!model.paired || !model.serverSupported) {
                    return "#FFB300"
                }
                return "#4CAF50"
            }

            visible: tvCard
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 24
            width: statusRow.implicitWidth + 32
            height: statusRow.implicitHeight + 12
            radius: height / 2
            color: tvSelected ? TvTheme.surface : TvTheme.surfaceRaised

            Row {
                id: statusRow
                anchors.centerIn: parent
                spacing: 10

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12
                    height: 12
                    radius: 6
                    color: statusChip.dotColor
                    Behavior on color { ColorAnimation { duration: TvTheme.animationNormal } }
                }

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    font.pointSize: 14
                    text: {
                        if (model.statusUnknown) {
                            return qsTr("Checking...")
                        }
                        else if (!model.online) {
                            return qsTr("Offline")
                        }
                        else if (!model.serverSupported) {
                            return qsTr("Update required")
                        }
                        else if (!model.paired) {
                            return qsTr("Not paired")
                        }
                        return qsTr("Online")
                    }
                }
            }
        }

        Loader {
            id: pcContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: pcContextMenu
                initiator: pcContextMenuLoader.parent
                MenuItem {
                    text: qsTr("PC Status: %1").arg(model.online ? qsTr("Online") : qsTr("Offline"))
                    font.bold: true
                    enabled: false
                }
                NavigableMenuItem {
                    text: qsTr("View All Apps")
                    onTriggered: {
                        var component = Qt.createComponent("AppView.qml")
                        var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name, "showHiddenGames": true})
                        stackView.push(appView)
                    }
                    visible: model.online && model.paired
                }
                NavigableMenuItem {
                    text: qsTr("Wake PC")
                    onTriggered: computerModel.wakeComputer(index)
                    visible: !model.online && model.wakeable
                }
                NavigableMenuItem {
                    text: qsTr("Test Network")
                    onTriggered: {
                        computerModel.testConnectionForComputer(index)
                        testConnectionDialog.open()
                    }
                }

                NavigableMenuItem {
                    text: qsTr("Rename PC")
                    onTriggered: {
                        renamePcDialog.pcIndex = index
                        renamePcDialog.originalName = model.name
                        renamePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("Delete PC")
                    onTriggered: {
                        deletePcDialog.pcIndex = index
                        deletePcDialog.pcName = model.name
                        deletePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("View Details")
                    onTriggered: {
                        showPcDetailsDialog.pcDetails = model.details
                        showPcDetailsDialog.open()
                    }
                }
            }
        }

        onClicked: {
            if (model.online) {
                if (!model.serverSupported) {
                    errorDialog.text = qsTr("The version of GeForce Experience on %1 is not supported by this build of Moonlight. You must update Moonlight to stream from %1.").arg(model.name)
                    errorDialog.helpText = ""
                    errorDialog.open()
                }
                else if (model.paired) {
                    // go to game view
                    var component = Qt.createComponent("AppView.qml")
                    var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name})
                    stackView.push(appView)
                }
                else {
                    var pin = computerModel.generatePinString()

                    // Kick off pairing in the background
                    computerModel.pairComputer(index, pin)

                    // Display the pairing dialog
                    pairDialog.pin = pin
                    pairDialog.pcName = model.name
                    pairDialog.open()
                }
            } else if (!model.online) {
                // Using open() here because it may be activated by keyboard
                pcContextMenu.open()
            }
        }

        onPressAndHold: {
            // popup() ensures the menu appears under the mouse cursor
            if (pcContextMenu.popup) {
                pcContextMenu.popup()
            }
            else {
                // Qt 5.9 doesn't have popup()
                pcContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton;
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onMenuPressed: {
            // We must use open() here so the menu is positioned on
            // the ItemDelegate and not where the mouse cursor is
            pcContextMenu.open()
        }

        Keys.onDeletePressed: {
            deletePcDialog.pcIndex = index
            deletePcDialog.pcName = model.name
            deletePcDialog.open()
        }
    }

    ErrorMessageDialog {
        id: errorDialog

        // Using Setup-Guide here instead of Troubleshooting because it's likely that users
        // will arrive here by forgetting to enable GameStream or not forwarding ports.
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide"
    }

    NavigableDialog {
        id: pairDialog
        closePolicy: Popup.CloseOnEscape

        // don't allow edits to the rest of the window while open
        property string pin : "0000"
        property string pcName : ""
        standardButtons: Dialog.Cancel

        onOpened: {
            // Focus the button so the gamepad and keyboard can reach it
            if (footer && footer.count > 0) {
                footer.itemAt(0).forceActiveFocus(Qt.TabFocus)
            }
        }

        // Cancel, Escape, and the gamepad's back button all reject. Closing the
        // dialog because pairing finished doesn't.
        onRejected: computerModel.cancelPairing()

        ColumnLayout {
            spacing: SystemProperties.tvMode ? 24 : 16

            Label {
                Layout.maximumWidth: SystemProperties.tvMode ? 640 : 420
                text: qsTr("Enter this PIN on %1:").arg(pairDialog.pcName)
                font.bold: true
                wrapMode: Text.Wrap
            }

            // The PIN in large digits, so it can be read from across the room
            Row {
                Layout.alignment: Qt.AlignHCenter
                spacing: SystemProperties.tvMode ? 16 : 10

                Repeater {
                    model: pairDialog.pin.split("")

                    Rectangle {
                        width: SystemProperties.tvMode ? 80 : 56
                        height: SystemProperties.tvMode ? 104 : 72
                        radius: SystemProperties.tvMode ? TvTheme.cardRadius : 8
                        color: SystemProperties.tvMode ? TvTheme.surface : Qt.rgba(1, 1, 1, 0.08)

                        Label {
                            anchors.centerIn: parent
                            text: modelData
                            font.pointSize: SystemProperties.tvMode ? 44 : 30
                            font.bold: true
                        }
                    }
                }
            }

            RowLayout {
                spacing: 12

                BusyIndicator {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                    running: pairDialog.visible
                }

                Label {
                    Layout.fillWidth: true
                    Layout.maximumWidth: SystemProperties.tvMode ? 580 : 370
                    text: qsTr("If your host PC is running Sunshine, navigate to the Sunshine web UI to enter the PIN.") + " " +
                          qsTr("This dialog will close when pairing is completed.")
                    color: SystemProperties.tvMode ? TvTheme.textSecondary : Material.foreground
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    NavigableMessageDialog {
        id: deletePcDialog
        // don't allow edits to the rest of the window while open
        property int pcIndex : -1
        property string pcName : ""
        text: qsTr("Are you sure you want to remove '%1'?").arg(pcName)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            computerModel.deleteComputer(pcIndex)
        }
    }

    NavigableMessageDialog {
        id: testConnectionDialog
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok

        onAboutToShow: {
            testConnectionDialog.text = qsTr("Moonlight is testing your network connection to determine if any required ports are blocked.") + "\n\n" + qsTr("This may take a few seconds…")
            showSpinner = true
        }

        function connectionTestComplete(result, blockedPorts)
        {
            if (result === -1) {
                text = qsTr("The network test could not be performed because none of Moonlight's connection testing servers were reachable from this PC. Check your Internet connection or try again later.")
                imageSrc = "qrc:/res/baseline-warning-24px.svg"
            }
            else if (result === 0) {
                text = qsTr("This network does not appear to be blocking Moonlight. If you still have trouble connecting, check your PC's firewall settings.") + "\n\n" + qsTr("If you are trying to stream over the Internet, install the Moonlight Internet Hosting Tool on your gaming PC and run the included Internet Streaming Tester to check your gaming PC's Internet connection.")
                imageSrc = "qrc:/res/baseline-check_circle_outline-24px.svg"
            }
            else {
                text = qsTr("Your PC's current network connection seems to be blocking Moonlight. Streaming over the Internet may not work while connected to this network.") + "\n\n" + qsTr("The following network ports were blocked:") + "\n"
                text += blockedPorts
                imageSrc = "qrc:/res/baseline-error_outline-24px.svg"
            }

            // Stop showing the spinner and show the image instead
            showSpinner = false
        }
    }

    NavigableDialog {
        id: renamePcDialog
        property string label: qsTr("Enter the new name for this PC:")
        property string originalName
        property int pcIndex : -1;

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                computerModel.renameComputer(pcIndex, editText.text)
            }
        }

        ColumnLayout {
            Label {
                text: renamePcDialog.label
                font.bold: true
            }

            TextField {
                id: editText
                placeholderText: renamePcDialog.originalName
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    renamePcDialog.accept()
                }

                Keys.onEnterPressed: {
                    renamePcDialog.accept()
                }
            }
        }
    }

    NavigableMessageDialog {
        id: showPcDetailsDialog
        property string pcDetails : "";
        text: showPcDetailsDialog.pcDetails
        imageSrc: "qrc:/res/baseline-help_outline-24px.svg"
        standardButtons: Dialog.Ok
    }

    ScrollBar.vertical: ScrollBar {}
}
