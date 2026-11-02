import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.2

import StreamingPreferences 1.0
import SystemProperties 1.0
import TvTheme 1.0

// A * in the corner of a PC's or app's card when it has its own stream
// settings. A PC's card also shows it when any of its apps has them.
Rectangle {
    id: badge

    property string uuid: ""
    // The app, or 0 for the PC itself
    property int appId: 0

    // Checked again whenever any PC's or app's settings are saved
    visible: StreamingPreferences.overridesRevision >= 0 && uuid !== "" &&
             StreamingPreferences.hasOverrides(uuid, appId)

    width: SystemProperties.tvMode ? 40 : 30
    height: width
    radius: width / 2
    color: SystemProperties.tvMode ? TvTheme.accent : Material.accent

    Accessible.role: Accessible.StaticText
    Accessible.name: appId !== 0 ? qsTr("This app has its own stream settings")
                                 : qsTr("This PC or one of its apps has its own stream settings")

    Label {
        anchors.horizontalCenter: parent.horizontalCenter
        // The asterisk sits high in its line, so center its shape instead
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: parent.height * 0.1
        text: "*"
        color: SystemProperties.tvMode ? TvTheme.accentText : "white"
        font.pixelSize: parent.height * 1.2
        font.bold: true
    }
}
