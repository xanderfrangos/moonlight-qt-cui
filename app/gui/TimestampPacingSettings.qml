import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

// The buffer target, its limits and the V-blank margin are not shown: on the
// captures evaluated they either made no difference or traded smoothness for
// latency badly, and the margin adapts by itself where display times are
// reported. They can still be changed in the settings file for testing; see
// docs/timestamp-pacing-findings.md.
GroupBox {
    id: root
    title: qsTr("Timestamp pacing")
    property int smoothing: 1
    property bool vsyncEnabled: true
    signal smoothingEdited(int value)

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

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: root.vsyncEnabled
            text: qsTr("With V-Sync, each frame is shown at the first refresh after its time. Frames are handed to the renderer early enough to make that refresh, and earlier still if they are seen arriving a refresh late.") + "\n\n" +
                  qsTr("On Steam Deck and other Gamescope sessions, this follows Gamescope's own settings: frames are placed on the refresh grid at a fixed refresh rate, and shown as they arrive when Gamescope's VRR or Allow Tearing is on. Steam's frame limit forces queued presentation, so turn it off.")
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Reconnect the stream after changing these settings.")
        }
    }
}
