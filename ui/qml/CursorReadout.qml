import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What cursors A and B of a plot read (`view`: a SignalPlot or SpectrumPlot, its cursorInfo): where they are, the
// gap between them, and for each channel the values under A and B and what lies between them. Floats over
// the plot at its bottom left; it can be dragged out of the way.
Rectangle {
    id: root
    property var view: null // the SignalPlot or SpectrumPlot
    property string deltaName: "Δt"
    property string inverseName: "1/Δt"
    // The cross hides the cursors of `view`; a view whose cursors are shared with other plots sets this to
    // false and handles closeRequested (all of them hidden, as with the button "Curseurs").
    property bool closes: true
    signal closeRequested()
    readonly property var info: view ? view.cursorInfo : ({})
    readonly property var rows: info.rows ?? []

    visible: view !== null && view.cursorsVisible && info.rows !== undefined
    x: 70
    y: parent ? parent.height - height - 48 : 0
    width: Math.min(content.implicitWidth + 18, parent ? parent.width - 90 : 640)
    height: content.implicitHeight + 12
    radius: 6
    color: Qt.alpha(Theme.panel, 0.94)
    border.color: Theme.border

    component Tag: Rectangle {
        property alias text: tagLabel.text
        implicitWidth: tagLabel.implicitWidth + 10
        implicitHeight: tagLabel.implicitHeight + 2
        radius: 3
        Label {
            id: tagLabel
            anchors.centerIn: parent
            color: Theme.window
            font.pixelSize: Theme.smallText
            font.bold: true
        }
    }

    DragHandler {
        target: root
        cursorShape: Qt.SizeAllCursor
    }

    ColumnLayout {
        id: content
        x: 9
        y: 6
        width: root.width - 18
        spacing: 3

        RowLayout {
            spacing: 10
            Tag { text: "A"; color: Theme.accent }
            Label {
                text: root.info.a ?? ""
                color: Theme.text
                font.pixelSize: Theme.smallText
                font.family: Theme.monospace
            }
            Tag { text: "B"; color: "#E57373" }
            Label {
                text: root.info.b ?? ""
                color: Theme.text
                font.pixelSize: Theme.smallText
                font.family: Theme.monospace
            }
            Label {
                text: "%1 = %2".arg(root.deltaName).arg(root.info.delta ?? "")
                color: Theme.text
                font.pixelSize: Theme.smallText
                font.family: Theme.monospace
            }
            Label {
                visible: (root.info.inverse ?? "").length > 0
                text: "%1 = %2".arg(root.inverseName).arg(root.info.inverse ?? "")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
                font.family: Theme.monospace
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                text: "×"
                implicitWidth: 22
                implicitHeight: 22
                padding: 0
                font.pixelSize: 14
                onClicked: {
                    root.closeRequested()
                    if (root.closes)
                        root.view.cursorsVisible = false
                }
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: qsTr("Retirer les curseurs")
            }
        }
        Repeater {
            model: root.rows
            delegate: ColumnLayout {
                id: channelRow
                required property var modelData
                Layout.fillWidth: true
                spacing: 0
                RowLayout {
                    spacing: 6
                    Rectangle {
                        width: 10
                        height: 4
                        radius: 1.5
                        color: Theme.channelColors[channelRow.modelData.channel % Theme.channelColors.length]
                    }
                    Label {
                        text: channelRow.modelData.name
                        color: Theme.text
                        font.pixelSize: Theme.smallText
                        font.bold: true
                    }
                    Label {
                        text: qsTr("A %1 · B %2 · Δ %3").arg(channelRow.modelData.a).arg(channelRow.modelData.b).arg(channelRow.modelData.delta)
                        color: Theme.text
                        font.pixelSize: Theme.smallText
                        font.family: Theme.monospace
                    }
                }
                Label {
                    visible: channelRow.modelData.stats.length > 0
                    Layout.leftMargin: 16
                    text: channelRow.modelData.stats
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText - 1
                    font.family: Theme.monospace
                }
            }
        }
        Label {
            text: qsTr("Glissez les lignes A et B ; Ctrl + clic place A, Maj + clic place B.")
            color: Theme.textMuted
            font.pixelSize: Theme.smallText - 1
            font.italic: true
        }
    }
}
