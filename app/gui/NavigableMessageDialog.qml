import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2

import SystemProperties 1.0
import TvTheme 1.0

NavigableDialog {
    id: dialog

    property string text
    property bool showSpinner: false
    property url imageSrc: (standardButtons & Dialog.Yes) ?
                               "qrc:/res/baseline-help_outline-24px.svg" :
                               "qrc:/res/baseline-error_outline-24px.svg"

    property string helpText
    property string helpUrl : "https://github.com/moonlight-stream/moonlight-docs/wiki/Troubleshooting"
    property string helpTextSeparator : " "

    // TV mode: a headline above the text, like "Quit Moonlight?". When there
    // is one, the text below it is the secondary explanation.
    property string headline: ""

    readonly property string fullText: text + ((helpText && (standardButtons & Dialog.Help)) ? (helpTextSeparator + helpText) : "")

    onTvHelpRequested: {
        Qt.openUrlExternally(helpUrl)
        close()
    }

    onOpened: {
        // Force keyboard focus on a button so keyboard navigation works
        if (SystemProperties.tvMode) {
            focusFirstButton()
        }
        else if (dialogButtonBox.count > 0) {
            dialogButtonBox.itemAt(dialogButtonBox.count - 1).forceActiveFocus(Qt.TabFocus)
        }
    }

    // Only one of the layouts below is shown, so the dialog is sized to it
    contentWidth: SystemProperties.tvMode ? tvLayout.implicitWidth : desktopLayout.implicitWidth
    contentHeight: SystemProperties.tvMode ? tvLayout.implicitHeight : desktopLayout.implicitHeight

    RowLayout {
        id: desktopLayout
        visible: !SystemProperties.tvMode
        spacing: 10

        BusyIndicator {
            visible: dialog.showSpinner
            running: visible
        }

        Image {
            source: dialog.imageSrc
            sourceSize {
                // The icon should be square so use the height as the width too
                width: 50
                height: 50
            }
            visible: !dialog.showSpinner
        }

        Label {
            text: dialog.fullText
            wrapMode: Text.Wrap
            elide: Label.ElideRight

            // Cap the width so the dialog doesn't grow horizontally forever. This
            // will cause word wrap to kick in.
            Layout.maximumWidth: 400
            Layout.maximumHeight: 400
        }
    }

    // TV mode: an icon, then the headline and text. The buttons are stacked
    // under them by NavigableDialog.
    ColumnLayout {
        id: tvLayout
        visible: SystemProperties.tvMode
        spacing: TvTheme.dialogSpacing

        Rectangle {
            Layout.preferredWidth: TvTheme.dialogIconSize
            Layout.preferredHeight: TvTheme.dialogIconSize
            radius: width / 2
            color: TvTheme.surface

            BusyIndicator {
                anchors.centerIn: parent
                width: TvTheme.dialogIconSize * 0.7
                height: TvTheme.dialogIconSize * 0.7
                visible: dialog.showSpinner
                running: visible
            }

            Image {
                anchors.centerIn: parent
                source: dialog.imageSrc
                sourceSize.width: Math.round(TvTheme.dialogIconSize * 0.57)
                sourceSize.height: Math.round(TvTheme.dialogIconSize * 0.57)
                visible: !dialog.showSpinner
            }
        }

        Label {
            Layout.preferredWidth: TvTheme.dialogContentWidth
            Layout.topMargin: 7
            visible: dialog.headline !== ""
            text: dialog.headline
            color: TvTheme.textPrimary
            font.pixelSize: TvTheme.dialogTitleFont
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
        }

        Label {
            Layout.preferredWidth: TvTheme.dialogContentWidth
            Layout.maximumHeight: 450
            visible: dialog.fullText !== ""
            text: dialog.fullText
            // Under a headline, the text explains it
            color: dialog.headline !== "" ? TvTheme.textSecondary : TvTheme.textPrimary
            font.pixelSize: TvTheme.dialogBodyFont
            lineHeight: 1.3
            wrapMode: Text.Wrap
            elide: Label.ElideRight
        }
    }

    footer: DialogButtonBox {
        id: dialogButtonBox
        standardButtons: dialog.standardButtons

        // TV mode replaces this with NavigableDialog's stacked buttons

        delegate: Button {
            flat: true

            Keys.onReturnPressed: clicked()
            Keys.onEnterPressed: clicked()
            Keys.onRightPressed: nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
            Keys.onLeftPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
        }

        onHelpRequested: {
            Qt.openUrlExternally(helpUrl)
            close()
        }
    }
}
