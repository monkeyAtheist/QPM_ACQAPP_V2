import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qacq.App

// Right panel, two tabs: the settings of the module on screen (the session for "Signaux", the source and
// the format for "Acquisition", the computation and the measurements for "Analyse"), and the properties
// of the selected signal.
Rectangle {
    id: root
    required property var session
    required property var acquisition
    required property var serialPorts
    required property var appSettings
    required property var analysis
    property int module: 0
    property var signalId: 0
    property int tab: 0
    signal tabSelected(int index)
    signal collapseRequested()
    signal firmwareRequested()
    signal recentFileChosen(string file)

    color: Theme.panel

    Rectangle {
        anchors.left: parent.left
        width: 1
        height: parent.height
        color: Theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.gap
        anchors.leftMargin: Theme.gap + 1
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            TabBar {
                id: tabs
                Layout.fillWidth: true
                onCurrentIndexChanged: if (currentIndex !== root.tab) root.tabSelected(currentIndex)
                TabButton { text: root.module === 1 ? qsTr("Acquisition") : root.module === 2 ? qsTr("Analyse") : qsTr("Session") }
                TabButton { text: qsTr("Signal") }
            }
            // The tab follows the shell (a signal chosen opens "Signal"); a click on a tab tells the shell.
            Binding {
                target: tabs
                property: "currentIndex"
                value: root.tab
            }
            FoldButton {
                pointsLeft: false
                toolTip: qsTr("Masquer les propriétés (F10)")
                onClicked: root.collapseRequested()
            }
        }

        // The pages fill this area exactly (anchored): a long text in a page never widens the panel.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            // The session.
            ScrollView {
                id: sessionPage
                anchors.fill: parent
                visible: root.tab === 0 && root.module !== 1 && root.module !== 2
                contentWidth: availableWidth
                clip: true
                ColumnLayout {
                    width: sessionPage.availableWidth
                    spacing: 10
                    SectionTitle { text: qsTr("Fichier") }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WrapAnywhere
                        text: root.session.fileUrl.toString().length > 0
                              ? decodeURIComponent(root.session.fileUrl.toString().replace(/^file:\/{2,3}/, ""))
                              : qsTr("Session pas encore enregistrée")
                        color: Theme.text
                        font.pixelSize: Theme.smallText
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: root.session.modified ? qsTr("Modifications non enregistrées (Ctrl+S)") : qsTr("Tout est enregistré")
                        color: root.session.modified ? Theme.warning : Theme.textMuted
                        font.pixelSize: Theme.smallText
                    }
                    SectionTitle { text: qsTr("Contenu") }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: root.session.signalList.count === 0 ? qsTr("Aucun signal")
                              : root.session.signalList.count === 1 ? qsTr("1 signal") : qsTr("%1 signaux").arg(root.session.signalList.count)
                        color: Theme.text
                        font.pixelSize: Theme.smallText
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Historique d'annulation : %1").arg(Units.bytes(root.session.signalList.historyBytes))
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                    }
                    SectionTitle {
                        visible: root.appSettings.recentFiles.length > 0
                        text: qsTr("Sessions récentes")
                    }
                    Repeater {
                        model: root.appSettings.recentFiles
                        delegate: ItemDelegate {
                            required property string modelData
                            Layout.fillWidth: true
                            implicitHeight: 34
                            text: modelData.split("/").pop()
                            font.pixelSize: Theme.smallText
                            onClicked: root.recentFileChosen(modelData)
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            ToolTip.text: modelData
                        }
                    }
                }
            }

            AcquisitionSettings {
                anchors.fill: parent
                visible: root.tab === 0 && root.module === 1
                acquisition: root.acquisition
                serialPorts: root.serialPorts
                appSettings: root.appSettings
                onFirmwareRequested: root.firmwareRequested()
            }

            AnalysisPanel {
                anchors.fill: parent
                visible: root.tab === 0 && root.module === 2
                analysis: root.analysis
                appSettings: root.appSettings
            }

            SignalProperties {
                anchors.fill: parent
                visible: root.tab === 1
                session: root.session
                signalId: root.signalId
            }
        }
    }
}
