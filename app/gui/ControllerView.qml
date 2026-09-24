import QtQuick 2.9
import QtQuick.Controls 2.3
import QtQuick.Controls.Material 2.2
import QtQuick.Layouts 1.3

import SdlGamepadKeyNavigation 1.0
import InputModeTracker 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import TvTheme 1.0

Item {
    id: controllerPage
    objectName: qsTr("Controllers")

    // The controls in each row, left to right
    readonly property int identifyColumn: 0
    readonly property int enabledColumn: 1
    readonly property int moveUpColumn: 2
    readonly property int moveDownColumn: 3

    // Where focus goes after an action rebuilds the list, since the rows are
    // recreated whenever a controller changes
    property string pendingFocusId: ""
    property int pendingFocusColumn: 0

    function rememberFocus(id, column) {
        pendingFocusId = id
        pendingFocusColumn = column
    }

    // Focuses the control in a row's column, or the nearest one to it that
    // can be used. Returns whether anything was focused.
    function focusControl(row, column) {
        if (row < 0 || row >= controllerList.count) {
            return false
        }

        controllerList.currentIndex = row
        controllerList.positionViewAtIndex(row, ListView.Contain)
        var rowItem = controllerList.itemAtIndex(row)
        if (!rowItem) {
            return false
        }

        var controls = rowItem.controls
        for (var distance = 0; distance < controls.length; distance++) {
            var candidates = [column - distance, column + distance]
            for (var i = 0; i < candidates.length; i++) {
                var control = controls[candidates[i]]
                if (control && control.enabled && control.visible) {
                    control.forceActiveFocus(Qt.TabFocusReason)
                    return true
                }
            }
        }
        return false
    }

    // Moves focus across a row with left and right, skipping controls that
    // can't be used, or between rows with up and down, staying in the same
    // column. Up from the first row goes to the top bar.
    function moveFocus(row, column, rowDelta, columnDelta, fromItem) {
        if (rowDelta !== 0) {
            if (!focusControl(row + rowDelta, column) && rowDelta < 0) {
                fromItem.nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocusReason)
            }
            return
        }

        var controls = controllerList.itemAtIndex(row).controls
        for (var c = column + columnDelta; c >= 0 && c < controls.length; c += columnDelta) {
            if (controls[c].enabled && controls[c].visible) {
                controls[c].forceActiveFocus(Qt.TabFocusReason)
                return
            }
        }
    }

    function restorePendingFocus() {
        if (!pendingFocusId) {
            return
        }
        var controllers = SdlGamepadKeyNavigation.controllers
        for (var i = 0; i < controllers.length; i++) {
            if (controllers[i].id === pendingFocusId) {
                var row = i
                var column = pendingFocusColumn
                Qt.callLater(function() { focusControl(row, column) })
                break
            }
        }
        pendingFocusId = ""
    }

    function toggleController(data) {
        rememberFocus(data.id, enabledColumn)
        SdlGamepadKeyNavigation.setControllerEnabled(data.id, !data.enabled)
    }

    function moveController(data, delta) {
        rememberFocus(data.id, delta < 0 ? moveUpColumn : moveDownColumn)
        SdlGamepadKeyNavigation.moveController(data.id, delta)
    }

    function detailsText(data, battery) {
        var parts = []
        if (!data.enabled) {
            parts.push(qsTr("Not passed to host"))
        }
        var batteryDescription = batteryText(battery)
        if (batteryDescription !== "") {
            parts.push(batteryDescription)
        }
        if (data.metadata.length > 0) {
            parts.push(data.metadata)
        }
        return parts.join(" · ")
    }

    // The battery level of a controller, from the status list that updates
    // without rebuilding the rows
    function batteryFor(id, status) {
        for (var i = 0; i < status.length; i++) {
            if (status[i].id === id) {
                return status[i].battery
            }
        }
        return -1
    }

    function batteryText(level) {
        switch (level) {
        case 0: return qsTr("Battery empty")
        case 1: return qsTr("Battery low")
        case 2: return qsTr("Battery medium")
        case 3: return qsTr("Battery full")
        case 4: return qsTr("Wired")
        }
        return ""
    }

    Connections {
        target: SdlGamepadKeyNavigation
        function onControllersChanged() {
            if (controllerPage.pendingFocusId) {
                controllerPage.restorePendingFocus()
                return
            }

            // The rows are recreated, so put focus back in the list if it
            // was there, or on the first row if one just appeared. Leave it
            // alone in the top bar and in dialogs.
            Qt.callLater(function() {
                if (controllerPage.StackView.status !== StackView.Active) {
                    return
                }
                var focusItem = window.activeFocusItem
                if (focusItem === null || focusItem === controllerPage ||
                        !(isSelfOrDescendantOf(focusItem, toolBar) || isInPopup(focusItem) ||
                          isSelfOrDescendantOf(focusItem, controllerList.contentItem))) {
                    if (controllerList.count > 0) {
                        focusControl(Math.min(Math.max(controllerList.currentIndex, 0), controllerList.count - 1),
                                     controllerPage.identifyColumn)
                    }
                    else {
                        controllerPage.forceActiveFocus()
                    }
                }
            })
        }
    }

    // The top bar puts focus on the page itself when moving down from it
    onActiveFocusChanged: {
        if (activeFocus && controllerList.count > 0) {
            focusControl(Math.max(controllerList.currentIndex, 0), identifyColumn)
        }
    }

    StackView.onActivated: {
        if (controllerList.count > 0) {
            Qt.callLater(function() { focusControl(0, identifyColumn) })
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: SystemProperties.tvMode ? TvTheme.focusBleed : 20
        anchors.rightMargin: SystemProperties.tvMode ? TvTheme.focusBleed : 20
        anchors.topMargin: 16
        anchors.bottomMargin: 16
        spacing: SystemProperties.tvMode ? TvTheme.spacingMediumLarge : 12

        Label {
            Layout.fillWidth: true
            text: (StreamingPreferences.multiController ?
                       qsTr("Controllers are passed to the host in this order.") :
                       qsTr("Enabled controllers are combined as Player 1 because Force gamepad #1 always connected is enabled.")) +
                  " " + qsTr("Press a button on a controller to find its row.")
            color: SystemProperties.tvMode ? TvTheme.textSecondary : Material.foreground
            font.pixelSize: SystemProperties.tvMode ? TvTheme.fontCaption : -1
            font.pointSize: SystemProperties.tvMode ? -1 : 12
            wrapMode: Text.WordWrap
        }

        Label {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: controllerList.count === 0
            text: qsTr("No controllers detected")
            color: SystemProperties.tvMode ? TvTheme.textSecondary : Material.foreground
            font.pixelSize: SystemProperties.tvMode ? TvTheme.fontTitle : -1
            font.pointSize: SystemProperties.tvMode ? -1 : 20
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        ListView {
            id: controllerList
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: count > 0
            spacing: SystemProperties.tvMode ? TvTheme.spacingMedium : 10
            model: SdlGamepadKeyNavigation.controllers
            // The page moves focus between rows itself
            keyNavigationEnabled: false

            // Room for the focus ring around the controls in the first and
            // last rows
            topMargin: TvTheme.spacingSmall
            bottomMargin: TvTheme.spacingSmall

            ScrollBar.vertical: ScrollBar {
                visible: !SystemProperties.tvMode
            }

            delegate: SystemProperties.tvMode ? tvRowComponent : desktopRowComponent
        }
    }

    Component {
        id: tvRowComponent

        Rectangle {
            id: controllerRow
            width: controllerList.width
            height: stacked ? rowGrid.implicitHeight + 2 * TvTheme.spacingMediumLarge : 128

            // The details need this much room to show the name. When a
            // narrow window can't fit them beside the controls, the
            // controls move to a line of their own under them.
            readonly property real detailsMinimumWidth: 72 + 56 + 2 * TvTheme.spacingMediumLarge + 300
            readonly property bool stacked: width - rowGrid.anchors.leftMargin - rowGrid.anchors.rightMargin <
                                            detailsMinimumWidth + rowGrid.columnSpacing + controlsRow.implicitWidth
            radius: TvTheme.tileRadius
            color: TvTheme.surface

            readonly property var controls: [identifyButton, enabledSwitch, upButton, downButton]
            readonly property int battery: controllerPage.batteryFor(modelData.id, SdlGamepadKeyNavigation.controllerStatus)

            function focusedColumn() {
                for (var i = 0; i < controls.length; i++) {
                    if (controls[i].activeFocus) {
                        return i
                    }
                }
                return 0
            }

            // Arrow keys the focused control doesn't use arrive here
            Keys.onLeftPressed: controllerPage.moveFocus(index, focusedColumn(), 0, -1, identifyButton)
            Keys.onRightPressed: controllerPage.moveFocus(index, focusedColumn(), 0, 1, identifyButton)
            Keys.onUpPressed: controllerPage.moveFocus(index, focusedColumn(), -1, 0, identifyButton)
            Keys.onDownPressed: controllerPage.moveFocus(index, focusedColumn(), 1, 0, identifyButton)

            // Lights up whenever a button is pressed on this controller, so
            // the user can match the rows to the controllers in their hands
            Rectangle {
                id: activityLight
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                width: 6
                height: parent.height - 48
                radius: width / 2
                color: TvTheme.accent
                opacity: 0.15

                SequentialAnimation {
                    id: activityAnimation
                    PropertyAction { target: activityLight; property: "opacity"; value: 1.0 }
                    PauseAnimation { duration: 150 }
                    NumberAnimation {
                        target: activityLight
                        property: "opacity"
                        to: 0.15
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

            GridLayout {
                id: rowGrid
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 38
                anchors.rightMargin: TvTheme.spacingLarge
                columns: controllerRow.stacked ? 1 : 2
                columnSpacing: TvTheme.spacingMediumLarge
                rowSpacing: TvTheme.spacingMedium

                // The controller's details. Controllers that aren't passed
                // to the host are dimmed, but their controls aren't, so they
                // can still be turned back on.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: TvTheme.spacingMediumLarge
                    opacity: modelData.enabled ? 1.0 : 0.55

                    // The player number
                    Rectangle {
                        Layout.preferredWidth: 72
                        Layout.preferredHeight: 72
                        radius: width / 2
                        color: modelData.enabled ? TvTheme.accent : TvTheme.surfaceRaised

                        Label {
                            anchors.centerIn: parent
                            text: modelData.enabled ? qsTr("P%1").arg(modelData.playerNumber) : "–"
                            color: modelData.enabled ? TvTheme.accentText : TvTheme.textTertiary
                            font.pixelSize: TvTheme.fontLabel
                            font.weight: Font.Bold
                            font.styleName: TvTheme.fontStyleName(TvTheme.bodyFontFamily, font.weight)
                        }
                    }

                    Image {
                        source: "qrc:/res/gamepad.svg"
                        sourceSize.width: 56
                        sourceSize.height: 56
                        opacity: 0.6
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            Layout.fillWidth: true
                            text: modelData.name
                            color: TvTheme.textPrimary
                            font.pixelSize: TvTheme.fontBody
                            font.weight: Font.DemiBold
                            font.styleName: TvTheme.fontStyleName(TvTheme.bodyFontFamily, font.weight)
                            elide: Text.ElideRight
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10

                            TvBatteryIcon {
                                level: controllerRow.battery
                            }

                            Label {
                                Layout.fillWidth: true
                                text: controllerPage.detailsText(modelData, controllerRow.battery)
                                color: TvTheme.textSecondary
                                font.pixelSize: TvTheme.fontCaption
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                // Under the details when stacked, lined up with the name
                RowLayout {
                    id: controlsRow
                    spacing: TvTheme.spacingMediumLarge
                    Layout.leftMargin: controllerRow.stacked ? 72 + 56 + 2 * TvTheme.spacingMediumLarge : 0

                    TvPillButton {
                        id: identifyButton
                        text: qsTr("Identify")
                        enabled: modelData.canIdentify
                        onClicked: SdlGamepadKeyNavigation.identifyController(modelData.id)
                        onActiveFocusChanged: if (activeFocus) controllerList.currentIndex = index
                    }

                    Switch {
                        id: enabledSwitch
                        text: modelData.enabled ? qsTr("Enabled") : qsTr("Disabled")
                        checked: modelData.enabled
                        font.pixelSize: TvTheme.fontLabel
                        font.weight: Font.DemiBold
                        font.styleName: TvTheme.fontStyleName(TvTheme.bodyFontFamily, font.weight)
                        Layout.leftMargin: TvTheme.spacingSmall
                        Layout.rightMargin: TvTheme.spacingSmall

                        // Clicks, touch, and Space toggle the switch themselves.
                        // The gamepad's A button arrives as Return.
                        onClicked: controllerPage.toggleController(modelData)
                        Keys.onReturnPressed: controllerPage.toggleController(modelData)
                        Keys.onEnterPressed: controllerPage.toggleController(modelData)
                        onActiveFocusChanged: if (activeFocus) controllerList.currentIndex = index
                    }

                    TvPillButton {
                        id: upButton
                        iconSource: "qrc:/res/tv_arrow_left.svg"
                        iconRotation: 90
                        enabled: modelData.canMoveUp
                        Accessible.name: qsTr("Move up")
                        onClicked: controllerPage.moveController(modelData, -1)
                        onActiveFocusChanged: if (activeFocus) controllerList.currentIndex = index

                        ToolTip.delay: 1000
                        ToolTip.timeout: 3000
                        ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                        ToolTip.text: qsTr("Move up")
                    }

                    TvPillButton {
                        id: downButton
                        iconSource: "qrc:/res/tv_arrow_left.svg"
                        iconRotation: -90
                        enabled: modelData.canMoveDown
                        Accessible.name: qsTr("Move down")
                        onClicked: controllerPage.moveController(modelData, 1)
                        onActiveFocusChanged: if (activeFocus) controllerList.currentIndex = index

                        ToolTip.delay: 1000
                        ToolTip.timeout: 3000
                        ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                        ToolTip.text: qsTr("Move down")
                    }
                }
            }
        }
    }

    // Desktop mode's rows, in the style of the rest of the desktop app
    Component {
        id: desktopRowComponent

        Rectangle {
            id: desktopRow
            width: controllerList.width - 12
            height: 84
            radius: 4
            // A shade lighter than the page, like a Material card
            color: Qt.lighter(Material.background, 1.35)

            readonly property var controls: [desktopIdentifyButton, desktopEnabledSwitch, desktopUpButton, desktopDownButton]
            readonly property int battery: controllerPage.batteryFor(modelData.id, SdlGamepadKeyNavigation.controllerStatus)

            function focusedColumn() {
                for (var i = 0; i < controls.length; i++) {
                    if (controls[i].activeFocus) {
                        return i
                    }
                }
                return 0
            }

            Keys.onLeftPressed: controllerPage.moveFocus(index, focusedColumn(), 0, -1, desktopIdentifyButton)
            Keys.onRightPressed: controllerPage.moveFocus(index, focusedColumn(), 0, 1, desktopIdentifyButton)
            Keys.onUpPressed: controllerPage.moveFocus(index, focusedColumn(), -1, 0, desktopIdentifyButton)
            Keys.onDownPressed: controllerPage.moveFocus(index, focusedColumn(), 1, 0, desktopIdentifyButton)

            // Lights up whenever a button is pressed on this controller
            Rectangle {
                id: desktopActivityLight
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 4
                color: Material.accent
                opacity: 0

                SequentialAnimation {
                    id: desktopActivityAnimation
                    PropertyAction { target: desktopActivityLight; property: "opacity"; value: 1.0 }
                    PauseAnimation { duration: 150 }
                    NumberAnimation {
                        target: desktopActivityLight
                        property: "opacity"
                        to: 0
                        duration: 600
                        easing.type: Easing.InQuad
                    }
                }
            }

            Connections {
                target: SdlGamepadKeyNavigation
                function onControllerInput(id) {
                    if (id === modelData.id) {
                        desktopActivityAnimation.restart()
                    }
                }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 12
                spacing: 12

                // The player number
                Rectangle {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                    radius: width / 2
                    // The top bar's color, which white text reads well on
                    color: modelData.enabled ? Material.primary : "transparent"
                    border.width: modelData.enabled ? 0 : 2
                    border.color: Material.hintTextColor

                    Label {
                        anchors.centerIn: parent
                        text: modelData.enabled ? qsTr("P%1").arg(modelData.playerNumber) : "–"
                        color: modelData.enabled ? "white" : Material.hintTextColor
                        font.pointSize: 11
                        font.bold: true
                    }
                }

                Image {
                    source: "qrc:/res/ic_videogame_asset_white_48px.svg"
                    sourceSize.width: 40
                    sourceSize.height: 40
                    opacity: modelData.enabled ? 1.0 : 0.5
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    opacity: modelData.enabled ? 1.0 : 0.6

                    Label {
                        Layout.fillWidth: true
                        text: modelData.name
                        font.pointSize: 13
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        TvBatteryIcon {
                            level: desktopRow.battery
                        }

                        Label {
                            Layout.fillWidth: true
                            text: controllerPage.detailsText(modelData, desktopRow.battery)
                            color: Material.secondaryTextColor
                            font.pointSize: 10
                            elide: Text.ElideRight
                        }
                    }
                }

                Button {
                    id: desktopIdentifyButton
                    text: qsTr("Identify")
                    enabled: modelData.canIdentify
                    onClicked: SdlGamepadKeyNavigation.identifyController(modelData.id)
                    onActiveFocusChanged: if (activeFocus) controllerList.currentIndex = index
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                }

                Switch {
                    id: desktopEnabledSwitch
                    text: modelData.enabled ? qsTr("Enabled") : qsTr("Disabled")
                    checked: modelData.enabled
                    font.pointSize: 12

                    // Clicks, touch, and Space toggle the switch themselves.
                    // The gamepad's A button arrives as Return.
                    onClicked: controllerPage.toggleController(modelData)
                    Keys.onReturnPressed: controllerPage.toggleController(modelData)
                    Keys.onEnterPressed: controllerPage.toggleController(modelData)
                    onActiveFocusChanged: if (activeFocus) controllerList.currentIndex = index
                }

                ToolButton {
                    id: desktopUpButton
                    icon.source: "qrc:/res/arrow_left.svg"
                    // Turns the left arrow to point up. The button is round,
                    // so its highlight looks the same turned.
                    rotation: 90
                    enabled: modelData.canMoveUp
                    Accessible.name: qsTr("Move up")
                    onClicked: controllerPage.moveController(modelData, -1)
                    onActiveFocusChanged: if (activeFocus) controllerList.currentIndex = index
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()

                    ToolTip.delay: 1000
                    ToolTip.timeout: 3000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Move up")
                }

                ToolButton {
                    id: desktopDownButton
                    icon.source: "qrc:/res/arrow_left.svg"
                    rotation: -90
                    enabled: modelData.canMoveDown
                    Accessible.name: qsTr("Move down")
                    onClicked: controllerPage.moveController(modelData, 1)
                    onActiveFocusChanged: if (activeFocus) controllerList.currentIndex = index
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()

                    ToolTip.delay: 1000
                    ToolTip.timeout: 3000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Move down")
                }
            }
        }
    }
}
