import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.2
import QtQuick.Layouts 1.3

import SystemProperties 1.0
import TvTheme 1.0

// One setting in a PC's settings dialog: its name, the global setting, a
// switch that overrides it for this PC, and the controls for the override,
// which are shown while it is on. TV mode draws it as a card.
Rectangle {
    id: row

    property string title
    // The global setting, in words
    property string globalText
    property bool overridden: false
    property alias overrideSwitch: overrideSwitch

    // The controls for the override
    default property alias editors: editorColumn.data

    signal overrideToggled(bool on)

    readonly property int padding: SystemProperties.tvMode ? TvTheme.spacingMediumLarge : 0

    implicitHeight: content.implicitHeight + 2 * padding + (divider.visible ? TvTheme.spacingMedium : 0)
    color: SystemProperties.tvMode ? TvTheme.surface : "transparent"
    radius: TvTheme.cardRadius

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: row.padding
        spacing: SystemProperties.tvMode ? TvTheme.spacingMedium : 8

        RowLayout {
            Layout.fillWidth: true
            spacing: SystemProperties.tvMode ? TvTheme.spacingMedium : 10

            ColumnLayout {
                Layout.fillWidth: true
                spacing: SystemProperties.tvMode ? 4 : 2

                Label {
                    Layout.fillWidth: true
                    text: row.title
                    color: SystemProperties.tvMode ? TvTheme.textPrimary : Material.foreground
                    font.weight: SystemProperties.tvMode ? Font.DemiBold : Font.Bold
                    wrapMode: Text.Wrap
                }

                Label {
                    id: globalLabel
                    Layout.fillWidth: true
                    text: row.overridden ? qsTr("Global setting: %1").arg(row.globalText)
                                         : qsTr("Uses the global setting: %1").arg(row.globalText)
                    color: SystemProperties.tvMode ? TvTheme.textSecondary : Material.foreground
                    opacity: SystemProperties.tvMode ? 1.0 : 0.7
                    font.pointSize: 9
                    wrapMode: Text.Wrap

                    // A size down from the dialog's text
                    Binding {
                        target: globalLabel
                        property: "font.pixelSize"
                        value: TvTheme.dialogBodyFont - 3
                        when: SystemProperties.tvMode
                    }
                }
            }

            Switch {
                id: overrideSwitch
                text: qsTr("Override")
                checked: row.overridden
                hoverEnabled: !SystemProperties.hoverEffectsDisabled
                onToggled: row.overrideToggled(checked)

                // What the gamepad's select button does here
                readonly property string tvHintSelect: checked ? qsTr("Use global") : qsTr("Override")

                Accessible.name: qsTr("Override %1 for this PC").arg(row.title)
            }
        }

        ColumnLayout {
            id: editorColumn
            Layout.fillWidth: true
            visible: row.overridden
            spacing: SystemProperties.tvMode ? TvTheme.spacingSmall : 4
        }
    }

    // Desktop mode separates the settings with a line
    Rectangle {
        id: divider
        visible: !SystemProperties.tvMode
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Material.foreground
        opacity: 0.12
    }
}
