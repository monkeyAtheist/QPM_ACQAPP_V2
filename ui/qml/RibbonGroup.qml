import QtQuick
import QtQuick.Controls

// A named group of the tool header: its controls in a row, its name under them, a hairline after it.
// The controls are 54 px high at most; one that is not a RibbonButton centres itself with
// 'anchors.verticalCenter: parent.verticalCenter'.
Item {
    id: group
    property string title: ""
    default property alias content: row.data

    implicitWidth: Math.max(row.implicitWidth, caption.implicitWidth) + 17
    implicitHeight: 76
    width: implicitWidth
    height: implicitHeight

    Row {
        id: row
        x: Math.round((group.width - 1 - width) / 2)
        y: 3
        height: 54
        spacing: 2
    }
    Label {
        id: caption
        x: Math.round((group.width - 1 - width) / 2)
        y: 58
        text: group.title
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
    }
    Rectangle {
        x: group.width - 1
        y: 8
        width: 1
        height: group.height - 16
        color: Theme.border
    }
}
