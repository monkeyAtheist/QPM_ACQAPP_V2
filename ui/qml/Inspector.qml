import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Right panel: the pages of the module on screen (for "Signaux", the measurements of the zone and the
// settings of the computations; for "Acquisition", the source and the format; for "Générateur", the
// composition of the signal and the measurements of its preview; for "Filtres", the filter in detail and
// the measurements of its preview, before and after; for "Modulations", the link in detail and the measurements
// of its signals sent and received), then the properties of the selected signal (always the last tab: signalTab). The session itself is in the top bar (its name, "Ouvrir ▾" for the recent ones).
Rectangle {
    id: root
    required property var session
    required property var acquisition
    required property var serialPorts
    required property var appSettings
    required property var analysis
    property var curves: null // the curves added in "Signaux" (their spectrograms have settings here)
    property var generator: null
    property var filters: null
    property var modulations: null
    property int module: 0 // 0 Signaux, 1 Acquisition, 2 Générateur, 3 Filtres, 4 Modulations
    property var signalId: 0
    property int tab: 0
    property int generatorChannel: 0 // the channel edited in the composition
    // The generator, the filters and the modulations have three tabs: their page, the measurements of their
    // preview, the signal.
    readonly property bool threeTabs: module === 2 || module === 3 || module === 4
    readonly property int signalTab: threeTabs ? 2 : 1
    signal tabSelected(int index)
    signal collapseRequested()
    signal firmwareRequested()
    signal generatorChannelSelected(int index)
    signal takeUpRequested(var id) // the recipe of a generated signal, back into the generator

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
                visible: !root.threeTabs
                Layout.fillWidth: true
                onCurrentIndexChanged: if (visible && currentIndex !== root.tab) root.tabSelected(currentIndex)
                TabButton { text: root.module === 1 ? qsTr("Acquisition") : qsTr("Mesures") }
                TabButton { text: qsTr("Signal") }
            }
            // The generator and the filters have three: their page, the measurements of their preview, the signal.
            TabBar {
                id: generatorTabs
                visible: root.threeTabs
                Layout.fillWidth: true
                onCurrentIndexChanged: if (visible && currentIndex !== root.tab) root.tabSelected(currentIndex)
                TabButton { text: root.module === 4 ? qsTr("Liaison") : root.module === 3 ? qsTr("Filtre") : qsTr("Composition") }
                TabButton { text: qsTr("Mesures") }
                TabButton { text: qsTr("Signal") }
            }
            // The tab follows the shell (a signal chosen opens "Signal"); a click on a tab tells the shell.
            Binding {
                target: tabs
                property: "currentIndex"
                value: Math.min(root.tab, 1)
            }
            Binding {
                target: generatorTabs
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
                visible: root.tab === 0 && root.module === 0
                analysis: root.analysis
                appSettings: root.appSettings
                curves: root.curves
                sampleRate: root.sampleRate
            }

            GeneratorPanel {
                anchors.fill: parent
                visible: root.tab === 0 && root.module === 2 && root.generator !== null
                generator: root.generator
                appSettings: root.appSettings
                channel: root.generatorChannel
                onChannelSelected: (index) => root.generatorChannelSelected(index)
            }

            // The measurements of the preview of the generator, and the settings of its spectrum.
            AnalysisPanel {
                anchors.fill: parent
                visible: root.tab === 1 && root.module === 2 && root.generator !== null
                analysis: root.generator ? root.generator.previewAnalysis : root.analysis
                appSettings: root.appSettings
                sampleRate: root.generator ? (root.generator.recipe.sampleRate ?? 0) : 0
                settingsKey: "generator/analysis"
                zoneHint: qsTr("Sur tout l'aperçu, %1")
            }

            FilterPanel {
                anchors.fill: parent
                visible: root.tab === 0 && root.module === 3 && root.filters !== null
                filters: root.filters
            }

            // The measurements of the preview of the filters (before and after), and the settings of their spectra.
            AnalysisPanel {
                anchors.fill: parent
                visible: root.tab === 1 && root.module === 3 && root.filters !== null
                analysis: root.filters ? root.filters.previewAnalysis : root.analysis
                appSettings: root.appSettings
                sampleRate: root.filters ? ((root.filters.input.sampleRate ?? 0) > 0 ? root.filters.input.sampleRate : 0) : 0
                settingsKey: "filters/analysis"
                zoneHint: qsTr("Sur tout l'aperçu, %1")
            }

            ModulationPanel {
                anchors.fill: parent
                visible: root.tab === 0 && root.module === 4 && root.modulations !== null
                modulations: root.modulations
            }

            // The measurements of the preview of the modulations (sent and received), and the settings of their spectra.
            AnalysisPanel {
                anchors.fill: parent
                visible: root.tab === 1 && root.module === 4 && root.modulations !== null
                analysis: root.modulations ? root.modulations.previewAnalysis : root.analysis
                appSettings: root.appSettings
                sampleRate: root.modulations ? (root.modulations.recipe.sampleRate ?? 0) : 0
                settingsKey: "modulations/analysis"
                zoneHint: qsTr("Sur tout l'aperçu, %1")
            }

            SignalProperties {
                anchors.fill: parent
                visible: root.tab === root.signalTab
                session: root.session
                signalId: root.signalId
                generator: root.generator
                onTakeUpRequested: (id) => root.takeUpRequested(id)
            }
        }
    }
}
