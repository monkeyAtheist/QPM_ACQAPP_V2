import QtQuick

// Small "window with a side panel" icon, drawn rather than taken from a font.
Item {
    id: root
    property bool rightSide: false
    property bool active: true
    property color color: Theme.text

    implicitWidth: 18
    implicitHeight: 14

    Rectangle {
        anchors.fill: parent
        radius: 2
        color: "transparent"
        border.color: root.color
        border.width: 1.5
    }
    Rectangle {
        width: 6
        height: parent.height - 3
        y: 1.5
        x: root.rightSide ? parent.width - width - 1.5 : 1.5
        color: root.color
        opacity: root.active ? 0.9 : 0.25
    }
}
