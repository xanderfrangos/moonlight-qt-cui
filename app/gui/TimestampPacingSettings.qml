import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

GroupBox {
    id: root
    title: qsTr("Timestamp pacing")
    property int smoothing: 1
    property int targetPerMille: 990
    property int minBufferMs: 2
    property int maxBufferMs: 16
    property int vsyncMarginUs: 2000
    property bool vsyncEnabled: true
    signal smoothingEdited(int value)
    signal targetEdited(int value)
    signal minBufferEdited(int value)
    signal maxBufferEdited(int value)
    signal vsyncMarginEdited(int value)

    ColumnLayout {
        width: parent.width
        spacing: 8

        Label { text: qsTr("Smoothing") }
        AutoResizingComboBox {
            objectName: "timestampSmoothing"
            Layout.fillWidth: true
            model: [qsTr("Off (exact host timestamps)"), qsTr("Light (recommended)"), qsTr("Standard")]
            currentIndex: root.smoothing
            onActivated: root.smoothingEdited(currentIndex)
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Host timestamps can be a few milliseconds uneven even when the game is steady. Smoothing follows the game's cadence instead, and still follows real changes in frame rate. Standard is slightly steadier for games locked to one frame rate, but slower to follow a frame rate that changes.")
        }

        Label { text: qsTr("Frames ready on time (%)") }
        SpinBox {
            id: target
            objectName: "timestampTarget"
            Layout.fillWidth: true
            from: 900; to: 999; stepSize: 5
            value: root.targetPerMille
            editable: true
            Component.onCompleted: contentItem.Keys.forwardTo = [target]
            validator: DoubleValidator { bottom: 90; top: 99.9; decimals: 1; locale: target.locale.name }
            textFromValue: function(value, locale) { return Number(value / 10).toLocaleString(locale, 'f', 1) }
            valueFromText: function(text, locale) { return Math.round(Number.fromLocaleString(locale, text) * 10) }
            onValueModified: root.targetEdited(value)
            Keys.onLeftPressed: root.targetEdited(Math.max(from, value - stepSize))
            Keys.onRightPressed: root.targetEdited(Math.min(to, value + stepSize))
            Accessible.name: qsTr("Frames ready on time in percent")
        }

        Label { text: qsTr("Minimum buffer (ms)") }
        SpinBox {
            id: minBuffer
            objectName: "timestampMinBuffer"
            Layout.fillWidth: true
            from: 0; to: 20; stepSize: 1
            value: root.minBufferMs
            editable: true
            Component.onCompleted: contentItem.Keys.forwardTo = [minBuffer]
            onValueModified: root.minBufferEdited(value)
            Keys.onLeftPressed: root.minBufferEdited(Math.max(from, value - stepSize))
            Keys.onRightPressed: root.minBufferEdited(Math.min(to, value + stepSize))
            Accessible.name: qsTr("Minimum buffer in milliseconds")
        }

        Label { text: qsTr("Maximum buffer (ms)") }
        SpinBox {
            id: maxBuffer
            objectName: "timestampMaxBuffer"
            Layout.fillWidth: true
            from: 1; to: 50; stepSize: 1
            value: root.maxBufferMs
            editable: true
            Component.onCompleted: contentItem.Keys.forwardTo = [maxBuffer]
            onValueModified: root.maxBufferEdited(value)
            Keys.onLeftPressed: root.maxBufferEdited(Math.max(from, value - stepSize))
            Keys.onRightPressed: root.maxBufferEdited(Math.min(to, value + stepSize))
            Accessible.name: qsTr("Maximum buffer in milliseconds")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("The buffer delays frames just enough that the chosen share is ready by the time the host's cadence says to show them. A higher share or a larger minimum is smoother but adds delay. The buffer is also limited to three waiting frames.")
        }

        Label {
            text: qsTr("Submit before V-blank (ms)")
            visible: root.vsyncEnabled
        }
        SpinBox {
            id: margin
            objectName: "timestampVsyncMargin"
            Layout.fillWidth: true
            visible: root.vsyncEnabled
            from: 250; to: 8000; stepSize: 250
            value: root.vsyncMarginUs
            editable: false
            textFromValue: function(value, locale) { return Number(value / 1000).toLocaleString(locale, 'f', 2) }
            onValueModified: root.vsyncMarginEdited(value)
            Keys.onLeftPressed: root.vsyncMarginEdited(Math.max(from, value - stepSize))
            Keys.onRightPressed: root.vsyncMarginEdited(Math.min(to, value + stepSize))
            Accessible.name: qsTr("Submit before V-blank in milliseconds")
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: root.vsyncEnabled
            text: qsTr("With V-Sync, each frame is shown at the first refresh after its time and handed to the renderer this long before that refresh, plus its measured rendering time. Increase it if frames are repeated or skipped while the stream is steady.") + "\n\n" +
                  qsTr("On Steam Deck and other Gamescope sessions, this follows Gamescope's own settings: frames are placed on the refresh grid at a fixed refresh rate, and shown as they arrive when Gamescope's VRR or Allow Tearing is on. The margin also grows by itself if Gamescope reports frames shown a refresh late. Steam's frame limit forces queued presentation, so turn it off.")
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Reconnect the stream after changing these settings.")
        }
    }
}
