import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Example program for the board, written for the format set in the acquisition settings (text, raw
// samples or frames with their synchronisation, counter and checksum): an Arduino sketch or portable C
// for STM32, ESP32, PIC... Copy it, replace the reading of the samples by the board's own.
Dialog {
    id: root
    required property var acquisition
    property int target: 0 // 0 Arduino, 1 portable C
    readonly property string code: visible ? acquisition.firmwareExample(target === 0 ? "arduino" : "c") : ""

    title: qsTr("Programme d'exemple pour la carte")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent ? parent.width - 80 : 900, 900)
    height: Math.min(parent ? parent.height - 80 : 700, 720)
    footer: DialogButtonBox {
        Button {
            text: qsTr("Fermer")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Il envoie exactement le format réglé dans le panneau Acquisition (synchronisation, compteur, somme de contrôle compris) : l'application le décode sans autre réglage. Remplacez la lecture des échantillons par celle de votre carte.")
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
        }
        RowLayout {
            Layout.fillWidth: true
            TabBar {
                id: tabs
                currentIndex: root.target
                onCurrentIndexChanged: root.target = currentIndex
                TabButton { text: qsTr("Arduino") }
                TabButton { text: qsTr("C portable (STM32, ESP32, PIC…)") }
            }
            Item { Layout.fillWidth: true }
            Label {
                id: copied
                visible: false
                text: qsTr("Copié")
                color: Theme.success
                font.pixelSize: Theme.smallText
            }
            Button {
                text: qsTr("Copier")
                onClicked: {
                    codeArea.selectAll()
                    codeArea.copy()
                    codeArea.deselect()
                    copied.visible = true
                    copiedTimer.restart()
                }
            }
            Timer {
                id: copiedTimer
                interval: 2000
                onTriggered: copied.visible = false
            }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            TextArea {
                id: codeArea
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.NoWrap
                font.family: Theme.monospace
                font.pixelSize: 12
                text: root.code
                background: Rectangle {
                    color: Theme.canvas
                    border.color: Theme.border
                    radius: 4
                }
            }
        }
    }
}
