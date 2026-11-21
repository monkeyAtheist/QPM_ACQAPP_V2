import QtQuick
import QtQuick.Controls

// Thin bar under a plot that is zoomed: where the view is in all there is to see. Drag the thumb to slide
// the view, click beside it to move by a page. `view`: a SignalPlot or SpectrumPlot (viewStart, viewSize,
// zoomed, scrollTo, scrollBy). Takes no room when the plot shows everything.
Item {
    id: root
    property var view: null
    readonly property bool active: view !== null && view.hasData && view.zoomed

    implicitHeight: active ? 12 : 0
    visible: active

    Rectangle {
        id: track
        anchors.fill: parent
        anchors.topMargin: 3
        anchors.bottomMargin: 3
        radius: height / 2
        color: Qt.alpha(Theme.text, area.containsMouse || area.pressed ? 0.10 : 0.06)

        Rectangle {
            id: thumb
            readonly property real span: Math.max(1, track.width - width)
            readonly property real fraction: root.active && root.view.viewSize < 1
                                             ? root.view.viewStart / (1 - root.view.viewSize) : 0
            width: Math.max(28, track.width * (root.active ? root.view.viewSize : 1))
            height: parent.height
            radius: height / 2
            x: Math.max(0, Math.min(span, fraction * span))
            color: area.pressed && area.dragging ? Theme.accent
                   : Qt.alpha(Theme.text, area.containsMouse ? 0.45 : 0.30)
        }

        MouseArea {
            id: area
            property bool dragging: false
            property real grab: 0
            anchors.fill: parent
            anchors.topMargin: -3
            anchors.bottomMargin: -3
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onPressed: (mouse) => {
                if (mouse.x >= thumb.x && mouse.x <= thumb.x + thumb.width) {
                    dragging = true
                    grab = mouse.x - thumb.x
                } else {
                    // A page towards the click.
                    root.view.scrollBy(mouse.x < thumb.x ? -0.9 : 0.9)
                }
            }
            onPositionChanged: (mouse) => {
                if (!dragging || !root.active)
                    return
                const f = Math.max(0, Math.min(1, (mouse.x - grab) / thumb.span))
                root.view.scrollTo(f * (1 - root.view.viewSize))
            }
            onReleased: dragging = false
            onCanceled: dragging = false
        }
    }
    ToolTip.visible: area.containsMouse && !area.pressed
    ToolTip.delay: 800
    ToolTip.text: qsTr("Glissez pour vous déplacer ; aussi : glisser sur le tracé, Maj + molette, flèches ← →")
}
