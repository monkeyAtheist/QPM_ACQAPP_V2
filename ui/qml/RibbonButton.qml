import QtQuick
import QtQuick.Controls

// Button of the tool header: an icon over its name. 'checked' marks the current tool or page,
// 'dropDown' adds the small arrow of a button that opens a list, 'primary' colours the button that
// starts a calculation. With an 'action', the name, the state and the effect come from it. 'soon' greys a
// tool that is announced and not there yet: it keeps its tip, which says when it comes, and does nothing.
// 'compact' puts the icon beside the name on one low line: the quick tools beside the tabs.
AbstractButton {
    id: control
    property string glyph: ""
    property string tip: ""
    property bool dropDown: false
    property bool primary: false
    property bool soon: false
    property bool compact: false
    readonly property color ink: checked || primary ? Theme.accent : Theme.text

    implicitWidth: compact ? compactRow.implicitWidth + 16 : Math.max(46, labelRow.implicitWidth + 14)
    implicitHeight: compact ? 24 : 54
    hoverEnabled: true
    focusPolicy: Qt.NoFocus
    opacity: enabled && !soon ? 1 : 0.4

    contentItem: Item {
        RibbonIcon {
            visible: !control.compact
            anchors.horizontalCenter: parent.horizontalCenter
            y: 6
            name: control.glyph
            tint: control.ink
        }
        Row {
            id: labelRow
            visible: !control.compact
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 5
            spacing: 3
            Label {
                text: control.text
                color: control.ink
                font.pixelSize: Theme.smallText
            }
            RibbonIcon {
                visible: control.dropDown
                anchors.verticalCenter: parent.verticalCenter
                width: 8
                height: 8
                name: "caret"
                tint: Theme.textMuted
            }
        }
        Row {
            id: compactRow
            visible: control.compact
            anchors.centerIn: parent
            spacing: 5
            RibbonIcon {
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                name: control.glyph
                tint: control.ink
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                color: control.ink
                font.pixelSize: Theme.smallText
            }
            RibbonIcon {
                visible: control.dropDown
                anchors.verticalCenter: parent.verticalCenter
                width: 8
                height: 8
                name: "caret"
                tint: Theme.textMuted
            }
        }
    }
    background: Rectangle {
        radius: control.compact ? 5 : 6
        color: control.checked ? Qt.alpha(Theme.accent, 0.16)
             : control.soon ? "transparent"
             : control.down ? Qt.alpha(Theme.text, 0.16)
             : control.hovered ? Qt.alpha(Theme.text, 0.08) : "transparent"
        border.color: control.checked ? Qt.alpha(Theme.accent, 0.55) : "transparent"
    }

    ToolTip.visible: hovered && tip.length > 0
    ToolTip.delay: 500
    ToolTip.text: tip
}
