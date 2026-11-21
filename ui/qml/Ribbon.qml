import QtQuick
import QtQuick.Controls

// Tool header (the "ruban") of a module, across the window part the shell gives (as in QTsignalApp V2):
// the views of the module as tabs, quick tools at the right of the tabs, and under them the tools in
// named groups (RibbonGroup). In a window narrower than the groups, they scroll sideways (wheel, drag,
// or the bar under them). A module view places it with `parent: ribbonHost` and shows it with itself.
Rectangle {
    id: ribbon
    property var tabs: []
    property int currentTab: 0
    signal tabClicked(int index)
    default property alias content: toolsRow.data
    // Compact items at the right end of the tab row (a state, a short text).
    property alias corner: cornerRow.data

    anchors.fill: parent
    z: 10
    color: Theme.panel

    Row {
        x: 8
        height: 30
        spacing: 2
        Repeater {
            model: ribbon.tabs
            delegate: Item {
                id: tabItem
                required property string modelData
                required property int index
                readonly property bool current: index === ribbon.currentTab
                width: tabLabel.implicitWidth + 28
                height: 30
                Rectangle {
                    anchors.fill: parent
                    anchors.topMargin: 3
                    anchors.bottomMargin: 2
                    radius: 5
                    color: tabArea.containsMouse && !tabItem.current ? Qt.alpha(Theme.text, 0.08) : "transparent"
                }
                Label {
                    id: tabLabel
                    anchors.centerIn: parent
                    text: tabItem.modelData
                    font.pixelSize: Theme.bodyText
                    color: tabItem.current ? Theme.accent : Theme.textMuted
                }
                Rectangle {
                    x: 8
                    y: parent.height - 2
                    width: parent.width - 16
                    height: 2
                    color: Theme.accent
                    visible: tabItem.current
                }
                MouseArea {
                    id: tabArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: ribbon.tabClicked(tabItem.index)
                }
            }
        }
    }
    Row {
        id: cornerRow
        anchors.right: parent.right
        anchors.rightMargin: 10
        y: 3
        height: 24
        spacing: 8
    }
    Rectangle {
        y: 30
        width: parent.width
        height: 1
        color: Theme.border
    }

    Item {
        y: 31
        width: parent.width
        height: 78
        WheelHandler {
            enabled: toolsFlick.contentWidth > toolsFlick.width
            onWheel: (event) => {
                const span = toolsFlick.contentWidth - toolsFlick.width
                toolsFlick.contentX = Math.max(0, Math.min(span, toolsFlick.contentX - event.angleDelta.y - event.angleDelta.x))
            }
        }
        Flickable {
            id: toolsFlick
            anchors.fill: parent
            contentWidth: toolsRow.width
            contentHeight: height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.HorizontalFlick
            interactive: contentWidth > width
            ScrollBar.horizontal: ScrollBar {
                height: 6
                policy: toolsFlick.contentWidth > toolsFlick.width ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
            }
            Row {
                id: toolsRow
                leftPadding: 4
            }
        }
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.border
    }
}
