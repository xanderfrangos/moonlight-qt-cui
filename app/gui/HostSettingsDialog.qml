import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Controls.Material 2.2
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2

import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0
import InputModeTracker 1.0
import TvTheme 1.0

// Settings a PC streams with in place of the global ones. Each setting
// either uses the global setting or overrides it for this PC. Changes are
// edited here and only saved with Save.
NavigableDialog {
    id: dialog

    property string uuid: ""
    property string pcName: ""

    title: qsTr("Stream settings for %1").arg(pcName)
    standardButtons: Dialog.Ok | Dialog.Cancel
    acceptText: qsTr("Save")
    tvContentWidth: 760

    contentWidth: SystemProperties.tvMode ? tvContentWidth : 560
    contentHeight: settingsColumn.implicitHeight
    // Scroll the settings when they don't fit in the window
    height: Math.min(implicitHeight, (Overlay.overlay ? Overlay.overlay.height : 1080) - (SystemProperties.tvMode ? 2 * TvTheme.spacingXLarge : 40))

    // The overrides being edited. A setting that isn't overridden keeps the
    // global value here, so turning its override on starts from it.
    property bool resolutionOn: false
    property int resolutionWidth: 0
    property int resolutionHeight: 0
    property bool resolutionNative: false

    property bool fpsOn: false
    property int fpsValue: 0
    property bool fpsNative: false
    property bool fpsRecommended: false

    property bool bitrateOn: false
    property int bitrateKbps: 0

    property bool hdrOn: false
    property bool hdrEnabled: false

    property bool vsyncOn: false
    property bool vsyncEnabled: true
    property bool framePacing: false
    property bool vrrEnabled: false
    property bool timestampPacing: false
    property int timestampSmoothing: 1
    property int vrrLatencyMode: 1
    property int vrrBufferPerMille: 1000
    property int vrrTargetHundredths: 9950
    property int vrrHistorySeconds: 120
    property int vrrToleranceUs: 500
    property bool smoothVrrFrameTiming: true
    property bool absorbVrrMicrostutter: false

    property bool codecOn: false
    property int codecValue: StreamingPreferences.VCC_AUTO

    property bool yuv444On: false
    property bool yuv444Enabled: false

    // What a stream from this PC would use
    readonly property int effectiveWidth: resolutionOn ? resolutionWidth : StreamingPreferences.width
    readonly property int effectiveHeight: resolutionOn ? resolutionHeight : StreamingPreferences.height
    readonly property int effectiveFps: fpsOn ? fpsValue : StreamingPreferences.fps
    readonly property bool effectiveHdr: hdrOn ? hdrEnabled : StreamingPreferences.enableHdr
    readonly property bool effectiveYuv444: yuv444On ? yuv444Enabled : StreamingPreferences.enableYUV444
    readonly property bool effectivePyroWave: (codecOn ? codecValue : StreamingPreferences.videoCodecConfig) === StreamingPreferences.VCC_FORCE_PYROWAVE
    // Gamescope owns V-Sync, so it is always on there
    readonly property bool effectiveVsync: SystemProperties.isGamescope || (vsyncOn ? vsyncEnabled : StreamingPreferences.enableVsync)
    readonly property bool effectiveVrr: effectiveVsync && (vsyncOn ? vrrEnabled && !timestampPacing : StreamingPreferences.enableVrr)

    // The global bitrate follows the global settings, and follows this PC's
    // settings the same way when they differ
    readonly property bool bitrateFollowsSettings: StreamingPreferences.autoAdjustBitrate &&
                                                   (resolutionOn || fpsOn || hdrOn || yuv444On || codecOn)
    readonly property int bitrateWithoutOverride: bitrateFollowsSettings ? defaultBitrate() : StreamingPreferences.bitrateKbps

    readonly property var codecNames: {
        var names = {}
        names[StreamingPreferences.VCC_AUTO] = qsTr("Automatic (Recommended)")
        names[StreamingPreferences.VCC_FORCE_H264] = qsTr("H.264")
        names[StreamingPreferences.VCC_FORCE_HEVC] = qsTr("HEVC (H.265)")
        names[StreamingPreferences.VCC_FORCE_AV1] = qsTr("AV1")
        names[StreamingPreferences.VCC_FORCE_PYROWAVE] = qsTr("PyroWave (wired LAN, experimental)")
        return names
    }

    function defaultBitrate() {
        if (effectivePyroWave) {
            return StreamingPreferences.getDefaultPyroWaveBitrate(effectiveWidth, effectiveHeight, effectiveFps,
                                                                  effectiveYuv444, effectiveHdr)
        }
        return StreamingPreferences.getDefaultBitrate(effectiveWidth, effectiveHeight, effectiveFps, effectiveYuv444)
    }

    function onOff(on) {
        return on ? qsTr("On") : qsTr("Off")
    }

    function vsyncSummary(vsync, pacing, vrr, timestamps) {
        var parts = []
        if (!SystemProperties.isGamescope) {
            parts.push(vsync ? qsTr("V-Sync on") : qsTr("V-Sync off"))
        }
        if (timestamps) {
            parts.push(qsTr("timestamp pacing"))
        }
        else {
            if (vsync && pacing && !SystemProperties.isGamescope) {
                parts.push(qsTr("frame pacing"))
            }
            if ((vsync || SystemProperties.isGamescope) && vrr) {
                parts.push(qsTr("VRR"))
            }
        }
        if (parts.length === 0) {
            return qsTr("Controlled by Gamescope")
        }
        return parts.join(", ")
    }

    function load() {
        var saved = StreamingPreferences.getHostOverrides(uuid)
        function value(group, key, fallback) {
            var values = saved[group]
            return values !== undefined && values[key] !== undefined ? values[key] : fallback
        }

        resolutionOn = saved.resolution !== undefined
        resolutionWidth = value("resolution", "width", StreamingPreferences.width)
        resolutionHeight = value("resolution", "height", StreamingPreferences.height)
        resolutionNative = value("resolution", "native", StreamingPreferences.nativeResolution)

        fpsOn = saved.fps !== undefined
        fpsValue = value("fps", "fps", StreamingPreferences.fps)
        fpsNative = value("fps", "native", StreamingPreferences.nativeFps)
        fpsRecommended = value("fps", "recommended", StreamingPreferences.recommendedFps)

        hdrOn = saved.hdr !== undefined
        hdrEnabled = value("hdr", "enabled", StreamingPreferences.enableHdr)

        yuv444On = saved.yuv444 !== undefined
        yuv444Enabled = value("yuv444", "enabled", StreamingPreferences.enableYUV444)

        codecOn = saved.codec !== undefined
        codecValue = value("codec", "value", StreamingPreferences.videoCodecConfig)

        vsyncOn = saved.vsync !== undefined
        vsyncEnabled = value("vsync", "vsync", StreamingPreferences.enableVsync)
        framePacing = value("vsync", "framePacing", StreamingPreferences.framePacing)
        vrrEnabled = value("vsync", "vrr", StreamingPreferences.enableVrr)
        timestampPacing = value("vsync", "timestampPacing", StreamingPreferences.timestampPacing)
        timestampSmoothing = value("vsync", "timestampSmoothing", StreamingPreferences.timestampSmoothing)
        vrrLatencyMode = value("vsync", "vrrLatencyMode", StreamingPreferences.vrrLatencyMode)
        vrrBufferPerMille = value("vsync", "vrrBufferPerMille", StreamingPreferences.vrrBufferPerMille)
        vrrTargetHundredths = value("vsync", "vrrTargetHundredths", StreamingPreferences.vrrTargetHundredths)
        vrrHistorySeconds = value("vsync", "vrrHistorySeconds", StreamingPreferences.vrrHistorySeconds)
        vrrToleranceUs = value("vsync", "vrrToleranceUs", StreamingPreferences.vrrToleranceUs)
        smoothVrrFrameTiming = value("vsync", "smoothVrrFrameTiming", StreamingPreferences.smoothVrrFrameTiming)
        absorbVrrMicrostutter = value("vsync", "absorbVrrMicrostutter", StreamingPreferences.absorbVrrMicrostutter)

        // Loaded after the settings it depends on
        bitrateOn = saved.bitrate !== undefined
        bitrateKbps = value("bitrate", "kbps", bitrateWithoutOverride)

        resolutionComboBox.rebuild()
        fpsComboBox.rebuild()
        codecComboBox.select()
    }

    function save() {
        var overrides = {}
        if (resolutionOn) {
            overrides.resolution = { "width": resolutionWidth, "height": resolutionHeight, "native": resolutionNative }
        }
        if (fpsOn) {
            overrides.fps = { "fps": fpsValue, "native": fpsNative, "recommended": fpsRecommended }
        }
        if (bitrateOn) {
            overrides.bitrate = { "kbps": Math.max(bitrateSlider.from, Math.min(bitrateSlider.to, bitrateKbps)) }
        }
        if (hdrOn) {
            overrides.hdr = { "enabled": hdrEnabled }
        }
        if (vsyncOn) {
            overrides.vsync = {
                "vsync": vsyncEnabled,
                "framePacing": framePacing,
                "vrr": vrrEnabled && !timestampPacing,
                "timestampPacing": timestampPacing,
                "timestampSmoothing": timestampSmoothing,
                "vrrLatencyMode": vrrLatencyMode,
                "vrrBufferPerMille": vrrBufferPerMille,
                "vrrTargetHundredths": vrrTargetHundredths,
                "vrrHistorySeconds": vrrHistorySeconds,
                "vrrToleranceUs": vrrToleranceUs,
                "smoothVrrFrameTiming": smoothVrrFrameTiming,
                "absorbVrrMicrostutter": absorbVrrMicrostutter
            }
        }
        if (codecOn) {
            overrides.codec = { "value": codecValue }
        }
        if (yuv444On) {
            overrides.yuv444 = { "enabled": yuv444Enabled }
        }
        StreamingPreferences.setHostOverrides(uuid, overrides)
    }

    // The frame rate choices depend on whether VRR is on
    onEffectiveVrrChanged: {
        if (visible) {
            fpsComboBox.rebuild()
        }
    }

    onAboutToShow: load()

    onOpened: {
        // Up and down move between the controls, as on the settings page
        SdlGamepadKeyNavigation.setUiNavMode(true)
        flickable.contentY = 0
        // Only show the focus cue when the gamepad or keyboard opened it
        resolutionRow.overrideSwitch.forceActiveFocus(InputModeTracker.gamepadActive ? Qt.TabFocusReason : Qt.OtherFocusReason)
    }

    onClosed: SdlGamepadKeyNavigation.setUiNavMode(false)

    onAccepted: save()

    Component.onCompleted: {
        // Desktop mode uses the style's buttons, which don't take acceptText
        if (!SystemProperties.tvMode && standardButton(Dialog.Ok)) {
            standardButton(Dialog.Ok).text = acceptText
        }
    }

    Flickable {
        id: flickable
        anchors.fill: parent
        contentHeight: settingsColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ScrollBar {}

        NumberAnimation on contentY {
            id: autoScrollAnimation
            duration: 100
        }

        // Keep the focused control in view while moving through the
        // settings with a gamepad or keyboard
        Window.onActiveFocusItemChanged: {
            var item = Window.activeFocusItem
            if (!item || !dialog.visible) {
                return
            }

            for (var ancestor = item.parent; ancestor && ancestor !== flickable.contentItem; ancestor = ancestor.parent) {
            }
            if (!ancestor) {
                return
            }

            var pos = item.mapToItem(flickable.contentItem, 0, 0)
            var margin = SystemProperties.tvMode ? TvTheme.spacingLarge : 20
            if (pos.y - margin < flickable.contentY) {
                autoScrollAnimation.from = flickable.contentY
                autoScrollAnimation.to = Math.max(pos.y - margin, 0)
                autoScrollAnimation.start()
            }
            else if (pos.y + item.height + margin > flickable.contentY + flickable.height) {
                autoScrollAnimation.from = flickable.contentY
                autoScrollAnimation.to = Math.min(pos.y + item.height + margin - flickable.height,
                                                  flickable.contentHeight - flickable.height)
                autoScrollAnimation.start()
            }
        }

        ColumnLayout {
            id: settingsColumn
            // Leave room for the scroll bar when the settings scroll
            width: flickable.width - (flickable.contentHeight > flickable.height ? 12 : 0)
            spacing: SystemProperties.tvMode ? TvTheme.spacingMedium : 10

            Label {
                Layout.fillWidth: true
                Layout.bottomMargin: SystemProperties.tvMode ? TvTheme.spacingSmall : 0
                text: qsTr("Choose the settings this PC streams with instead of your global settings. The rest of your settings still apply.")
                color: SystemProperties.tvMode ? TvTheme.textSecondary : Material.foreground
                wrapMode: Text.Wrap
            }

            HostSettingRow {
                id: resolutionRow
                Layout.fillWidth: true
                title: qsTr("Resolution")
                globalText: StreamingPreferences.nativeResolution ? qsTr("Native") :
                                qsTr("%1x%2").arg(StreamingPreferences.width).arg(StreamingPreferences.height)
                overridden: dialog.resolutionOn
                onOverrideToggled: function(on) { dialog.resolutionOn = on }

                AutoResizingComboBox {
                    id: resolutionComboBox
                    Layout.fillWidth: true
                    textRole: "text"
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    model: ListModel {
                        id: resolutionModel
                    }

                    readonly property bool customSelected: currentIndex >= 0 && currentIndex < count &&
                                                           resolutionModel.get(currentIndex).custom

                    function rebuild() {
                        SystemProperties.refreshDisplays()

                        var choices = [
                            { "text": qsTr("720p"), "w": 1280, "h": 720 },
                            { "text": qsTr("1080p"), "w": 1920, "h": 1080 },
                            { "text": qsTr("1440p"), "w": 2560, "h": 1440 },
                            { "text": qsTr("4K"), "w": 3840, "h": 2160 }
                        ]
                        function addDisplay(prefix, rect) {
                            if (rect.width <= 0 || rect.height <= 0) {
                                return
                            }
                            for (var j = 0; j < choices.length; j++) {
                                if (choices[j].w === rect.width && choices[j].h === rect.height) {
                                    return
                                }
                            }
                            var size = rect.width + "x" + rect.height
                            choices.push({ "text": prefix ? prefix + " (" + size + ")" : size, "w": rect.width, "h": rect.height })
                        }
                        for (var displayIndex = 0; ; displayIndex++) {
                            var screenRect = SystemProperties.getNativeResolution(displayIndex)
                            if (screenRect.width === 0) {
                                break
                            }
                            addDisplay("", screenRect)
                            addDisplay(qsTr("Excluding Notch"), SystemProperties.getSafeAreaResolution(displayIndex))
                        }

                        // Only what the decoder can handle
                        var maxPixels = SystemProperties.maximumResolution.width * SystemProperties.maximumResolution.height
                        choices = choices.filter(function(choice) { return maxPixels <= 0 || choice.w * choice.h <= maxPixels })
                        choices.sort(function(a, b) { return a.w * a.h - b.w * b.h })

                        resolutionModel.clear()

                        // Native follows the stream's display, detected again at launch
                        var display = SystemProperties.getStreamDisplayMode()
                        var hasNative = display.width > 0 && display.height > 0
                        resolutionModel.append({
                            "text": hasNative ? qsTr("Native (%1x%2)").arg(display.width).arg(display.height) : qsTr("Native"),
                            "w": hasNative ? display.width : dialog.resolutionWidth,
                            "h": hasNative ? display.height : dialog.resolutionHeight,
                            "native": true,
                            "custom": false
                        })

                        var selected = dialog.resolutionNative ? 0 : -1
                        for (var i = 0; i < choices.length; i++) {
                            resolutionModel.append({ "text": choices[i].text, "w": choices[i].w, "h": choices[i].h,
                                                     "native": false, "custom": false })
                            if (selected < 0 && choices[i].w === dialog.resolutionWidth && choices[i].h === dialog.resolutionHeight) {
                                selected = resolutionModel.count - 1
                            }
                        }

                        resolutionModel.append({ "text": selected >= 0 ? qsTr("Custom") :
                                                             qsTr("Custom (%1x%2)").arg(dialog.resolutionWidth).arg(dialog.resolutionHeight),
                                                 "w": dialog.resolutionWidth, "h": dialog.resolutionHeight,
                                                 "native": false, "custom": true })
                        currentIndex = selected >= 0 ? selected : resolutionModel.count - 1
                        if (dialog.resolutionNative) {
                            dialog.resolutionWidth = resolutionModel.get(0).w
                            dialog.resolutionHeight = resolutionModel.get(0).h
                        }

                        customWidthField.text = dialog.resolutionWidth
                        customHeightField.text = dialog.resolutionHeight
                        recalculateWidth()
                    }

                    function updateCustomText() {
                        resolutionModel.setProperty(currentIndex, "text",
                                                    qsTr("Custom (%1x%2)").arg(dialog.resolutionWidth).arg(dialog.resolutionHeight))
                    }

                    onActivated: {
                        var choice = resolutionModel.get(currentIndex)
                        dialog.resolutionNative = choice.native
                        if (!choice.custom) {
                            dialog.resolutionWidth = choice.w
                            dialog.resolutionHeight = choice.h
                        }
                        customWidthField.text = dialog.resolutionWidth
                        customHeightField.text = dialog.resolutionHeight
                    }
                }

                // A custom resolution is typed in here
                RowLayout {
                    Layout.fillWidth: true
                    visible: resolutionComboBox.customSelected
                    spacing: 10

                    TextField {
                        id: customWidthField
                        Layout.fillWidth: true
                        maximumLength: 5
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { bottom: 256; top: 8192 }
                        Accessible.name: qsTr("Custom width")
                        onTextChanged: {
                            if (acceptableInput && resolutionComboBox.customSelected) {
                                dialog.resolutionWidth = parseInt(text)
                                resolutionComboBox.updateCustomText()
                            }
                        }
                    }

                    Label {
                        text: "x"
                        font.bold: true
                    }

                    TextField {
                        id: customHeightField
                        Layout.fillWidth: true
                        maximumLength: 5
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { bottom: 256; top: 8192 }
                        Accessible.name: qsTr("Custom height")
                        onTextChanged: {
                            if (acceptableInput && resolutionComboBox.customSelected) {
                                dialog.resolutionHeight = parseInt(text)
                                resolutionComboBox.updateCustomText()
                            }
                        }
                    }
                }
            }

            HostSettingRow {
                Layout.fillWidth: true
                title: qsTr("Frame rate")
                globalText: StreamingPreferences.nativeFps ? qsTr("Native") :
                            StreamingPreferences.recommendedFps ? qsTr("Recommended") :
                            qsTr("%1 FPS").arg(StreamingPreferences.fps)
                overridden: dialog.fpsOn
                onOverrideToggled: function(on) { dialog.fpsOn = on }

                AutoResizingComboBox {
                    id: fpsComboBox
                    Layout.fillWidth: true
                    textRole: "text"
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    model: ListModel {
                        id: fpsModel
                    }

                    readonly property bool customSelected: currentIndex >= 0 && currentIndex < count &&
                                                           fpsModel.get(currentIndex).custom

                    function rebuild() {
                        var refreshRates = []
                        for (var displayIndex = 0; ; displayIndex++) {
                            var refreshRate = SystemProperties.getRefreshRate(displayIndex)
                            if (refreshRate === 0) {
                                break
                            }
                            refreshRates.push(refreshRate)
                        }

                        // Native and Recommended follow the stream's display,
                        // detected again at launch. Where VRR can't be
                        // detected, Recommended is named VRR and assumes it.
                        var display = SystemProperties.getStreamDisplayMode()
                        var nativeFps = display.refreshRate
                        var detectable = display.vrrDetectable
                        var vrrDisplay = (detectable ? display.vrr : true) || dialog.effectiveVrr
                        var recommended = nativeFps > 0 ? (vrrDisplay ? StreamingPreferences.getVrrFps(nativeFps) : nativeFps) : 0
                        if (dialog.fpsNative && nativeFps > 0) {
                            dialog.fpsValue = nativeFps
                        }
                        else if (dialog.fpsRecommended && recommended > 0) {
                            dialog.fpsValue = recommended
                        }

                        fpsModel.clear()
                        fpsModel.append({
                            "text": nativeFps > 0 ? qsTr("Native (%1 Hz)").arg(nativeFps) : qsTr("Native"),
                            "fps": nativeFps > 0 ? nativeFps : dialog.fpsValue,
                            "native": true, "recommended": false, "custom": false
                        })
                        fpsModel.append({
                            "text": detectable ?
                                        (recommended > 0 ? qsTr("Recommended (%1 FPS)").arg(recommended) : qsTr("Recommended")) :
                                        (recommended > 0 ? qsTr("VRR (%1 FPS)").arg(recommended) : qsTr("VRR")),
                            "fps": recommended > 0 ? recommended : dialog.fpsValue,
                            "native": false, "recommended": true, "custom": false
                        })

                        var selected = dialog.fpsNative ? 0 : dialog.fpsRecommended ? 1 : -1
                        var hasCustomChoice = false
                        var choices = StreamingPreferences.getFpsChoicesFor(refreshRates, dialog.fpsValue, dialog.effectiveVrr)
                        for (var i = 0; i < choices.length; i++) {
                            var choice = choices[i]
                            var choiceFps = parseInt(choice.video_fps)
                            // A saved rate that isn't one of the choices
                            if (choice.is_custom) {
                                hasCustomChoice = true
                                fpsModel.append({ "text": qsTr("Custom (%1 FPS)").arg(choiceFps), "fps": choiceFps,
                                                  "native": false, "recommended": false, "custom": true })
                                if (selected < 0 && choiceFps === dialog.fpsValue) {
                                    selected = fpsModel.count - 1
                                }
                                continue
                            }
                            // Already listed as the VRR choice
                            if (!detectable && choice.kind === "vrr" && choiceFps === recommended) {
                                if (selected < 0 && choiceFps === dialog.fpsValue) {
                                    selected = 1
                                }
                                continue
                            }

                            var text = choice.kind === "vrr" ? qsTr("VRR (%1 FPS)").arg(choiceFps) :
                                       choice.kind === "low-latency-vrr" ? qsTr("Low-latency VRR (%1 FPS)").arg(choiceFps) :
                                       qsTr("%1 FPS").arg(choiceFps)
                            fpsModel.append({ "text": text, "fps": choiceFps, "native": false, "recommended": false, "custom": false })
                            if (selected < 0 && choiceFps === dialog.fpsValue) {
                                selected = fpsModel.count - 1
                            }
                        }

                        if (!hasCustomChoice) {
                            fpsModel.append({ "text": qsTr("Custom"), "fps": dialog.fpsValue,
                                              "native": false, "recommended": false, "custom": true })
                        }
                        currentIndex = selected >= 0 ? selected : fpsModel.count - 1
                        customFpsField.text = dialog.fpsValue
                        recalculateWidth()
                    }

                    onActivated: {
                        var choice = fpsModel.get(currentIndex)
                        dialog.fpsNative = choice.native
                        dialog.fpsRecommended = choice.recommended
                        if (!choice.custom) {
                            dialog.fpsValue = choice.fps
                        }
                        customFpsField.text = dialog.fpsValue
                    }
                }

                // A custom frame rate is typed in here
                TextField {
                    id: customFpsField
                    Layout.fillWidth: true
                    visible: fpsComboBox.customSelected
                    maximumLength: 4
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: IntValidator { bottom: 10; top: 9999 }
                    Accessible.name: qsTr("Custom frame rate")
                    onTextChanged: {
                        if (acceptableInput && fpsComboBox.customSelected) {
                            dialog.fpsValue = parseInt(text)
                            fpsModel.setProperty(fpsComboBox.currentIndex, "text", qsTr("Custom (%1 FPS)").arg(dialog.fpsValue))
                        }
                    }
                }
            }

            HostSettingRow {
                Layout.fillWidth: true
                title: qsTr("Video bitrate")
                globalText: dialog.bitrateFollowsSettings ?
                                qsTr("Automatic, %1 Mbps for this PC's settings").arg(dialog.bitrateWithoutOverride / 1000.0) :
                                qsTr("%1 Mbps").arg(StreamingPreferences.bitrateKbps / 1000.0)
                overridden: dialog.bitrateOn
                onOverrideToggled: function(on) {
                    if (on) {
                        dialog.bitrateKbps = dialog.bitrateWithoutOverride
                    }
                    dialog.bitrateOn = on
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1 Mbps").arg(dialog.bitrateKbps / 1000.0)
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    NavigableSlider {
                        id: bitrateSlider
                        Layout.fillWidth: true
                        hoverEnabled: !SystemProperties.hoverEffectsDisabled
                        stepSize: dialog.effectivePyroWave ? 5000 : 500
                        from: dialog.effectivePyroWave ? 5000 : 500
                        to: dialog.effectivePyroWave ? 3000000 : (StreamingPreferences.unlockBitrate ? 500000 : 150000)
                        snapMode: "SnapOnRelease"
                        value: dialog.bitrateKbps
                        onMoved: dialog.bitrateKbps = Math.round(value / stepSize) * stepSize
                        Accessible.name: qsTr("Video bitrate")
                    }

                    Button {
                        text: qsTr("Use Default (%1 Mbps)").arg(dialog.defaultBitrate() / 1000.0)
                        enabled: dialog.bitrateKbps !== dialog.defaultBitrate()
                        hoverEnabled: !SystemProperties.hoverEffectsDisabled
                        onClicked: dialog.bitrateKbps = dialog.defaultBitrate()
                    }
                }
            }

            HostSettingRow {
                Layout.fillWidth: true
                title: qsTr("HDR")
                globalText: dialog.onOff(StreamingPreferences.enableHdr)
                overridden: dialog.hdrOn
                onOverrideToggled: function(on) { dialog.hdrOn = on }

                CheckBox {
                    Layout.fillWidth: true
                    text: qsTr("Enable HDR / 10-bit color")
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    enabled: SystemProperties.supportsHdr
                    checked: dialog.hdrEnabled
                    onToggled: dialog.hdrEnabled = checked
                }

                Label {
                    Layout.fillWidth: true
                    visible: !SystemProperties.supportsHdr
                    text: qsTr("HDR streaming is not supported on this PC.")
                    opacity: 0.7
                    wrapMode: Text.Wrap
                }
            }

            HostSettingRow {
                Layout.fillWidth: true
                title: qsTr("V-Sync and pacing")
                globalText: dialog.vsyncSummary(StreamingPreferences.enableVsync, StreamingPreferences.framePacing,
                                                StreamingPreferences.enableVrr, StreamingPreferences.timestampPacing)
                overridden: dialog.vsyncOn
                onOverrideToggled: function(on) { dialog.vsyncOn = on }

                // Gamescope owns V-Sync, and Moonlight has no V-sync source
                // to pace frames by there
                Label {
                    Layout.fillWidth: true
                    visible: SystemProperties.isGamescope
                    text: qsTr("Gamescope controls V-Sync. To allow tearing, use Allow Tearing in Steam's Quick Access menu.")
                    opacity: 0.7
                    wrapMode: Text.Wrap
                }

                CheckBox {
                    Layout.fillWidth: true
                    visible: !SystemProperties.isGamescope
                    text: qsTr("V-Sync")
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    checked: dialog.vsyncEnabled
                    onToggled: dialog.vsyncEnabled = checked
                }

                CheckBox {
                    Layout.fillWidth: true
                    visible: !SystemProperties.isGamescope
                    text: qsTr("Frame pacing")
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    // Timestamp pacing replaces it
                    enabled: dialog.vsyncEnabled && !dialog.timestampPacing
                    checked: dialog.vsyncEnabled && dialog.framePacing
                    onToggled: dialog.framePacing = checked
                }

                CheckBox {
                    Layout.fillWidth: true
                    text: qsTr("VRR")
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    // VRR and timestamp pacing are mutually exclusive
                    enabled: dialog.effectiveVsync && !dialog.timestampPacing
                    checked: dialog.vrrEnabled && !dialog.timestampPacing
                    onToggled: dialog.vrrEnabled = checked
                }

                CheckBox {
                    Layout.fillWidth: true
                    text: qsTr("Timestamp pacing")
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    checked: dialog.timestampPacing
                    onToggled: {
                        // Turning it on also turns VRR off
                        dialog.timestampPacing = checked
                        if (checked) {
                            dialog.vrrEnabled = false
                        }
                    }
                }

                TimestampPacingSettings {
                    Layout.fillWidth: true
                    visible: dialog.timestampPacing
                    smoothing: dialog.timestampSmoothing
                    vsyncEnabled: dialog.effectiveVsync
                    onSmoothingEdited: function(value) { dialog.timestampSmoothing = value }
                }

                VrrTimingSettings {
                    Layout.fillWidth: true
                    visible: dialog.effectiveVrr
                    bufferPerMille: dialog.vrrBufferPerMille
                    targetHundredths: dialog.vrrTargetHundredths
                    historySeconds: dialog.vrrHistorySeconds
                    toleranceUs: dialog.vrrToleranceUs
                    onPresetPicked: function(mode) {
                        var preset = StreamingPreferences.getVrrPreset(mode)
                        dialog.vrrLatencyMode = mode
                        dialog.vrrBufferPerMille = preset.bufferPerMille
                        dialog.vrrTargetHundredths = preset.targetHundredths
                        dialog.vrrHistorySeconds = preset.historySeconds
                        dialog.vrrToleranceUs = preset.toleranceUs
                    }
                    // Same limits as the global settings
                    onBufferEdited: function(value) { dialog.vrrBufferPerMille = Math.max(250, Math.min(4000, value)) }
                    onTargetEdited: function(value) { dialog.vrrTargetHundredths = Math.max(9000, Math.min(9999, value)) }
                    onHistoryEdited: function(value) { dialog.vrrHistorySeconds = Math.max(10, Math.min(300, value)) }
                    onToleranceEdited: function(value) { dialog.vrrToleranceUs = Math.round(Math.max(250, Math.min(2000, value)) / 250) * 250 }
                }

                CheckBox {
                    Layout.fillWidth: true
                    visible: dialog.effectiveVrr
                    text: qsTr("Reduce judder")
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    checked: dialog.smoothVrrFrameTiming
                    onToggled: dialog.smoothVrrFrameTiming = checked
                }

                CheckBox {
                    Layout.fillWidth: true
                    visible: dialog.effectiveVrr
                    text: qsTr("Absorb microstutter (adds latency)")
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    enabled: dialog.smoothVrrFrameTiming
                    checked: dialog.absorbVrrMicrostutter
                    onToggled: dialog.absorbVrrMicrostutter = checked
                }
            }

            HostSettingRow {
                Layout.fillWidth: true
                title: qsTr("Video codec")
                globalText: dialog.codecNames[StreamingPreferences.videoCodecConfig] || qsTr("Automatic (Recommended)")
                overridden: dialog.codecOn
                onOverrideToggled: function(on) { dialog.codecOn = on }

                AutoResizingComboBox {
                    id: codecComboBox
                    Layout.fillWidth: true
                    textRole: "text"
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    model: ListModel {
                        id: codecModel
                    }

                    function select() {
                        if (codecModel.count === 0) {
                            var codecs = [StreamingPreferences.VCC_AUTO, StreamingPreferences.VCC_FORCE_H264,
                                          StreamingPreferences.VCC_FORCE_HEVC, StreamingPreferences.VCC_FORCE_AV1]
                            if (SystemProperties.hasPyroWave) {
                                codecs.push(StreamingPreferences.VCC_FORCE_PYROWAVE)
                            }
                            for (var j = 0; j < codecs.length; j++) {
                                codecModel.append({ "text": dialog.codecNames[codecs[j]], "val": codecs[j] })
                            }
                        }

                        currentIndex = 0
                        for (var i = 0; i < codecModel.count; i++) {
                            if (codecModel.get(i).val === dialog.codecValue) {
                                currentIndex = i
                                break
                            }
                        }
                        recalculateWidth()
                    }

                    onActivated: dialog.codecValue = codecModel.get(currentIndex).val
                }
            }

            HostSettingRow {
                Layout.fillWidth: true
                title: qsTr("YUV 4:4:4")
                globalText: dialog.onOff(StreamingPreferences.enableYUV444)
                overridden: dialog.yuv444On
                onOverrideToggled: function(on) { dialog.yuv444On = on }

                CheckBox {
                    Layout.fillWidth: true
                    text: qsTr("Enable YUV 4:4:4")
                    hoverEnabled: !SystemProperties.hoverEffectsDisabled
                    checked: dialog.yuv444Enabled
                    onToggled: dialog.yuv444Enabled = checked
                }
            }
        }
    }
}
