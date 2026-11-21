import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Right panel, two tabs: the page of the module on screen (for "Signaux", the measurements of the zone and
// the settings of the computations; for "Acquisition", the source and the format), and the properties of
// the selected signal. The session itself is in the top bar (its name, "Ouvrir ▾" for the recent ones).
Rectangle {
    id: root
    required property var session
    required property var acquisition
    required property var serialPorts
    required property var appSettings
    required property var analysis
    property var curves: null // the curves added in "Signaux" (their spectrograms have settings here)
    property int module: 0 // 0 Signaux, 1 Acquisition
    property var signalId: 0
    property int tab: 0
    signal tabSelected(int index)
    signal collapseRequested()
    signal firmwareRequested()

    // The rate of the selected signal, for the resolution of the spectrograms (Δf, duration of a segment).
    property real sampleRate: 0
    function refreshRate() {
        const list = root.session.signalList
        root.sampleRate = root.signalId > 0 && list.rowOf(root.signalId) >= 0 ? (list.info(root.signalId).sampleRate ?? 0) : 0
    }
    onSignalIdChanged: refreshRate()
    Component.onCompleted: refreshRate()
    Connections {
        target: root.session.signalList
        function onDataChanged() { root.refreshRate() }
        function onCountChanged() { root.refreshRate() }
        function onModelReset() { root.refreshRate() }
    }

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
                TabButton { text: root.module === 1 ? qsTr("Acquisition") : qsTr("Mesures") }
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
                toolTip: qsTr("Masquer le panneau (F10)")
                onClicked: root.collapseRequested()
            }
        }

        // The pages fill this area exactly (anchored): a long text in a page never widens the panel.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

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
                visible: root.tab === 0 && root.module !== 1
                analysis: root.analysis
                appSettings: root.appSettings
                curves: root.curves
                sampleRate: root.sampleRate
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
