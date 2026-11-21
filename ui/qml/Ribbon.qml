import QtQuick
import QtQuick.Controls

// Tool header (the "ruban") of a module, across the window part the shell gives (as in QTsignalApp V2):
// the views of the module as tabs, quick tools at the right of the tabs, and under them the tools in
// named groups (RibbonGroup). In a window narrower than the groups, they scroll sideways (wheel, drag,
// or the bar under them). A module view places it with `parent: ribbonHost` and shows it with itself.
// The tabs are pills that stand out (the current one filled), each with its icon and its tip: with
// several tabs, the tools of the others are easy to miss otherwise.
Rectangle {
    id: ribbon
    property var tabs: []
    property var tabIcons: [] // glyph of RibbonIcon for each tab ("" or none: no icon)
    property var tabTips: []  // what each tab holds
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
        y: 3
        height: 24
        spacing: 6
        Repeater {
            model: ribbon.tabs
            delegate: Rectangle {
                id: tabItem
                required property string modelData
                required property int index
                readonly property bool current: index === ribbon.currentTab
                readonly property bool switchable: ribbon.tabs.length > 1
                readonly property string glyph: ribbon.tabIcons[index] ?? ""
                readonly property string tip: ribbon.tabTips[index] ?? ""
                width: tabRow.implicitWidth + 26
                height: 24
                radius: 12
                color: current ? Qt.alpha(Theme.accent, 0.18)
                       : tabArea.containsMouse ? Qt.alpha(Theme.text, 0.10) : Qt.alpha(Theme.text, 0.04)
                border.width: 1
                border.color: current ? Theme.accent : tabArea.containsMouse ? Qt.alpha(Theme.text, 0.35) : Theme.border
                Row {
                    id: tabRow
                    anchors.centerIn: parent
                    spacing: 6
                    RibbonIcon {
                        visible: tabItem.glyph.length > 0
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        name: tabItem.glyph
                        tint: tabItem.current ? Theme.accent : Theme.text
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: tabItem.modelData
                        font.pixelSize: Theme.bodyText
                        font.bold: true // all of them: the pills keep their width from one tab to the other
                        color: tabItem.current ? Theme.accent : Theme.text
                    }
                }
                MouseArea {
                    id: tabArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: tabItem.switchable && !tabItem.current ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: ribbon.tabClicked(tabItem.index)
                }
                ToolTip.visible: tabArea.containsMouse && tabItem.tip.length > 0
                ToolTip.delay: 500
                ToolTip.text: tabItem.tip
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
