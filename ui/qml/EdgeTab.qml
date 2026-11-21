import QtQuick
import QtQuick.Controls

// Tab on the edge of the view that brings back a folded side panel.
Rectangle {
    id: root
    property bool atLeft: true
    property string toolTip
    signal clicked()

    width: 20
    height: 64
    radius: 8
    color: area.containsMouse ? Qt.alpha(Theme.accent, 0.35) : Qt.alpha(Theme.panel, 0.92)
    border.color: area.containsMouse ? Theme.accent : Theme.border

    // Square off the side glued to the edge.
    Rectangle {
        width: root.radius
        height: root.height
        x: root.atLeft ? 0 : root.width - width
        color: root.color
    }

    Label {
        anchors.centerIn: parent
        text: root.atLeft ? "›" : "‹"
        font.pixelSize: 20
        color: area.containsMouse ? Theme.text : Theme.textMuted
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
    ToolTip.visible: area.containsMouse && toolTip.length > 0
    ToolTip.delay: 400
    ToolTip.text: toolTip
}
