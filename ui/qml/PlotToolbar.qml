import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Strip over the top right of a plot: what is shown ("0 à 1,2 s (sur 10 s)"), one chip per channel that
// hides or shows it, and the mouse gestures of the plot.
Rectangle {
    id: root
    required property var view

    implicitWidth: row.implicitWidth + 12
    implicitHeight: 28
    radius: 6
    color: Qt.alpha(Theme.panel, 0.92)
    border.color: Theme.border
    visible: view.hasData

    function toggle(channel) {
        const hidden = view.hiddenChannels.slice()
        const at = hidden.indexOf(channel)
        if (at >= 0)
            hidden.splice(at, 1)
        else
            hidden.push(channel)
        view.hiddenChannels = hidden
    }

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 6
        Label {
            text: root.view.viewText
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
            Layout.rightMargin: 4
        }
        Repeater {
            model: root.view.channelCount > 1 ? root.view.channelNames : []
            delegate: Rectangle {
                id: chip
                required property string modelData
                required property int index
                readonly property bool shown: root.view.hiddenChannels.indexOf(index) < 0
                implicitWidth: chipRow.implicitWidth + 12
                implicitHeight: 20
                radius: 10
                color: chipArea.containsMouse ? Qt.alpha(Theme.text, 0.10) : "transparent"
                border.color: shown ? Qt.alpha(Theme.channelColors[index % Theme.channelColors.length], 0.7) : Theme.border
                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: 5
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 8
                        height: 8
                        radius: 4
                        color: chip.shown ? Theme.channelColors[chip.index % Theme.channelColors.length] : "transparent"
                        border.color: Theme.channelColors[chip.index % Theme.channelColors.length]
                    }
                    Label {
                        text: chip.modelData
                        font.pixelSize: Theme.smallText
                        color: chip.shown ? Theme.text : Theme.textMuted
                    }
                }
                MouseArea {
                    id: chipArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.toggle(chip.index)
                }
                ToolTip.visible: chipArea.containsMouse
                ToolTip.delay: 600
                ToolTip.text: chip.shown ? qsTr("Masquer cette voie") : qsTr("Afficher cette voie")
            }
        }
        Label {
            id: help
            text: "?"
            font.pixelSize: Theme.bodyText
            font.bold: true
            color: helpArea.containsMouse ? Theme.text : Theme.textMuted
            Layout.leftMargin: 2
            Layout.rightMargin: 2
            MouseArea {
                id: helpArea
                anchors.fill: parent
                anchors.margins: -4
                hoverEnabled: true
            }
            ToolTip.visible: helpArea.containsMouse
            ToolTip.text: qsTr("Molette : zoom du temps autour du pointeur (en direct : largeur de la fenêtre)\n"
                               + "Ctrl + molette : zoom des valeurs de la voie sous le pointeur\n"
                               + "Maj + molette ou glisser : déplacer dans le temps\n"
                               + "Clic droit glissé : zoom sur l'intervalle choisi\n"
                               + "Double clic : tout voir")
        }
    }
}
