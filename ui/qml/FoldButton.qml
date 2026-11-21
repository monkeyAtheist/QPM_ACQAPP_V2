import QtQuick
import QtQuick.Controls

// Chevron button in a panel header that folds the panel away.
ToolButton {
    id: root
    property bool pointsLeft: true
    property string toolTip

    implicitWidth: 30
    implicitHeight: 30
    padding: 0
    contentItem: Label {
        text: root.pointsLeft ? "‹" : "›"
        font.pixelSize: 22
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: root.hovered ? Theme.text : Theme.textMuted
    }
    ToolTip.visible: hovered && toolTip.length > 0
    ToolTip.delay: 400
    ToolTip.text: toolTip
}
