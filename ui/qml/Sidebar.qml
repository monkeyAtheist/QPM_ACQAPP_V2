import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Module navigation: one entry per module, the same layout behind each one (as in QTsignalApp V2). The
// analysis of a signal (spectra, curves, measurements) is part of "Signaux".
Rectangle {
    id: root
    property int current: 0
    signal moduleSelected(int index)

    implicitWidth: 84
    color: Theme.panel

    readonly property var modules: [
        { icon: "signals", label: qsTr("Signaux"), ready: true, tip: "" },
        { icon: "acquire", label: qsTr("Acqui-\nsition"), ready: true, tip: "" },
        { icon: "generator", label: qsTr("Généra-\nteur"), ready: true, tip: "" },
        { icon: "filter", label: qsTr("Filtres"), ready: true, tip: "" },
        { icon: "modulation", label: qsTr("Modula-\ntions"), ready: true, tip: "" }
    ]

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 10
        spacing: 4

        Repeater {
            model: root.modules
            delegate: ItemDelegate {
                id: entry
                required property var modelData
                required property int index
                readonly property bool active: index === root.current
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 72
                Layout.preferredHeight: 72
                hoverEnabled: true
                onClicked: if (modelData.ready) root.moduleSelected(index)
                ToolTip.visible: hovered && modelData.tip.length > 0
                ToolTip.delay: 300
                ToolTip.text: modelData.tip

                background: Rectangle {
                    radius: Theme.radius
                    color: entry.active ? Qt.alpha(Theme.accent, 0.16)
                                        : entry.hovered && entry.modelData.ready ? Qt.alpha(Theme.text, 0.06) : "transparent"
                    border.color: entry.active ? Qt.alpha(Theme.accent, 0.5) : "transparent"
                }
                contentItem: ColumnLayout {
                    spacing: 3
                    opacity: entry.modelData.ready ? 1.0 : 0.38
                    RibbonIcon {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 24
                        Layout.preferredHeight: 24
                        name: entry.modelData.icon
                        tint: entry.active ? Theme.accent : Theme.text
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        horizontalAlignment: Text.AlignHCenter
                        text: entry.modelData.label
                        font.pixelSize: 10
                        lineHeight: 0.9
                        color: entry.active ? Theme.accent : Theme.textMuted
                    }
                }
            }
        }
        Item { Layout.fillHeight: true }
    }
}
