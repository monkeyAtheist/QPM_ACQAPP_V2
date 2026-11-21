import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Under the live plot: the state of the acquisition and what arrives (frames, measured rate, losses,
// errors, memory, recording), refreshed with each batch of data; the warnings of the source under it.
Rectangle {
    id: root
    required property var acquisition
    readonly property var st: acquisition.statistics
    readonly property string phase: acquisition.state

    implicitHeight: column.implicitHeight + 12
    color: Theme.panel

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.border
    }

    // One figure of the strip: a name above a value, coloured when something is wrong.
    component Figure: ColumnLayout {
        property string name
        property string value
        property color tint: Theme.text
        spacing: 0
        Label {
            text: parent.name
            color: Theme.textMuted
            font.pixelSize: 10
        }
        Label {
            text: parent.value
            color: parent.tint
            font.pixelSize: Theme.bodyText
        }
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 6
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 18

            RowLayout {
                spacing: 8
                Rectangle {
                    id: dot
                    property bool dim: false
                    width: 10
                    height: 10
                    radius: 5
                    color: root.phase === "running" ? Theme.success
                         : root.phase === "connecting" ? Theme.warning
                         : root.phase === "error" ? Theme.error : Theme.textMuted
                    // Blinks while connecting.
                    opacity: root.phase === "connecting" && dim ? 0.3 : 1
                    Timer {
                        interval: 500
                        repeat: true
                        running: root.phase === "connecting"
                        onTriggered: dot.dim = !dot.dim
                    }
                }
                Label {
                    Layout.maximumWidth: 360
                    elide: Text.ElideRight
                    text: root.phase === "error" ? root.acquisition.lastError
                          : root.acquisition.statusText.length > 0 ? root.acquisition.statusText : qsTr("À l'arrêt")
                    color: root.phase === "error" ? Theme.error : Theme.text
                    font.pixelSize: Theme.bodyText
                }
            }
            Figure {
                name: qsTr("Échantillons")
                value: Units.count(root.st.totalFrames ?? 0)
            }
            Figure {
                name: (root.acquisition.settings.declaredSampleRateHz ?? 0) > 0 ? qsTr("Cadence mesurée (annoncée %1)").arg(root.st.rateText ?? "")
                                                                                : qsTr("Cadence mesurée")
                value: (root.st.measuredRate ?? 0) > 0
                       ? Units.format(root.st.measuredRate, "Hz", 5) + ((root.st.measuredRateUncertainty ?? 0) > 0
                             ? qsTr(" ± %1 %").arg(Units.compact(100 * root.st.measuredRateUncertainty, "", 2)) : "")
                       : "—"
            }
            Figure {
                name: qsTr("Débit")
                value: (root.st.bytesPerSecond ?? 0) > 0 ? Units.bytes(root.st.bytesPerSecond) + qsTr("/s") : "—"
            }
            Figure {
                readonly property int lost: (root.st.lostFrames ?? 0) + (root.st.droppedFrames ?? 0)
                name: qsTr("Trames perdues")
                value: Units.count(lost)
                tint: lost > 0 ? Theme.warning : Theme.text
            }
            Figure {
                readonly property int bad: (root.st.checksumErrors ?? 0) + (root.st.rejected ?? 0)
                name: qsTr("Rejetées")
                value: Units.count(bad)
                tint: bad > 0 ? Theme.warning : Theme.text
            }
            Figure {
                visible: (root.st.timeouts ?? 0) > 0
                name: qsTr("Sans réponse")
                value: Units.count(root.st.timeouts ?? 0)
                tint: Theme.warning
            }
            Figure {
                name: root.st.memoryFull ? qsTr("Mémoire pleine") : qsTr("En mémoire")
                value: (root.st.memoryText ?? "—") + ((root.st.recordedSeconds ?? 0) > 0 ? " · " + Units.duration(root.st.recordedSeconds) : "")
                tint: root.st.memoryFull ? Theme.error : Theme.text
            }
            Figure {
                visible: root.acquisition.recording
                name: qsTr("Sur disque")
                value: Units.count(root.st.recordingFrames ?? 0)
                tint: Theme.error
            }
            Figure {
                visible: root.acquisition.trigger.enabled ?? false
                name: qsTr("Captures")
                value: Units.count(root.acquisition.captureCount)
            }
            Item { Layout.fillWidth: true }
        }
        Label {
            visible: root.acquisition.warnings.length > 0
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: "⚠ " + root.acquisition.warnings[root.acquisition.warnings.length - 1]
                  + (root.acquisition.warnings.length > 1 ? qsTr("  (+%1)").arg(root.acquisition.warnings.length - 1) : "")
            color: Theme.warning
            font.pixelSize: Theme.smallText
            ToolTip.visible: warnArea.containsMouse
            ToolTip.text: root.acquisition.warnings.join("\n")
            MouseArea {
                id: warnArea
                anchors.fill: parent
                hoverEnabled: true
            }
        }
    }
}
