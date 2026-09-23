import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2
import QtQuick.Window 2.2

import StreamingPreferences 1.0
import ComputerManager 1.0
import SdlGamepadKeyNavigation 1.0
import InputModeTracker 1.0
import SystemProperties 1.0

Flickable {
    // The TV mode toolbar and hint bar are transparent, so settings must not
    // scroll underneath them
    clip: SystemProperties.tvMode

    id: settingsPage
    objectName: qsTr("Settings")

    signal languageChanged()

    boundsBehavior: Flickable.OvershootBounds

    // Stack the two setting columns vertically when the page is too narrow
    // to fit both side by side, such as with a large GUI scale
    readonly property bool singleColumn: width < 1000

    contentWidth: settingsColumn1.width > settingsColumn2.width ? settingsColumn1.width : settingsColumn2.width
    contentHeight: singleColumn ?
                       settingsColumn1.height + settingsColumn2.height :
                       (settingsColumn1.height > settingsColumn2.height ? settingsColumn1.height : settingsColumn2.height)

    ScrollBar.vertical: ScrollBar {
        anchors {
            left: parent.right
            leftMargin: -10
        }
    }

    // Some GUI preferences are only applied when Moonlight starts. Offer to
    // restart if the saved values no longer match what is currently in effect.
    function promptRestartIfNeeded() {
        // A command line option keeps TV mode fixed across a restart
        var tvModeAfterRestart = SystemProperties.tvModeOverridden ? SystemProperties.tvMode : StreamingPreferences.tvMode

        if ((SystemProperties.supportsUiScale && StreamingPreferences.uiScale !== SystemProperties.activeUiScale) ||
                tvModeAfterRestart !== SystemProperties.tvMode) {
            restartDialog.open()
        }
    }

    NavigableMessageDialog {
        id: tvModeScaleDialog
        standardButtons: Dialog.Yes | Dialog.No
        text: qsTr("Your display has a high resolution. Would you also like to increase the GUI scale to 200% so it is easier to read from a distance?")
        onAccepted: {
            StreamingPreferences.uiScale = 200
            for (var i = 0; i < uiScaleListModel.count; i++) {
                if (uiScaleListModel.get(i).val === 200) {
                    uiScaleComboBox.currentIndex = i
                    break
                }
            }
        }

        // Ask about restarting once the scale question has been answered
        onClosed: promptRestartIfNeeded()
    }

    NavigableMessageDialog {
        id: restartDialog
        standardButtons: Dialog.Yes | Dialog.No
        text: qsTr("Moonlight must be restarted for this change to take effect. Restart now?")
        onAccepted: SystemProperties.restartApplication()
    }

    function isDescendantOf(item, ancestor) {
        for (item = item ? item.parent : null; item; item = item.parent) {
            if (item === ancestor) {
                return true
            }
        }
        return false
    }

    function isChildOfFlickable(item) {
        return isDescendantOf(item, contentItem)
    }

    // Moves focus to the nearest focusable control to the left or right of
    // fromItem. Controls that share a row are preferred; otherwise the closest
    // control vertically in that direction (e.g. in the other column) is used.
    // If nothing lies in that direction, focus stays where it is.
    function navigateHorizontally(fromItem, forward) {
        if (!isChildOfFlickable(fromItem)) {
            return
        }

        var from = fromItem.mapToItem(contentItem, 0, 0)
        var fromLeft = from.x
        var fromRight = from.x + fromItem.width
        var fromTop = from.y
        var fromBottom = from.y + fromItem.height

        var bestItem = null
        var bestOverlaps = false
        var bestScore = 0

        // Walk the tab focus chain, which only contains visible, enabled controls
        var item = fromItem
        for (var i = 0; i < 1000; i++) {
            item = item.nextItemInFocusChain(true)
            if (!item || item === fromItem) {
                break
            }
            if (!isChildOfFlickable(item)) {
                continue
            }

            var pos = item.mapToItem(contentItem, 0, 0)
            var left = pos.x
            var right = pos.x + item.width

            // Horizontal gap in the requested direction. Allow a tiny overlap
            // to tolerate rounding in adjacent layouts.
            var dx = forward ? left - fromRight : fromLeft - right
            if (dx < -2) {
                continue
            }
            dx = Math.max(dx, 0)

            var dy = Math.max(0, pos.y - fromBottom, fromTop - (pos.y + item.height))
            var overlaps = dy === 0

            // Among overlapping controls, prefer the nearest one whose vertical
            // center is best aligned with ours.
            var centerDy = Math.abs((pos.y + item.height / 2) - (fromTop + fromItem.height / 2))
            var score = overlaps ? dx + centerDy : dy * 10 + dx

            if (bestItem === null ||
                    (overlaps && !bestOverlaps) ||
                    (overlaps === bestOverlaps && score < bestScore)) {
                bestItem = item
                bestOverlaps = overlaps
                bestScore = score
            }
        }

        if (bestItem !== null) {
            bestItem.forceActiveFocus(Qt.TabFocus)
        }
    }

    // Left/Right key events from focused controls that don't consume them
    // (check boxes, combo boxes, buttons) bubble up to here.
    Keys.onLeftPressed: {
        navigateHorizontally(Window.activeFocusItem, false)
    }

    Keys.onRightPressed: {
        navigateHorizontally(Window.activeFocusItem, true)
    }

    NumberAnimation on contentY {
        id: autoScrollAnimation
        duration: 100
    }

    Window.onActiveFocusItemChanged: {
        var item = Window.activeFocusItem
        if (item) {
            // Ignore non-child elements like the toolbar buttons
            if (!isChildOfFlickable(item)) {
                return
            }

            // Map the focus item's position into our content item's coordinate space
            var pos = item.mapToItem(contentItem, 0, 0)

            // Ensure some extra space is visible around the element we're scrolling to
            var scrollMargin = height > 100 ? 50 : 0

            if (pos.y - scrollMargin < contentY) {
                autoScrollAnimation.from = contentY
                autoScrollAnimation.to = Math.max(pos.y - scrollMargin, 0)
                autoScrollAnimation.start()
            }
            else if (pos.y + item.height + scrollMargin > contentY + height) {
                autoScrollAnimation.from = contentY
                autoScrollAnimation.to = Math.min(pos.y + item.height + scrollMargin - height, contentHeight - height)
                autoScrollAnimation.start()
            }
        }
    }

    StackView.onActivated: {
        // This enables Tab and BackTab based navigation rather than arrow keys.
        // It is required to shift focus between controls on the settings page.
        SdlGamepadKeyNavigation.setUiNavMode(true)

        // Highlight the first item if a gamepad is connected
        if (SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            resolutionComboBox.forceActiveFocus(Qt.TabFocus)
        }
    }

    StackView.onDeactivating: {
        SdlGamepadKeyNavigation.setUiNavMode(false)

        // Save the prefs so the Session can observe the changes
        StreamingPreferences.save()
    }

    Component.onDestruction: {
        // Also save preferences on destruction, since we won't get a
        // deactivating callback if the user just closes Moonlight
        StreamingPreferences.save()
    }

    Column {
        padding: 10
        // Leave room for the scroll bar when this column spans the page
        rightPadding: singleColumn ? 20 : 10
        id: settingsColumn1
        width: singleColumn ? settingsPage.width : settingsPage.width / 2
        spacing: 15

        SettingsGroupBox {
            id: basicSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Basic Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: resFPStitle
                    text: qsTr("Resolution and FPS")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: resFPSdesc
                    text: qsTr("Setting values too high for your PC or network connection may cause lag, stuttering, or errors.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                Row {
                    spacing: 5
                    width: parent.width

                    AutoResizingComboBox {
                        property int lastIndexValue

                        function addDetectedResolution(friendlyNamePrefix, rect) {
                            var indexToAdd = 0
                            for (var j = 0; j < resolutionComboBox.count; j++) {
                                var existing_width = parseInt(resolutionListModel.get(j).video_width);
                                var existing_height = parseInt(resolutionListModel.get(j).video_height);

                                if (rect.width === existing_width && rect.height === existing_height) {
                                    // Duplicate entry, skip
                                    indexToAdd = -1
                                    break
                                }
                                else if (rect.width * rect.height > existing_width * existing_height) {
                                    // Candidate entrypoint after this entry
                                    indexToAdd = j + 1
                                }
                            }

                            // Insert this display's resolution if it's not a duplicate
                            if (indexToAdd >= 0) {
                                resolutionListModel.insert(indexToAdd,
                                                           {
                                                               "text": friendlyNamePrefix+" ("+rect.width+"x"+rect.height+")",
                                                               "video_width": ""+rect.width,
                                                               "video_height": ""+rect.height,
                                                               "is_custom": false
                                                           })
                            }
                        }

                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            // Refresh display data before using it to build the list
                            SystemProperties.refreshDisplays()

                            // Add native and safe area resolutions for all attached displays
                            var done = false
                            for (var displayIndex = 0; !done; displayIndex++) {
                                var screenRect = SystemProperties.getNativeResolution(displayIndex);
                                var safeAreaRect = SystemProperties.getSafeAreaResolution(displayIndex);

                                if (screenRect.width === 0) {
                                    // Exceeded max count of displays
                                    done = true
                                    break
                                }

                                addDetectedResolution(qsTr("Native"), screenRect)
                                addDetectedResolution(qsTr("Native (Excluding Notch)"), safeAreaRect)
                            }

                            // Prune resolutions that are over the decoder's maximum
                            var max_pixels = SystemProperties.maximumResolution.width * SystemProperties.maximumResolution.height;
                            if (max_pixels > 0) {
                                for (var j = 0; j < resolutionComboBox.count; j++) {
                                    var existing_width = parseInt(resolutionListModel.get(j).video_width);
                                    var existing_height = parseInt(resolutionListModel.get(j).video_height);

                                    if (existing_width * existing_height > max_pixels) {
                                        resolutionListModel.remove(j)
                                        j--
                                    }
                                }
                            }

                            // load the saved width/height, and iterate through the ComboBox until a match is found
                            // and set it to that index.
                            var saved_width = StreamingPreferences.width
                            var saved_height = StreamingPreferences.height
                            var index_set = false
                            for (var i = 0; i < resolutionListModel.count; i++) {
                                var el_width = parseInt(resolutionListModel.get(i).video_width);
                                var el_height = parseInt(resolutionListModel.get(i).video_height);

                                if (saved_width === el_width && saved_height === el_height) {
                                    currentIndex = i
                                    index_set = true
                                    break
                                }
                            }

                            if (!index_set) {
                                // We did not find a match. This must be a custom resolution.
                                resolutionListModel.append({
                                                               "text": qsTr("Custom")+" ("+StreamingPreferences.width+"x"+StreamingPreferences.height+")",
                                                               "video_width": ""+StreamingPreferences.width,
                                                               "video_height": ""+StreamingPreferences.height,
                                                               "is_custom": true
                                                           })
                                currentIndex = resolutionListModel.count - 1
                            }
                            else {
                                resolutionListModel.append({
                                                               "text": qsTr("Custom"),
                                                               "video_width": "",
                                                               "video_height": "",
                                                               "is_custom": true
                                                           })
                            }

                            // Since we don't call activate() here, we need to trigger
                            // width calculation manually
                            recalculateWidth()

                            lastIndexValue = currentIndex
                        }

                        id: resolutionComboBox
                        maximumWidth: parent.width / 2
                        textRole: "text"
                        model: ListModel {
                            id: resolutionListModel
                            // Other elements may be added at runtime
                            // based on attached display resolution
                            ListElement {
                                text: qsTr("720p")
                                video_width: "1280"
                                video_height: "720"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("1080p")
                                video_width: "1920"
                                video_height: "1080"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("1440p")
                                video_width: "2560"
                                video_height: "1440"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("4K")
                                video_width: "3840"
                                video_height: "2160"
                                is_custom: false
                            }
                        }

                        function updateBitrateForSelection() {
                            var selectedWidth = parseInt(resolutionListModel.get(currentIndex).video_width)
                            var selectedHeight = parseInt(resolutionListModel.get(currentIndex).video_height)

                            // Only modify the bitrate if the values actually changed
                            if (StreamingPreferences.width !== selectedWidth || StreamingPreferences.height !== selectedHeight) {
                                StreamingPreferences.width = selectedWidth
                                StreamingPreferences.height = selectedHeight

                                if (StreamingPreferences.autoAdjustBitrate) {
                                    StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                                              StreamingPreferences.height,
                                                                                                              StreamingPreferences.fps,
                                                                                                              StreamingPreferences.enableYUV444);
                                    slider.value = StreamingPreferences.bitrateKbps
                                }
                            }

                            lastIndexValue = currentIndex
                        }

                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated : {
                            if (resolutionListModel.get(currentIndex).is_custom) {
                                customResolutionDialog.open()
                            }
                            else {
                                updateBitrateForSelection()
                            }
                        }

                        NavigableDialog {
                            id: customResolutionDialog
                            standardButtons: Dialog.Ok | Dialog.Cancel
                            onOpened: {
                                // Force keyboard focus on the textbox so keyboard navigation works
                                widthField.forceActiveFocus()

                                // standardButton() was added in Qt 5.10, so we must check for it first
                                if (customResolutionDialog.standardButton) {
                                    customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                }
                            }

                            onClosed: {
                                widthField.clear()
                                heightField.clear()
                            }

                            onRejected: {
                                resolutionComboBox.currentIndex = resolutionComboBox.lastIndexValue
                            }

                            function isInputValid() {
                                // If we have text in either textbox that isn't valid,
                                // reject the input.
                                if ((!widthField.acceptableInput && widthField.text) ||
                                        (!heightField.acceptableInput && heightField.text)) {
                                    return false
                                }

                                // The textboxes need to have text or placeholder text
                                if ((!widthField.text && !widthField.placeholderText) ||
                                        (!heightField.text && !heightField.placeholderText)) {
                                    return false
                                }

                                return true
                            }

                            onAccepted: {
                                // Reject if there's invalid input
                                if (!isInputValid()) {
                                    reject()
                                    return
                                }

                                var width = widthField.text ? widthField.text : widthField.placeholderText
                                var height = heightField.text ? heightField.text : heightField.placeholderText

                                // Find and update the custom entry
                                for (var i = 0; i < resolutionListModel.count; i++) {
                                    if (resolutionListModel.get(i).is_custom) {
                                        resolutionListModel.setProperty(i, "video_width", width)
                                        resolutionListModel.setProperty(i, "video_height", height)
                                        resolutionListModel.setProperty(i, "text", "Custom ("+width+"x"+height+")")

                                        // Now update the bitrate using the custom resolution
                                        resolutionComboBox.currentIndex = i
                                        resolutionComboBox.updateBitrateForSelection()

                                        // Update the combobox width too
                                        resolutionComboBox.recalculateWidth()
                                        break
                                    }
                                }
                            }

                            ColumnLayout {
                                Label {
                                    text: qsTr("Custom resolutions are not officially supported by GeForce Experience, so it will not set your host display resolution. You will need to set it manually while in game.") + "\n\n" +
                                          qsTr("Resolutions that are not supported by your client or host PC may cause streaming errors.") + "\n"
                                    wrapMode: Label.WordWrap
                                    Layout.maximumWidth: 300
                                }

                                Label {
                                    text: qsTr("Enter a custom resolution:")
                                    font.bold: true
                                }

                                RowLayout {
                                    TextField {
                                        id: widthField
                                        maximumLength: 5
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: resolutionListModel.get(resolutionComboBox.currentIndex).video_width
                                        validator: IntValidator{bottom:256; top:8192}
                                        focus: true

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customResolutionDialog.standardButton) {
                                                customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customResolutionDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customResolutionDialog.accept()
                                        }
                                    }

                                    Label {
                                        text: "x"
                                        font.bold: true
                                    }

                                    TextField {
                                        id: heightField
                                        maximumLength: 5
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: resolutionListModel.get(resolutionComboBox.currentIndex).video_height
                                        validator: IntValidator{bottom:256; top:8192}

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customResolutionDialog.standardButton) {
                                                customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customResolutionDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customResolutionDialog.accept()
                                        }
                                    }
                                }
                            }
                        }
                    }

                    AutoResizingComboBox {
                        property int lastIndexValue

                        function updateBitrateForSelection() {
                            var selectedFps = parseInt(model.get(fpsComboBox.currentIndex).video_fps)
                            var fpsChanged = StreamingPreferences.fps !== selectedFps
                            StreamingPreferences.fps = selectedFps

                            if (fpsChanged && StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                                          StreamingPreferences.height,
                                                                                                          StreamingPreferences.fps,
                                                                                                          StreamingPreferences.enableYUV444);
                                slider.value = StreamingPreferences.bitrateKbps
                            }

                            lastIndexValue = currentIndex
                        }

                        NavigableDialog {
                            function isInputValid() {
                                // If we have text that isn't valid, reject the input.
                                if (!fpsField.acceptableInput && fpsField.text) {
                                    return false
                                }

                                // The textbox needs to have text or placeholder text
                                if (!fpsField.text && !fpsField.placeholderText) {
                                    return false
                                }

                                return true
                            }

                            id: customFpsDialog
                            standardButtons: Dialog.Ok | Dialog.Cancel
                            onOpened: {
                                // Force keyboard focus on the textbox so keyboard navigation works
                                fpsField.forceActiveFocus()

                                // standardButton() was added in Qt 5.10, so we must check for it first
                                if (customFpsDialog.standardButton) {
                                    customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                                }
                            }

                            onClosed: {
                                fpsField.clear()
                            }

                            onRejected: {
                                fpsComboBox.currentIndex = fpsComboBox.lastIndexValue
                            }

                            onAccepted: {
                                // Reject if there's invalid input
                                if (!isInputValid()) {
                                    reject()
                                    return
                                }

                                var fps = fpsField.text ? fpsField.text : fpsField.placeholderText

                                // Find and update the custom entry
                                for (var i = 0; i < fpsListModel.count; i++) {
                                    if (fpsListModel.get(i).is_custom) {
                                        fpsListModel.setProperty(i, "video_fps", fps)
                                        fpsListModel.setProperty(i, "text", qsTr("Custom (%1 FPS)").arg(fps))

                                        // Now update the bitrate using the custom resolution
                                        fpsComboBox.currentIndex = i
                                        fpsComboBox.updateBitrateForSelection()

                                        // Update the combobox width too
                                        fpsComboBox.recalculateWidth()
                                        break
                                    }
                                }
                            }

                            ColumnLayout {
                                Label {
                                    text: qsTr("Enter a custom frame rate:")
                                    font.bold: true
                                }

                                RowLayout {
                                    TextField {
                                        id: fpsField
                                        maximumLength: 4
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: fpsListModel.get(fpsComboBox.currentIndex).video_fps
                                        validator: IntValidator{bottom:10; top:9999}
                                        focus: true

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customFpsDialog.standardButton) {
                                                customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customFpsDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customFpsDialog.accept()
                                        }
                                    }
                                }
                            }
                        }

                        function getRefreshRates() {
                            var refreshRates = []
                            for (var displayIndex = 0; ; displayIndex++) {
                                var refreshRate = SystemProperties.getRefreshRate(displayIndex)
                                if (refreshRate === 0) {
                                    break
                                }

                                refreshRates.push(refreshRate)
                            }

                            return refreshRates
                        }

                        function choiceText(choice) {
                            switch (choice.kind) {
                            case "vrr":
                                return qsTr("VRR (%1 FPS)").arg(choice.video_fps)
                            case "low-latency-vrr":
                                return qsTr("Low-latency VRR (%1 FPS)").arg(choice.video_fps)
                            case "custom":
                                return qsTr("Custom (%1 FPS)").arg(choice.video_fps)
                            default:
                                return qsTr("%1 FPS").arg(choice.video_fps)
                            }
                        }

                        function reinitialize() {
                            var choices = StreamingPreferences.getFpsChoices(getRefreshRates())
                            model.clear()
                            var hasCustomChoice = false

                            for (var i = 0; i < choices.length; i++) {
                                var choice = choices[i]
                                hasCustomChoice = hasCustomChoice || choice.is_custom
                                model.append({
                                                 "text": choiceText(choice),
                                                 "video_fps": choice.video_fps,
                                                 "is_custom": choice.is_custom
                                             })
                            }

                            var saved_fps = StreamingPreferences.fps
                            var found = false
                            for (var i = 0; i < model.count; i++) {
                                var el_fps = parseInt(model.get(i).video_fps);

                                // Look for a matching frame rate
                                if (saved_fps === el_fps) {
                                    currentIndex = i
                                    found = true
                                    break
                                }
                            }

                            // Saved custom and native maximum choices remain visible.
                            if (!found) {
                                currentIndex = model.count > 0 ? 0 : -1
                            }

                            if (!hasCustomChoice) {
                                model.append({
                                                 "text": qsTr("Custom"),
                                                 "video_fps": "",
                                                 "is_custom": true
                                             })
                            }

                            recalculateWidth()

                            lastIndexValue = currentIndex
                        }

                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            reinitialize()
                            languageChanged.connect(reinitialize)
                            StreamingPreferences.enableVsyncChanged.connect(reinitialize)
                            StreamingPreferences.enableVrrChanged.connect(reinitialize)
                        }

                        model: ListModel {
                            id: fpsListModel
                        }

                        id: fpsComboBox
                        maximumWidth: parent.width / 2
                        textRole: "text"
                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated : {
                            if (model.get(currentIndex).is_custom) {
                                customFpsDialog.open()
                            }
                            else {
                                updateBitrateForSelection()
                            }
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: bitrateTitle
                    text: qsTr("Video bitrate:")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: bitrateDesc
                    text: qsTr("Lower the bitrate on slower connections. Raise the bitrate to increase image quality.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                Row {
                    width: parent.width
                    spacing: 5

                    NavigableSlider {
                        id: slider

                        value: StreamingPreferences.bitrateKbps

                        stepSize: 500
                        from : 500
                        to: StreamingPreferences.unlockBitrate ? 500000 : 150000

                        snapMode: "SnapOnRelease"
                        width: Math.min(bitrateDesc.implicitWidth, parent.width - (resetBitrateButton.visible ? resetBitrateButton.width + parent.spacing : 0))

                        onValueChanged: {
                            bitrateTitle.text = qsTr("Video bitrate: %1 Mbps").arg(value / 1000.0)
                            StreamingPreferences.bitrateKbps = value
                        }

                        onMoved: {
                            StreamingPreferences.autoAdjustBitrate = false
                        }

                        Component.onCompleted: {
                            // Refresh the text after translations change
                            languageChanged.connect(valueChanged)
                        }
                    }

                    Button {
                        id: resetBitrateButton
                        text: qsTr("Use Default (%1 Mbps)").arg(StreamingPreferences.getDefaultBitrate(StreamingPreferences.width, StreamingPreferences.height, StreamingPreferences.fps, StreamingPreferences.enableYUV444) / 1000.0)
                        visible: StreamingPreferences.bitrateKbps !== StreamingPreferences.getDefaultBitrate(StreamingPreferences.width, StreamingPreferences.height, StreamingPreferences.fps, StreamingPreferences.enableYUV444)
                        onClicked: {
                            var defaultBitrate = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width, StreamingPreferences.height, StreamingPreferences.fps, StreamingPreferences.enableYUV444)
                            StreamingPreferences.bitrateKbps = defaultBitrate
                            StreamingPreferences.autoAdjustBitrate = true
                            slider.value = defaultBitrate
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: windowModeTitle
                    text: qsTr("Display mode")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: SystemProperties.hasDesktopEnvironment
                }

                AutoResizingComboBox {
                    function createModel() {
                        var model = Qt.createQmlObject('import QtQuick 2.0; ListModel {}', parent, '')

                        model.append({
                                         text: qsTr("Fullscreen"),
                                         val: StreamingPreferences.WM_FULLSCREEN
                                     })

                        model.append({
                                         text: qsTr("Borderless windowed"),
                                         val: StreamingPreferences.WM_FULLSCREEN_DESKTOP
                                     })

                        model.append({
                                         text: qsTr("Windowed"),
                                         val: StreamingPreferences.WM_WINDOWED
                                     })


                        // Set the recommended option based on the OS
                        for (var i = 0; i < model.count; i++) {
                            var thisWm = model.get(i).val;
                            if (thisWm === StreamingPreferences.recommendedFullScreenMode) {
                                model.get(i).text += " " + qsTr("(Recommended)")
                                model.move(i, 0, 1)
                                break
                            }
                        }

                        return model
                    }


                    // This is used on initialization and upon retranslation
                    function reinitialize() {
                        if (!visible) {
                            // Do nothing if the control won't even be visible
                            return
                        }

                        model = createModel()
                        currentIndex = 0

                        // VRR sessions use borderless presentation, but the
                        // saved window-mode preference is never overwritten.
                        var savedWm = vrrForced ?
                                          StreamingPreferences.WM_FULLSCREEN_DESKTOP :
                                          StreamingPreferences.windowMode
                        for (var i = 0; i < model.count; i++) {
                             var thisWm = model.get(i).val;
                             if (savedWm === thisWm) {
                                 currentIndex = i
                                 break
                             }
                        }

                        if (!vrrForced) {
                            activated(currentIndex)
                        }
                    }

                    Component.onCompleted: {
                        reinitialize()
                        languageChanged.connect(reinitialize)
                    }

                    id: windowModeComboBox
                    property bool vrrForced: StreamingPreferences.enableVsync && StreamingPreferences.enableVrr
                    onVrrForcedChanged: reinitialize()
                    visible: SystemProperties.hasDesktopEnvironment
                    enabled: !SystemProperties.rendererAlwaysFullScreen && !vrrForced
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    textRole: "text"
                    onActivated: {
                        StreamingPreferences.windowMode = model.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: vrrForced ?
                                      qsTr("Borderless windowed mode is required for active VRR streaming. Your saved display mode will be restored for non-VRR sessions.")
                                    :
                                      qsTr("Fullscreen generally provides the best performance, but borderless windowed may work better with features like macOS Spaces, Alt+Tab, screenshot tools, on-screen overlays, etc.")
                }

                Row {
                    spacing: 5
                    width: parent.width

                    CheckBox {
                        id: vsyncCheck
                        hoverEnabled: !SystemProperties.hoverEffectsDisabled
                        text: qsTr("V-Sync")
                        font.pointSize:  12
                        checked: StreamingPreferences.enableVsync
                        onCheckedChanged: {
                            StreamingPreferences.enableVsync = checked
                        }

                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                        ToolTip.text: qsTr("Disabling V-Sync allows sub-frame rendering latency, but it can display visible tearing")
                    }

                    CheckBox {
                        id: framePacingCheck
                        hoverEnabled: !SystemProperties.hoverEffectsDisabled
                        text: qsTr("Frame pacing")
                        font.pointSize:  12
                        enabled: StreamingPreferences.enableVsync
                        checked: StreamingPreferences.enableVsync && StreamingPreferences.framePacing
                        onCheckedChanged: {
                            StreamingPreferences.framePacing = checked
                        }
                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                        ToolTip.text: qsTr("Frame pacing reduces micro-stutter by delaying frames that come in too early")
                    }

                    CheckBox {
                        hoverEnabled: !SystemProperties.hoverEffectsDisabled
                        text: qsTr("VRR")
                        font.pointSize: 12
                        enabled: StreamingPreferences.enableVsync
                        checked: StreamingPreferences.enableVrr
                        onCheckedChanged: {
                            StreamingPreferences.enableVrr = checked
                        }

                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                        ToolTip.text: enabled ?
                                          qsTr("VRR uses adaptive presentation in borderless fullscreen. Choose your display's full refresh rate, or a lower VRR option for more headroom or lower latency.")
                                        :
                                          qsTr("VRR requires V-Sync. Enable V-Sync to change this setting.")
                    }
                }

                Column {
                    width: parent.width
                    spacing: 5
                    visible: StreamingPreferences.enableVrr
                    enabled: StreamingPreferences.enableVsync && StreamingPreferences.enableVrr

                    Label {
                        width: parent.width
                        text: qsTr("VRR timing")
                        font.pointSize: 12
                        wrapMode: Text.Wrap
                    }

                    AutoResizingComboBox {
                        id: vrrLatencyModeComboBox
                        textRole: "text"
                        model: ListModel {
                            id: vrrLatencyModeListModel
                            ListElement {
                                text: qsTr("Low Latency")
                                val: StreamingPreferences.VLM_LOW_LATENCY
                            }
                            ListElement {
                                text: qsTr("Balanced Target")
                                val: StreamingPreferences.VLM_BALANCED_TARGET
                            }
                            ListElement {
                                text: qsTr("Smooth")
                                val: StreamingPreferences.VLM_SMOOTH
                            }
                        }
                        currentIndex: {
                            for (var i = 0; i < vrrLatencyModeListModel.count; i++) {
                                if (vrrLatencyModeListModel.get(i).val === StreamingPreferences.vrrLatencyMode) {
                                    return i
                                }
                            }
                            return 1
                        }
                        onActivated: {
                            StreamingPreferences.vrrLatencyMode = vrrLatencyModeListModel.get(currentIndex).val
                        }
                        Component.onCompleted: {
                            recalculateWidth()
                            languageChanged.connect(recalculateWidth)
                        }
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: StreamingPreferences.vrrLatencyMode === StreamingPreferences.VLM_LOW_LATENCY ?
                                  qsTr("Minimizes added delay. Uneven delivery can cause more stutter or skipped frames.") :
                              StreamingPreferences.vrrLatencyMode === StreamingPreferences.VLM_SMOOTH ?
                                  qsTr("Uses more padding and holds it longer for steadier motion, with more input delay.") :
                                  qsTr("Targets steadier motion with a moderate timing reserve and balanced input delay.")
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: StreamingPreferences.vrrLatencyMode === StreamingPreferences.VLM_SMOOTH ?
                                  qsTr("Buffer allowance: up to 4 source frames, at most 24 ms, limited by queue capacity. Actual learned delay may be lower.") :
                                  StreamingPreferences.vrrLatencyMode === StreamingPreferences.VLM_LOW_LATENCY ?
                                  qsTr("Buffer allowance: up to 1 source frame, at most 16 ms, limited by queue capacity. Actual learned delay may be lower.") :
                                  qsTr("Buffer allowance: up to 2 source frames, at most 16 ms, limited by queue capacity. Actual learned delay may be lower.")
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Applies at all VRR frame rates. Reconnect the stream after changing this setting.")
                    }
                }

                CheckBox {
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    text: qsTr("Reduce judder")
                    font.pointSize: 12
                    visible: StreamingPreferences.enableVrr
                    enabled: StreamingPreferences.enableVsync && StreamingPreferences.enableVrr
                    checked: StreamingPreferences.smoothVrrFrameTiming
                    onCheckedChanged: StreamingPreferences.smoothVrrFrameTiming = checked

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("Evens out when frames are displayed, including games whose frame rate does not divide the host display's refresh rate. Adds up to a few milliseconds of delay only while uneven frames need it. Does not blend images or eliminate game stalls.") + "\n\n" +
                                  qsTr("Reconnect the stream after changing this setting.")
                }

                CheckBox {
                    id: enableHdr
                    width: parent.width
                    text: qsTr("Enable HDR / 10-bit color")
                    font.pointSize: 12

                    enabled: SystemProperties.supportsHdr
                    checked: enabled && StreamingPreferences.enableHdr
                    onCheckedChanged: {
                        StreamingPreferences.enableHdr = checked
                    }

                    // Updating StreamingPreferences.videoCodecConfig is handled above

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: enabled ?
                                      qsTr("The stream will be HDR-capable, but some games may require an HDR monitor on your host PC to enable HDR mode. Alternatively, enables 10-bit SDR if enabled in Vibeshine or Vibepollo.")
                                    :
                                      qsTr("HDR streaming is not supported on this PC.")
                }

                Column {
                    width: parent.width
                    spacing: 5
                    visible: SystemProperties.supportsVideoDithering && enableHdr.checked

                    Label {
                        width: parent.width
                        text: qsTr("Dither 10-bit video")
                        font.pointSize: 12
                        wrapMode: Text.Wrap
                    }

                    AutoResizingComboBox {
                        id: ditheringModeComboBox
                        textRole: "text"
                        model: ListModel {
                            id: ditheringModeListModel
                            ListElement {
                                text: qsTr("Off")
                                val: StreamingPreferences.DM_OFF
                            }
                            ListElement {
                                text: qsTr("Fast")
                                val: StreamingPreferences.DM_ORDERED
                            }
                            ListElement {
                                text: qsTr("Balanced")
                                val: StreamingPreferences.DM_BLUE_NOISE
                            }
                            ListElement {
                                text: qsTr("High Quality")
                                val: StreamingPreferences.DM_ERROR_DIFFUSION
                            }
                            ListElement {
                                text: qsTr("Highest Quality")
                                val: StreamingPreferences.DM_ERROR_DIFFUSION_HQ
                            }
                        }
                        currentIndex: {
                            for (var i = 0; i < ditheringModeListModel.count; i++) {
                                if (ditheringModeListModel.get(i).val === StreamingPreferences.ditheringMode) {
                                    return i
                                }
                            }
                            return 0
                        }
                        onActivated: {
                            StreamingPreferences.ditheringMode = ditheringModeListModel.get(currentIndex).val
                        }
                        Component.onCompleted: {
                            recalculateWidth()
                            languageChanged.connect(recalculateWidth)
                        }
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: StreamingPreferences.ditheringMode === StreamingPreferences.DM_OFF ?
                                  qsTr("10-bit video is reduced to the output depth without dithering, which can show banding in gradients.") :
                              StreamingPreferences.ditheringMode === StreamingPreferences.DM_ORDERED ?
                                  qsTr("Cheapest kernel. Breaks up banding with a fixed pattern that can be visible up close.") :
                              StreamingPreferences.ditheringMode === StreamingPreferences.DM_BLUE_NOISE ?
                                  qsTr("Recommended. Good quality with a small, steady cost per frame.") :
                              StreamingPreferences.ditheringMode === StreamingPreferences.DM_ERROR_DIFFUSION ?
                                  qsTr("Better gradients at a higher GPU cost. Falls back to Balanced if the GPU cannot run it.") :
                                  qsTr("Best gradients at the highest GPU cost, which may add latency. Falls back to Balanced if the GPU cannot run it.")
                    }

                    CheckBox {
                        id: temporalDithering
                        width: parent.width
                        text: qsTr("Vary the pattern each frame")
                        font.pointSize: 12

                        visible: StreamingPreferences.ditheringMode !== StreamingPreferences.DM_OFF
                        checked: StreamingPreferences.temporalDithering
                        onCheckedChanged: {
                            StreamingPreferences.temporalDithering = checked
                        }

                        ToolTip.delay: 1000
                        ToolTip.timeout: 10000
                        ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                        ToolTip.text: qsTr("Stops the dither pattern from sitting still on screen, which can otherwise look like a faint fixed texture during motion. Some panels show this as flicker, so try it both ways.")
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Only affects 10-bit SDR streams. Reconnect the stream after changing this setting.")
                    }
                }

                Column {
                    width: parent.width
                    spacing: 5
                    visible: SystemProperties.supportsVideoDebanding

                    Label {
                        width: parent.width
                        text: qsTr("Smooth banded gradients")
                        font.pointSize: 12
                        wrapMode: Text.Wrap
                    }

                    AutoResizingComboBox {
                        id: debandModeComboBox
                        textRole: "text"
                        model: ListModel {
                            id: debandModeListModel
                            ListElement {
                                text: qsTr("Off")
                                val: StreamingPreferences.DB_OFF
                            }
                            ListElement {
                                text: qsTr("Grain Only")
                                val: StreamingPreferences.DB_GRAIN_ONLY
                            }
                            ListElement {
                                text: qsTr("Light")
                                val: StreamingPreferences.DB_LIGHT
                            }
                            ListElement {
                                text: qsTr("Medium")
                                val: StreamingPreferences.DB_MEDIUM
                            }
                            ListElement {
                                text: qsTr("Strong")
                                val: StreamingPreferences.DB_STRONG
                            }
                        }
                        currentIndex: {
                            for (var i = 0; i < debandModeListModel.count; i++) {
                                if (debandModeListModel.get(i).val === StreamingPreferences.debandMode) {
                                    return i
                                }
                            }
                            return 0
                        }
                        onActivated: {
                            StreamingPreferences.debandMode = debandModeListModel.get(currentIndex).val
                        }
                        Component.onCompleted: {
                            recalculateWidth()
                            languageChanged.connect(recalculateWidth)
                        }
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: StreamingPreferences.debandMode === StreamingPreferences.DB_OFF ?
                                  qsTr("Wide, soft gradients keep whatever banding the host encoder left in them.") :
                              StreamingPreferences.debandMode === StreamingPreferences.DB_GRAIN_ONLY ?
                                  qsTr("Cheapest. Covers contours with noise without rebuilding the gradient.") :
                              StreamingPreferences.debandMode === StreamingPreferences.DB_LIGHT ?
                                  qsTr("Gentle. Keeps fine detail, so heavy banding may still show.") :
                              StreamingPreferences.debandMode === StreamingPreferences.DB_MEDIUM ?
                                  qsTr("Recommended starting point for visible banding in dark scenes.") :
                                  qsTr("Reaches the widest gradients at the highest GPU cost, and can soften fine detail.")
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("Rebuilds gradients before they are reduced for output, so unlike dithering it can also reduce banding that came from the host. Works best with 10-bit SDR streams. Reconnect the stream after changing this setting.")
                    }
                }
            }
        }

        GroupBox {
            width: parent.width - (parent.leftPadding + parent.rightPadding)
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("VRR diagnostics") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 8

                CheckBox {
                    id: traceVrrFramesCheckBox
                    text: qsTr("Trace VRR frames for debugging")
                    font.pointSize: 12
                    checked: StreamingPreferences.traceVrrFrames
                    onCheckedChanged: StreamingPreferences.traceVrrFrames = checked
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Saves frame traces and session logs to the vrr-diagnostics folder on your Desktop, with a separate folder for each stream. Does not change your VRR timing settings.")
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Enable VRR and reconnect the stream to start recording. Tracing can use substantial disk space and add diagnostic overhead. Uncheck this after debugging.")
                }

                Button {
                    text: StreamingPreferences.exportingDiagnostics ? qsTr("Exporting...") : qsTr("Export latest recording (ZIP)")
                    enabled: !StreamingPreferences.exportingDiagnostics
                    onClicked: StreamingPreferences.exportLatestDiagnostics()
                }

                Button {
                    text: qsTr("Open diagnostics folder")
                    onClicked: StreamingPreferences.openDiagnosticsFolder()
                }

                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Disconnect before exporting. Logs contain hardware and connection details; review them before sharing. Nothing is uploaded automatically.")
                }

                Label {
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                    visible: text.length > 0
                    text: StreamingPreferences.diagnosticsStatus
                }
            }
        }

        SettingsGroupBox {

            id: audioSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Audio Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: resAudioTitle
                    text: qsTr("Audio configuration")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_audio = StreamingPreferences.audioConfig
                        currentIndex = 0
                        for (var i = 0; i < audioListModel.count; i++) {
                            var el_audio = audioListModel.get(i).val;
                            if (saved_audio === el_audio) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
                    }

                    id: audioComboBox
                    textRole: "text"
                    model: ListModel {
                        id: audioListModel
                        ListElement {
                            text: qsTr("Stereo")
                            val: StreamingPreferences.AC_STEREO
                        }
                        ListElement {
                            text: qsTr("5.1 surround sound")
                            val: StreamingPreferences.AC_51_SURROUND
                        }
                        ListElement {
                            text: qsTr("7.1 surround sound")
                            val: StreamingPreferences.AC_71_SURROUND
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        StreamingPreferences.audioConfig = audioListModel.get(currentIndex).val
                    }
                }


                CheckBox {
                    id: audioPcCheck
                    width: parent.width
                    text: qsTr("Mute host PC speakers while streaming")
                    font.pointSize: 12
                    checked: !StreamingPreferences.playAudioOnHost
                    onCheckedChanged: {
                        StreamingPreferences.playAudioOnHost = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("You must restart any game currently in progress for this setting to take effect")
                }

                CheckBox {
                    id: muteOnFocusLossCheck
                    width: parent.width
                    text: qsTr("Mute audio stream when Moonlight is not the active window")
                    font.pointSize: 12
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: StreamingPreferences.muteOnFocusLoss
                    onCheckedChanged: {
                        StreamingPreferences.muteOnFocusLoss = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("Mutes Moonlight's audio when you Alt+Tab out of the stream or click on a different window.")
                }
            }
        }

        SettingsGroupBox {
            id: hostSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Host Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: optimizeGameSettingsCheck
                    width: parent.width
                    text: qsTr("Optimize game settings for streaming")
                    font.pointSize:  12
                    checked: StreamingPreferences.gameOptimizations
                    onCheckedChanged: {
                        StreamingPreferences.gameOptimizations = checked
                    }
                }

                CheckBox {
                    id: quitAppAfter
                    width: parent.width
                    text: qsTr("Quit app on host PC after ending stream")
                    font.pointSize: 12
                    checked: StreamingPreferences.quitAppAfter
                    onCheckedChanged: {
                        StreamingPreferences.quitAppAfter = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("This will close the app or game you are streaming when you end your stream. You will lose any unsaved progress!")
                }
            }
        }

        SettingsGroupBox {
            id: uiSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("UI Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: languageTitle
                    text: qsTr("Language")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_language = StreamingPreferences.language
                        currentIndex = 0
                        for (var i = 0; i < languageListModel.count; i++) {
                            var el_language = languageListModel.get(i).val;
                            if (saved_language === el_language) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: languageComboBox
                    textRole: "text"
                    model: ListModel {
                        id: languageListModel
                        ListElement {
                            text: qsTr("Automatic")
                            val: StreamingPreferences.LANG_AUTO
                        }
                        ListElement {
                            text: "Deutsch" // German
                            val: StreamingPreferences.LANG_DE
                        }
                        ListElement {
                            text: "English"
                            val: StreamingPreferences.LANG_EN
                        }
                        ListElement {
                            text: "Français" // French
                            val: StreamingPreferences.LANG_FR
                        }
                        ListElement {
                            text: "简体中文" // Simplified Chinese
                            val: StreamingPreferences.LANG_ZH_CN
                        }
                        ListElement {
                            text: "Norwegian Bokmål"
                            val: StreamingPreferences.LANG_NB_NO
                        }
                        ListElement {
                            text: "русский" // Russian
                            val: StreamingPreferences.LANG_RU
                        }
                        ListElement {
                            text: "Español" // Spanish
                            val: StreamingPreferences.LANG_ES
                        }
                        ListElement {
                            text: "日本語" // Japanese
                            val: StreamingPreferences.LANG_JA
                        }
                        ListElement {
                            text: "Tiếng Việt" // Vietnamese
                            val: StreamingPreferences.LANG_VI
                        }
                        ListElement {
                            text: "ภาษาไทย" // Thai
                            val: StreamingPreferences.LANG_TH
                        }
                        ListElement {
                            text: "한국어" // Korean
                            val: StreamingPreferences.LANG_KO
                        }
                        ListElement {
                            text: "Magyar" // Hungarian
                            val: StreamingPreferences.LANG_HU
                        }
                        ListElement {
                            text: "Nederlands" // Dutch
                            val: StreamingPreferences.LANG_NL
                        }
                        ListElement {
                            text: "Svenska" // Swedish
                            val: StreamingPreferences.LANG_SV
                        }
                        ListElement {
                            text: "Türkçe" // Turkish
                            val: StreamingPreferences.LANG_TR
                        }
                        /* ListElement {
                            text: "Українська" // Ukrainian
                            val: StreamingPreferences.LANG_UK
                        } */
                        ListElement {
                            text: "繁體中文" // Traditional Chinese
                            val: StreamingPreferences.LANG_ZH_TW
                        }
                        ListElement {
                            text: "Português" // Portuguese
                            val: StreamingPreferences.LANG_PT
                        }
                        ListElement {
                            text: "Português do Brasil" // Brazilian Portuguese
                            val: StreamingPreferences.LANG_PT_BR
                        }
                        ListElement {
                            text: "Ελληνικά" // Greek
                            val: StreamingPreferences.LANG_EL
                        }
                        ListElement {
                            text: "Italiano" // Italian
                            val: StreamingPreferences.LANG_IT
                        }
                        /* ListElement {
                            text: "हिन्दी, हिंदी" // Hindi
                            val: StreamingPreferences.LANG_HI
                        } */
                        ListElement {
                            text: "Język polski" // Polish
                            val: StreamingPreferences.LANG_PL
                        }
                        ListElement {
                            text: "Čeština" // Czech
                            val: StreamingPreferences.LANG_CS
                        }
                        /* ListElement {
                            text: "עִבְרִית" // Hebrew
                            val: StreamingPreferences.LANG_HE
                        } */
                        /* ListElement {
                            text: "کرمانجیی خواروو" // Central Kurdish
                            val: StreamingPreferences.LANG_CKB
                        } */
                        /* ListElement {
                            text: "Lietuvių kalba" // Lithuanian
                            val: StreamingPreferences.LANG_LT
                        } */
                        /* ListElement {
                            text: "Eesti" // Estonian
                            val: StreamingPreferences.LANG_ET
                        } */
                        ListElement {
                            text: "Български" // Bulgarian
                            val: StreamingPreferences.LANG_BG
                        }
                        /* ListElement {
                            text: "Esperanto"
                            val: StreamingPreferences.LANG_EO
                        } */
                        ListElement {
                            text: "தமிழ்" // Tamil
                            val: StreamingPreferences.LANG_TA
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        // Retranslating is expensive, so only do it if the language actually changed
                        var new_language = languageListModel.get(currentIndex).val
                        if (StreamingPreferences.language !== new_language) {
                            StreamingPreferences.language = languageListModel.get(currentIndex).val
                            if (!StreamingPreferences.retranslate()) {
                                ToolTip.show(qsTr("You must restart Moonlight for this change to take effect"), 5000)
                            }
                            else {
                                // Force the back operation to pop any AppView pages that exist.
                                // The AppView stops working after retranslate() for some reason.
                                window.clearOnBack = true

                                // Signal other controls to adjust their text
                                languageChanged()
                            }
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: uiDisplayModeTitle
                    text: qsTr("GUI display mode")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: SystemProperties.hasDesktopEnvironment
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        if (!visible) {
                            // Do nothing if the control won't even be visible
                            return
                        }

                        var saved_uidisplaymode = StreamingPreferences.uiDisplayMode
                        currentIndex = 0
                        for (var i = 0; i < uiDisplayModeListModel.count; i++) {
                            var el_uidisplaymode = uiDisplayModeListModel.get(i).val;
                            if (saved_uidisplaymode === el_uidisplaymode) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: uiDisplayModeComboBox
                    visible: SystemProperties.hasDesktopEnvironment
                    textRole: "text"
                    model: ListModel {
                        id: uiDisplayModeListModel
                        ListElement {
                            text: qsTr("Windowed")
                            val: StreamingPreferences.UI_WINDOWED
                        }
                        ListElement {
                            text: qsTr("Maximized")
                            val: StreamingPreferences.UI_MAXIMIZED
                        }   
                        ListElement {
                            text: qsTr("Fullscreen")
                            val: StreamingPreferences.UI_FULLSCREEN
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        StreamingPreferences.uiDisplayMode = uiDisplayModeListModel.get(currentIndex).val
                    }
                }

                Label {
                    width: parent.width
                    id: uiScaleTitle
                    text: qsTr("GUI scale")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: SystemProperties.supportsUiScale
                }

                AutoResizingComboBox {
                    id: uiScaleComboBox
                    visible: SystemProperties.supportsUiScale
                    textRole: "text"
                    model: ListModel {
                        id: uiScaleListModel
                    }

                    Component.onCompleted: {
                        var scales = [100, 125, 150, 175, 200, 250, 300, 350, 400]
                        currentIndex = 0
                        for (var i = 0; i < scales.length; i++) {
                            uiScaleListModel.append({ "text": qsTr("%1%").arg(scales[i]), "val": scales[i] })
                            if (scales[i] === StreamingPreferences.uiScale) {
                                currentIndex = i
                            }
                        }

                        activated(currentIndex)
                    }

                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated: {
                        var scale = uiScaleListModel.get(currentIndex).val
                        if (StreamingPreferences.uiScale !== scale) {
                            StreamingPreferences.uiScale = scale
                            promptRestartIfNeeded()
                        }
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("Increases the size of text and controls in Moonlight, such as when using a TV. Requires restarting Moonlight.")
                }

                CheckBox {
                    id: tvModeCheck
                    width: parent.width
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    text: qsTr("TV mode")
                    font.pointSize: 12
                    checked: StreamingPreferences.tvMode
                    onToggled: {
                        StreamingPreferences.tvMode = checked

                        // Offer a larger scale when enabling TV mode on a 4K or larger display
                        if (checked && !SystemProperties.tvModeOverridden &&
                                SystemProperties.supportsUiScale && StreamingPreferences.uiScale === 100 &&
                                Screen.width * Screen.devicePixelRatio >= 3840) {
                            tvModeScaleDialog.open()
                        }
                        else {
                            promptRestartIfNeeded()
                        }
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("A GUI suited to gamepads and TVs. Runs fullscreen with larger controls and no mouse hover effects. Requires restarting Moonlight.")
                }

                Label {
                    width: parent.width
                    text: SystemProperties.tvMode ?
                              qsTr("TV mode is currently turned on by the --tv-mode command line option.") :
                              qsTr("TV mode is currently turned off by the --no-tv-mode command line option.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                    visible: SystemProperties.tvModeOverridden
                }

                CheckBox {
                    id: connectionWarningsCheck
                    width: parent.width
                    text: qsTr("Show connection quality warnings")
                    font.pointSize: 12
                    checked: StreamingPreferences.connectionWarnings
                    onCheckedChanged: {
                        StreamingPreferences.connectionWarnings = checked
                    }
                }

                CheckBox {
                    id: configurationWarningsCheck
                    width: parent.width
                    text: qsTr("Show configuration warnings")
                    font.pointSize: 12
                    checked: StreamingPreferences.configurationWarnings
                    onCheckedChanged: {
                        StreamingPreferences.configurationWarnings = checked
                    }
                }

                CheckBox {
                    visible: SystemProperties.hasDiscordIntegration
                    id: discordPresenceCheck
                    width: parent.width
                    text: qsTr("Discord Rich Presence integration")
                    font.pointSize: 12
                    checked: StreamingPreferences.richPresence
                    onCheckedChanged: {
                        StreamingPreferences.richPresence = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("Updates your Discord status to display the name of the game you're streaming.")
                }

                CheckBox {
                    id: keepAwakeCheck
                    width: parent.width
                    text: qsTr("Keep the display awake while streaming")
                    font.pointSize: 12
                    checked: StreamingPreferences.keepAwake
                    onCheckedChanged: {
                        StreamingPreferences.keepAwake = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("Prevents the screensaver from starting or the display from going to sleep while streaming.")
                }
            }
        }
    }

    Column {
        padding: 10
        rightPadding: 20
        x: singleColumn ? 0 : settingsColumn1.width
        y: singleColumn ? settingsColumn1.height : 0
        id: settingsColumn2
        width: singleColumn ? settingsPage.width : settingsPage.width / 2
        spacing: 15

        SettingsGroupBox {
            id: inputSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Input Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: absoluteMouseCheck
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    width: parent.width
                    text: qsTr("Optimize mouse for remote desktop instead of games")
                    font.pointSize:  12
                    checked: StreamingPreferences.absoluteMouseMode
                    onCheckedChanged: {
                        StreamingPreferences.absoluteMouseMode = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("This enables seamless mouse control without capturing the client's mouse cursor. It is ideal for remote desktop usage but will not work in most games.") + " " +
                                  qsTr("You can toggle this while streaming using Ctrl+Alt+Shift+M.") + "\n\n" +
                                  qsTr("NOTE: Due to a bug in GeForce Experience, this option may not work properly if your host PC has multiple monitors.")
                }

                Row {
                    spacing: 5
                    width: parent.width

                    CheckBox {
                        id: captureSysKeysCheck
                        hoverEnabled: !SystemProperties.hoverEffectsDisabled
                        text: qsTr("Capture system keyboard shortcuts")
                        font.pointSize: 12
                        enabled: SystemProperties.hasDesktopEnvironment
                        checked: StreamingPreferences.captureSysKeysMode !== StreamingPreferences.CSK_OFF || !SystemProperties.hasDesktopEnvironment

                        ToolTip.delay: 1000
                        ToolTip.timeout: 10000
                        ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                        ToolTip.text: qsTr("This enables the capture of system-wide keyboard shortcuts like Alt+Tab that would normally be handled by the client OS while streaming.") + "\n\n" +
                                      qsTr("NOTE: Certain keyboard shortcuts like Ctrl+Alt+Del on Windows cannot be intercepted by any application, including Moonlight.")
                    }

                    AutoResizingComboBox {
                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            if (!visible) {
                                // Do nothing if the control won't even be visible
                                return
                            }

                            var saved_syskeysmode = StreamingPreferences.captureSysKeysMode
                            currentIndex = 0
                            for (var i = 0; i < captureSysKeysModeListModel.count; i++) {
                                var el_syskeysmode = captureSysKeysModeListModel.get(i).val;
                                if (saved_syskeysmode === el_syskeysmode) {
                                    currentIndex = i
                                    break
                                }
                            }

                            activated(currentIndex)
                        }

                        enabled: captureSysKeysCheck.checked && captureSysKeysCheck.enabled
                        textRole: "text"
                        model: ListModel {
                            id: captureSysKeysModeListModel
                            ListElement {
                                text: qsTr("in fullscreen")
                                val: StreamingPreferences.CSK_FULLSCREEN
                            }
                            ListElement {
                                text: qsTr("always")
                                val: StreamingPreferences.CSK_ALWAYS
                            }
                        }

                        function updatePref() {
                            if (!enabled) {
                                StreamingPreferences.captureSysKeysMode = StreamingPreferences.CSK_OFF
                            }
                            else {
                                StreamingPreferences.captureSysKeysMode = captureSysKeysModeListModel.get(currentIndex).val
                            }
                        }

                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated: {
                            updatePref()
                        }

                        // This handles transition of the checkbox state
                        onEnabledChanged: {
                            updatePref()
                        }
                    }
                }

                CheckBox {
                    id: absoluteTouchCheck
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    width: parent.width
                    text: qsTr("Use touchscreen as a virtual trackpad")
                    font.pointSize:  12
                    checked: !StreamingPreferences.absoluteTouchMode
                    onCheckedChanged: {
                        StreamingPreferences.absoluteTouchMode = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("When checked, the touchscreen acts like a trackpad. When unchecked, the touchscreen will directly control the mouse pointer.")
                }

                CheckBox {
                    id: swapMouseButtonsCheck
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    width: parent.width
                    text: qsTr("Swap left and right mouse buttons")
                    font.pointSize:  12
                    checked: StreamingPreferences.swapMouseButtons
                    onCheckedChanged: {
                        StreamingPreferences.swapMouseButtons = checked
                    }
                }

                CheckBox {
                    id: reverseScrollButtonsCheck
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    width: parent.width
                    text: qsTr("Reverse mouse scrolling direction")
                    font.pointSize: 12
                    checked: StreamingPreferences.reverseScrollDirection
                    onCheckedChanged: {
                        StreamingPreferences.reverseScrollDirection = checked
                    }
                }
            }
        }

        SettingsGroupBox {
            id: gamepadSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Gamepad Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: swapFaceButtonsCheck
                    width: parent.width
                    text: qsTr("Swap A/B and X/Y gamepad buttons")
                    font.pointSize: 12
                    checked: StreamingPreferences.swapFaceButtons
                    onCheckedChanged: {
                        StreamingPreferences.swapFaceButtons = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("This switches gamepads into a Nintendo-style button layout")
                }

                Label {
                    width: parent.width
                    id: gamepadMenuTriggerTitle
                    text: qsTr("Open the Moonlight menu while streaming with")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    id: gamepadMenuTriggerComboBox
                    textRole: "text"
                    model: ListModel {
                        id: gamepadMenuTriggerListModel
                        ListElement {
                            text: qsTr("Start+Select+L1+R1")
                            val: StreamingPreferences.GMT_COMBO
                        }
                        ListElement {
                            text: qsTr("Start+Select")
                            val: StreamingPreferences.GMT_START_SELECT
                        }
                        ListElement {
                            text: qsTr("Hold Select")
                            val: StreamingPreferences.GMT_HOLD_SELECT
                        }
                        ListElement {
                            text: qsTr("Hold Start")
                            val: StreamingPreferences.GMT_HOLD_START
                        }
                    }

                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var savedTrigger = StreamingPreferences.gamepadMenuTrigger
                        currentIndex = 0
                        for (var i = 0; i < gamepadMenuTriggerListModel.count; i++) {
                            if (gamepadMenuTriggerListModel.get(i).val === savedTrigger) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated: {
                        StreamingPreferences.gamepadMenuTrigger = gamepadMenuTriggerListModel.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("The Moonlight menu lets you disconnect, end the session, show stream statistics, and more.") + "\n\n" +
                                  qsTr("Start+Select+L1+R1 always opens it too.") + " " +
                                  qsTr("With the other choices, Start and Select still reach the game when tapped or pressed together with other buttons.") + "\n\n" +
                                  qsTr("Hold Start replaces holding Start for gamepad mouse mode, which can then be turned on from the menu instead.")
                }

                CheckBox {
                    id: singleControllerCheck
                    width: parent.width
                    text: qsTr("Force gamepad #1 always connected")
                    font.pointSize:  12
                    checked: !StreamingPreferences.multiController
                    onCheckedChanged: {
                        StreamingPreferences.multiController = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("Forces a single gamepad to always stay connected to the host, even if no gamepads are actually connected to this PC.") + " " +
                                  qsTr("Only enable this option when streaming a game that doesn't support gamepads being connected after startup.")
                }

                CheckBox {
                    id: gamepadMouseCheck
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    width: parent.width
                    // Holding Start opens the menu instead when it's set up that way
                    text: StreamingPreferences.gamepadMenuTrigger === StreamingPreferences.GMT_HOLD_START ?
                              qsTr("Enable mouse control with gamepads from the Moonlight menu") :
                              qsTr("Enable mouse control with gamepads by holding the 'Start' button")
                    font.pointSize: 12
                    checked: StreamingPreferences.gamepadMouse
                    onCheckedChanged: {
                        StreamingPreferences.gamepadMouse = checked
                    }
                }

                CheckBox {
                    id: backgroundGamepadCheck
                    width: parent.width
                    text: qsTr("Process gamepad input when Moonlight is in the background")
                    font.pointSize: 12
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: StreamingPreferences.backgroundGamepad
                    onCheckedChanged: {
                        StreamingPreferences.backgroundGamepad = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("Allows Moonlight to capture gamepad inputs even if it's not the current window in focus")
                }
            }
        }

        SettingsGroupBox {
            id: advancedSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Advanced Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: resVDSTitle
                    text: qsTr("Video decoder")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_vds = StreamingPreferences.videoDecoderSelection
                        currentIndex = 0
                        for (var i = 0; i < decoderListModel.count; i++) {
                            var el_vds = decoderListModel.get(i).val;
                            if (saved_vds === el_vds) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
                    }

                    id: decoderComboBox
                    textRole: "text"
                    model: ListModel {
                        id: decoderListModel
                        ListElement {
                            text: qsTr("Automatic (Recommended)")
                            val: StreamingPreferences.VDS_AUTO
                        }
                        ListElement {
                            text: qsTr("Force software decoding")
                            val: StreamingPreferences.VDS_FORCE_SOFTWARE
                        }
                        ListElement {
                            text: qsTr("Force hardware decoding")
                            val: StreamingPreferences.VDS_FORCE_HARDWARE
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated: {
                        if (enabled) {
                            StreamingPreferences.videoDecoderSelection = decoderListModel.get(currentIndex).val
                        }
                    }
                }

                CheckBox {
                    width: parent.width
                    text: qsTr("Request low-latency AMD VAAPI decoding")
                    font.pointSize: 12
                    visible: SystemProperties.supportsAmdLowLatencyDecode
                    checked: StreamingPreferences.amdLowLatencyDecode
                    onToggled: {
                        StreamingPreferences.amdLowLatencyDecode = checked
                        restartDialog.open()
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("Requests Mesa's low-latency decoding mode for AMD VAAPI hardware. Requires restarting Moonlight.")
                }

                Label {
                    width: parent.width
                    id: resVCCTitle
                    text: qsTr("Video codec")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_vcc = StreamingPreferences.videoCodecConfig

                        // Default to Automatic (relevant if HDR is enabled,
                        // where we will match none of the codecs in the list)
                        currentIndex = 0

                        for(var i = 0; i < codecListModel.count; i++) {
                            var el_vcc = codecListModel.get(i).val;
                            if (saved_vcc === el_vcc) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: codecComboBox
                    textRole: "text"
                    model: ListModel {
                        id: codecListModel
                        ListElement {
                            text: qsTr("Automatic (Recommended)")
                            val: StreamingPreferences.VCC_AUTO
                        }
                        ListElement {
                            text: qsTr("H.264")
                            val: StreamingPreferences.VCC_FORCE_H264
                        }
                        ListElement {
                            text: qsTr("HEVC (H.265)")
                            val: StreamingPreferences.VCC_FORCE_HEVC
                        }
                        ListElement {
                            text: qsTr("AV1")
                            val: StreamingPreferences.VCC_FORCE_AV1
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        if (enabled) {
                            StreamingPreferences.videoCodecConfig = codecListModel.get(currentIndex).val
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: rendererTitle
                    text: qsTr("Renderer")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: SystemProperties.isDarwin
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_rs = StreamingPreferences.rendererSelection

                        // Default to Automatic
                        currentIndex = 0

                        for(var i = 0; i < rendererListModel.count; i++) {
                            var el_rs = rendererListModel.get(i).val;
                            if (saved_rs === el_rs) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: rendererComboBox
                    visible: SystemProperties.isDarwin
                    textRole: "text"
                    model: ListModel {
                        id: rendererListModel
                        ListElement {
                            text: qsTr("Automatic (Recommended)")
                            val: StreamingPreferences.RS_AUTO
                        }
                        ListElement {
                            text: "Vulkan"
                            val: StreamingPreferences.RS_VULKAN
                        }
                        ListElement {
                            text: "Metal"
                            val: StreamingPreferences.RS_METAL
                        }
                        ListElement {
                            text: "AVSampleBufferDisplayLayer"
                            val: StreamingPreferences.RS_AVSBDL
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        StreamingPreferences.rendererSelection = rendererListModel.get(currentIndex).val
                    }
                }

                CheckBox {
                    id: enableYUV444
                    width: parent.width
                    text: qsTr("Enable YUV 4:4:4")
                    font.pointSize: 12

                    checked: StreamingPreferences.enableYUV444
                    onCheckedChanged: {
                        // This is called on init, so only reset to default bitrate when checked state changes.
                        if (StreamingPreferences.enableYUV444 != checked) {
                            StreamingPreferences.enableYUV444 = checked
                            if (StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                                          StreamingPreferences.height,
                                                                                                          StreamingPreferences.fps,
                                                                                                          StreamingPreferences.enableYUV444);
                                slider.value = StreamingPreferences.bitrateKbps
                            }
                        }
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: enabled ?
                                      qsTr("Good for streaming desktop and text-heavy games, but not recommended for fast-paced games.")
                                    :
                                      qsTr("YUV 4:4:4 is not supported on this PC.")
                }

                CheckBox {
                    id: unlockBitrate
                    width: parent.width
                    text: qsTr("Unlock bitrate limit (Experimental)")
                    font.pointSize: 12

                    checked: StreamingPreferences.unlockBitrate
                    onCheckedChanged: {
                        StreamingPreferences.unlockBitrate = checked
                        StreamingPreferences.bitrateKbps = Math.min(StreamingPreferences.bitrateKbps, slider.to)
                        slider.value = StreamingPreferences.bitrateKbps
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("This unlocks extremely high video bitrates for use with Sunshine hosts. It should only be used when streaming over an Ethernet LAN connection.")
                }

                CheckBox {
                    id: enableMdns
                    width: parent.width
                    text: qsTr("Automatically find PCs on the local network (Recommended)")
                    font.pointSize: 12
                    checked: StreamingPreferences.enableMdns
                    onCheckedChanged: {
                        // This is called on init, so only do the work if we've
                        // actually changed the value.
                        if (StreamingPreferences.enableMdns != checked) {
                            StreamingPreferences.enableMdns = checked

                            // Restart polling so the mDNS change takes effect
                            if (window.pollingActive) {
                                ComputerManager.stopPollingAsync()
                                ComputerManager.startPolling()
                            }
                        }
                    }
                }

                CheckBox {
                    id: detectNetworkBlocking
                    width: parent.width
                    text: qsTr("Automatically detect blocked connections (Recommended)")
                    font.pointSize: 12
                    checked: StreamingPreferences.detectNetworkBlocking
                    onCheckedChanged: {
                        StreamingPreferences.detectNetworkBlocking = checked
                    }
                }
            }
        }

        SettingsGroupBox {
            id: statisticsSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"skyblue\">" + qsTr("Statistics") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: showPerformanceOverlay
                    width: parent.width
                    text: qsTr("Show performance stats while streaming")
                    font.pointSize: 12
                    checked: StreamingPreferences.showPerformanceOverlay
                    onCheckedChanged: {
                        StreamingPreferences.showPerformanceOverlay = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                    ToolTip.text: qsTr("Display real-time stream performance information while streaming.") + "\n\n" +
                                  qsTr("You can toggle it at any time while streaming using Ctrl+Alt+Shift+S or Select+L1+R1+X.") + "\n\n" +
                                  qsTr("The performance overlay is not supported on Steam Link or Raspberry Pi.")
                }

                Column {
                    width: parent.width
                    spacing: 5

                    Label {
                        width: parent.width
                        text: qsTr("Performance stats arrangement")
                        font.pointSize: 12
                        wrapMode: Text.Wrap
                    }

                    AutoResizingComboBox {
                        id: performanceOverlayModeComboBox
                        textRole: "text"
                        model: ListModel {
                            id: performanceOverlayModeListModel
                            ListElement {
                                text: qsTr("Text only")
                                val: StreamingPreferences.POM_TEXT_ONLY
                            }
                            ListElement {
                                text: qsTr("Text and graphs")
                                val: StreamingPreferences.POM_TEXT_AND_GRAPHS
                            }
                            ListElement {
                                text: qsTr("Graphs only")
                                val: StreamingPreferences.POM_GRAPHS_ONLY
                            }
                        }
                        currentIndex: {
                            for (var i = 0; i < performanceOverlayModeListModel.count; i++) {
                                if (performanceOverlayModeListModel.get(i).val === StreamingPreferences.performanceOverlayMode) {
                                    return i
                                }
                            }
                            return 1
                        }
                        onActivated: {
                            StreamingPreferences.performanceOverlayMode = performanceOverlayModeListModel.get(currentIndex).val
                        }
                        Component.onCompleted: {
                            recalculateWidth()
                            languageChanged.connect(recalculateWidth)
                        }
                    }
                }
                Column {
                    width: parent.width
                    spacing: 5
                    visible: StreamingPreferences.performanceOverlayMode !== StreamingPreferences.POM_TEXT_ONLY

                    Label {
                        width: parent.width
                        text: qsTr("Graph size")
                        font.pointSize: 12
                        wrapMode: Text.Wrap
                    }

                    AutoResizingComboBox {
                        id: performanceGraphSizeComboBox
                        textRole: "text"
                        model: ListModel {
                            id: performanceGraphSizeListModel
                            ListElement {
                                text: qsTr("Automatic (match stream window)")
                                val: StreamingPreferences.PGS_AUTO
                            }
                            ListElement {
                                text: qsTr("75%")
                                val: 75
                            }
                            ListElement {
                                text: qsTr("100%")
                                val: 100
                            }
                            ListElement {
                                text: qsTr("125%")
                                val: 125
                            }
                            ListElement {
                                text: qsTr("150%")
                                val: 150
                            }
                            ListElement {
                                text: qsTr("200%")
                                val: 200
                            }
                        }
                        currentIndex: {
                            for (var i = 0; i < performanceGraphSizeListModel.count; i++) {
                                if (performanceGraphSizeListModel.get(i).val === StreamingPreferences.performanceGraphSize) {
                                    return i
                                }
                            }
                            return 2
                        }
                        onActivated: {
                            StreamingPreferences.performanceGraphSize = performanceGraphSizeListModel.get(currentIndex).val
                        }
                        Component.onCompleted: {
                            recalculateWidth()
                            languageChanged.connect(recalculateWidth)
                        }

                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                        ToolTip.text: qsTr("Automatic sizes the graphs for the stream window, the way 100% looks at 1080p.")
                    }

                    Label {
                        width: parent.width
                        text: qsTr("Graph height")
                        font.pointSize: 12
                        wrapMode: Text.Wrap
                    }

                    AutoResizingComboBox {
                        id: performanceGraphHeightComboBox
                        textRole: "text"
                        model: ListModel {
                            id: performanceGraphHeightListModel
                            ListElement {
                                text: qsTr("Compact")
                                val: StreamingPreferences.PGH_COMPACT
                            }
                            ListElement {
                                text: qsTr("Normal")
                                val: StreamingPreferences.PGH_NORMAL
                            }
                            ListElement {
                                text: qsTr("Tall")
                                val: StreamingPreferences.PGH_TALL
                            }
                        }
                        currentIndex: {
                            for (var i = 0; i < performanceGraphHeightListModel.count; i++) {
                                if (performanceGraphHeightListModel.get(i).val === StreamingPreferences.performanceGraphHeight) {
                                    return i
                                }
                            }
                            return 1
                        }
                        onActivated: {
                            StreamingPreferences.performanceGraphHeight = performanceGraphHeightListModel.get(currentIndex).val
                        }
                        Component.onCompleted: {
                            recalculateWidth()
                            languageChanged.connect(recalculateWidth)
                        }
                    }

                    Label {
                        width: parent.width
                        text: qsTr("Graph position")
                        font.pointSize: 12
                        wrapMode: Text.Wrap
                    }

                    AutoResizingComboBox {
                        id: performanceGraphPositionComboBox
                        textRole: "text"
                        model: ListModel {
                            id: performanceGraphPositionListModel
                            ListElement {
                                text: qsTr("Right (text stats on the left)")
                                val: StreamingPreferences.PGP_RIGHT
                            }
                            ListElement {
                                text: qsTr("Left (text stats on the right)")
                                val: StreamingPreferences.PGP_LEFT
                            }
                        }
                        currentIndex: {
                            for (var i = 0; i < performanceGraphPositionListModel.count; i++) {
                                if (performanceGraphPositionListModel.get(i).val === StreamingPreferences.performanceGraphPosition) {
                                    return i
                                }
                            }
                            return 0
                        }
                        onActivated: {
                            StreamingPreferences.performanceGraphPosition = performanceGraphPositionListModel.get(currentIndex).val
                        }
                        Component.onCompleted: {
                            recalculateWidth()
                            languageChanged.connect(recalculateWidth)
                        }
                    }

                    Label {
                        width: parent.width
                        text: qsTr("Graph background opacity")
                        font.pointSize: 12
                        wrapMode: Text.Wrap
                    }

                    AutoResizingComboBox {
                        id: performanceGraphOpacityComboBox
                        textRole: "text"
                        model: ListModel {
                            id: performanceGraphOpacityListModel
                            ListElement {
                                text: qsTr("95%")
                                val: 95
                            }
                            ListElement {
                                text: qsTr("75%")
                                val: 75
                            }
                            ListElement {
                                text: qsTr("50%")
                                val: 50
                            }
                            ListElement {
                                text: qsTr("25%")
                                val: 25
                            }
                        }
                        currentIndex: {
                            for (var i = 0; i < performanceGraphOpacityListModel.count; i++) {
                                if (performanceGraphOpacityListModel.get(i).val === StreamingPreferences.performanceGraphOpacity) {
                                    return i
                                }
                            }
                            return 1
                        }
                        onActivated: {
                            StreamingPreferences.performanceGraphOpacity = performanceGraphOpacityListModel.get(currentIndex).val
                        }
                        Component.onCompleted: {
                            recalculateWidth()
                            languageChanged.connect(recalculateWidth)
                        }
                    }

                    Label {
                        width: parent.width
                        text: qsTr("Graph history")
                        font.pointSize: 12
                        wrapMode: Text.Wrap
                    }

                    AutoResizingComboBox {
                        id: performanceGraphHistoryComboBox
                        textRole: "text"
                        model: ListModel {
                            id: performanceGraphHistoryListModel
                            ListElement {
                                text: qsTr("5 seconds")
                                val: 5
                            }
                            ListElement {
                                text: qsTr("10 seconds")
                                val: 10
                            }
                            ListElement {
                                text: qsTr("30 seconds")
                                val: 30
                            }
                        }
                        currentIndex: {
                            for (var i = 0; i < performanceGraphHistoryListModel.count; i++) {
                                if (performanceGraphHistoryListModel.get(i).val === StreamingPreferences.performanceGraphHistory) {
                                    return i
                                }
                            }
                            return 1
                        }
                        onActivated: {
                            StreamingPreferences.performanceGraphHistory = performanceGraphHistoryListModel.get(currentIndex).val
                        }
                        Component.onCompleted: {
                            recalculateWidth()
                            languageChanged.connect(recalculateWidth)
                        }
                    }

                    Label {
                        width: parent.width
                        text: qsTr("Graphs to show")
                        font.pointSize: 12
                        wrapMode: Text.Wrap
                    }

                    Repeater {
                        // The C++ side owns the list of graphs, their names,
                        // sections and defaults
                        model: StreamingPreferences.getPerformanceGraphs()

                        delegate: Column {
                            width: parent.width
                            spacing: 0

                            Label {
                                visible: modelData.section !== ""
                                topPadding: 5
                                leftPadding: 5
                                text: modelData.section
                                font.pointSize: 10
                                font.bold: true
                                opacity: 0.7
                            }

                            CheckBox {
                                width: parent.width
                                text: modelData.text
                                font.pointSize: 12
                                // The preference records changes from each
                                // graph's default, so a click just flips its bit
                                checked: ((StreamingPreferences.performanceGraphsDefault ^
                                           StreamingPreferences.performanceGraphsToggled) & (1 << modelData.bit)) !== 0
                                onToggled: {
                                    StreamingPreferences.performanceGraphsToggled ^= (1 << modelData.bit)
                                }
                            }
                        }
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("While the text stats are hidden, the graphs also show the stream's resolution, frame rate, VRR or V-Sync, codec, bit depth, HDR, chroma subsampling and renderer.")
                    }
                }
            }
        }
    }
}
