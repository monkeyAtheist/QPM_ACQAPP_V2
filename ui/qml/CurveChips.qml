import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The curves of one graph of the module "Signaux", as chips over its top right, in their colours: a click
// hides or shows a curve, its cross takes it away. At the end, while they are computed, a turning wheel,
// and a "?" that tells on hovering how they are computed (rather than a strip over the graph).
Rectangle {
    id: root
    required property var curves // the CurvesController
    property string graph: "time" // or "frequency", "spectrogram"
    property string title: ""
    property string info: "" // lines shown by "?" (none: no "?")
    property bool busy: false
    readonly property var items: curves.curves.filter((c) => c.graph === root.graph)

    visible: items.length > 0
    implicitWidth: row.implicitWidth + 12
    implicitHeight: 28
    radius: 6
    color: Qt.alpha(Theme.panel, 0.92)
    border.color: Theme.border

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 6
        Label {
            visible: root.title.length > 0
            text: root.title
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
            Layout.rightMargin: 2
        }
        Repeater {
            model: root.items
            delegate: Rectangle {
                id: chip
                required property var modelData
                readonly property color tint: Theme.channelColors[modelData.colorIndex % Theme.channelColors.length]
                implicitWidth: chipRow.implicitWidth + 12
                implicitHeight: 20
                radius: 10
                color: chipArea.containsMouse ? Qt.alpha(Theme.text, 0.10) : "transparent"
                border.color: modelData.visible ? Qt.alpha(tint, 0.7) : Theme.border
                opacity: modelData.missing ? 0.5 : 1

                // Under the cross: a click on the rest of the chip hides or shows the curve.
                MouseArea {
                    id: chipArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.curves.setVisible(chip.modelData.id, !chip.modelData.visible)
                }
                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: 5
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 8
                        height: 8
                        radius: 4
                        color: chip.modelData.visible ? chip.tint : "transparent"
                        border.color: chip.tint
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: chip.modelData.name + (chip.modelData.part.length > 0 ? " · " + chip.modelData.part : "")
                              + (chip.modelData.ready ? "" : " …")
                        font.pixelSize: Theme.smallText
                        color: chip.modelData.visible ? Theme.text : Theme.textMuted
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "×"
                        font.pixelSize: Theme.bodyText
                        color: crossArea.containsMouse ? Theme.text : Theme.textMuted
                        MouseArea {
                            id: crossArea
                            anchors.fill: parent
                            anchors.margins: -3
                            hoverEnabled: true
                            onClicked: root.curves.remove(chip.modelData.id)
                        }
                    }
                }
                ToolTip.visible: chipArea.containsMouse && !crossArea.containsMouse
                ToolTip.delay: 600
                ToolTip.text: (chip.modelData.missing ? qsTr("Cette voie n'existe pas dans le signal choisi. ") : "")
                              + (chip.modelData.visible ? qsTr("Masquer cette courbe (la croix la retire)")
                                                        : qsTr("Afficher cette courbe (la croix la retire)"))
            }
        }
        BusyIndicator {
            visible: root.busy
            running: visible
            Layout.preferredWidth: 16
            Layout.preferredHeight: 16
        }
        Label {
            visible: root.info.length > 0
            text: "?"
            font.pixelSize: Theme.bodyText
            font.bold: true
            color: infoArea.containsMouse ? Theme.text : Theme.textMuted
            Layout.leftMargin: 2
            Layout.rightMargin: 2
            MouseArea {
                id: infoArea
                anchors.fill: parent
                anchors.margins: -4
                hoverEnabled: true
            }
            ToolTip.visible: infoArea.containsMouse
            ToolTip.text: root.info
        }
    }
}
