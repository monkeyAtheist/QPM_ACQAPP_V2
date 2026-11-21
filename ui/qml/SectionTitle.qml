import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Small upper-case section header used in the side panels.
RowLayout {
    id: root
    property alias text: label.text
    Layout.fillWidth: true
    Layout.topMargin: 6
    spacing: 8

    Label {
        id: label
        font.pixelSize: Theme.smallText
        font.bold: true
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.8
        color: Theme.textMuted
    }
    Rectangle {
        Layout.fillWidth: true
        height: 1
        color: Theme.border
    }
}
