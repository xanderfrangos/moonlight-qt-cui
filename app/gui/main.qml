import QtQuick 2.9
import QtQuick.Controls 2.3
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import QtQuick.Controls.Material 2.2

import ComputerManager 1.0
import AutoUpdateChecker 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0
import InputModeTracker 1.0
import TvTheme 1.0

ApplicationWindow {
    property bool pollingActive: false

    // Set by SettingsView to force the back operation to pop all
    // pages except the initial view. This is required when doing
    // a retranslate() because AppView breaks for some reason.
    property bool clearOnBack: false

    id: window
    width: 1280
    height: 600

    // TV mode keeps everything this far in from the window edges, so TVs
    // that overscan don't crop it
    readonly property int tvSafeX: SystemProperties.tvMode ? Math.round(width * TvTheme.safeAreaHorizontal) : 0
    readonly property int tvSafeY: SystemProperties.tvMode ? Math.round(height * TvTheme.safeAreaVertical) : 0

    // This function runs prior to creation of the initial StackView item
    function doEarlyInit() {
        // Override the background color to Material 2 colors for Qt 6.5+
        // in order to improve contrast between GFE's placeholder box art
        // and the background of the app grid.
        if (SystemProperties.tvMode) {
            Material.background = TvTheme.background
            Material.accent = TvTheme.accent
        }
        else if (SystemProperties.usesMaterial3Theme) {
            Material.background = "#303030"
        }

        SdlGamepadKeyNavigation.enable()
    }

    Component.onCompleted: {
        // Show the window according to the user's preferences
        if (SystemProperties.hasDesktopEnvironment) {
            if (SystemProperties.tvMode) {
                // TV mode is always fullscreen
                window.showFullScreen()
            }
            else if (StreamingPreferences.uiDisplayMode == StreamingPreferences.UI_MAXIMIZED) {
                window.showMaximized()
            }
            else if (StreamingPreferences.uiDisplayMode == StreamingPreferences.UI_FULLSCREEN) {
                window.showFullScreen()
            }
            else {
                // With a large GUI scale, our default size can exceed the screen.
                // Leave some room for the window frame and title bar.
                if (Screen.desktopAvailableWidth > 0 && Screen.desktopAvailableHeight > 0) {
                    window.width = Math.min(window.width, Screen.desktopAvailableWidth * 0.9)
                    window.height = Math.min(window.height, Screen.desktopAvailableHeight * 0.9)
                }

                window.show()
            }
        } else {
            window.showFullScreen()
        }

        // Display any modal dialogs for configuration warnings
        if (runConfigChecks) {
            if (SystemProperties.isWow64) {
                wow64Dialog.open()
            }

            // Hardware acceleration and unmapped gamepads are checked asynchronously
            SystemProperties.hasHardwareAccelerationChanged.connect(hasHardwareAccelerationChanged)
            SystemProperties.unmappedGamepadsChanged.connect(hasUnmappedGamepadsChanged)
            SystemProperties.startAsyncLoad()
        }
    }

    function hasHardwareAccelerationChanged() {
        if (!SystemProperties.hasHardwareAcceleration && StreamingPreferences.videoDecoderSelection !== StreamingPreferences.VDS_FORCE_SOFTWARE) {
            if (SystemProperties.isRunningXWayland) {
                xWaylandDialog.open()
            }
            else {
                noHwDecoderDialog.open()
            }
        }
    }

    function hasUnmappedGamepadsChanged() {
        if (SystemProperties.unmappedGamepads) {
            unmappedGamepadDialog.unmappedGamepads = SystemProperties.unmappedGamepads
            unmappedGamepadDialog.open()
        }
    }

    // It would be better to use TextMetrics here, but it always lays out
    // the text slightly more compactly than real Text does in ToolTip,
    // causing unexpected line breaks to be inserted
    Text {
        id: tooltipTextLayoutHelper
        visible: false
        font: ToolTip.toolTip.font
        text: ToolTip.toolTip.text
    }

    // This configures the maximum width of the singleton attached QML ToolTip. If left unconstrained,
    // it will never insert a line break and just extend on forever.
    ToolTip.toolTip.contentWidth: Math.min(tooltipTextLayoutHelper.width, 400)

    function goBack() {
        if (clearOnBack) {
            // Pop all items except the first one
            stackView.pop(null)
            clearOnBack = false
        }
        else {
            stackView.pop()
        }
    }

    // Escape or Back: leave the current page, or ask whether to quit if this
    // is the first one. The toolbar calls this too, since it lives outside
    // the StackView and never sees the StackView's own key handlers.
    function goBackOrQuit() {
        if (stackView.depth > 1) {
            goBack()
        }
        else {
            quitConfirmationDialog.open()
        }
    }

    // The TV mode title: the names of the pages in the stack, with the
    // current page last. The arguments make bindings update on navigation.
    function breadcrumbText(depth, currentItem) {
        var names = []
        for (var i = 0; i < depth; i++) {
            var page = stackView.get(i)
            if (page && page.objectName) {
                names.push(page.objectName.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"))
            }
        }

        names = names.slice(-3)
        if (names.length === 0) {
            return ""
        }

        var current = names.pop()
        if (names.length === 0) {
            return current
        }

        var separator = "  \u203A  "
        return "<font color=\"" + TvTheme.textSecondary + "\">" + names.join(separator) + separator + "</font>" + current
    }

    function isInPopup(item) {
        return item !== null && isSelfOrDescendantOf(item, Overlay.overlay)
    }

    function isSelfOrDescendantOf(item, ancestor) {
        for (; item; item = item.parent) {
            if (item === ancestor) {
                return true
            }
        }
        return false
    }

    // Returns whether focus can be returned to an item that had it before a
    // dialog opened or before we navigated away from its page
    function canRestoreFocusTo(item) {
        if (!item || !item.visible || !item.enabled || !stackView.currentItem) {
            return false
        }

        // Only return focus to the current page or the toolbar
        if (!isSelfOrDescendantOf(item, stackView.currentItem) && !isSelfOrDescendantOf(item, toolBar)) {
            return false
        }

        // Grid and list views manage focus for their delegates themselves, so
        // don't focus a delegate that is no longer the current item
        for (var ancestor = item.parent; ancestor; ancestor = ancestor.parent) {
            if (ancestor.currentIndex !== undefined && ancestor.currentItem !== undefined &&
                    !isSelfOrDescendantOf(item, ancestor.currentItem)) {
                return false
            }
        }

        return true
    }

    // Moves focus back to an item that previously had it, or to the current
    // page if that isn't possible. We must always put focus somewhere, or
    // gamepad and keyboard navigation will break.
    function restoreFocus(item, hadVisualFocus) {
        if (canRestoreFocusTo(item)) {
            item.forceActiveFocus(hadVisualFocus ? Qt.TabFocusReason : Qt.OtherFocusReason)
        }
        else {
            stackView.forceActiveFocus()
        }
    }

    // TV mode draws a subtle gradient behind the whole window. Pages leave
    // their backgrounds transparent, so it shows through everywhere.
    background: Rectangle {
        color: window.Material.backgroundColor

        TvGradient {
            anchors.fill: parent
            visible: SystemProperties.tvMode
            topColor: TvTheme.backgroundTop
            bottomColor: TvTheme.backgroundBottom
        }

        // Pages can show art behind themselves by providing tvBackdropSource
        TvBackdrop {
            anchors.fill: parent
            visible: SystemProperties.tvMode
            source: SystemProperties.tvMode && stackView.currentItem &&
                    stackView.currentItem.tvBackdropSource !== undefined ?
                        stackView.currentItem.tvBackdropSource : ""
        }
    }

    StackView {
        id: stackView
        anchors.fill: parent
        anchors.leftMargin: SystemProperties.tvMode ? tvSafeX - TvTheme.focusBleed : 0
        anchors.rightMargin: SystemProperties.tvMode ? tvSafeX - TvTheme.focusBleed : 0
        focus: true

        // What had focus on each page in the stack when we navigated away from
        // it, so it can be focused again when we return
        property Item previousItem: null
        property var savedFocus: []

        Component.onCompleted: {
            // Perform our early initialization before constructing
            // the initial view and pushing it to the StackView
            doEarlyInit()
            push(initialView)
        }

        onCurrentItemChanged: {
            var focusItem = window.activeFocusItem

            // Forget pages that are no longer in the stack
            var entries = []
            for (var i = 0; i < savedFocus.length; i++) {
                var entry = savedFocus[i]
                if (entry.page && entry.page !== previousItem &&
                        find(function(page) { return page === entry.page }) !== null) {
                    entries.push(entry)
                }
            }

            // Remember what had focus on the page we're leaving
            if (previousItem && focusItem &&
                    (isSelfOrDescendantOf(focusItem, previousItem) || isSelfOrDescendantOf(focusItem, toolBar))) {
                entries.push({ "page": previousItem, "item": focusItem, "visualFocus": focusItem.visualFocus === true })
            }

            savedFocus = entries
            previousItem = currentItem

            // Ensure focus travels to the next view when going back
            if (currentItem) {
                currentItem.forceActiveFocus()

                // If we're returning to a page, go back to what had focus there.
                // This is deferred so bindings that depend on the current page
                // (like toolbar button visibility) are up to date.
                for (i = 0; i < savedFocus.length; i++) {
                    if (savedFocus[i].page === currentItem) {
                        var savedEntry = savedFocus[i]
                        Qt.callLater(function() {
                            if (savedEntry.page === stackView.currentItem && canRestoreFocusTo(savedEntry.item)) {
                                savedEntry.item.forceActiveFocus(savedEntry.visualFocus ? Qt.TabFocusReason : Qt.OtherFocusReason)
                            }
                        })
                        break
                    }
                }
            }
        }

        Keys.onEscapePressed: goBackOrQuit()

        Keys.onBackPressed: goBackOrQuit()

        Keys.onMenuPressed: {
            settingsButton.clicked()
        }

        // This is a keypress we've reserved for letting the
        // SdlGamepadKeyNavigation object tell us to show settings
        // when Menu is consumed by a focused control.
        Keys.onHangupPressed: {
            settingsButton.clicked()
        }
    }

    // This timer keeps us polling for 5 minutes of inactivity
    // to allow the user to work with Moonlight on a second display
    // while dealing with configuration issues. This will ensure
    // machines come online even if the input focus isn't on Moonlight.
    Timer {
        id: inactivityTimer
        interval: 5 * 60000
        onTriggered: {
            if (!active && pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
    }

    onVisibleChanged: {
        // When we become invisible while streaming is going on,
        // stop polling immediately.
        if (!visible) {
            inactivityTimer.stop()

            if (pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
        else if (active) {
            // When we become visible and active again, start polling
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }

        // Poll for gamepad input only when the window is in focus
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active)
    }

    onActiveChanged: {
        if (active) {
            // Stop the inactivity timer
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }
        else {
            // Start the inactivity timer to stop polling
            // if focus does not return within a few minutes.
            inactivityTimer.restart()
        }

        // Poll for gamepad input only when the window is in focus
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active)
    }

    function navigateTo(url, objectType)
    {
        var existingItem = stackView.find(function(item, index) {
            return item instanceof objectType
        })

        if (existingItem !== null) {
            // Pop to the existing item
            stackView.pop(existingItem)
        }
        else {
            // Create a new item
            stackView.push(url)
        }
    }

    header: ToolBar {
        id: toolBar
        height: SystemProperties.tvMode ? tvSafeY + TvTheme.pillHeight + TvTheme.spacingMedium : 60
        anchors.topMargin: 5
        anchors.bottomMargin: 5

        // TV mode: how far the top bar is condensed to fit beside the page
        // title when the window is narrow. 0 shows everything. 1 shows only
        // the icons in the buttons, except for the focused one. 2 also
        // summarizes the controllers in one chip. 3 also hides the clock. 4
        // also hides the controllers.
        readonly property int tvBarLevel: {
            if (!SystemProperties.tvMode) {
                return 0
            }

            // Leave the title some room. Past this it elides.
            var available = width - 2 * tvSafeX - Math.min(titleRowLabel.implicitWidth, 420) - barRow.spacing
            if (backButton.visible) {
                available -= TvTheme.pillHeight + barRow.spacing
            }

            for (var level = 0; level < 4; level++) {
                if (tvBarWidth(level) <= available) {
                    return level
                }
            }
            return 4
        }

        // The width of everything right of the title at a condensing level.
        // This is worked out from sizes and text metrics rather than from the
        // laid out items, whose widths depend on the level.
        function tvBarWidth(level) {
            var labelled = [[addPcButton, addPcLabelMetrics], [controllersButton, controllersLabelMetrics],
                            [settingsButton, settingsLabelMetrics]]
            var iconOnly = [discordButton, updateButton, helpButton]
            var width = 0
            var items = 0
            var widestLabel = 0

            for (var i = 0; i < labelled.length; i++) {
                if (!labelled[i][0].visible) {
                    continue
                }
                var labelWidth = labelled[i][1].width
                widestLabel = Math.max(widestLabel, labelWidth)
                width += level === 0 ? TvTheme.pillPadding * 2 + TvTheme.pillIconSize + 12 + labelWidth
                                     : TvTheme.pillHeight
                items++
            }

            // Condensed buttons still show the focused one's label
            if (level >= 1 && widestLabel > 0) {
                width += TvTheme.pillPadding * 2 + TvTheme.pillIconSize + 12 + widestLabel - TvTheme.pillHeight
            }

            for (i = 0; i < iconOnly.length; i++) {
                if (iconOnly[i].visible) {
                    width += TvTheme.pillHeight
                    items++
                }
            }

            if (versionLabel.visible) {
                width += versionLabel.implicitWidth
                items++
            }

            if (level < 4 && controllerStatus.count > 0) {
                width += barDivider.Layout.preferredWidth + barDivider.Layout.leftMargin + barDivider.Layout.rightMargin + (level <= 1 ? controllerStatus.fullWidth : controllerStatus.condensedWidth)
                items += 2
            }

            if (level < 3) {
                width += clockMetrics.width + clockLabel.Layout.leftMargin
                items++
            }

            return width + Math.max(items - 1, 0) * barRow.spacing
        }

        TextMetrics {
            id: addPcLabelMetrics
            font.pixelSize: TvTheme.fontLabel
            font.weight: Font.DemiBold
            text: addPcButton.tvLabel
        }

        TextMetrics {
            id: controllersLabelMetrics
            font.pixelSize: TvTheme.fontLabel
            font.weight: Font.DemiBold
            text: controllersButton.tvLabel
        }

        TextMetrics {
            id: settingsLabelMetrics
            font.pixelSize: TvTheme.fontLabel
            font.weight: Font.DemiBold
            text: settingsButton.tvLabel
        }

        // The clock's width, which only changes with the number of characters
        TextMetrics {
            id: clockMetrics
            font: clockLabel.font
            text: clockLabel.widestText
        }

        // Measures text for the clock as it's needed
        TextMetrics {
            id: clockMeasure
            font: clockLabel.font
        }

        // TV mode shows the window's background through the toolbar. This is
        // a Binding rather than an assignment in Component.onCompleted, which
        // can run after the window has been shown and drawn a frame with the
        // toolbar still visible. Bindings are applied before any of those
        // handlers run.
        Binding {
            target: toolBar.background
            property: "opacity"
            value: 0
            when: SystemProperties.tvMode
        }

        // Key presses that a focused toolbar button doesn't handle arrive
        // here, rather than at the StackView with the page below
        Keys.onEscapePressed: goBackOrQuit()

        Keys.onBackPressed: goBackOrQuit()

        Label {
            id: titleLabel
            // TV mode shows a left-aligned breadcrumb in the row instead
            visible: !SystemProperties.tvMode && toolBar.width > 700
            anchors.fill: parent
            text: stackView.currentItem.objectName
            font.pointSize: SystemProperties.tvMode ? 22 : 20
            elide: Label.ElideRight
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
        }

        RowLayout {
            id: barRow
            spacing: SystemProperties.tvMode ? 12 : 10
            anchors.leftMargin: SystemProperties.tvMode ? tvSafeX : 10
            anchors.rightMargin: SystemProperties.tvMode ? tvSafeX : 10
            anchors.topMargin: tvSafeY
            anchors.fill: parent

            NavigableToolButton {
                id: backButton

                // Only make the button visible if the user has navigated somewhere.
                visible: stackView.depth > 1

                iconSource: SystemProperties.tvMode ? "qrc:/res/tv_arrow_left.svg" : "qrc:/res/arrow_left.svg"

                onClicked: goBack()

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            // This label will appear when the window gets too small and
            // we need to ensure the toolbar controls don't collide
            Label {
                id: titleRowLabel
                font.pointSize: titleLabel.font.pointSize
                elide: Label.ElideRight
                horizontalAlignment: SystemProperties.tvMode ? Qt.AlignLeft : Qt.AlignHCenter
                verticalAlignment: Qt.AlignVCenter
                Layout.fillWidth: true

                // The TV mode type scale overrides the point size above
                Binding {
                    target: titleRowLabel
                    property: "font.pixelSize"
                    value: TvTheme.fontTitle
                    when: SystemProperties.tvMode
                }

                Binding {
                    target: titleRowLabel
                    property: "font.weight"
                    value: Font.DemiBold
                    when: SystemProperties.tvMode
                }

                Binding {
                    target: titleRowLabel
                    property: "font.family"
                    value: TvTheme.displayFontFamily
                    when: SystemProperties.tvMode
                }
                textFormat: SystemProperties.tvMode ? Text.StyledText : Text.PlainText

                // We need this label to always be visible so it can occupy
                // the remaining space in the RowLayout. To "hide" it, we
                // just set the text to empty string.
                text: SystemProperties.tvMode ? breadcrumbText(stackView.depth, stackView.currentItem) :
                                                !titleLabel.visible ? stackView.currentItem.objectName : ""
            }

            Label {
                id: versionLabel
                visible: stackView.currentItem instanceof SettingsView
                text: qsTr("Version %1").arg(SystemProperties.versionString)
                font.pointSize: 12
                color: SystemProperties.tvMode ? TvTheme.textSecondary : Material.foreground

                Binding {
                    target: versionLabel
                    property: "font.pixelSize"
                    value: TvTheme.fontCaption
                    when: SystemProperties.tvMode
                }
                horizontalAlignment: Qt.AlignRight
                verticalAlignment: Qt.AlignVCenter
            }

            NavigableToolButton {
                id: discordButton
                visible: SystemProperties.hasBrowser &&
                         stackView.currentItem instanceof SettingsView

                iconSource: "qrc:/res/discord.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                ToolTip.text: qsTr("Join our community on Discord")

                // TODO need to make sure browser is brought to foreground.
                onClicked: Qt.openUrlExternally("https://moonlight-stream.org/discord");

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: helpButton
                visible: SystemProperties.hasBrowser

                iconSource: SystemProperties.tvMode ? "qrc:/res/tv_question_mark.svg" : "qrc:/res/question_mark.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                ToolTip.text: qsTr("Help") + (helpShortcut.nativeText ? (" ("+helpShortcut.nativeText+")") : "")

                Shortcut {
                    id: helpShortcut
                    sequence: StandardKey.HelpContents
                    onActivated: helpButton.clicked()
                }

                // TODO need to make sure browser is brought to foreground.
                onClicked: Qt.openUrlExternally("https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide");

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: addPcButton
                visible: stackView.currentItem instanceof PcView

                iconSource: SystemProperties.tvMode ? "qrc:/res/add_pc.svg" : "qrc:/res/ic_add_to_queue_white_48px.svg"
                tvLabel: qsTr("Add PC")
                tvShowLabel: toolBar.tvBarLevel < 1

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                ToolTip.text: qsTr("Add PC manually") + (newPcShortcut.nativeText ? (" ("+newPcShortcut.nativeText+")") : "")

                Shortcut {
                    id: newPcShortcut
                    sequence: StandardKey.New
                    onActivated: addPcButton.clicked()
                }

                onClicked: {
                    addPcDialog.open()
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                property string browserUrl: ""

                id: updateButton

                iconSource: "qrc:/res/update.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered || visible

                // Invisible until we get a callback notifying us that
                // an update is available
                visible: false

                onClicked: {
                    if (SystemProperties.hasBrowser) {
                        Qt.openUrlExternally(browserUrl);
                    }
                }

                function updateAvailable(version, url)
                {
                    ToolTip.text = qsTr("Update available for Moonlight: Version %1").arg(version)
                    updateButton.browserUrl = url
                    updateButton.visible = true
                }

                Component.onCompleted: {
                    AutoUpdateChecker.onUpdateAvailable.connect(updateAvailable)
                    AutoUpdateChecker.start()
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                // TODO: Implement gamepad mapping then unhide this button
                visible: false

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                ToolTip.text: qsTr("Gamepad Mapper")

                iconSource: SystemProperties.tvMode ? "qrc:/res/gamepad.svg" : "qrc:/res/ic_videogame_asset_white_48px.svg"

                onClicked: navigateTo("qrc:/gui/GamepadMapper.qml", GamepadMapper)

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: controllersButton

                iconSource: SystemProperties.tvMode ? "qrc:/res/gamepad.svg" : "qrc:/res/ic_videogame_asset_white_48px.svg"
                tvLabel: qsTr("Controllers")
                tvShowLabel: toolBar.tvBarLevel < 1

                onClicked: navigateTo("qrc:/gui/ControllerView.qml", ControllerView)

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                ToolTip.text: qsTr("Controllers")
            }

            NavigableToolButton {
                id: settingsButton

                iconSource: SystemProperties.tvMode ? "qrc:/res/tv_settings.svg" : "qrc:/res/settings.svg"
                tvLabel: qsTr("Settings")
                tvShowLabel: toolBar.tvBarLevel < 1

                onClicked: navigateTo("qrc:/gui/SettingsView.qml", SettingsView)

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }

                Shortcut {
                    id: settingsShortcut
                    sequence: StandardKey.Preferences
                    onActivated: settingsButton.clicked()
                }

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: InputModeTracker.gamepadActive ? visualFocus : hovered
                ToolTip.text: qsTr("Settings") + (settingsShortcut.nativeText ? (" ("+settingsShortcut.nativeText+")") : "")
            }

            // TV mode: the connected controllers, with their player numbers
            // and batteries
            Rectangle {
                id: barDivider
                visible: controllerStatus.visible
                Layout.preferredWidth: 2
                Layout.preferredHeight: 36
                Layout.leftMargin: 4
                Layout.rightMargin: 4
                color: TvTheme.stroke
            }

            TvControllerStatus {
                id: controllerStatus
                visible: SystemProperties.tvMode && count > 0 && toolBar.tvBarLevel < 4
                condensed: toolBar.tvBarLevel >= 2
            }

            // TV mode: a clock, since the app usually runs fullscreen
            Label {
                id: clockLabel
                visible: SystemProperties.tvMode && toolBar.tvBarLevel < 3
                Layout.leftMargin: 8
                font.pixelSize: TvTheme.fontBody
                font.weight: Font.DemiBold

                // The locale's short time without seconds, which some locales
                // include, so it stays 12 or 24 hour as the locale prefers
                readonly property string timeFormat: Qt.locale().timeFormat(Locale.ShortFormat)
                                                         .replace(/[:.]?s+/g, "")
                                                         .replace(/\s*t+/g, "")
                                                         .trim()

                // The time with every digit as the widest one, and the wider of
                // AM and PM. Sizing the clock for this keeps the top bar from
                // shifting as the time changes, except when it gains or loses
                // a character.
                property string widestText: ""

                Layout.preferredWidth: Math.ceil(clockMetrics.advanceWidth)
                horizontalAlignment: Text.AlignRight

                function measure(text) {
                    clockMeasure.text = text
                    return clockMeasure.advanceWidth
                }

                function update() {
                    text = Qt.formatTime(new Date(), timeFormat)

                    var widestDigit = "0"
                    for (var digit = 1; digit <= 9; digit++) {
                        if (measure(String(digit)) > measure(widestDigit)) {
                            widestDigit = String(digit)
                        }
                    }
                    var widest = text.replace(/[0-9]/g, widestDigit)

                    var am = Qt.locale().amText
                    var pm = Qt.locale().pmText
                    if (am !== "" && pm !== "") {
                        var widerSuffix = measure(am) >= measure(pm) ? am : pm
                        widest = widest.replace(am, widerSuffix).replace(pm, widerSuffix)
                    }
                    widestText = widest
                }

                Timer {
                    interval: 1000
                    repeat: true
                    triggeredOnStart: true
                    running: clockLabel.visible
                    onTriggered: clockLabel.update()
                }
            }
        }
    }

    ErrorMessageDialog {
        id: noHwDecoderDialog
        text: qsTr("No functioning hardware accelerated video decoder was detected by Moonlight. " +
                   "Your streaming performance may be severely degraded in this configuration.")
        helpText: qsTr("Click the Help button for more information on solving this problem.")
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Fixing-Hardware-Decoding-Problems"
    }

    ErrorMessageDialog {
        id: xWaylandDialog
        text: qsTr("Hardware acceleration doesn't work on XWayland. Continuing on XWayland may result in poor streaming performance. " +
                   "Try running with QT_QPA_PLATFORM=wayland or switch to X11.")
        helpText: qsTr("Click the Help button for more information.")
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Fixing-Hardware-Decoding-Problems"
    }

    NavigableMessageDialog {
        id: wow64Dialog
        standardButtons: Dialog.Ok | Dialog.Cancel
        text: qsTr("This version of Moonlight isn't optimized for your PC. Please download the '%1' version of Moonlight for the best streaming performance.").arg(SystemProperties.friendlyNativeArchName)
        onAccepted: {
            Qt.openUrlExternally("https://github.com/moonlight-stream/moonlight-qt/releases");
        }
    }

    ErrorMessageDialog {
        id: unmappedGamepadDialog
        property string unmappedGamepads : ""
        text: qsTr("Moonlight detected gamepads without a mapping:") + "\n" + unmappedGamepads
        helpTextSeparator: "\n\n"
        helpText: qsTr("Click the Help button for information on how to map your gamepads.")
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Gamepad-Mapping"
    }

    // This dialog appears when quitting via keyboard or gamepad button
    NavigableMessageDialog {
        id: quitConfirmationDialog
        standardButtons: Dialog.Yes | Dialog.No
        headline: qsTr("Quit Moonlight?")
        text: SystemProperties.tvMode ? qsTr("Games running on your host PCs keep running after Moonlight closes.")
                                       : qsTr("Are you sure you want to quit?")
        acceptText: qsTr("Quit")
        rejectText: qsTr("Keep using Moonlight")
        destructive: true
        imageSrc: SystemProperties.tvMode ? "qrc:/res/power.svg" : "qrc:/res/baseline-help_outline-24px.svg"
        // For keyboard/gamepad navigation
        onAccepted: Qt.quit()
    }

    // HACK: This belongs in StreamSegue but keeping a dialog around after the parent
    // dies can trigger bugs in Qt 5.12 that cause the app to crash. For now, we will
    // host this dialog in a QML component that is never destroyed.
    //
    // To repro: Start a stream, cut the network connection to trigger the "Connection
    // terminated" dialog, wait until the app grid times out back to the PC grid, then
    // try to dismiss the dialog.
    ErrorMessageDialog {
        id: streamSegueErrorDialog

        property bool quitAfter: false

        onClosed: {
            if (quitAfter) {
                Qt.quit()
            }

            // StreamSegue assumes its dialog will be re-created each time we
            // start streaming, so fake it by wiping out the text each time.
            text = ""
        }
    }

    NavigableDialog {
        id: addPcDialog
        property string label: qsTr("Enter the IP address of your host PC:")

        standardButtons: Dialog.Ok | Dialog.Cancel
        // TV mode shows a headline and says what the buttons do
        title: SystemProperties.tvMode ? qsTr("Add a PC") : ""
        acceptText: qsTr("Add PC")

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                ComputerManager.addNewHostManually(editText.text.trim())
            }
        }

        ColumnLayout {
            width: parent ? parent.width : implicitWidth

            Label {
                text: addPcDialog.label
                // Under the TV mode headline, this explains it
                font.bold: !SystemProperties.tvMode
                color: SystemProperties.tvMode ? TvTheme.textSecondary : Material.foreground
            }

            TextField {
                id: editText
                Layout.fillWidth: true
                focus: true

                // Move on to the buttons with the gamepad
                Keys.onDownPressed: nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)

                Keys.onReturnPressed: {
                    addPcDialog.accept()
                }

                Keys.onEnterPressed: {
                    addPcDialog.accept()
                }
            }
        }
    }

    footer: TvHintBar {
        visible: SystemProperties.tvMode

        // Streaming and quitting pages have no title and take no input
        active: stackView.currentItem !== null && stackView.currentItem.objectName !== ""
        inPopup: isInPopup(window.activeFocusItem)
        inGrid: window.activeFocusItem !== null &&
                (window.activeFocusItem instanceof GridView || window.activeFocusItem.grid !== undefined)
        canGoBack: stackView.depth > 1
        canOpenSettings: !(stackView.currentItem instanceof SettingsView)
        focusItem: window.activeFocusItem
        safeX: tvSafeX
        safeY: tvSafeY
    }

    // A clearly visible indicator around the focused control while navigating
    // with a gamepad. The Material style's own focus cues are too subtle to
    // see from across the room. It lives in the overlay so it is also drawn
    // for controls inside dialogs and menus.
    Rectangle {
        id: focusRing

        readonly property int ringMargin: target && target.focusRingFlush ? 0 : 4

        // Only shown while the gamepad is in use. Only controls report
        // visualFocus, and only when focus was gained through keyboard or
        // gamepad navigation (not a mouse click).
        property Item target: {
            if (!InputModeTracker.gamepadActive) {
                return null
            }

            // TV mode grid cards draw their own focus glow
            var item = window.activeFocusItem
            return (item && item.visualFocus === true && item.tvCard !== true) ? item : null
        }

        property bool targetOnScreen: false

        // In TV mode the ring glides to each newly focused control. It only
        // animates briefly after the target changes, so it still tracks
        // scrolling and layout changes without lagging behind.
        property bool gliding: false

        parent: Overlay.overlay
        z: 1000000
        visible: target !== null && target.visible && targetOnScreen
        color: "transparent"
        // Controls can match the ring to their own shape
        radius: target && target.focusRingPill ? height / 2 :
                target && target.focusRingRadius !== undefined ? target.focusRingRadius + ringMargin :
                SystemProperties.tvMode ? TvTheme.focusRingRadius : 6
        border.width: SystemProperties.tvMode ? TvTheme.focusRingWidth : 3
        border.color: Material.accent

        Behavior on x { enabled: focusRing.gliding; NumberAnimation { duration: TvTheme.animationFast; easing.type: Easing.OutCubic } }
        Behavior on y { enabled: focusRing.gliding; NumberAnimation { duration: TvTheme.animationFast; easing.type: Easing.OutCubic } }
        Behavior on width { enabled: focusRing.gliding; NumberAnimation { duration: TvTheme.animationFast; easing.type: Easing.OutCubic } }
        Behavior on height { enabled: focusRing.gliding; NumberAnimation { duration: TvTheme.animationFast; easing.type: Easing.OutCubic } }

        // A soft outer glow so the ring stands out from across the room
        Rectangle {
            visible: SystemProperties.tvMode
            anchors.fill: parent
            anchors.margins: -TvTheme.focusGlowWidth
            color: "transparent"
            radius: parent.radius + TvTheme.focusGlowWidth
            border.width: TvTheme.focusGlowWidth
            border.color: TvTheme.focusGlow
        }

        // Stops the glide once the ring has reached its new target
        Timer {
            id: glideTimer
            interval: TvTheme.animationFast * 2
            onTriggered: focusRing.gliding = false
        }

        function updateGeometry() {
            if (target === null) {
                targetOnScreen = false
                return
            }

            // Map both corners so a scaled target is measured correctly
            var pos = target.mapToItem(parent, 0, 0)
            var end = target.mapToItem(parent, target.width, target.height)
            var left = pos.x - ringMargin
            var top = pos.y - ringMargin
            var right = end.x + ringMargin
            var bottom = end.y + ringMargin

            // Keep the ring inside any clipping ancestors, and inside the page
            // area so a partially scrolled control doesn't draw over the toolbar
            for (var item = target.parent; item; item = item.parent) {
                if (item.clip || item === stackView) {
                    var clipPos = item.mapToItem(parent, 0, 0)
                    left = Math.max(left, clipPos.x)
                    top = Math.max(top, clipPos.y)
                    right = Math.min(right, clipPos.x + item.width)
                    bottom = Math.min(bottom, clipPos.y + item.height)
                }
            }

            targetOnScreen = right > left && bottom > top && target.width > 0 && target.height > 0
            x = left
            y = top
            width = right - left
            height = bottom - top
        }

        onTargetChanged: {
            // Only glide from a control that was showing the ring. Otherwise
            // the ring would fly in from wherever it was last shown.
            if (SystemProperties.tvMode && visible && target !== null) {
                gliding = true
                glideTimer.restart()
            }
            else {
                gliding = false
            }

            updateGeometry()
        }

        // Follow the target while it moves (scrolling, animations, layout changes)
        Timer {
            interval: 16
            repeat: true
            running: focusRing.target !== null
            onTriggered: focusRing.updateGeometry()
        }
    }
}
